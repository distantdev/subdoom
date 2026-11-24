#pragma once

#include "PresetLibrary.h"
#include "subdoom/Processor.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <map>

class SubdoomAudioProcessor : public juce::AudioProcessor,
                              private juce::AsyncUpdater
{
public:
    SubdoomAudioProcessor();

    // The preset folder is a parameter so tests own a temp library. JUCE uses the
    // default constructor, which points at the per-user folder.
    explicit SubdoomAudioProcessor(const juce::File& presetRoot);
    ~SubdoomAudioProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Subdoom"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    // Host program index is the MIDI PC code, 0-127. The browser list is separate.
    static constexpr int kHostProgramCount = 128;
    // Learn targets for the header arrows. They are not parameters.
    static constexpr int kPresetPrevTarget = 1001;
    static constexpr int kPresetNextTarget = 1002;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // Presets -----------------------------------------------------------------
    PresetLibrary& presets() noexcept { return library; }
    const PresetLibrary& presets() const noexcept { return library; }
    static juce::File presetFolder();

    void refreshPresets();
    int currentPresetIndex() const;
    juce::String currentPresetName() const;
    juce::String currentPresetCategory() const;
    juce::String currentPresetNotes() const;
    bool currentPresetIsFactory() const;
    bool presetIsDirty() const;

    // True when SAVE has somewhere to write: a user preset is selected, or the sound
    // has been edited. An unsaved sound with no edits still counts.
    bool saveIsAvailable() const;

    // Reads the parameters and installs them through the DSP fade mailbox.
    void loadPreset(int index);
    void stepPreset(int delta);
    void revertPreset();
    // code is 0-127, or -1 for a blank PC that program-change switching skips.
    bool setPresetProgram(int index, int code, juce::String& error);

    bool savePresetCopy(const juce::String& name, const juce::String& category,
                        const juce::String& notes, bool allowOverwrite, juce::String& error);
    bool saveCurrentPreset(const juce::String& notes, juce::String& error);
    bool renamePreset(int index, const juce::String& newName, juce::String& error);
    bool deletePreset(int index, juce::String& error);

    void toggleCompare();
    int compareSlot() const noexcept { return abActive; }

    // MIDI --------------------------------------------------------------------
    int ccForParameter(int index) const noexcept;
    // Parameters and the two preset-step targets.
    int ccForControl(int index) const noexcept;
    juce::String mappingLabel(int index) const;
    int learnTarget() const noexcept { return learnIndex.load(std::memory_order_relaxed); }
    void beginLearn(int index) noexcept;
    void clearMapping(int index);
    void clearAllMappings();
    int midiChannel() const noexcept { return channel.load(std::memory_order_relaxed); }
    void setMidiChannel(int newChannel);
    bool programChangeEnabled() const noexcept
    {
        return programChangeOn.load(std::memory_order_relaxed);
    }
    void setProgramChangeEnabled(bool enabled);
    int midiActivity() const noexcept { return activity.load(std::memory_order_relaxed); }

    // Engine ------------------------------------------------------------------
    void servicePresetHold();
    subdoom::Parameters captureSound() const;

    // Peak and RMS of everything since the last call, for the editor's meter. The
    // audio thread accumulates between reads; the reader resets the window.
    void consumeLevels(float& peak, float& rms) noexcept;

    subdoom::Processor& engine() noexcept { return dsp; }
    const subdoom::Processor& engine() const noexcept { return dsp; }

private:
    void handleAsyncUpdate() override;
    void handleMidi(const juce::MidiBuffer& midi);
    void notifyProgramListChanged();
    void publishLevels(const subdoom::BlockMetrics& metrics) noexcept;
    void bindMapping(int index, int cc);
    static juce::String editKey(bool factory, const juce::String& name);
    void storeMidiState();
    void loadMidiState();
    void noteReportedProgram();
    static bool isPresetStepTarget(int index) noexcept;
    int presetStepSlot(int index) const noexcept;
    void storePresetIdentity();
    void restorePresetIdentity();
    void storeCompareState();
    void restoreCompareState();
    void applySound(const subdoom::Parameters& sound, bool immediate);
    subdoom::Parameters readParameters() const;
    void writeParameters(const subdoom::Parameters& params);

    subdoom::Processor dsp;
    PresetLibrary library;

    mutable juce::CriticalSection presetLock;
    subdoom::Parameters baseline;
    // Edits kept for the session when a dirty preset is left. The file stays
    // untouched, so coming back restores the * and the sound.
    std::map<juce::String, subdoom::Parameters> editedSounds;
    int currentPreset = 0;
    juce::String currentName;
    juce::String currentCategory;
    juce::String currentNotes;
    bool currentFactory = true;

    std::array<subdoom::Parameters, 2> abSound {};
    int abActive = 0;

    std::atomic<bool> holdHostParams { false };
    int holdMiss = 0;
    subdoom::Parameters pendingPreset;
    bool prepared = false;

    subdoom::BlockMetrics blockMetrics;
    double meterPeak = 0.0;
    double meterSumSquares = 0.0;
    long long meterSamples = 0;
    std::atomic<float> publishedPeak { 0.0f };
    std::atomic<float> publishedRms { 0.0f };
    std::atomic<bool> meterReset { false };

    std::array<std::atomic<int>, subdoom::kParameterCount + 1> ccMap;
    std::array<std::atomic<int>, subdoom::kParameterCount + 1> pendingCc;
    std::array<std::atomic<int>, subdoom::kParameterCount + 1> lastCc;
    std::atomic<int> learnIndex { -1 };
    std::atomic<int> pendingLearnIndex { -1 };
    std::atomic<int> pendingLearnCc { -1 };
    std::atomic<int> channel { 0 };
    std::atomic<bool> programChangeOn { true };
    std::atomic<int> pendingProgram { -1 };
    std::atomic<int> pendingPresetStep { 0 };
    std::array<std::atomic<int>, 2> presetStepCc;
    std::array<std::atomic<int>, 2> presetStepEdge;
    // The PC of the preset on screen, or 0 when that preset's code is blank.
    std::atomic<int> reportedProgram { 0 };
    // Host program changes are ignored until a block has run, and again across
    // setStateInformation, so a restored chunk is not replaced by the slot the
    // host had stored. MIDI program change does not consult this.
    std::atomic<bool> acceptHostProgram { false };
    // Ignores setCurrentProgram while we are telling the host the program changed,
    // so its echo of the previous slot cannot undo a step.
    std::atomic<int> hostProgramGate { 0 };
    std::atomic<int> activity { 0 };

    // Resolved once so the audio callback never looks a parameter up by string.
    std::array<std::atomic<float>*, subdoom::kParameterCount + 1> raw {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SubdoomAudioProcessor)
};
