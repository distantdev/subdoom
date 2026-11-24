#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_events/juce_events.h>

#include <atomic>
#include <cmath>
#include <thread>

int main(int argc, char** argv)
{
    // The MIDI path posts AsyncUpdater callbacks, so the wrapper needs a
    // MessageManager even though these tests never create a window. It has to be
    // created here rather than as a global: JUCE's own statics are not ready yet
    // when a namespace-scope initialiser runs.
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;
    return Catch::Session().run(argc, argv);
}

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 256;

// A preset library the test owns. Without this the processor would scan, and the
// save tests would write into, the user's real %APPDATA%\Subdoom\Presets folder.
struct TempPresetRoot
{
    TempPresetRoot()
        : folder (juce::File::getSpecialLocation(juce::File::tempDirectory)
                      .getChildFile("subdoom-tests-" + juce::Uuid().toString()))
    {
        folder.createDirectory();
    }

    ~TempPresetRoot() { folder.deleteRecursively(); }

    juce::File folder;
};

void processBlockOnce(SubdoomAudioProcessor& processor, juce::MidiBuffer& midi)
{
    juce::AudioBuffer<float> buffer(2, kBlockSize);
    buffer.clear();
    processor.processBlock(buffer, midi);
}

void runBlocks(SubdoomAudioProcessor& processor, int blocks)
{
    juce::MidiBuffer empty;
    for (int block = 0; block < blocks; ++block)
        processBlockOnce(processor, empty);
}

// Pending AsyncUpdater work, e.g. a learned CC being written to its parameter.
void pumpMessages()
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
}

float hostParameter(SubdoomAudioProcessor& processor, const char* id)
{
    const std::atomic<float>* value = processor.apvts.getRawParameterValue(id);
    REQUIRE(value != nullptr);
    return value->load(std::memory_order_relaxed);
}

void setHostParameter(SubdoomAudioProcessor& processor, const char* id, float plain)
{
    auto* parameter = processor.apvts.getParameter(id);
    REQUIRE(parameter != nullptr);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(plain));
}

// 128 characters, so the stored name has to be capped.
juce::String longPresetName()
{
    return juce::String::repeatedString("Long Preset Name ", 8).trim();
}

struct ProgramChangeCounter : juce::AudioProcessorListener
{
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override
    {
        if (details.programChanged)
            programChanges.fetch_add(1, std::memory_order_relaxed);
    }

    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}

    std::atomic<int> programChanges { 0 };
};

// Some hosts answer a program-list change by putting the previous program back.
struct HostPutsProgramBack : juce::AudioProcessorListener
{
    int restore = -1;

    void audioProcessorChanged(juce::AudioProcessor* processor, const ChangeDetails& details) override
    {
        if (details.programChanged && restore >= 0 && processor != nullptr)
            processor->setCurrentProgram(restore);
    }

    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
};

void sendController(SubdoomAudioProcessor& processor, int cc, int value)
{
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::controllerEvent(1, cc, value), 0);
    processBlockOnce(processor, midi);
    pumpMessages();
}

void sendProgram(SubdoomAudioProcessor& processor, int program)
{
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::programChange(1, program), 0);
    processBlockOnce(processor, midi);
    pumpMessages();
}

juce::TextButton* findButton(juce::Component& root, const juce::String& text)
{
    if (auto* button = dynamic_cast<juce::TextButton*>(&root))
        if (button->getButtonText() == text)
            return button;

    for (int index = 0; index < root.getNumChildComponents(); ++index)
        if (auto* child = root.getChildComponent(index))
            if (auto* found = findButton(*child, text))
                return found;
    return nullptr;
}

} // namespace

// The editor timer used to be the only place that released the host-parameter
// hold taken by a preset load. With the editor closed the hold stayed set and the
// engine ignored the host, the DAW's generic UI and CC moves for good.
TEST_CASE("a preset load releases the host hold with no editor open", "[preset][hold]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);
    processor.prepareToPlay(kSampleRate, kBlockSize);

    const int funeral = processor.presets().indexOfName("FUNERAL WALL", true);
    REQUIRE(funeral >= 0);
    processor.loadPreset(funeral); // installed through the preset mailbox
    runBlocks(processor, 16); // drain the mailbox and the 12 ms fade

    const double loaded = processor.presets().entry(funeral).params.masterDb;
    REQUIRE(processor.engine().parameters().masterDb == Catch::Approx(loaded).margin(1.0e-3));

    setHostParameter(processor, "masterDb", -1.5f);
    runBlocks(processor, 2);

    REQUIRE(processor.engine().parameters().masterDb == Catch::Approx(-1.5).margin(1.0e-4));
}

// A load that happens while the editor is open is covered by servicePresetHold(),
// but the release must not depend on that path either.
TEST_CASE("rapid preset requests leave the last one loaded", "[preset][hold]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);
    processor.prepareToPlay(kSampleRate, kBlockSize);

    const int wool = processor.presets().indexOfName("WOOL HYMN", true);
    const int serpent = processor.presets().indexOfName("SERPENT", true);
    const int empress = processor.presets().indexOfName("EMPRESS", true);
    REQUIRE(wool >= 0);
    REQUIRE(serpent >= 0);
    REQUIRE(empress >= 0);
    processor.loadPreset(wool);
    runBlocks(processor, 2);
    processor.loadPreset(serpent); // two more requests before the audio thread drains,
    processor.loadPreset(empress); // which alternates the two mailbox slots
    runBlocks(processor, 16);

    REQUIRE(processor.engine().parameters().masterDb
            == Catch::Approx(processor.presets().entry(empress).params.masterDb).margin(1.0e-3));
}

TEST_CASE("state restore reaches the engine", "[state]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor source(root.folder);
    source.prepareToPlay(kSampleRate, kBlockSize);
    setHostParameter(source, "masterDb", 1.5f);
    setHostParameter(source, "thumpDb", -4.0f);

    juce::MemoryBlock state;
    source.getStateInformation(state);

    SubdoomAudioProcessor target(root.folder);
    target.prepareToPlay(kSampleRate, kBlockSize);
    REQUIRE(std::abs(target.engine().parameters().masterDb - 1.5) > 1.0e-3);

    target.setStateInformation(state.getData(), static_cast<int>(state.getSize()));

    // The immediate path writes the DSP directly. Without the callback-lock barrier
    // that write races the audio thread reading the same parameter block.
    REQUIRE(target.engine().parameters().masterDb == Catch::Approx(1.5).margin(1.0e-4));
    REQUIRE(target.engine().parameters().thumpDb == Catch::Approx(-4.0).margin(1.0e-4));
}

TEST_CASE("state restore waits for the audio callback lock", "[state][threading]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor source(root.folder);
    source.prepareToPlay(kSampleRate, kBlockSize);
    setHostParameter(source, "masterDb", 1.5f);

    juce::MemoryBlock state;
    source.getStateInformation(state);

    SubdoomAudioProcessor target(root.folder);
    target.prepareToPlay(kSampleRate, kBlockSize);

    std::atomic<bool> holding { false };
    std::thread holder([&target, &holding] {
        const juce::ScopedLock lock(target.getCallbackLock());
        holding.store(true, std::memory_order_release);
        juce::Thread::sleep(500);
    });

    while (!holding.load(std::memory_order_acquire))
        juce::Thread::sleep(1);

    const double start = juce::Time::getMillisecondCounterHiRes();
    target.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    const double elapsed = juce::Time::getMillisecondCounterHiRes() - start;

    holder.join();

    // Rendering the restore at once means the audio thread was still inside the
    // window the DSP mutation needs to be excluded from.
    REQUIRE(elapsed >= 100.0);
}

// All-notes-off and all-sound-off are CC 123 and CC 120, not status bytes of their
// own. The old code tested them in an else-branch the controller branch had already
// claimed, so the switch edge detector was never reset.
TEST_CASE("all-notes-off resets the switch edge detector", "[midi]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);
    processor.prepareToPlay(kSampleRate, kBlockSize);

    // Gate is parameter 2 and starts on CC 77 in the default LPD8 map.
    REQUIRE(processor.ccForParameter(2) == 77);
    REQUIRE(hostParameter(processor, "gate") < 0.5f);

    const auto send = [&processor](const juce::MidiMessage& message) {
        juce::MidiBuffer midi;
        midi.addEvent(message, 0);
        processBlockOnce(processor, midi);
        pumpMessages();
    };

    send(juce::MidiMessage::controllerEvent(1, 77, 127));
    REQUIRE(hostParameter(processor, "gate") >= 0.5f);

    send(juce::MidiMessage::allNotesOff(1));

    // A fresh press has to toggle again. While the panic reset was unreachable the
    // detector still believed the switch was held down and ignored it.
    send(juce::MidiMessage::controllerEvent(1, 77, 127));
    REQUIRE(hostParameter(processor, "gate") < 0.5f);
}

TEST_CASE("a switch still toggles on its own controller", "[midi]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);
    processor.prepareToPlay(kSampleRate, kBlockSize);

    const auto send = [&processor](int cc, int value) {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::controllerEvent(1, cc, value), 0);
        processBlockOnce(processor, midi);
        pumpMessages();
    };

    send(77, 127);
    send(77, 0); // release: no second toggle
    REQUIRE(hostParameter(processor, "gate") >= 0.5f);

    send(77, 127);
    send(77, 0);
    REQUIRE(hostParameter(processor, "gate") < 0.5f);
}

TEST_CASE("a saved preset takes its identity from the stored file", "[preset][library]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);
    processor.prepareToPlay(kSampleRate, kBlockSize);

    const juce::String typed = longPresetName();
    REQUIRE(typed.length() > 64);

    juce::String error;
    REQUIRE(processor.savePresetCopy(typed, "Doom", "held low", false, error));

    // The name in the file is capped at 64 bytes, so looking the typed name up again
    // used to leave the fresh preset unlisted: no selection, and SAVE then refused.
    REQUIRE(processor.currentPresetIndex() >= 0);
    REQUIRE(processor.currentPresetName().length() <= 64);
    REQUIRE(!processor.currentPresetIsFactory());
    REQUIRE(!processor.presetIsDirty());
    REQUIRE(processor.saveIsAvailable());
    REQUIRE(processor.presets().entry(processor.currentPresetIndex()).name
            == processor.currentPresetName());
}

TEST_CASE("a saved user preset loads the sound that was written", "[preset][library]")
{
    TempPresetRoot root;

    {
        SubdoomAudioProcessor processor(root.folder);
        processor.prepareToPlay(kSampleRate, kBlockSize);
        setHostParameter(processor, "masterDb", -12.0f);
        setHostParameter(processor, "thumpDb", 6.5f);
        setHostParameter(processor, "gate", 1.0f);

        juce::String error;
        REQUIRE(processor.savePresetCopy("CELLAR", "User", "low cellar", false, error));
        const int saved = processor.currentPresetIndex();
        REQUIRE(saved >= 0);
        REQUIRE(!processor.presets().entry(saved).factory);
        REQUIRE(processor.presets().entry(saved).params.masterDb == Catch::Approx(-12.0).margin(1.0e-3));
        REQUIRE(processor.presets().entry(saved).params.thumpDb == Catch::Approx(6.5).margin(1.0e-3));

        setHostParameter(processor, "masterDb", 0.0f);
        processor.loadPreset(saved);
        runBlocks(processor, 16);

        REQUIRE(processor.engine().parameters().masterDb == Catch::Approx(-12.0).margin(1.0e-3));
        REQUIRE(processor.engine().parameters().thumpDb == Catch::Approx(6.5).margin(1.0e-3));
        REQUIRE(processor.engine().parameters().gate == Catch::Approx(1.0).margin(1.0e-3));
        REQUIRE(hostParameter(processor, "masterDb") == Catch::Approx(-12.0f).margin(0.05f));

        setHostParameter(processor, "masterDb", -3.0f);
        REQUIRE(processor.saveCurrentPreset("low cellar", error));
        setHostParameter(processor, "masterDb", 1.0f);
        processor.loadPreset(processor.currentPresetIndex());
        runBlocks(processor, 16);
        REQUIRE(processor.engine().parameters().masterDb == Catch::Approx(-3.0).margin(1.0e-3));
    }

    SubdoomAudioProcessor reopened(root.folder);
    reopened.prepareToPlay(kSampleRate, kBlockSize);
    const int index = reopened.presets().indexOfName("CELLAR", false);
    REQUIRE(index >= 0);
    REQUIRE(!reopened.presets().entry(index).factory);
    reopened.loadPreset(index);
    runBlocks(reopened, 16);
    REQUIRE(reopened.engine().parameters().masterDb == Catch::Approx(-3.0).margin(1.0e-3));
    REQUIRE(reopened.engine().parameters().thumpDb == Catch::Approx(6.5).margin(1.0e-3));
    REQUIRE(reopened.currentPresetName() == "CELLAR");
    REQUIRE_FALSE(reopened.currentPresetIsFactory());
}

TEST_CASE("a restored user preset survives the host program that follows it", "[preset][state]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor source(root.folder);
    source.prepareToPlay(kSampleRate, kBlockSize);

    juce::String error;
    REQUIRE(source.savePresetCopy("CELLAR", "User", "", false, error));
    setHostParameter(source, "masterDb", -9.0f);

    juce::MemoryBlock state;
    source.getStateInformation(state);

    SubdoomAudioProcessor target(root.folder);
    target.prepareToPlay(kSampleRate, kBlockSize);
    target.setStateInformation(state.getData(), static_cast<int>(state.getSize()));

    // The host's stored program slot is not a preset. Applying it must leave
    // the restored user preset alone.
    target.setCurrentProgram(0);
    target.setCurrentProgram(3);
    runBlocks(target, 16);
    REQUIRE(target.engine().parameters().masterDb == Catch::Approx(-9.0).margin(1.0e-3));
    REQUIRE(target.currentPresetName() == "CELLAR");
}

TEST_CASE("a rename keeps the identity the library stored", "[preset][library]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);

    juce::String error;
    REQUIRE(processor.savePresetCopy("SHORT NAME", "User", "", false, error));
    const int before = processor.currentPresetIndex();
    REQUIRE(before >= 0);

    REQUIRE(processor.renamePreset(before, longPresetName(), error));

    REQUIRE(processor.currentPresetIndex() >= 0);
    REQUIRE(processor.currentPresetName().length() <= 64);
    REQUIRE(processor.presets().entry(processor.currentPresetIndex()).name
            == processor.currentPresetName());
}

TEST_CASE("a rename that cannot move its file leaves the preset untouched", "[preset][library]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);

    juce::String error;
    REQUIRE(processor.savePresetCopy("KEEPER", "User", "", false, error));
    const int index = processor.currentPresetIndex();
    REQUIRE(index >= 0);

    // A non-empty directory on the destination path makes the file move fail:
    // JUCE deletes an empty one before moving, but RemoveDirectory refuses this one.
    const juce::File blocker = root.folder.getChildFile("BLOCKED.subdoom");
    REQUIRE(blocker.createDirectory());
    REQUIRE(blocker.getChildFile("keep.txt").replaceWithText("keep"));

    REQUIRE_FALSE(processor.renamePreset(index, "BLOCKED", error));

    // Writing the new name before moving used to leave the file renamed inside while
    // the move failed, so a rescan showed the new name under the old filename.
    const juce::File kept = root.folder.getChildFile("KEEPER.subdoom");
    REQUIRE(kept.existsAsFile());
    REQUIRE(kept.loadFileAsString().contains("name=KEEPER"));
    REQUIRE(processor.presets().indexOfName("KEEPER", false) >= 0);
}

TEST_CASE("deleting the loaded preset keeps the sound saveable", "[preset][library]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);
    processor.prepareToPlay(kSampleRate, kBlockSize);

    juce::String error;
    REQUIRE(processor.savePresetCopy("TO DELETE", "User", "", false, error));
    const int index = processor.currentPresetIndex();
    REQUIRE(index >= 0);

    setHostParameter(processor, "masterDb", 0.5f);
    runBlocks(processor, 1);
    const double soundDb = processor.engine().parameters().masterDb;

    REQUIRE(processor.deletePreset(index, error));
    REQUIRE(processor.currentPresetIndex() == -1);
    REQUIRE(processor.currentPresetName() == "UNSAVED");
    REQUIRE(processor.engine().parameters().masterDb == Catch::Approx(soundDb).margin(1.0e-6));

    // Nothing to revert to, but the sound still has to be savable: it no longer has
    // a file, so the SAVE button must stay live instead of greying out.
    REQUIRE(!processor.presetIsDirty());
    REQUIRE(processor.saveIsAvailable());
}

TEST_CASE("the meter window follows the latest level, not the loudest", "[meter]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);
    processor.prepareToPlay(kSampleRate, kBlockSize);
    setHostParameter(processor, "roomMix", 0.0f); // no room tail in the measurement
    runBlocks(processor, 1);

    const auto processTone = [&processor](float amplitude) {
        juce::AudioBuffer<float> buffer(2, kBlockSize);
        juce::MidiBuffer midi;
        for (int i = 0; i < kBlockSize; ++i)
        {
            const float sample = amplitude
                * std::sin(2.0f * juce::MathConstants<float>::pi * 100.0f
                           * static_cast<float>(i) / static_cast<float>(kSampleRate));
            buffer.setSample(0, i, sample);
            buffer.setSample(1, i, sample);
        }
        processor.processBlock(buffer, midi);
    };

    float peak = 0.0f;
    float rms = 0.0f;
    processTone(0.5f);
    processor.consumeLevels(peak, rms);
    REQUIRE(peak > 0.05f);
    REQUIRE(rms > 0.0f);
    REQUIRE(rms <= peak);
    const float loudPeak = peak;

    // Let the filter ring from the loud block die away, discarding that window.
    for (int block = 0; block < 32; ++block)
        runBlocks(processor, 1);
    processor.consumeLevels(peak, rms);

    // A quiet block now reports the quiet level. If the reader's reset were lost,
    // the window would still hold the loud peak.
    processTone(0.02f);
    processor.consumeLevels(peak, rms);
    REQUIRE(peak > 0.001f);
    REQUIRE(peak < loudPeak * 0.25f);
}

TEST_CASE("a clean factory preset does not offer SAVE", "[preset]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);

    REQUIRE(processor.currentPresetIsFactory());
    REQUIRE(!processor.presetIsDirty());
    REQUIRE(!processor.saveIsAvailable());

    setHostParameter(processor, "masterDb", 3.0f);
    REQUIRE(processor.saveIsAvailable());
}

TEST_CASE("A/B stores and recalls both sounds", "[preset][compare]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);
    processor.prepareToPlay(kSampleRate, kBlockSize);

    setHostParameter(processor, "masterDb", 1.5f);
    runBlocks(processor, 4);
    REQUIRE(processor.compareSlot() == 0);

    processor.toggleCompare(); // store A, flip to B
    REQUIRE(processor.compareSlot() == 1);
    runBlocks(processor, 16);

    setHostParameter(processor, "masterDb", -2.5f);
    runBlocks(processor, 4);

    processor.toggleCompare(); // store B, flip back to A
    REQUIRE(processor.compareSlot() == 0);
    runBlocks(processor, 16);

    REQUIRE(processor.engine().parameters().masterDb == Catch::Approx(1.5).margin(1.0e-4));
}

TEST_CASE("a preset save tells the host the program list changed", "[preset][host]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);

    ProgramChangeCounter counter;
    processor.addListener(&counter);

    juce::String error;
    REQUIRE(processor.savePresetCopy("HOST NOTICE", "User", "", false, error));
    REQUIRE(counter.programChanges.load(std::memory_order_relaxed) > 0);

    const int afterSave = counter.programChanges.load(std::memory_order_relaxed);
    REQUIRE(processor.deletePreset(processor.currentPresetIndex(), error));
    REQUIRE(counter.programChanges.load(std::memory_order_relaxed) > afterSave);

    processor.removeListener(&counter);
}

TEST_CASE("preset stepping wraps both ways", "[preset]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);
    const int last = processor.presets().size() - 1;
    REQUIRE(last > 0);

    processor.loadPreset(last);
    pumpMessages();
    REQUIRE(processor.currentPresetIndex() == last);

    HostPutsProgramBack host;
    host.restore = last;
    processor.addListener(&host);
    processor.stepPreset(1);
    processor.removeListener(&host);

    REQUIRE(processor.currentPresetIndex() == 0);
    REQUIRE(processor.currentPresetName() == processor.presets().entry(0).name);

    pumpMessages();
    processor.setCurrentProgram(last);
    REQUIRE(processor.currentPresetIndex() == 0);

    processor.loadPreset(last);
    REQUIRE(processor.currentPresetIndex() == last);
    processor.stepPreset(1);
    REQUIRE(processor.currentPresetIndex() == 0);

    processor.loadPreset(0);
    pumpMessages();
    processor.stepPreset(-1);
    REQUIRE(processor.currentPresetIndex() == last);
    REQUIRE(processor.currentPresetName() == processor.presets().entry(last).name);
}

TEST_CASE("the next preset button wraps after its click is dispatched", "[preset]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);
    processor.prepareToPlay(kSampleRate, kBlockSize);
    const int last = processor.presets().size() - 1;
    processor.loadPreset(last);
    pumpMessages();

    SubdoomAudioProcessorEditor editor(processor);
    editor.setSize(1100, 680);

    REQUIRE(findButton(editor, "LOAD") != nullptr);
    REQUIRE(findButton(editor, "SAVE") != nullptr);
    REQUIRE(findButton(editor, "SAVE AS") != nullptr);
    REQUIRE(findButton(editor, "RENAME") != nullptr);
    REQUIRE(findButton(editor, "DELETE") != nullptr);

    auto* next = findButton(editor, ">");
    REQUIRE(next != nullptr);

    HostPutsProgramBack host;
    host.restore = last;
    processor.addListener(&host);
    next->triggerClick();
    pumpMessages();
    processor.removeListener(&host);

    REQUIRE(processor.currentPresetIndex() == 0);

    pumpMessages();
    processor.setCurrentProgram(last);
    REQUIRE(processor.currentPresetIndex() == 0);
}

TEST_CASE("leaving a dirty preset keeps the edit", "[preset]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);
    processor.loadPreset(0);
    const float factoryMaster = hostParameter(processor, "masterDb");

    setHostParameter(processor, "masterDb", 4.0f);
    REQUIRE(processor.presetIsDirty());

    processor.stepPreset(1);
    REQUIRE(processor.currentPresetIndex() == 1);
    REQUIRE_FALSE(processor.presetIsDirty());

    processor.stepPreset(-1);
    REQUIRE(processor.currentPresetIndex() == 0);
    REQUIRE(hostParameter(processor, "masterDb") == Catch::Approx(4.0f).margin(0.05f));
    REQUIRE(processor.presetIsDirty());

    processor.revertPreset();
    REQUIRE_FALSE(processor.presetIsDirty());
    REQUIRE(hostParameter(processor, "masterDb") == Catch::Approx(factoryMaster).margin(0.05f));

    processor.stepPreset(1);
    processor.stepPreset(-1);
    REQUIRE(processor.currentPresetIndex() == 0);
    REQUIRE_FALSE(processor.presetIsDirty());
    REQUIRE(hostParameter(processor, "masterDb") == Catch::Approx(factoryMaster).margin(0.05f));
}

TEST_CASE("a stashed edit follows a rename and dies with a delete", "[preset]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);

    juce::String error;
    setHostParameter(processor, "masterDb", -12.0f);
    REQUIRE(processor.savePresetCopy("CELLAR", "User", "", false, error));
    setHostParameter(processor, "masterDb", 4.0f);
    REQUIRE(processor.presetIsDirty());

    const int cellar = processor.currentPresetIndex();
    processor.stepPreset(1);
    REQUIRE(processor.currentPresetIndex() != cellar);

    REQUIRE(processor.renamePreset(cellar, "VAULT", error));
    const int vault = processor.presets().indexOfName("VAULT", false);
    REQUIRE(vault >= 0);
    processor.loadPreset(vault);
    REQUIRE(hostParameter(processor, "masterDb") == Catch::Approx(4.0f).margin(0.05f));
    REQUIRE(processor.presetIsDirty());

    const int away = processor.currentPresetIndex() == 0 ? 1 : 0;
    processor.loadPreset(away);
    const int vaultNow = processor.presets().indexOfName("VAULT", false);
    REQUIRE(processor.deletePreset(vaultNow, error));

    // A new file of the same name must not inherit the edit delete dropped.
    // Saving through the processor would clear that stash on its own.
    REQUIRE(PresetLibrary::writeFile(root.folder.getChildFile("VAULT.subdoom"),
                                      "VAULT", "User", "",
                                      subdoom::factoryPreset(0), -1, error));
    processor.refreshPresets();
    const int recreated = processor.presets().indexOfName("VAULT", false);
    REQUIRE(recreated >= 0);
    processor.loadPreset(recreated);
    REQUIRE_FALSE(processor.presetIsDirty());
    REQUIRE(hostParameter(processor, "masterDb")
            == Catch::Approx(static_cast<float>(subdoom::factoryPreset(0).masterDb)).margin(0.05f));
}

TEST_CASE("program changes follow the assigned PC, including a blank", "[preset][midi]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);
    processor.prepareToPlay(kSampleRate, kBlockSize);

    const int lowMass = processor.presets().indexOfName("LOW MASS", true);
    const int dragon = processor.presets().indexOfName("DRAGON LUNG", true);
    REQUIRE(lowMass >= 0);
    REQUIRE(dragon >= 0);
    REQUIRE(processor.presets().programCode(lowMass) == 0);
    REQUIRE(processor.presets().programCode(dragon) == 4);

    juce::String error;
    REQUIRE(processor.setPresetProgram(lowMass, -1, error));
    REQUIRE(error.isEmpty());
    REQUIRE(processor.presets().programCode(lowMass) == -1);
    REQUIRE(root.folder.getChildFile("program-map.txt").loadFileAsString().contains("0=-"));

    // Next and previous still visit a blank PC.
    const int beside = lowMass == 0 ? 1 : lowMass - 1;
    processor.loadPreset(beside);
    pumpMessages();
    processor.stepPreset(lowMass > beside ? 1 : -1);
    REQUIRE(processor.currentPresetIndex() == lowMass);

    processor.loadPreset(beside);
    pumpMessages();
    sendProgram(processor, 0);
    REQUIRE(processor.currentPresetIndex() == beside);

    REQUIRE(processor.setPresetProgram(dragon, 40, error));
    pumpMessages();
    sendProgram(processor, 40);
    REQUIRE(processor.currentPresetIndex() == dragon);

    // 4 is nobody's code now. It must not fall back to list slot 4.
    sendProgram(processor, 4);
    REQUIRE(processor.currentPresetIndex() == dragon);
    processor.setCurrentProgram(4);
    pumpMessages();
    REQUIRE(processor.currentPresetIndex() == dragon);

    REQUIRE(processor.savePresetCopy("CELLAR", "User", "", false, error));
    const int cellar = processor.currentPresetIndex();
    REQUIRE(cellar >= 0);
    REQUIRE(!processor.presets().entry(cellar).factory);
    REQUIRE(processor.presets().programCode(cellar) == -1);
    const juce::String cellarText = processor.presets().entry(cellar).file.loadFileAsString();
    REQUIRE(cellarText.contains("subdoom-preset 1"));
    REQUIRE(cellarText.contains("pc=-"));

    REQUIRE(processor.setPresetProgram(cellar, 70, error));
    REQUIRE(processor.presets().programCode(cellar) == 70);
    REQUIRE(processor.presets().entry(cellar).file.loadFileAsString().contains("pc=70"));

    pumpMessages();
    sendProgram(processor, 70);
    REQUIRE(processor.currentPresetName() == "CELLAR");

    const int serpent = processor.presets().indexOfName("SERPENT", true);
    REQUIRE(serpent >= 0);
    REQUIRE(serpent != cellar);
    REQUIRE(processor.setPresetProgram(serpent, 70, error));
    REQUIRE(processor.presets().programCode(cellar) == -1);
    REQUIRE(processor.presets().programCode(serpent) == 70);
    REQUIRE(processor.presets().entry(cellar).file.loadFileAsString().contains("pc=-"));
    pumpMessages();
    sendProgram(processor, 70);
    REQUIRE(processor.currentPresetIndex() == serpent);

    REQUIRE(root.folder.getChildFile("OLD.subdoom").replaceWithText(
        "subdoom-preset 1\n"
        "name=OLD BANK\n"
        "category=User\n"
        "futureKey=1\n"));

    SubdoomAudioProcessor again(root.folder);
    const int againCellar = again.presets().indexOfName("CELLAR", false);
    const int oldBank = again.presets().indexOfName("OLD BANK", false);
    const int againLow = again.presets().indexOfName("LOW MASS", true);
    const int againDragon = again.presets().indexOfName("DRAGON LUNG", true);
    const int againSerpent = again.presets().indexOfName("SERPENT", true);
    REQUIRE(againLow >= 0);
    REQUIRE(againDragon >= 0);
    REQUIRE(againSerpent >= 0);
    REQUIRE(again.presets().programCode(againLow) == -1);
    REQUIRE(again.presets().programCode(againDragon) == 40);
    REQUIRE(again.presets().programCode(againSerpent) == 70);
    REQUIRE(againCellar >= 0);
    REQUIRE(again.presets().programCode(againCellar) == -1);
    REQUIRE(oldBank >= 0);
    REQUIRE(!again.presets().entry(oldBank).factory);
    REQUIRE(again.presets().programCode(oldBank) == -1);

    again.prepareToPlay(kSampleRate, kBlockSize);
    again.loadPreset(1);
    pumpMessages();
    sendProgram(again, 99);
    REQUIRE(again.currentPresetIndex() == 1);

    // After audio is running, the host program number is the PC code.
    const int ownerOf3 = again.presets().indexForProgram(3);
    REQUIRE(ownerOf3 >= 0);
    again.setCurrentProgram(3);
    pumpMessages();
    REQUIRE(again.currentPresetIndex() == ownerOf3);
}

TEST_CASE("a host program change selects the PC code", "[preset][host]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);
    processor.prepareToPlay(kSampleRate, kBlockSize);

    REQUIRE(processor.getNumPrograms() == 128);

    juce::String error;
    const int lowMass = processor.presets().indexOfName("LOW MASS", true);
    REQUIRE(lowMass >= 0);
    REQUIRE(processor.setPresetProgram(lowMass, 34, error));
    REQUIRE(processor.getProgramName(34) == "LOW MASS");
    REQUIRE(processor.getProgramName(120) == "---");

    const int away = lowMass == 0 ? 1 : 0;
    processor.loadPreset(away);
    pumpMessages();

    // Before the first block, a host program is the slot stored with a project,
    // not a preset load.
    processor.setCurrentProgram(34);
    pumpMessages();
    REQUIRE(processor.currentPresetIndex() == away);

    juce::MidiBuffer empty;
    processBlockOnce(processor, empty);

    processor.setCurrentProgram(34);
    pumpMessages();
    REQUIRE(processor.currentPresetName() == "LOW MASS");
    REQUIRE(processor.getCurrentProgram() == 34);

    setHostParameter(processor, "masterDb", 4.0f);
    REQUIRE(processor.presetIsDirty());
    processor.setCurrentProgram(34);
    pumpMessages();
    REQUIRE(processor.currentPresetName() == "LOW MASS");
    REQUIRE(processor.presetIsDirty());
    REQUIRE(hostParameter(processor, "masterDb") == Catch::Approx(4.0f).margin(0.05f));

    processor.setCurrentProgram(120);
    pumpMessages();
    REQUIRE(processor.currentPresetName() == "LOW MASS");
    processor.changeProgramName(34, "RENAMED");
    REQUIRE(processor.currentPresetName() == "LOW MASS");
}

// MIDI program change selects the preset assigned that code. The host's program
// slot is not consulted, so the same number cannot land on a different preset.
TEST_CASE("repeating a program change stays on the preset that owns the code", "[preset][midi]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);
    processor.prepareToPlay(kSampleRate, kBlockSize);

    juce::String error;
    const int lowMass = processor.presets().indexOfName("LOW MASS", true);
    REQUIRE(lowMass >= 0);
    REQUIRE(processor.setPresetProgram(lowMass, 34, error));

    const int away = lowMass == 0 ? 1 : 0;
    processor.loadPreset(away);
    pumpMessages();

    sendProgram(processor, 34);
    REQUIRE(processor.currentPresetName() == "LOW MASS");
    REQUIRE(processor.currentPresetIndex() == lowMass);

    setHostParameter(processor, "masterDb", 4.0f);
    REQUIRE(processor.presetIsDirty());

    processor.setCurrentProgram(34);
    sendProgram(processor, 34);

    REQUIRE(processor.currentPresetName() == "LOW MASS");
    REQUIRE(processor.currentPresetIndex() == lowMass);
    REQUIRE(processor.presetIsDirty());
    REQUIRE(hostParameter(processor, "masterDb") == Catch::Approx(4.0f).margin(0.05f));

    processor.loadPreset(away);
    pumpMessages();
    {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::programChange(1, 34), 0);
        processBlockOnce(processor, midi);
    }
    processor.setCurrentProgram(34);
    pumpMessages();
    REQUIRE(processor.currentPresetName() == "LOW MASS");
    REQUIRE(processor.currentPresetIndex() == lowMass);
}

TEST_CASE("the preset list is alphabetical", "[preset][library]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);

    juce::String error;
    REQUIRE(processor.savePresetCopy("ALPHA", "User", "", false, error));
    REQUIRE(processor.savePresetCopy("ZEBRA", "User", "", false, error));

    const PresetLibrary& library = processor.presets();
    const int alpha = library.indexOfName("ALPHA", false);
    const int zebra = library.indexOfName("ZEBRA", false);
    const int caveman = library.indexOfName("CAVEMAN", true);
    const int wool = library.indexOfName("WOOL HYMN", true);
    REQUIRE(alpha >= 0);
    REQUIRE(zebra >= 0);
    REQUIRE(caveman >= 0);
    REQUIRE(wool >= 0);
    REQUIRE(alpha < caveman);
    REQUIRE(zebra > wool);

    for (int index = 1; index < library.size(); ++index)
        REQUIRE(library.entry(index - 1).name.compareIgnoreCase(library.entry(index).name) <= 0);
}

TEST_CASE("saving a copy leaves the source preset and keeps the copy's PC", "[preset][library]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);
    processor.prepareToPlay(kSampleRate, kBlockSize);

    const int lowMass = processor.presets().indexOfName("LOW MASS", true);
    REQUIRE(lowMass >= 0);
    processor.loadPreset(lowMass);
    const double factoryMaster = processor.presets().entry(lowMass).params.masterDb;

    setHostParameter(processor, "masterDb", -12.0f);
    juce::String error;
    REQUIRE(processor.savePresetCopy("CELLAR", "User", "down", false, error));
    const int cellar = processor.presets().indexOfName("CELLAR", false);
    REQUIRE(cellar >= 0);
    REQUIRE(!processor.presets().entry(cellar).factory);
    REQUIRE(processor.presets().programCode(cellar) == -1);
    REQUIRE(processor.presets().entry(cellar).params.masterDb == Catch::Approx(-12.0).margin(1.0e-3));
    REQUIRE(processor.presets().entry(processor.presets().indexOfName("LOW MASS", true)).params.masterDb
            == Catch::Approx(factoryMaster).margin(1.0e-3));

    REQUIRE_FALSE(processor.savePresetCopy("CELLAR", "User", "", false, error));
    REQUIRE(error.isNotEmpty());

    REQUIRE(processor.setPresetProgram(cellar, 41, error));
    setHostParameter(processor, "masterDb", -6.0f);
    REQUIRE(processor.saveCurrentPreset("down", error));
    const int cellarSaved = processor.presets().indexOfName("CELLAR", false);
    REQUIRE(cellarSaved >= 0);
    REQUIRE(processor.presets().programCode(cellarSaved) == 41);
    REQUIRE(processor.presets().entry(cellarSaved).params.masterDb == Catch::Approx(-6.0).margin(1.0e-3));
    REQUIRE(processor.presets().entry(cellarSaved).file.loadFileAsString().contains("pc=41"));

    const int lowMassNow = processor.presets().indexOfName("LOW MASS", true);
    processor.loadPreset(lowMassNow);
    runBlocks(processor, 16);
    REQUIRE(processor.currentPresetName() == "LOW MASS");
    // The copy took the edit with it, and leaving the dirty source kept that
    // edit for the session. The stored factory preset is still the old sound.
    REQUIRE(processor.engine().parameters().masterDb == Catch::Approx(-12.0).margin(1.0e-3));
    processor.revertPreset();
    runBlocks(processor, 16);
    REQUIRE(processor.engine().parameters().masterDb == Catch::Approx(factoryMaster).margin(1.0e-3));

    processor.loadPreset(processor.presets().indexOfName("CELLAR", false));
    runBlocks(processor, 16);
    REQUIRE(processor.engine().parameters().masterDb == Catch::Approx(-6.0).margin(1.0e-3));
    REQUIRE(processor.presets().programCode(processor.currentPresetIndex()) == 41);
}

TEST_CASE("a learned switch toggles on the next press", "[midi]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);
    processor.prepareToPlay(kSampleRate, kBlockSize);

    // Gate is parameter 2. CC 10 is outside the default LPD8 map.
    constexpr int gate = 2;
    constexpr int cc = 10;
    REQUIRE(processor.ccForParameter(gate) != cc);
    REQUIRE(hostParameter(processor, "gate") < 0.5f);

    processor.beginLearn(gate);
    sendController(processor, cc, 127);
    REQUIRE(processor.ccForParameter(gate) == cc);
    REQUIRE(hostParameter(processor, "gate") < 0.5f);

    // Same value again, with no 0 in between. The press that was learned must
    // not already count as the switch being held.
    sendController(processor, cc, 127);
    REQUIRE(hostParameter(processor, "gate") >= 0.5f);
}

TEST_CASE("one controller moves every knob mapped to it", "[midi]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);
    processor.prepareToPlay(kSampleRate, kBlockSize);

    constexpr int trim = 1;
    constexpr int master = 19;
    constexpr int cc = 12;
    REQUIRE(processor.ccForParameter(trim) == cc);
    REQUIRE(processor.ccForParameter(master) != cc);

    processor.beginLearn(master);
    sendController(processor, cc, 64);
    REQUIRE(processor.ccForParameter(trim) == cc);
    REQUIRE(processor.ccForParameter(master) == cc);

    sendController(processor, cc, 127);
    REQUIRE(hostParameter(processor, "inputTrim") == Catch::Approx(12.0f).margin(0.05f));
    REQUIRE(hostParameter(processor, "masterDb") == Catch::Approx(6.0f).margin(0.05f));

    sendController(processor, cc, 0);
    REQUIRE(hostParameter(processor, "inputTrim") == Catch::Approx(-24.0f).margin(0.05f));
    REQUIRE(hostParameter(processor, "masterDb") == Catch::Approx(-24.0f).margin(0.05f));
}

TEST_CASE("a learned preset arrow steps once per press", "[midi][preset]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);
    processor.prepareToPlay(kSampleRate, kBlockSize);

    const int start = processor.currentPresetIndex();
    REQUIRE(start >= 0);
    constexpr int cc = 14;
    REQUIRE(processor.ccForControl(SubdoomAudioProcessor::kPresetPrevTarget) == 89);
    REQUIRE(processor.ccForControl(SubdoomAudioProcessor::kPresetNextTarget) == 90);
    processor.clearMapping(SubdoomAudioProcessor::kPresetNextTarget);
    REQUIRE(processor.ccForControl(SubdoomAudioProcessor::kPresetNextTarget) < 0);

    processor.beginLearn(SubdoomAudioProcessor::kPresetNextTarget);
    sendController(processor, cc, 127);
    REQUIRE(processor.ccForControl(SubdoomAudioProcessor::kPresetNextTarget) == cc);
    REQUIRE(processor.currentPresetIndex() == start);

    sendController(processor, cc, 127);
    const int stepped = processor.currentPresetIndex();
    REQUIRE(stepped != start);

    sendController(processor, cc, 127);
    REQUIRE(processor.currentPresetIndex() == stepped);

    sendController(processor, cc, 0);
    sendController(processor, cc, 127);
    REQUIRE(processor.currentPresetIndex() != stepped);

    processor.clearMapping(SubdoomAudioProcessor::kPresetNextTarget);
    const int cleared = processor.currentPresetIndex();
    sendController(processor, cc, 0);
    sendController(processor, cc, 127);
    REQUIRE(processor.currentPresetIndex() == cleared);

    processor.beginLearn(SubdoomAudioProcessor::kPresetPrevTarget);
    sendController(processor, cc, 80);
    REQUIRE(processor.ccForControl(SubdoomAudioProcessor::kPresetPrevTarget) == cc);

    juce::MemoryBlock state;
    processor.getStateInformation(state);
    SubdoomAudioProcessor restored(root.folder);
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    REQUIRE(restored.ccForControl(SubdoomAudioProcessor::kPresetPrevTarget) == cc);
    REQUIRE(restored.ccForControl(SubdoomAudioProcessor::kPresetNextTarget) < 0);
}

TEST_CASE("new projects start on the LPD8 default map and stay cleared after clear all", "[midi]")
{
    TempPresetRoot root;
    SubdoomAudioProcessor processor(root.folder);

    const int expectedKnobs[] = { 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 };
    for (int i = 0; i < 18; ++i)
        REQUIRE(processor.ccForParameter(expectedKnobs[i]) == 12 + i);

    REQUIRE(processor.ccForParameter(2) == 77);
    REQUIRE(processor.ccForParameter(20) == 88);
    REQUIRE(processor.ccForParameter(21) == 92);
    REQUIRE(processor.ccForControl(SubdoomAudioProcessor::kPresetPrevTarget) == 89);
    REQUIRE(processor.ccForControl(SubdoomAudioProcessor::kPresetNextTarget) == 90);

    processor.clearAllMappings();
    juce::MemoryBlock state;
    processor.getStateInformation(state);

    SubdoomAudioProcessor restored(root.folder);
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    for (int index = 1; index <= subdoom::kParameterCount; ++index)
        REQUIRE(restored.ccForParameter(index) < 0);
    REQUIRE(restored.ccForControl(SubdoomAudioProcessor::kPresetPrevTarget) < 0);
    REQUIRE(restored.ccForControl(SubdoomAudioProcessor::kPresetNextTarget) < 0);
}

TEST_CASE("release tags compare as dotted numbers", "[release]")
{
    REQUIRE_FALSE(isNewerRelease("v0.1.0", "0.1.0"));
    REQUIRE_FALSE(isNewerRelease("V0.1.0-beta", "0.1.0"));
    REQUIRE_FALSE(isNewerRelease("0.1.0-rc.1", "0.1.0"));
    REQUIRE_FALSE(isNewerRelease("0.1", "0.1.0"));
    REQUIRE_FALSE(isNewerRelease("0.1.0", "0.1"));
    REQUIRE_FALSE(isNewerRelease("0.1.0", "0.1.1"));
    REQUIRE_FALSE(isNewerRelease("", "0.1.0"));
    REQUIRE_FALSE(isNewerRelease("not-a-version", "0.1.0"));
    REQUIRE(isNewerRelease("v0.1.1", "0.1.0"));
    REQUIRE(isNewerRelease("0.2.0", "0.1.9"));
    REQUIRE(isNewerRelease("1.0.0", "0.9.9"));
    REQUIRE(isNewerRelease("1.10.0", "1.9.0"));
    REQUIRE(isNewerRelease("0.1.1-rc1", "0.1.0"));
}
