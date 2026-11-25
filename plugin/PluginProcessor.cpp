#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> parameters;
    parameters.reserve(static_cast<std::size_t>(subdoom::kParameterCount));
    const subdoom::Parameters defaults;

    for (const int index : subdoom::kHostParameterOrder)
    {
        const juce::ParameterID id { subdoom::parameterId(index), 1 };
        const juce::String name(subdoom::parameterHostName(index));
        const bool automatable = subdoom::parameterIsAutomatable(index);
        const auto kind = subdoom::parameterKind(index);

        switch (kind)
        {
            case subdoom::ParameterKind::toggle:
                parameters.push_back(std::make_unique<juce::AudioParameterBool>(
                    id, name, defaults.at(index) >= 0.5,
                    juce::AudioParameterBoolAttributes().withAutomatable(automatable)));
                break;

            case subdoom::ParameterKind::choice:
            {
                const subdoom::ChoiceList choices = subdoom::parameterChoices(index);
                juce::StringArray labels;
                for (int item = 0; item < choices.count; ++item)
                    labels.add(choices.names[item]);
                const int initial = juce::jlimit(
                    0, juce::jmax(0, choices.count - 1),
                    juce::roundToInt(defaults.at(index)));
                parameters.push_back(std::make_unique<juce::AudioParameterChoice>(
                    id, name, labels, initial,
                    juce::AudioParameterChoiceAttributes().withAutomatable(automatable)));
                break;
            }

            case subdoom::ParameterKind::continuous:
            {
                const subdoom::ParameterRange range = subdoom::parameterRange(index);
                parameters.push_back(std::make_unique<juce::AudioParameterFloat>(
                    id, name,
                    juce::NormalisableRange<float>(static_cast<float>(range.min),
                                                   static_cast<float>(range.max),
                                                   static_cast<float>(range.step)),
                    static_cast<float>(defaults.at(index)),
                    juce::AudioParameterFloatAttributes()
                        .withLabel(subdoom::parameterUnit(index))
                        .withAutomatable(automatable)));
                break;
            }

            default:
            {
                const auto unhandled = kind;
                juce::ignoreUnused(unhandled);
                jassertfalse;
                break;
            }
        }
    }

    return { parameters.begin(), parameters.end() };
}

bool isSwitchParameter(int index)
{
    return subdoom::parameterIsSwitch(index);
}

// Parameters travel in the project file as CSV so A/B and user presets survive
// a DAW reload without a second schema.
juce::String packSound(const subdoom::Parameters& params)
{
    juce::String text;
    for (int index = 1; index <= subdoom::kParameterCount; ++index)
    {
        if (index > 1)
            text << ",";
        text << juce::String(params.at(index), 6);
    }
    return text;
}

bool unpackSound(const juce::String& text, subdoom::Parameters& params)
{
    const juce::StringArray tokens = juce::StringArray::fromTokens(text, ",", "");
    if (tokens.size() != subdoom::kParameterCount)
        return false;

    subdoom::Parameters parsed;
    for (int index = 1; index <= subdoom::kParameterCount; ++index)
    {
        const juce::String token = tokens[index - 1].trim();
        if (token.isEmpty())
            return false;
        parsed.at(index) = token.getDoubleValue();
    }
    parsed.clampIntoRange();
    params = parsed;
    return true;
}

juce::Identifier ccProperty(int index)
{
    return juce::Identifier(juce::String("midi.cc.") + subdoom::parameterId(index));
}

} // namespace

SubdoomAudioProcessor::SubdoomAudioProcessor()
    : SubdoomAudioProcessor(PresetLibrary::userFolder())
{
}

SubdoomAudioProcessor::SubdoomAudioProcessor(const juce::File& presetRoot)
    : AudioProcessor(BusesProperties()
          .withInput("Input", juce::AudioChannelSet::stereo(), true)
          .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMS", createLayout()),
      library(presetRoot)
{
    for (auto& value : ccMap)
        value.store(-1, std::memory_order_relaxed);
    for (auto& value : pendingCc)
        value.store(-1, std::memory_order_relaxed);
    for (auto& value : lastCc)
        value.store(0, std::memory_order_relaxed);
    for (auto& value : presetStepCc)
        value.store(-1, std::memory_order_relaxed);
    for (auto& value : presetStepEdge)
        value.store(0, std::memory_order_relaxed);

    for (int index = 1; index <= subdoom::kParameterCount; ++index)
        raw[static_cast<std::size_t>(index)] =
            apvts.getRawParameterValue(subdoom::parameterId(index));

    loadMidiState();
    restoreCompareState();

    currentName = juce::String(subdoom::presetName(0));
    currentCategory = juce::String(subdoom::presetCategory(0));
    currentFactory = true;
    currentPreset = library.indexOfName(currentName, true);
    if (currentPreset < 0)
        currentPreset = 0;
    noteReportedProgram();
    baseline = subdoom::factoryPreset(0);

    abSound[0] = baseline;
    abSound[1] = baseline;

    applySound(baseline, true);

    storePresetIdentity();
    storeCompareState();
}

void SubdoomAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    dsp.prepare(sampleRate, samplesPerBlock);
    dsp.setParameters(readParameters());
    dsp.reset();
    prepared = true;
}

void SubdoomAudioProcessor::releaseResources()
{
    dsp.reset();
    prepared = false;
}

bool SubdoomAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainInputChannelSet() != layouts.getMainOutputChannelSet())
        return false;
    if (layouts.inputBuses.size() > 1 || layouts.outputBuses.size() > 1)
        return false;
    const auto set = layouts.getMainOutputChannelSet();
    return set == juce::AudioChannelSet::mono() || set == juce::AudioChannelSet::stereo();
}

void SubdoomAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // Armed for the next host program change. The change that arrived in this
    // same process() call already ran, which keeps a restore's stored slot
    // from landing in the first block.
    acceptHostProgram.store(true, std::memory_order_release);

    handleMidi(midi);

    // The audio thread drains the preset mailbox, so it can release the host-parameter
    // hold itself. Waiting for the editor's timer left the hold stuck whenever the
    // editor was closed, and every host or MIDI move was then ignored.
    if (holdHostParams.load(std::memory_order_acquire) && dsp.consumePresetApplied())
        holdHostParams.store(false, std::memory_order_release);

    if (!holdHostParams.load(std::memory_order_acquire))
        dsp.setParameters(readParameters());

    if (buffer.getNumChannels() <= 0)
        return;

    for (int channelIndex = 2; channelIndex < buffer.getNumChannels(); ++channelIndex)
        buffer.clear(channelIndex, 0, buffer.getNumSamples());

    float* left = buffer.getWritePointer(0);
    float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : left;

    blockMetrics = subdoom::BlockMetrics {};
    dsp.process(left, right, buffer.getNumSamples(), &blockMetrics);
    publishLevels(blockMetrics);
}

juce::AudioProcessorEditor* SubdoomAudioProcessor::createEditor()
{
    return new SubdoomAudioProcessorEditor(*this);
}

double SubdoomAudioProcessor::getTailLengthSeconds() const
{
    return subdoom::tailSeconds(readParameters());
}

int SubdoomAudioProcessor::getNumPrograms()
{
    // 128 programs so a host can deliver PC 0-127. The index is the code, not
    // the alphabetical browser row. VST3 only creates the program parameter
    // when this is greater than 1, and that parameter is how most hosts send
    // a program change.
    return kHostProgramCount;
}

int SubdoomAudioProcessor::getCurrentProgram()
{
    const juce::ScopedTryLock lock(presetLock);
    if (lock.isLocked())
        noteReportedProgram();
    return reportedProgram.load(std::memory_order_relaxed);
}

void SubdoomAudioProcessor::setCurrentProgram(int index)
{
    if (index < 0 || index >= kHostProgramCount)
        return;
    // A project restore is followed by the program the host stored. Until a
    // block has run, that call is not a preset load. MIDI still selects.
    if (!acceptHostProgram.load(std::memory_order_acquire))
        return;
    if (hostProgramGate.load(std::memory_order_acquire) > 0)
        return;

    pendingProgram.store(index, std::memory_order_relaxed);
    triggerAsyncUpdate();
}

const juce::String SubdoomAudioProcessor::getProgramName(int index)
{
    if (index < 0 || index >= kHostProgramCount)
        return {};

    const juce::ScopedLock lock(presetLock);
    const int owner = library.indexForProgram(index);
    if (owner < 0)
        return "---";
    return library.entry(owner).name;
}

void SubdoomAudioProcessor::changeProgramName(int, const juce::String&)
{
}

void SubdoomAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    storeMidiState();
    storeCompareState();
    storePresetIdentity();

    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void SubdoomAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr)
        return;

    const juce::ValueTree state = juce::ValueTree::fromXml(*xml);
    if (!state.isValid() || !state.hasType(apvts.state.getType()))
        return;

    acceptHostProgram.store(false, std::memory_order_release);
    pendingProgram.store(-1, std::memory_order_relaxed);
    pendingPresetStep.store(0, std::memory_order_relaxed);
    apvts.replaceState(state);
    loadMidiState();
    restorePresetIdentity();
    restoreCompareState();

    const subdoom::Parameters restored = readParameters();
    {
        const juce::ScopedLock lock(presetLock);
        baseline = restored;
    }
    applySound(restored, true);
    triggerAsyncUpdate();
}

subdoom::Parameters SubdoomAudioProcessor::readParameters() const
{
    subdoom::Parameters params;
    for (int index = 1; index <= subdoom::kParameterCount; ++index)
    {
        if (const auto* value = raw[static_cast<std::size_t>(index)])
            params.at(index) = static_cast<double>(value->load(std::memory_order_relaxed));
    }
    params.clampIntoRange();
    return params;
}

void SubdoomAudioProcessor::writeParameters(const subdoom::Parameters& params)
{
    for (int index = 1; index <= subdoom::kParameterCount; ++index)
    {
        if (auto* parameter = apvts.getParameter(subdoom::parameterId(index)))
        {
            const float plain = static_cast<float>(params.at(index));
            parameter->setValueNotifyingHost(parameter->convertTo0to1(plain));
        }
    }
}

subdoom::Parameters SubdoomAudioProcessor::captureSound() const
{
    return readParameters();
}

void SubdoomAudioProcessor::applySound(const subdoom::Parameters& sound, bool immediate)
{
    if (prepared && !immediate)
    {
        // Hold the host reads until the DSP has installed the sound, and publish the
        // host values before the request: the block that releases the hold then reads
        // the new sound back. Publishing after the request left a window where a fast
        // audio thread installed the preset and re-read the previous host values.
        pendingPreset = sound;
        holdHostParams.store(true, std::memory_order_release);
        holdMiss = 0;
        writeParameters(sound);
        dsp.requestPreset(sound);
        return;
    }

    {
        // State restore arrives on the message thread while the audio thread may be
        // in processBlock. The wrappers hold callbackLock around the audio callback,
        // so this is the same barrier servicePresetHold() gets from suspendProcessing().
        const juce::ScopedLock lock(getCallbackLock());
        dsp.setParameters(sound);
        dsp.reset();
    }

    writeParameters(sound);
}

void SubdoomAudioProcessor::servicePresetHold()
{
    if (!holdHostParams.load(std::memory_order_acquire))
        return;

    if (dsp.consumePresetApplied())
    {
        holdHostParams.store(false, std::memory_order_release);
        holdMiss = 0;
        return;
    }
    if (++holdMiss < 8)
        return;

    suspendProcessing(true);
    if (!dsp.consumePresetApplied())
    {
        dsp.setParameters(pendingPreset);
        dsp.reset();
    }
    holdHostParams.store(false, std::memory_order_release);
    holdMiss = 0;
    suspendProcessing(false);
}

// Presets ---------------------------------------------------------------------

juce::File SubdoomAudioProcessor::presetFolder()
{
    return PresetLibrary::userFolder();
}

void SubdoomAudioProcessor::refreshPresets()
{
    {
        const juce::ScopedLock lock(presetLock);
        library.reload();

        const int found = library.indexOfName(currentName, currentFactory);
        currentPreset = found;
        noteReportedProgram();
    }

    notifyProgramListChanged();
}

void SubdoomAudioProcessor::storePresetIdentity()
{
    apvts.state.setProperty("presetName", currentName, nullptr);
    apvts.state.setProperty("presetCategory", currentCategory, nullptr);
    apvts.state.setProperty("presetNotes", currentNotes, nullptr);
    apvts.state.setProperty("presetFactory", currentFactory, nullptr);
    apvts.state.setProperty("presetIndex", currentPreset, nullptr);
}

void SubdoomAudioProcessor::restorePresetIdentity()
{
    const juce::ScopedLock lock(presetLock);

    const juce::String name = apvts.state.getProperty("presetName", juce::String()).toString();
    const juce::String category =
        apvts.state.getProperty("presetCategory", juce::String()).toString();
    const juce::String notes = apvts.state.getProperty("presetNotes", juce::String()).toString();
    const bool factory = static_cast<bool>(apvts.state.getProperty("presetFactory", true));

    if (name.isEmpty())
    {
        // State written before presets had an identity: name the sound if it
        // still matches a factory preset, otherwise be honest about it.
        const subdoom::Parameters live = readParameters();
        int match = -1;
        for (int index = 0; index < subdoom::kFactoryPresetCount; ++index)
        {
            if (subdoom::parametersMatch(live, subdoom::factoryPreset(index)))
            {
                match = index;
                break;
            }
        }

        if (match >= 0)
        {
            currentPreset = match;
            currentName = juce::String(subdoom::presetName(match));
            currentCategory = juce::String(subdoom::presetCategory(match));
            currentFactory = true;
        }
        else
        {
            currentPreset = -1;
            currentName = "UNSAVED";
            currentCategory = "User";
            currentFactory = false;
        }
        currentNotes.clear();
        noteReportedProgram();
        return;
    }

    currentName = name;
    currentCategory = category.isEmpty() ? "User" : category;
    currentNotes = notes;
    currentFactory = factory;
    currentPreset = library.indexOfName(name, factory);
    noteReportedProgram();
}

int SubdoomAudioProcessor::currentPresetIndex() const
{
    const juce::ScopedLock lock(presetLock);
    return currentPreset;
}

juce::String SubdoomAudioProcessor::currentPresetName() const
{
    const juce::ScopedLock lock(presetLock);
    return currentName;
}

juce::String SubdoomAudioProcessor::currentPresetCategory() const
{
    const juce::ScopedLock lock(presetLock);
    return currentCategory;
}

juce::String SubdoomAudioProcessor::currentPresetNotes() const
{
    const juce::ScopedLock lock(presetLock);
    return currentNotes;
}

bool SubdoomAudioProcessor::currentPresetIsFactory() const
{
    const juce::ScopedLock lock(presetLock);
    return currentFactory;
}

bool SubdoomAudioProcessor::presetIsDirty() const
{
    const subdoom::Parameters live = readParameters();
    const juce::ScopedLock lock(presetLock);
    return !subdoom::parametersMatch(live, baseline);
}

bool SubdoomAudioProcessor::saveIsAvailable() const
{
    return !currentPresetIsFactory() || presetIsDirty();
}

void SubdoomAudioProcessor::publishLevels(const subdoom::BlockMetrics& metrics) noexcept
{
    if (meterReset.exchange(false, std::memory_order_acquire))
    {
        meterPeak = 0.0;
        meterSumSquares = 0.0;
        meterSamples = 0;
    }

    meterPeak = std::max(meterPeak, metrics.peak);
    meterSumSquares += metrics.sumSquares;
    meterSamples += metrics.samples;

    const double rms = meterSamples > 0
        ? std::sqrt(meterSumSquares / static_cast<double>(meterSamples))
        : 0.0;
    publishedPeak.store(static_cast<float>(meterPeak), std::memory_order_relaxed);
    publishedRms.store(static_cast<float>(rms), std::memory_order_relaxed);
}

void SubdoomAudioProcessor::consumeLevels(float& peak, float& rms) noexcept
{
    peak = publishedPeak.load(std::memory_order_relaxed);
    rms = publishedRms.load(std::memory_order_relaxed);
    meterReset.store(true, std::memory_order_release);
}

void SubdoomAudioProcessor::notifyProgramListChanged()
{
    hostProgramGate.fetch_add(1, std::memory_order_acq_rel);
    updateHostDisplay(juce::AudioProcessorListener::ChangeDetails{}
                          .withProgramChanged(true)
                          .withNonParameterStateChanged(true));
    hostProgramGate.fetch_sub(1, std::memory_order_acq_rel);
}

juce::String SubdoomAudioProcessor::editKey(bool factory, const juce::String& name)
{
    return (factory ? "f:" : "u:") + name;
}

void SubdoomAudioProcessor::loadPreset(int index)
{
    const bool leavingDirty = presetIsDirty();
    const subdoom::Parameters live = readParameters();

    subdoom::Parameters sound;
    {
        const juce::ScopedLock lock(presetLock);
        if (index < 0 || index >= library.size())
            return;

        const int previous = currentPreset;
        const PresetLibrary::Entry entry = library.entry(index);
        const juce::String arriving = editKey(entry.factory, entry.name);

        if (previous >= 0 && previous != index && leavingDirty)
            editedSounds[editKey(currentFactory, currentName)] = live;
        if (previous == index)
            editedSounds.erase(arriving);

        const auto found = editedSounds.find(arriving);
        const bool restoreEdit = previous != index && found != editedSounds.end();
        sound = restoreEdit ? found->second : entry.params;
        currentPreset = index;
        currentName = entry.name;
        currentCategory = entry.category;
        currentNotes = entry.notes;
        currentFactory = entry.factory;
        // Baseline stays the file or factory preset, so a restored edit keeps *.
        baseline = entry.params;
        storePresetIdentity();
        noteReportedProgram();
    }

    applySound(sound, false);
    notifyProgramListChanged();
}

void SubdoomAudioProcessor::stepPreset(int delta)
{
    int count = 0;
    int next = 0;
    {
        const juce::ScopedLock lock(presetLock);
        count = library.size();
        next = currentPreset;
    }
    if (count <= 0)
        return;

    next = next < 0 ? 0 : next + delta;
    next = ((next % count) + count) % count;
    loadPreset(next);
}

void SubdoomAudioProcessor::revertPreset()
{
    subdoom::Parameters sound;
    {
        const juce::ScopedLock lock(presetLock);
        if (currentPreset >= 0)
            editedSounds.erase(editKey(currentFactory, currentName));
        sound = baseline;
    }
    applySound(sound, false);
}

bool SubdoomAudioProcessor::setPresetProgram(int index, int code, juce::String& error)
{
    const juce::ScopedLock lock(presetLock);
    const bool wrote = library.setProgramCode(index, code, error);
    if (wrote)
        noteReportedProgram();
    return wrote;
}

bool SubdoomAudioProcessor::savePresetCopy(const juce::String& name,
                                           const juce::String& category,
                                           const juce::String& notes,
                                           bool allowOverwrite,
                                           juce::String& error)
{
    const bool dirty = presetIsDirty();
    const subdoom::Parameters sound = readParameters();

    {
        const juce::ScopedLock lock(presetLock);
        const juce::String previousKey = currentPreset >= 0 ? editKey(currentFactory, currentName)
                                                            : juce::String();

        int savedIndex = -1;
        if (!library.saveUser(name, category, notes, sound, allowOverwrite, savedIndex, error))
            return false;

        if (dirty && previousKey.isNotEmpty())
            editedSounds[previousKey] = sound;

        // The identity comes from the entry the file produced, not from the typed
        // name: the stored name is sanitised and capped at 64 bytes, so looking the
        // typed name up again can miss and leave the preset unsaved.
        const PresetLibrary::Entry& entry = library.entry(savedIndex);
        if (entry.name.isNotEmpty())
        {
            currentName = entry.name;
            currentCategory = entry.category;
            currentNotes = entry.notes;
        }
        else
        {
            currentName = name.trim();
            currentCategory = category.trim().isEmpty() ? juce::String("User") : category.trim();
            currentNotes = notes;
        }
        currentFactory = false;
        currentPreset = savedIndex;
        baseline = sound;
        editedSounds.erase(editKey(false, currentName));
        storePresetIdentity();
        noteReportedProgram();
    }

    notifyProgramListChanged();
    return true;
}

bool SubdoomAudioProcessor::saveCurrentPreset(const juce::String& notes, juce::String& error)
{
    const subdoom::Parameters sound = readParameters();

    {
        const juce::ScopedLock lock(presetLock);

        if (currentFactory)
        {
            error = "Factory presets cannot be overwritten. Use Save As.";
            return false;
        }
        if (currentPreset < 0)
        {
            error = "No user preset is selected. Use Save As.";
            return false;
        }
        if (!library.overwrite(currentPreset, notes, sound, error))
            return false;

        // Overwriting keeps the stored identity, including the sanitised notes.
        editedSounds.erase(editKey(false, currentName));
        baseline = sound;
        currentNotes = library.entry(currentPreset).notes;
        storePresetIdentity();
        noteReportedProgram();
    }

    notifyProgramListChanged();
    return true;
}

bool SubdoomAudioProcessor::renamePreset(int index, const juce::String& newName,
                                         juce::String& error)
{
    {
        const juce::ScopedLock lock(presetLock);

        if (index < 0 || index >= library.size())
        {
            error = "No preset selected";
            return false;
        }
        const juce::String oldName = library.entry(index).name;
        int renamedIndex = index;
        if (!library.renameUser(index, newName, renamedIndex, error))
            return false;

        if (renamedIndex >= 0)
        {
            const juce::String storedName = library.entry(renamedIndex).name;
            const auto found = editedSounds.find(editKey(false, oldName));
            if (found != editedSounds.end())
            {
                const subdoom::Parameters stashed = found->second;
                editedSounds.erase(found);
                if (storedName.isNotEmpty())
                    editedSounds[editKey(false, storedName)] = stashed;
            }
        }

        if (index == currentPreset)
        {
            // The stored name and the sanitised identity come from the file.
            const PresetLibrary::Entry& entry = library.entry(renamedIndex);
            currentName = entry.name.isNotEmpty() ? entry.name : newName.trim();
            currentCategory = entry.name.isNotEmpty() ? entry.category : currentCategory;
            currentPreset = renamedIndex;
            storePresetIdentity();
        }
        else
        {
            // Renaming re-sorts the list, so the current entry may have moved.
            currentPreset = library.indexOfName(currentName, currentFactory);
        }
        noteReportedProgram();
    }

    notifyProgramListChanged();
    return true;
}

bool SubdoomAudioProcessor::deletePreset(int index, juce::String& error)
{
    {
        const juce::ScopedLock lock(presetLock);
        juce::String removedName;
        if (index >= 0 && index < library.size() && !library.entry(index).factory)
            removedName = library.entry(index).name;
        if (!library.removeUser(index, error))
            return false;
        if (removedName.isNotEmpty())
            editedSounds.erase(editKey(false, removedName));

        if (index == currentPreset)
        {
            // The sound stays; its name does not, because the file that named it is gone.
            currentPreset = -1;
            currentName = "UNSAVED";
            currentCategory = "User";
            currentNotes.clear();
            currentFactory = false;
            baseline = readParameters();
            storePresetIdentity();
        }
        else
        {
            // Deleting a preset that sorts before the current one shifts its index.
            currentPreset = library.indexOfName(currentName, currentFactory);
        }
        noteReportedProgram();
    }

    notifyProgramListChanged();
    return true;
}

// A/B -------------------------------------------------------------------------

void SubdoomAudioProcessor::storeCompareState()
{
    apvts.state.setProperty("abSlot", abActive, nullptr);
    apvts.state.setProperty("abA", packSound(abSound[0]), nullptr);
    apvts.state.setProperty("abB", packSound(abSound[1]), nullptr);
}

void SubdoomAudioProcessor::restoreCompareState()
{
    abSound[0] = readParameters();
    abSound[1] = abSound[0];
    unpackSound(apvts.state.getProperty("abA", juce::String()).toString(), abSound[0]);
    unpackSound(apvts.state.getProperty("abB", juce::String()).toString(), abSound[1]);
    abActive = juce::jlimit(0, 1, static_cast<int>(apvts.state.getProperty("abSlot", 0)));
}

void SubdoomAudioProcessor::toggleCompare()
{
    {
        const subdoom::Parameters live = readParameters();
        abSound[static_cast<std::size_t>(abActive)] = live;
        abActive = 1 - abActive;
    }
    storeCompareState();
    applySound(abSound[static_cast<std::size_t>(abActive)], false);
}

// MIDI ------------------------------------------------------------------------

bool SubdoomAudioProcessor::isPresetStepTarget(int index) noexcept
{
    return index == kPresetPrevTarget || index == kPresetNextTarget;
}

int SubdoomAudioProcessor::presetStepSlot(int index) const noexcept
{
    return index == kPresetNextTarget ? 1 : 0;
}

void SubdoomAudioProcessor::noteReportedProgram()
{
    int code = 0;
    if (currentPreset >= 0 && currentPreset < library.size())
    {
        const int stored = library.programCode(currentPreset);
        if (stored >= 0 && stored < kHostProgramCount)
            code = stored;
    }
    reportedProgram.store(code, std::memory_order_relaxed);
}

int SubdoomAudioProcessor::ccForParameter(int index) const noexcept
{
    if (index < 1 || index > subdoom::kParameterCount)
        return -1;
    return ccMap[static_cast<std::size_t>(index)].load(std::memory_order_relaxed);
}

int SubdoomAudioProcessor::ccForControl(int index) const noexcept
{
    if (isPresetStepTarget(index))
        return presetStepCc[static_cast<std::size_t>(presetStepSlot(index))].load(
            std::memory_order_relaxed);
    return ccForParameter(index);
}

juce::String SubdoomAudioProcessor::mappingLabel(int index) const
{
    const int cc = ccForControl(index);
    return cc < 0 ? juce::String("--") : ("CC " + juce::String(cc));
}

void SubdoomAudioProcessor::beginLearn(int index) noexcept
{
    if ((index < 1 || index > subdoom::kParameterCount) && !isPresetStepTarget(index))
        return;
    learnIndex.store(index, std::memory_order_release);
}

void SubdoomAudioProcessor::clearMapping(int index)
{
    if (isPresetStepTarget(index))
    {
        const auto slot = static_cast<std::size_t>(presetStepSlot(index));
        presetStepCc[slot].store(-1, std::memory_order_relaxed);
        presetStepEdge[slot].store(0, std::memory_order_relaxed);
        apvts.state.removeProperty(index == kPresetNextTarget ? "midi.presetNext"
                                                              : "midi.presetPrev",
                                   nullptr);
        return;
    }

    if (index < 1 || index > subdoom::kParameterCount)
        return;
    ccMap[static_cast<std::size_t>(index)].store(-1, std::memory_order_relaxed);
    lastCc[static_cast<std::size_t>(index)].store(0, std::memory_order_relaxed);
    apvts.state.removeProperty(ccProperty(index), nullptr);
}

void SubdoomAudioProcessor::clearAllMappings()
{
    for (int index = 1; index <= subdoom::kParameterCount; ++index)
        clearMapping(index);
    clearMapping(kPresetPrevTarget);
    clearMapping(kPresetNextTarget);
}

void SubdoomAudioProcessor::setMidiChannel(int newChannel)
{
    channel.store(juce::jlimit(0, 16, newChannel), std::memory_order_relaxed);
    storeMidiState();
}

void SubdoomAudioProcessor::setProgramChangeEnabled(bool enabled)
{
    programChangeOn.store(enabled, std::memory_order_relaxed);
    storeMidiState();
}

void SubdoomAudioProcessor::bindMapping(int index, int cc)
{
    if (cc < 0 || cc >= 128)
        return;
    if ((index < 1 || index > subdoom::kParameterCount) && !isPresetStepTarget(index))
        return;

    for (int other = 1; other <= subdoom::kParameterCount; ++other)
    {
        if (other == index)
            continue;
        if (ccMap[static_cast<std::size_t>(other)].load(std::memory_order_relaxed) != cc)
            continue;
        lastCc[static_cast<std::size_t>(other)].store(0, std::memory_order_relaxed);
        // A preset arrow has to own the controller, or the knob still listed on
        // that CC would keep it. Knob-to-knob learn leaves the old row, and the
        // controller moves every knob still listed on it.
        if (isPresetStepTarget(index))
            ccMap[static_cast<std::size_t>(other)].store(-1, std::memory_order_relaxed);
    }

    for (int slot = 0; slot < 2; ++slot)
    {
        const int target = slot == 0 ? kPresetPrevTarget : kPresetNextTarget;
        if (target == index)
            continue;
        if (presetStepCc[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed) == cc)
        {
            presetStepCc[static_cast<std::size_t>(slot)].store(-1, std::memory_order_relaxed);
            presetStepEdge[static_cast<std::size_t>(slot)].store(0, std::memory_order_relaxed);
        }
    }

    if (isPresetStepTarget(index))
    {
        const auto slot = static_cast<std::size_t>(presetStepSlot(index));
        presetStepCc[slot].store(cc, std::memory_order_relaxed);
        // The press that was learned is not already a held switch.
        presetStepEdge[slot].store(0, std::memory_order_relaxed);
    }
    else
    {
        ccMap[static_cast<std::size_t>(index)].store(cc, std::memory_order_relaxed);
        lastCc[static_cast<std::size_t>(index)].store(0, std::memory_order_relaxed);
    }
    storeMidiState();

    activity.fetch_add(1, std::memory_order_relaxed);
}

void SubdoomAudioProcessor::storeMidiState()
{
    apvts.state.setProperty("midi.mapVersion", 1, nullptr);
    apvts.state.setProperty("midi.initialised", true, nullptr);
    apvts.state.setProperty("midi.channel", channel.load(std::memory_order_relaxed), nullptr);
    apvts.state.setProperty("midi.programChange", programChangeOn.load(std::memory_order_relaxed),
                            nullptr);

    for (int index = 1; index <= subdoom::kParameterCount; ++index)
    {
        const int cc = ccMap[static_cast<std::size_t>(index)].load(std::memory_order_relaxed);
        if (cc >= 0)
            apvts.state.setProperty(ccProperty(index), cc, nullptr);
        else
            apvts.state.removeProperty(ccProperty(index), nullptr);
    }

    const int previous = presetStepCc[0].load(std::memory_order_relaxed);
    const int next = presetStepCc[1].load(std::memory_order_relaxed);
    if (previous >= 0)
        apvts.state.setProperty("midi.presetPrev", previous, nullptr);
    else
        apvts.state.removeProperty("midi.presetPrev", nullptr);
    if (next >= 0)
        apvts.state.setProperty("midi.presetNext", next, nullptr);
    else
        apvts.state.removeProperty("midi.presetNext", nullptr);
}

void SubdoomAudioProcessor::loadMidiState()
{
    channel.store(juce::jlimit(0, 16, static_cast<int>(apvts.state.getProperty("midi.channel", 0))),
                  std::memory_order_relaxed);
    programChangeOn.store(static_cast<bool>(apvts.state.getProperty("midi.programChange", true)),
                          std::memory_order_relaxed);

    const bool initialised = static_cast<bool>(
        apvts.state.getProperty("midi.initialised", false));
    const int mapVersion = static_cast<int>(apvts.state.getProperty("midi.mapVersion", 0));

    bool anyBinding = false;
    for (int index = 1; index <= subdoom::kParameterCount; ++index)
    {
        const int cc = static_cast<int>(apvts.state.getProperty(ccProperty(index), -1));
        const int valid = (cc >= 0 && cc < 128) ? cc : -1;
        ccMap[static_cast<std::size_t>(index)].store(valid, std::memory_order_relaxed);
        lastCc[static_cast<std::size_t>(index)].store(0, std::memory_order_relaxed);
        if (valid >= 0)
            anyBinding = true;
    }

    const auto stepCc = [&anyBinding, this](const char* property) {
        const int cc = static_cast<int>(apvts.state.getProperty(property, -1));
        const int valid = (cc >= 0 && cc < 128) ? cc : -1;
        if (valid >= 0)
            anyBinding = true;
        return valid;
    };
    presetStepCc[0].store(stepCc("midi.presetPrev"), std::memory_order_relaxed);
    presetStepCc[1].store(stepCc("midi.presetNext"), std::memory_order_relaxed);
    presetStepEdge[0].store(0, std::memory_order_relaxed);
    presetStepEdge[1].store(0, std::memory_order_relaxed);

    // Projects that have never carried a map start on the Akai LPD8 layout
    // matching Doomgazer. Existing saved projects and cleared maps stay untouched.
    const bool installDefaults = !initialised && mapVersion < 1 && !anyBinding;
    if (installDefaults)
        installLpd8Map();

    apvts.state.setProperty("midi.mapVersion", 1, nullptr);
    apvts.state.setProperty("midi.initialised", true, nullptr);
    if (installDefaults)
        storeMidiState();
}

void SubdoomAudioProcessor::installLpd8Map() noexcept
{
    // Matches Doomgazer's Akai LPD8 banks:
    // - 18 panel knobs in order on CC 12-29
    // - Gate on CC 77, Limit on CC 88, Preset Prev on CC 89, Preset Next on CC 90, Mute on CC 92
    struct Mapping
    {
        int parameter;
        int cc;
    };
    static constexpr Mapping mappings[] = {
        { 1, 12 },  { 3, 13 },  { 4, 14 },  { 5, 15 },  { 6, 16 },  { 7, 17 },
        { 8, 18 },  { 9, 19 },  { 10, 20 }, { 11, 21 }, { 12, 22 }, { 13, 23 },
        { 14, 24 }, { 15, 25 }, { 16, 26 }, { 17, 27 }, { 18, 28 }, { 19, 29 },
        { 2, 77 },  { 20, 88 }, { 21, 92 },
    };

    for (const Mapping& mapping : mappings)
    {
        ccMap[static_cast<std::size_t>(mapping.parameter)].store(mapping.cc,
                                                                  std::memory_order_relaxed);
        lastCc[static_cast<std::size_t>(mapping.parameter)].store(0, std::memory_order_relaxed);
    }

    presetStepCc[0].store(89, std::memory_order_relaxed);
    presetStepCc[1].store(90, std::memory_order_relaxed);
    presetStepEdge[0].store(0, std::memory_order_relaxed);
    presetStepEdge[1].store(0, std::memory_order_relaxed);
}

void SubdoomAudioProcessor::handleMidi(const juce::MidiBuffer& midi)
{
    const int channelFilter = channel.load(std::memory_order_relaxed);
    const bool allowProgramChange = programChangeOn.load(std::memory_order_relaxed);

    for (const auto metadata : midi)
    {
        const juce::MidiMessage message = metadata.getMessage();
        if (channelFilter != 0 && message.getChannel() != channelFilter)
            continue;

        // All-notes-off and all-sound-off are controller messages (CC 123 and CC 120),
        // so they have to be handled before the isController() branch. Resetting the
        // edge detector keeps a held switch from toggling when the host panics.
        if (message.isAllNotesOff() || message.isAllSoundOff())
        {
            // Counted by the controller branch below; only the edge state is reset here.
            for (auto& edgeState : lastCc)
                edgeState.store(0, std::memory_order_relaxed);
            for (auto& edgeState : presetStepEdge)
                edgeState.store(0, std::memory_order_relaxed);
        }

        if (message.isController())
        {
            const int cc = message.getControllerNumber();
            const int value = message.getControllerValue();
            activity.fetch_add(1, std::memory_order_relaxed);

            const int learning = learnIndex.load(std::memory_order_relaxed);
            if (learning >= 1)
            {
                pendingLearnIndex.store(learning, std::memory_order_relaxed);
                pendingLearnCc.store(cc, std::memory_order_relaxed);
                learnIndex.store(-1, std::memory_order_relaxed);
                triggerAsyncUpdate();
                continue;
            }

            bool stepped = false;
            for (int slot = 0; slot < 2; ++slot)
            {
                if (presetStepCc[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed) != cc)
                    continue;

                const int previous = presetStepEdge[static_cast<std::size_t>(slot)].load(
                    std::memory_order_relaxed);
                presetStepEdge[static_cast<std::size_t>(slot)].store(value, std::memory_order_relaxed);
                if (previous < 64 && value >= 64)
                {
                    pendingPresetStep.fetch_add(slot == 0 ? -1 : 1, std::memory_order_relaxed);
                    triggerAsyncUpdate();
                }
                stepped = true;
                break;
            }
            if (stepped)
                continue;

            for (int index = 1; index <= subdoom::kParameterCount; ++index)
            {
                if (ccMap[static_cast<std::size_t>(index)].load(std::memory_order_relaxed) != cc)
                    continue;

                int target = -1;
                if (isSwitchParameter(index))
                {
                    const int previous = lastCc[static_cast<std::size_t>(index)].load(std::memory_order_relaxed);
                    lastCc[static_cast<std::size_t>(index)].store(value, std::memory_order_relaxed);
                    if (previous >= 64 || value < 64)
                        continue;

                    bool on = false;
                    if (const auto* valuePointer = raw[static_cast<std::size_t>(index)])
                        on = valuePointer->load(std::memory_order_relaxed) >= 0.5f;
                    target = on ? 0 : 1000;
                }
                else
                {
                    target = value * 1000 / 127;
                }

                pendingCc[static_cast<std::size_t>(index)].store(target, std::memory_order_relaxed);
                triggerAsyncUpdate();
            }
        }
        else if (message.isProgramChange() && allowProgramChange)
        {
            activity.fetch_add(1, std::memory_order_relaxed);
            // The number is a PC code, not a library index. The message thread
            // looks up whichever preset was assigned that code.
            const int code = message.getProgramChangeNumber();
            pendingProgram.store(code, std::memory_order_relaxed);
            triggerAsyncUpdate();
        }
    }
}

void SubdoomAudioProcessor::handleAsyncUpdate()
{
    const int learnAt = pendingLearnIndex.exchange(-1, std::memory_order_acq_rel);
    const int learnedCc = pendingLearnCc.exchange(-1, std::memory_order_acq_rel);
    if (learnAt >= 1 && learnedCc >= 0 && learnedCc < 128)
        bindMapping(learnAt, learnedCc);

    for (int index = 1; index <= subdoom::kParameterCount; ++index)
    {
        const int value = pendingCc[static_cast<std::size_t>(index)].exchange(
            -1, std::memory_order_acq_rel);
        if (value < 0)
            continue;
        if (auto* parameter = apvts.getParameter(subdoom::parameterId(index)))
            parameter->setValueNotifyingHost(
                juce::jlimit(0.0f, 1.0f, static_cast<float>(value) / 1000.0f));
    }

    const int steps = pendingPresetStep.exchange(0, std::memory_order_acq_rel);
    if (steps != 0)
        stepPreset(steps);

    const int program = pendingProgram.exchange(-1, std::memory_order_acq_rel);
    if (program >= 0)
    {
        int resolved = -1;
        {
            const juce::ScopedLock lock(presetLock);
            resolved = library.indexForProgram(program);
        }
        // The same code again leaves the preset selected, edit included.
        // A code nobody owns does not fall back to a browser row.
        if (resolved >= 0 && resolved != currentPresetIndex())
            loadPreset(resolved);
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SubdoomAudioProcessor();
}
