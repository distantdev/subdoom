#pragma once

#include "PluginProcessor.h"
#include "ReleaseCheck.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>
#include <vector>

namespace sdui {

// Two palettes share one set of roles. Light is cool paper with a teal accent.
// Dark is deep navy with a cyan accent.
struct Palette
{
    juce::Colour canvas;
    juce::Colour card;
    juce::Colour high;
    juce::Colour recess;
    juce::Colour hairline;
    juce::Colour ink;
    juce::Colour muted;
    juce::Colour faint;
    juce::Colour rust;
    juce::Colour moss;
    juce::Colour soft;
    juce::Colour slate;
    juce::Colour warn;
};

enum class AccentRole { neutral, rust, moss, slate, warn };

enum class Theme { light, dark };

const Palette& palette();
void setTheme(Theme newTheme);
Theme theme();
bool darkMode();
juce::Colour accentFor(AccentRole role);

inline juce::Colour ink() { return palette().canvas; }
inline juce::Colour panel() { return palette().card; }
inline juce::Colour panelHigh() { return palette().high; }
inline juce::Colour well() { return palette().recess; }
inline juce::Colour line() { return palette().hairline; }
inline juce::Colour text() { return palette().ink; }
inline juce::Colour dim() { return palette().muted; }
inline juce::Colour faint() { return palette().faint; }
inline juce::Colour amber() { return palette().rust; }
inline juce::Colour green() { return palette().moss; }
inline juce::Colour steel() { return palette().soft; }
inline juce::Colour blue() { return palette().slate; }
inline juce::Colour red() { return palette().warn; }

const juce::String& faceName();
juce::Font font(float height, bool strong = false);
juce::Font trackedFont(float height, float extraTracking, bool strong = false);

// Flat section label: no panel fill, no border. Whitespace does the grouping.
void drawPanel(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title,
               juce::Colour border);
void drawCard(juce::Graphics& g, juce::Rectangle<int> area, float opacity = -1.0f);
// A card with its own colour wash, for surfaces that float above the editor.
// Fully opaque: nothing behind it may read as content.
void drawFloatingCard(juce::Graphics& g, juce::Rectangle<int> area);
void fillWell(juce::Graphics& g, juce::Rectangle<float> area, float corner = 5.0f);
void strokeWell(juce::Graphics& g, juce::Rectangle<float> area, float corner = 5.0f,
                float alpha = 0.6f);
void drawChip(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& label,
              juce::Colour colour, bool filled);

int parameterIndexForId(const char* id) noexcept;

} // namespace sdui

class SubdoomLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SubdoomLookAndFeel();

    // Re-reads the active palette into the colour IDs JUCE widgets use.
    void updateColours();

    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPos, float startAngle, float endAngle,
                          juce::Slider&) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool shouldDrawButtonAsHighlighted,
                        bool shouldDrawButtonAsDown) override;
    void drawComboBox(juce::Graphics&, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH,
                      juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
    juce::Font getLabelFont(juce::Label&) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;
    juce::Slider::SliderLayout getSliderLayout(juce::Slider&) override;
    juce::Label* createSliderTextBox(juce::Slider&) override;
};

class KnobSlider : public juce::Slider
{
public:
    std::function<void()> onContextMenu;

    void mouseDown(const juce::MouseEvent& event) override;
};

// A control in the grid. The hint hit area is the drawn control, not the whole
// cell: the space between knobs belongs to the section.
class ControlCell : public juce::Component
{
public:
    ~ControlCell() override = default;

    virtual bool hitsHint(juce::Point<int> positionInCell) const = 0;
    std::function<void(int)> onHintLock;
};

class KnobCell : public ControlCell
{
public:
    KnobCell(SubdoomAudioProcessor& processor, const char* parameterId,
             const juce::String& caption, sdui::AccentRole role);

    void resized() override;
    void paint(juce::Graphics&) override;
    void lookAndFeelChanged() override;

    bool hitsHint(juce::Point<int> positionInCell) const override;

    void setLearning(bool shouldLearn);
    void tick();

    int parameterIndex() const noexcept { return index; }
    KnobSlider slider;

private:
    void showMenu();
    void applyPalette();

    SubdoomAudioProcessor& audioProcessor;
    int index = 1;
    juce::String captionText;
    sdui::AccentRole accentRole = sdui::AccentRole::neutral;
    bool learning = false;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KnobCell)
};

// On/off parameters are switches, not knobs forced into a boolean. A track with
// a knob that slides is the one form everyone already knows.
// Header preset arrows. Left click steps; right click opens MIDI learn.
class StepButton : public juce::TextButton
{
public:
    explicit StepButton(const juce::String& text) : juce::TextButton(text) {}

    std::function<void()> onContextMenu;

    void mouseDown(const juce::MouseEvent& event) override;
};

class SwitchButton : public juce::Button
{
public:
    SwitchButton();

    std::function<void()> onContextMenu;

    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
    void mouseDown(const juce::MouseEvent& event) override;
};

class SwitchCell : public ControlCell
{
public:
    SwitchCell(SubdoomAudioProcessor& processor, const char* parameterId,
               const juce::String& caption);

    void resized() override;
    void paint(juce::Graphics&) override;

    bool hitsHint(juce::Point<int> positionInCell) const override;

    void setLearning(bool shouldLearn);
    void tick();

    int parameterIndex() const noexcept { return index; }

private:
    void showMenu();

    SubdoomAudioProcessor& audioProcessor;
    int index = 1;
    juce::String captionText;
    SwitchButton button;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
    bool learning = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SwitchCell)
};

class ScopePanel : public juce::Component
{
public:
    explicit ScopePanel(SubdoomAudioProcessor& processor);

    void paint(juce::Graphics&) override;

private:
    void resample(const subdoom::Processor::ScopeRing& data, int write, int span,
                  std::vector<float>& target, int width) const;

    SubdoomAudioProcessor& audioProcessor;
    std::vector<float> outputWave;
    std::vector<float> inputWave;
};

class SpectrumPanel : public juce::Component
{
public:
    explicit SpectrumPanel(SubdoomAudioProcessor& processor);

    void paint(juce::Graphics&) override;

private:
    SubdoomAudioProcessor& audioProcessor;
    std::vector<float> smoothed;
};

class TunerPanel : public juce::Component
{
public:
    explicit TunerPanel(SubdoomAudioProcessor& processor);

    void paint(juce::Graphics&) override;

private:
    SubdoomAudioProcessor& audioProcessor;
};

// Peak and RMS as two horizontal tracks, between the tuner and the scope.
class LevelMeter : public juce::Component
{
public:
    explicit LevelMeter(SubdoomAudioProcessor& processor);

    void paint(juce::Graphics&) override;
    void tick();

private:
    SubdoomAudioProcessor& audioProcessor;
    float peak = 0.0f;
    float rms = 0.0f;
    bool clipped = false;
    juce::uint32 clipUntil = 0;
};

// Post-trim peak, read back as the level a direct bass arrived at. The mark is
// blue, green, or red around the -24 dBFS window the factory sounds were made
// in. The lamp is a pre-trim clip hold.
class InputLevelMeter : public juce::Component
{
public:
    explicit InputLevelMeter(SubdoomAudioProcessor& processor);

    void paint(juce::Graphics&) override;
    void tick();

private:
    juce::Colour zoneColour(float peak) const;

    SubdoomAudioProcessor& audioProcessor;
    float held = 0.0f;
    juce::uint32 holdUntil = 0;
    bool clipped = false;
    juce::uint32 clipUntil = 0;
};

class Led : public juce::Component
{
public:
    void paint(juce::Graphics&) override;
    void setLedColour(juce::Colour newColour);
    void flash();
    void tick();

private:
    juce::Colour colour = sdui::faint();
    float glow = 0.0f;
};

// Half-filled circle: the familiar light/dark glyph.
class ThemeButton : public juce::Button
{
public:
    ThemeButton();

    void paintButton(juce::Graphics&, bool highlighted, bool down) override;
};

// Turns the area under the mouse into a short explanation. Idle text when the
// pointer is over nothing useful.
class HintCard : public juce::Component
{
public:
    void setHint(const juce::String& section, const juce::String& title,
                 const juce::String& body);
    void paint(juce::Graphics&) override;

private:
    juce::String sectionText { "HINT" };
    juce::String titleText;
    juce::String bodyText { "Hover over a control for more details." };
};

class PresetBarButton : public juce::TextButton
{
public:
    std::function<void(int)> onStep;

    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    float wheelSteps = 0.0f;
};

class PresetBrowser : public juce::Component,
                      private juce::ListBoxModel
{
public:
    explicit PresetBrowser(SubdoomAudioProcessor& processor);
    ~PresetBrowser() override = default;

    void paint(juce::Graphics&) override;
    void resized() override;
    void refresh();
    void lookAndFeelChanged() override;

    std::function<void()> onLibraryChanged;
    std::function<void()> onClose;

    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics&, int width, int height,
                          bool rowIsSelected) override;
    void selectedRowsChanged(int row) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;

    void showSaveAs();
    void showRename();
    void pollHoverHint();
    bool handleOverlayKey(const juce::KeyPress& key);

private:
    enum class EditorMode { none, saveAs, rename };

    void commitInline();
    void cancelInline();
    void updateEditVisibility();
    void loadSelected();
    void deleteSelected();
    void setStatus(const juce::String& message, bool isError);
    int selectedEntry() const;
    void rebuildCategories();
    void syncProgramEditor(bool force);
    void commitProgramCode();

    SubdoomAudioProcessor& audioProcessor;
    juce::TextEditor search;
    juce::ComboBox categoryBox;
    juce::TextEditor pcEditor;
    juce::ListBox list;
    juce::TextButton loadButton { "LOAD" };
    juce::TextButton saveAsButton { "SAVE AS" };
    juce::TextButton renameButton { "RENAME" };
    juce::TextButton deleteButton { "DELETE" };
    juce::TextButton folderButton { "FOLDER" };
    juce::TextButton rescanButton { "RESCAN" };
    juce::TextButton closeButton { "CLOSE" };
    juce::TextEditor nameEditor;
    juce::TextEditor categoryEditor;
    juce::TextEditor notesEditor;
    juce::TextButton commitButton { "SAVE" };
    juce::TextButton cancelButton { "CANCEL" };
    juce::Label statusLabel;
    juce::String hoverText;
    juce::Rectangle<int> hintRowBounds;
    std::vector<int> visibleEntries;
    juce::StringArray knownCategories;
    EditorMode mode = EditorMode::none;
    int selectedRow = -1;
    int preferredSelection = -1;
    int programEditIndex = -1;
    bool deleteArmed = false;
    bool overwriteArmed = false;
    bool fillingProgramEditor = false;
    bool committingProgram = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetBrowser)
};

class MidiPage : public juce::Component
{
public:
    explicit MidiPage(SubdoomAudioProcessor& processor);
    ~MidiPage() override = default;

    void paint(juce::Graphics&) override;
    void resized() override;
    void refresh();
    void lookAndFeelChanged() override;
    void pollHoverHint();

    std::function<void()> onClose;

private:
    struct Row
    {
        std::unique_ptr<juce::Label> name;
        std::unique_ptr<juce::Label> mapping;
        std::unique_ptr<juce::TextButton> learn;
        std::unique_ptr<juce::TextButton> clear;
        int target = 0;
    };

    void buildRow(int target, const juce::String& label);

    SubdoomAudioProcessor& audioProcessor;
    std::vector<Row> rows;
    juce::Label channelLabel;
    juce::ComboBox channelBox;
    juce::Label programLabel;
    juce::TextButton programChange { "ON" };
    juce::TextButton clearAll { "CLEAR ALL" };
    juce::TextButton closeButton { "CLOSE" };
    juce::String hoverText;
    juce::Rectangle<int> hintRowBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiPage)
};

class SubdoomAudioProcessorEditor : public juce::AudioProcessorEditor,
                                     private juce::Timer
{
public:
    explicit SubdoomAudioProcessorEditor(SubdoomAudioProcessor&);
    ~SubdoomAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;
    void mouseDown(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    void timerCallback() override;
    void buildBackdrop();
    void refreshPresetBar();
    void openOverlay(juce::Component& overlayToShow);
    void closeOverlay();
    void layoutGroup(juce::Rectangle<int> area, const int* indices, int count,
                     int rows, int columns) const;
    void applyTheme(sdui::Theme newTheme);
    void updateHint();
    void showAvailableUpdate(const ReleaseInfo& info);
    int hintLock = 0;

    SubdoomAudioProcessor& audioProcessor;
    SubdoomLookAndFeel lookAndFeel;

    std::vector<std::unique_ptr<ControlCell>> cells;
    std::vector<KnobCell*> knobCells;
    std::vector<SwitchCell*> switchCells;
    ScopePanel scope;
    SpectrumPanel spectrum;
    TunerPanel tuner;
    InputLevelMeter inputLevel;
    LevelMeter levelMeter;
    HintCard hint;
    Led midiLed;
    PresetBarButton presetBar;
    StepButton prevButton { "<" };
    StepButton nextButton { ">" };
    juce::TextButton saveButton { "SAVE" };
    juce::TextButton revertButton { "REVERT" };
    ThemeButton themeButton;
    juce::TextButton compareButton { "A" };
    juce::TextButton midiButton { "MIDI" };
    juce::HyperlinkButton developerLink;
    juce::HyperlinkButton updateLink;
    PresetBrowser browser;
    MidiPage midiPage;

    juce::Rectangle<int> headerArea;
    juce::Rectangle<int> wordmarkArea;
    juce::Rectangle<int> bodyArea;
    juce::Rectangle<int> analyzerArea;
    juce::Rectangle<int> tunerArea;
    juce::Rectangle<int> spectrumArea;
    juce::Rectangle<int> scopeArea;
    juce::Rectangle<int> metersArea;
    juce::Rectangle<int> controlsArea;
    juce::Rectangle<int> meterArea;
    juce::Rectangle<int> hintArea;
    juce::Rectangle<int> frontArea;
    juce::Rectangle<int> shapeArea;
    juce::Rectangle<int> outArea;
    juce::Rectangle<int> rightArea;

    juce::Image backdrop;
    juce::Component* overlay = nullptr;
    bool fittedToScreen = false;
    bool backdropDirty = false;
    int tunerTick = 0;
    int shownLearn = -2;
    int shownMidiActivity = -1;
    juce::String shownPresetName;
    bool shownDirty = false;
    bool shownFactory = true;
    bool shownCompare = false;
    int shownActivity = 0;
    juce::String availableRelease;
    // Last member, so it is destroyed before the update link. The destructor joins first.
    ReleaseChecker releaseChecker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SubdoomAudioProcessorEditor)
};
