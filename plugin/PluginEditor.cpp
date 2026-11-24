#include "PluginEditor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace {

const int kFrontIds[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
const int kShapeIds[] = { 11, 12, 13, 14, 15, 16 };
const int kOutIds[] = { 17, 18, 19, 20, 21 };

// Size of the wordmark's rust square. The maker link sits under the name, offset
// from the same edge, so the two lines start on one column.
const int kWordmarkMark = 13;

// Factory sounds expect a direct bass near -24 dBFS. The meter undoes the fixed
// +12 dB align, so Trim at 0 leaves that bass in this window.
constexpr float kInputFloorDb = -48.0f;
constexpr float kInputWindowLowDb = -29.0f;
constexpr float kInputWindowHighDb = -19.0f;

struct ControlSpec
{
    const char* id;
    const char* caption;
    sdui::AccentRole role;
    bool isSwitch;
};

const ControlSpec kControlSpecs[] = {
    { "inputTrim", "TRIM", sdui::AccentRole::neutral, false },
    { "gate", "GATE", sdui::AccentRole::neutral, true },
    { "sustain", "SUSTAIN", sdui::AccentRole::neutral, false },
    { "splitHz", "SPLIT", sdui::AccentRole::neutral, false },
    { "sag", "SAG", sdui::AccentRole::neutral, false },
    { "lowDrive", "LOW DRIVE", sdui::AccentRole::rust, false },
    { "lowDb", "LOW", sdui::AccentRole::moss, false },
    { "highDrive", "HIGH DRIVE", sdui::AccentRole::rust, false },
    { "highDb", "HIGH", sdui::AccentRole::neutral, false },
    { "octave", "OCTAVE", sdui::AccentRole::neutral, false },
    { "thumpDb", "THUMP", sdui::AccentRole::moss, false },
    { "harmDb", "HARM", sdui::AccentRole::moss, false },
    { "growlDb", "GROWL", sdui::AccentRole::neutral, false },
    { "honkDb", "HONK", sdui::AccentRole::neutral, false },
    { "highCutHz", "CUT", sdui::AccentRole::neutral, false },
    { "grit", "GRIT", sdui::AccentRole::rust, false },
    { "roomMix", "ROOM", sdui::AccentRole::neutral, false },
    { "roomDecay", "DECAY", sdui::AccentRole::neutral, false },
    { "masterDb", "MASTER", sdui::AccentRole::warn, false },
    { "limiter", "LIMIT", sdui::AccentRole::neutral, true },
    { "tunerMute", "MUTE", sdui::AccentRole::neutral, true },
};

static_assert(std::size(kControlSpecs) == static_cast<std::size_t>(subdoom::kParameterCount));

bool isToggleParameter(int index)
{
    return subdoom::parameterIsSwitch(index);
}

bool isDecibelParameter(int index)
{
    switch (index)
    {
        case 1: case 7: case 9: case 11: case 12: case 13: case 14: case 19: return true;
        default: return false;
    }
}

bool isHertzParameter(int index)
{
    return index == 4 || index == 15;
}

const char* groupNameForParameter(int parameter)
{
    if (parameter <= 10)
        return "FRONT";
    if (parameter <= 16)
        return "SHAPE";
    return "OUT";
}

const char* helpForParameter(int parameter)
{
    switch (parameter)
    {
        case 1: return "Input level before anything else. The drive is voiced for a direct bass "
                       "around -24 dBFS, so Trim at 0 is that level. Hotter takes clip sooner, "
                       "colder stays cleaner. A fixed "
                       "highpass at 20 Hz, 24 dB per octave, cuts the subsonic energy that "
                       "overpowers a speaker.";
        case 2: return "Cuts hiss between songs. Slow enough that palm mutes and ringing notes "
                       "are not chopped.";
        case 3: return "Evens long notes without killing the pick attack. The leveler, not a "
                       "squash.";
        case 4: return "Where the low path becomes the high path. One-pole and complementary, "
                       "not a crossover. Keep it near 100 Hz so the low string stays clean.";
        case 5: return "Ducks the high path after loud notes and releases slowly. The "
                       "fundamental does not pump.";
        case 6: return "Saturation on the low path, where the fundamental lives. This is the "
                       "fuzz control; it squares the note.";
        case 7: return "Level of the low path after its drive. This is where the fundamental "
                       "comes out.";
        case 8: return "Fuzz on the note's harmonics. The fundamental stays on the low path, "
                       "and finger noise is not in the gain, so it reads with the cut down.";
        case 9: return "Level of the high path after its clipper.";
        case 10: return "Full-wave fold of the low band, added to the high path. Even harmonics, "
                        "no pitch shifter.";
        case 11: return "Peaking bell at 58 Hz. A few dB go a long way on a bass.";
        case 12: return "Bells at 55 Hz and 82.5 Hz, the second and third of A0. Same amount "
                        "on both, so a 27 Hz note still reads when the fundamental is mostly "
                        "speaker excursion.";
        case 13: return "Bell at 240 Hz. Body without mud.";
        case 14: return "Bell at 750 Hz. Brings the note forward when the drive buries it.";
        case 15: return "Two cascaded lowpasses, 24 dB per octave. The speaker rolloff as a "
                        "filter, no resonance.";
        case 16: return "Parallel hard-clipped band near 1.4 kHz. Pick and fret noise for the "
                        "nastier presets.";
        case 17: return "Short room. The wet path is highpassed at 160 Hz so the lows do not "
                        "pile up.";
        case 18: return "Length of the room.";
        case 19: return "Output level, before the always-on saturate and the limiter.";
        case 20: return "Catches peaks at -1 dBFS. Leave it on unless you want the raw output.";
        case 21: return "Ramps the output while you tune. The tracker keeps listening, so the "
                        "note still shows.";
        default: return "";
    }
}

juce::String formatValue(int index, double value)
{
    if (isToggleParameter(index))
        return value >= 0.5 ? "ON" : "OFF";

    if (index == 4)
        return juce::String(juce::roundToInt(value)) + " Hz";

    if (index == 15)
        return value >= 1000.0 ? juce::String(value / 1000.0, 2) + " kHz"
                               : juce::String(juce::roundToInt(value)) + " Hz";

    if (isDecibelParameter(index))
        return (value > 0.0 ? "+" : "") + juce::String(value, 1);

    return juce::String(juce::roundToInt(value * 100.0)) + "%";
}

double parseValue(int index, const juce::String& text)
{
    const juce::String trimmed = text.trim();
    if (isToggleParameter(index))
        return trimmed.startsWithIgnoreCase("on") || trimmed == "1" ? 1.0 : 0.0;

    const double number = trimmed.getDoubleValue();
    if (isHertzParameter(index) || isDecibelParameter(index))
        return number;
    return trimmed.containsChar('%') ? number / 100.0 : number;
}

void styleEditor(juce::TextEditor& editor)
{
    editor.setColour(juce::TextEditor::backgroundColourId, sdui::panel());
    editor.setColour(juce::TextEditor::textColourId, sdui::text());
    editor.setColour(juce::TextEditor::outlineColourId, sdui::line());
    editor.setColour(juce::TextEditor::focusedOutlineColourId, sdui::amber());
    editor.setColour(juce::TextEditor::highlightColourId, sdui::amber().withAlpha(0.22f));
    editor.setColour(juce::TextEditor::highlightedTextColourId, sdui::text());
    editor.setFont(sdui::font(12.0f));
    editor.setIndents(6, 0);
}

void styleButton(juce::TextButton& button, juce::Colour accent)
{
    button.setColour(juce::TextButton::buttonColourId, sdui::panel());
    button.setColour(juce::TextButton::buttonOnColourId, accent);
    button.setColour(juce::TextButton::textColourOffId, sdui::text());
    button.setColour(juce::TextButton::textColourOnId, sdui::ink());
}

} // namespace

namespace sdui {

// Sections are flat: a small corner and a hairline. Only the knobs, the sheet
// trim and the switches carry a shadow.
constexpr float sectionCorner = 4.0f;
constexpr float sheetCorner = 5.0f;

namespace {

const Palette& lightPalette()
{
    // Cool paper with a teal accent: the same studio language in daylight.
    static const Palette value {
        juce::Colour::fromRGB(0xDD, 0xE5, 0xEC), // canvas
        juce::Colour::fromRGB(0xF7, 0xFA, 0xFC), // card
        juce::Colour::fromRGB(0xFF, 0xFF, 0xFF), // high
        juce::Colour::fromRGB(0xD9, 0xE2, 0xEA), // recess
        juce::Colour::fromRGB(0xBD, 0xCB, 0xD7), // hairline
        juce::Colour::fromRGB(0x16, 0x24, 0x30), // ink
        juce::Colour::fromRGB(0x50, 0x67, 0x79), // muted
        juce::Colour::fromRGB(0x8B, 0x9E, 0xAD), // faint
        juce::Colour::fromRGB(0x0E, 0x9E, 0x9C), // teal accent
        juce::Colour::fromRGB(0x3B, 0x6F, 0xD4), // periwinkle
        juce::Colour::fromRGB(0x22, 0x33, 0x3F), // soft
        juce::Colour::fromRGB(0x2C, 0x6C, 0x8F), // slate
        juce::Colour::fromRGB(0xC7, 0x45, 0x2F), // warn
    };
    return value;
}

const Palette& darkPalette()
{
    // Deep navy with a cyan accent, after the studio-mastering look.
    static const Palette value {
        juce::Colour::fromRGB(0x0B, 0x16, 0x26), // canvas
        juce::Colour::fromRGB(0x13, 0x27, 0x3E), // card
        juce::Colour::fromRGB(0x1C, 0x35, 0x52), // high
        juce::Colour::fromRGB(0x07, 0x13, 0x21), // recess
        juce::Colour::fromRGB(0x27, 0x42, 0x5E), // hairline
        juce::Colour::fromRGB(0xDD, 0xEB, 0xF7), // ink
        juce::Colour::fromRGB(0x93, 0xAB, 0xC2), // muted
        juce::Colour::fromRGB(0x5F, 0x79, 0x93), // faint
        juce::Colour::fromRGB(0x4F, 0xD6, 0xC9), // rust -> cyan
        juce::Colour::fromRGB(0x6E, 0x8B, 0xFF), // moss -> periwinkle
        juce::Colour::fromRGB(0x9F, 0xC4, 0xE8), // soft
        juce::Colour::fromRGB(0x3E, 0x7A, 0xC0), // slate
        juce::Colour::fromRGB(0xFF, 0x6B, 0x6B), // warn
    };
    return value;
}

Theme& themeFlag()
{
    static Theme value = Theme::dark;
    return value;
}

} // namespace

const Palette& palette()
{
    return themeFlag() == Theme::dark ? darkPalette() : lightPalette();
}

void setTheme(Theme newTheme)
{
    themeFlag() = newTheme;
}

Theme theme()
{
    return themeFlag();
}

bool darkMode()
{
    return themeFlag() == Theme::dark;
}
juce::Colour accentFor(AccentRole role)
{
    switch (role)
    {
        case AccentRole::rust: return amber();
        case AccentRole::moss: return green();
        case AccentRole::slate: return blue();
        case AccentRole::warn: return red();
        case AccentRole::neutral: break;
    }
    return text();
}

const juce::String& faceName()
{
    static const juce::String chosen = [] {
        const auto names = juce::Font::findAllTypefaceNames();
        if (names.contains("Bahnschrift"))
            return juce::String("Bahnschrift");
        if (names.contains("Segoe UI"))
            return juce::String("Segoe UI");
        return juce::Font::getDefaultSansSerifFontName();
    }();
    return chosen;
}

juce::Font font(float height, bool strong)
{
    return juce::Font(juce::FontOptions(faceName(), height,
                                        strong ? juce::Font::bold : juce::Font::plain));
}

juce::Font trackedFont(float height, float extraTracking, bool strong)
{
    // JUCE measures extra kerning as a fraction of the font height, so convert
    // from the pixel gap the call sites talk about.
    return font(height, strong).withExtraKerningFactor(
        extraTracking / juce::jmax(1.0f, height));
}

void drawPanel(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title,
               juce::Colour)
{
    g.setColour(dim());
    g.setFont(trackedFont(9.0f, 1.8f, true));
    g.drawText(title.toUpperCase(), area.removeFromTop(13), juce::Justification::centredLeft);
}

void drawCard(juce::Graphics& g, juce::Rectangle<int> area, float opacity)
{
    const auto card = area.toFloat();

    // The light palette's card is close to its canvas, so it needs to be more
    // transparent for the wash behind to read at all. Dark keeps more body.
    const float fillAlpha = opacity >= 0.0f ? opacity : (darkMode() ? 0.52f : 0.44f);
    const juce::Colour base = panel().withAlpha(fillAlpha);
    juce::ColourGradient fill(base.brighter(0.02f), card.getX(), card.getY(),
                              base.darker(0.06f), card.getX(), card.getBottom(), false);
    g.setGradientFill(fill);
    g.fillRoundedRectangle(card, sectionCorner);

    // One quiet edge instead of a cast shadow: the surface still has a boundary,
    // but the knobs stay the only raised things on the page.
    g.setColour(line().withAlpha(0.65f));
    g.drawRoundedRectangle(card.reduced(0.5f), sectionCorner, 1.0f);
}

void drawFloatingCard(juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto card = area.toFloat();

    // A sheet over a scrim keeps a shadow, but a short offset one: enough to
    // lift it off the page, not enough to halo its corners.
    g.setColour(juce::Colours::black.withAlpha(darkMode() ? 0.35f : 0.14f));
    g.fillRoundedRectangle(card.reduced(1.0f).translated(0.0f, 3.0f), sheetCorner);

    juce::Path clip;
    clip.addRoundedRectangle(card, sheetCorner);
    g.saveState();
    g.reduceClipRegion(clip);

    g.setColour(panel());
    g.fillRect(card);

    // Same wash as the backdrop, painted into the sheet so it is opaque but not
    // flat, and so whatever is behind the sheet never shows through.
    const auto blob = [&g, card](float fx, float fy, float fraction, juce::Colour colour) {
        const float cx = card.getX() + card.getWidth() * fx;
        const float cy = card.getY() + card.getHeight() * fy;
        const float radius = juce::jmax(card.getWidth(), card.getHeight()) * fraction;
        const juce::ColourGradient gradient(colour, cx, cy, colour.withAlpha(0.0f),
                                            cx + radius, cy, true);
        g.setGradientFill(gradient);
        g.fillRect(card);
    };

    const float strength = darkMode() ? 0.9f : 1.0f;
    blob(0.08f, 0.00f, 0.85f, amber().withAlpha(0.26f * strength));
    blob(0.98f, 0.26f, 0.80f, green().withAlpha(0.22f * strength));
    blob(0.22f, 1.05f, 0.85f, blue().withAlpha(0.18f * strength));

    g.restoreState();
    g.setColour(line().withAlpha(0.85f));
    g.drawRoundedRectangle(card.reduced(0.5f), sheetCorner, 1.0f);
}

void fillWell(juce::Graphics& g, juce::Rectangle<float> area, float corner)
{
    // One flat recess, no ramp across the panel. The light card is translucent,
    // so the light well has to be nudged darker to sit behind it at all.
    g.setColour(darkMode() ? well() : well().darker(0.10f));
    g.fillRoundedRectangle(area, corner);

    g.setColour(juce::Colours::black.withAlpha(darkMode() ? 0.20f : 0.025f));
    g.drawHorizontalLine(juce::roundToInt(area.getY()) + 1, area.getX() + corner,
                         area.getRight() - corner);
}

void strokeWell(juce::Graphics& g, juce::Rectangle<float> area, float corner, float alpha)
{
    g.setColour(line().withAlpha(alpha));
    g.drawRoundedRectangle(area.reduced(0.5f), corner, 1.0f);
}

void drawChip(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& label,
              juce::Colour colour, bool filled)
{
    if (filled)
    {
        g.setColour(colour.withAlpha(0.14f));
        g.fillRoundedRectangle(area.toFloat(), 3.0f);
    }
    g.setColour(colour.withAlpha(filled ? 0.9f : 0.55f));
    g.drawRoundedRectangle(area.toFloat().reduced(0.5f), 3.0f, 1.0f);
    g.setColour(colour);
    g.setFont(trackedFont(9.0f, 1.0f, true));
    g.drawText(label, area, juce::Justification::centred);
}

int parameterIndexForId(const char* id) noexcept
{
    for (int index = 1; index <= subdoom::kParameterCount; ++index)
        if (std::strcmp(id, subdoom::parameterId(index)) == 0)
            return index;
    return 1;
}

} // namespace sdui

SubdoomLookAndFeel::SubdoomLookAndFeel()
{
    updateColours();
}

void SubdoomLookAndFeel::updateColours()
{
    setColour(juce::Slider::textBoxTextColourId, sdui::text());
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::thumbColourId, sdui::amber());
    setColour(juce::Slider::rotarySliderFillColourId, sdui::amber());
    setColour(juce::Label::textColourId, sdui::text());
    setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::TextButton::buttonColourId, sdui::panel());
    setColour(juce::TextButton::textColourOffId, sdui::text());
    setColour(juce::TextButton::textColourOnId, sdui::ink());
    setColour(juce::ComboBox::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::ComboBox::outlineColourId, sdui::line());
    setColour(juce::ComboBox::textColourId, sdui::text());
    setColour(juce::ComboBox::arrowColourId, sdui::dim());
    setColour(juce::PopupMenu::backgroundColourId, sdui::panel());
    setColour(juce::PopupMenu::textColourId, sdui::text());
    setColour(juce::PopupMenu::headerTextColourId, sdui::dim());
    setColour(juce::PopupMenu::highlightedBackgroundColourId, sdui::text().withAlpha(0.08f));
    setColour(juce::PopupMenu::highlightedTextColourId, sdui::text());
    setColour(juce::HyperlinkButton::textColourId, sdui::text().withAlpha(0.72f));
}

void SubdoomLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                          float sliderPos, float startAngle, float endAngle,
                                          juce::Slider& slider)
{
    // Flat by design: one hairline ring, the value carried by the arc, a pointer.
    // No body, no ticks, no glow.
    const float radius = static_cast<float>(juce::jmin(width, height)) * 0.44f;
    const float cx = static_cast<float>(x) + static_cast<float>(width) * 0.5f;
    const float cy = static_cast<float>(y) + static_cast<float>(height) * 0.5f;
    const float angle = startAngle + sliderPos * (endAngle - startAngle);
    const juce::Colour accent = slider.findColour(juce::Slider::rotarySliderFillColourId);
    const bool active = slider.isMouseOverOrDragging();
    const bool isSwitch = slider.getProperties().contains("switch");


    const float bodyRadius = radius * 0.88f;
    const juce::Rectangle<float> disc(cx - bodyRadius, cy - bodyRadius, bodyRadius * 2.0f,
                                      bodyRadius * 2.0f);

    // Long, soft cast shadow. The face itself stays flat: no gradient, no gloss.
    juce::Path discPath;
    discPath.addEllipse(disc);
    juce::DropShadow(juce::Colours::black.withAlpha(sdui::darkMode() ? 0.55f : 0.20f), 16,
                     { 0, 9 })
        .drawForPath(g, discPath);

    g.setColour(sdui::darkMode() ? sdui::panel().brighter(0.11f) : sdui::panel().darker(0.05f));
    g.fillEllipse(disc);

    // Subtle rounded edge: a hairline that catches the light at the top.
    g.setColour(sdui::line().withAlpha(0.85f));
    g.drawEllipse(disc.reduced(0.5f), 1.0f);
    juce::Path topEdge;
    topEdge.addCentredArc(cx, cy, bodyRadius - 0.7f, bodyRadius - 0.7f, 0.0f,
                          -juce::MathConstants<float>::halfPi - 1.15f,
                          -juce::MathConstants<float>::halfPi + 1.15f, true);
    g.setColour(juce::Colours::white.withAlpha(sdui::darkMode() ? 0.12f : 0.40f));
    g.strokePath(topEdge, juce::PathStrokeType(1.0f));
    if (isSwitch)
    {
        if (sliderPos >= 0.5f)
        {
            g.setColour(sdui::amber());
            g.fillEllipse(cx - radius * 0.38f, cy - radius * 0.38f, radius * 0.76f,
                          radius * 0.76f);
        }
        else
        {
            g.setColour(sdui::faint());
            g.drawEllipse(cx - radius * 0.30f, cy - radius * 0.30f, radius * 0.60f,
                          radius * 0.60f, 1.0f);
        }
        return;
    }
    // LANDR-style value: a single needle from the middle outward, no ring.
    juce::Path pointer;
    pointer.startNewSubPath(0.0f, -radius * 0.30f);
    pointer.lineTo(0.0f, -radius * 0.78f);
    g.setColour(slider.getProperties()["accented"] ? accent : sdui::dim());
    g.strokePath(pointer, juce::PathStrokeType(active ? 2.4f : 2.2f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded),
                 juce::AffineTransform::rotation(angle).translated(cx, cy));
}

void SubdoomLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                              const juce::Colour&, bool highlighted, bool down)
{
    // Links carry no body: no pill, no shadow, nothing to look at but their text.
    if (button.getProperties().contains("link"))
        return;

    const bool on = button.getToggleState();
    const auto area = button.getLocalBounds().toFloat().reduced(0.5f);
    const float radius = area.getHeight() * 0.5f;

    juce::Path shape;
    shape.addRoundedRectangle(area, radius);

    // A pill that sits just off the surface: a short shadow, no halo.
    juce::DropShadow(juce::Colours::black.withAlpha(sdui::darkMode() ? 0.30f : 0.08f), 4,
                     { 0, 2 })
        .drawForPath(g, shape);

    if (on)
        g.setColour(sdui::text());
    else if (down || highlighted)
        g.setColour(sdui::panelHigh());
    else
        g.setColour(sdui::panel().withAlpha(0.55f));
    g.fillPath(shape);

    if (!on)
    {
        juce::Path inner;
        inner.addRoundedRectangle(area.reduced(1.2f), radius - 1.2f);
        g.setColour(juce::Colours::white.withAlpha(sdui::darkMode() ? 0.05f : 0.22f));
        g.strokePath(inner, juce::PathStrokeType(1.0f));
    }

    g.setColour(on ? sdui::text() : sdui::line());
    g.drawRoundedRectangle(area, radius, 1.0f);
}
void SubdoomLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button,
                                        bool, bool)
{
    if (button.getProperties().contains("link"))
    {
        const bool on = button.getToggleState();
        const bool hot = button.isMouseOverOrDragging();
        const bool icon = button.getProperties().contains("icon");
        const juce::Colour accent = button.findColour(juce::TextButton::buttonOnColourId);

        // Hovering brings the action's own colour up, which is the whole affordance
        // a link needs.
        if (!button.isEnabled())
            g.setColour(sdui::faint().withAlpha(0.35f));
        else if (on || hot)
            g.setColour(accent);
        else
            g.setColour(sdui::text().withAlpha(icon ? 0.60f : 0.75f));

        g.setFont(icon ? sdui::font(15.0f, true) : sdui::trackedFont(9.0f, 1.2f, true));
        g.drawText(button.getButtonText(), button.getLocalBounds(),
                   juce::Justification::centred);
        return;
    }

    const bool on = button.getToggleState();
    const juce::Colour foreground = on ? sdui::ink()
                                      : (button.isEnabled() ? sdui::text() : sdui::faint());
    g.setColour(foreground);
    g.setFont(sdui::font(11.5f, button.getProperties().contains("strong")));

    auto area = button.getLocalBounds().reduced(9, 0);
    if (button.getProperties().contains("menu"))
    {
        auto triangleArea = area.removeFromRight(12);
        const float centreX = static_cast<float>(triangleArea.getCentreX());
        const float centreY = static_cast<float>(triangleArea.getCentreY());
        juce::Path arrow;
        arrow.addTriangle(centreX - 3.5f, centreY - 1.8f, centreX + 3.5f, centreY - 1.8f,
                          centreX, centreY + 2.4f);
        g.setColour(on ? sdui::ink().withAlpha(0.7f) : sdui::faint());
        g.fillPath(arrow);
        area.removeFromRight(4);
        g.setColour(foreground);
        g.drawFittedText(button.getButtonText(), area, juce::Justification::centredLeft, 1);
        return;
    }

    g.drawFittedText(button.getButtonText(), area, juce::Justification::centred, 1);
}

void SubdoomLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool,
                                      int, int, int, int, juce::ComboBox& box)
{
    const juce::Rectangle<float> area(0.0f, 0.0f, static_cast<float>(width),
                                       static_cast<float>(height));
    g.setColour(sdui::line());
    g.drawRoundedRectangle(area.reduced(0.5f), 4.0f, 1.0f);

    juce::Path arrow;
    const float cx = static_cast<float>(width) - 13.0f;
    const float cy = static_cast<float>(height) * 0.5f;
    arrow.addTriangle(cx - 3.5f, cy - 1.8f, cx + 3.5f, cy - 1.8f, cx, cy + 2.4f);
    g.setColour(box.findColour(juce::ComboBox::arrowColourId));
    g.fillPath(arrow);
}

void SubdoomLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(9, 0, box.getWidth() - 26, box.getHeight());
    label.setFont(sdui::font(12.0f));
}

juce::Font SubdoomLookAndFeel::getLabelFont(juce::Label&)
{
    return sdui::font(12.0f);
}

juce::Font SubdoomLookAndFeel::getTextButtonFont(juce::TextButton&, int)
{
    return sdui::font(11.5f);
}

juce::Font SubdoomLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return sdui::font(12.0f);
}

juce::Font SubdoomLookAndFeel::getPopupMenuFont()
{
    return sdui::font(13.0f);
}

juce::Slider::SliderLayout SubdoomLookAndFeel::getSliderLayout(juce::Slider& slider)
{
    juce::Slider::SliderLayout layout;
    auto bounds = slider.getLocalBounds();
    const int textHeight = juce::jlimit(15, 24, slider.getTextBoxHeight());
    layout.textBoxBounds = bounds.removeFromBottom(textHeight);
    const int side = juce::jmin(bounds.getWidth(), bounds.getHeight());
    layout.sliderBounds = bounds.withSizeKeepingCentre(side, juce::jmax(8, side));
    return layout;
}

juce::Label* SubdoomLookAndFeel::createSliderTextBox(juce::Slider& slider)
{
    juce::Label* label = LookAndFeel_V4::createSliderTextBox(slider);
    label->setJustificationType(juce::Justification::centred);
    label->setInterceptsMouseClicks(false, false);
    label->setFont(sdui::font(13.0f, true));
    return label;
}

void StepButton::mouseDown(const juce::MouseEvent& event)
{
    if (event.mods.isRightButtonDown())
    {
        if (onContextMenu)
            onContextMenu();
        return;
    }
    juce::TextButton::mouseDown(event);
}

void KnobSlider::mouseDown(const juce::MouseEvent& event)
{
    if (event.mods.isRightButtonDown())
    {
        if (onContextMenu)
            onContextMenu();
        return;
    }
    juce::Slider::mouseDown(event);
}

KnobCell::KnobCell(SubdoomAudioProcessor& processor, const char* parameterId,
                   const juce::String& caption, sdui::AccentRole role)
    : audioProcessor(processor),
      index(sdui::parameterIndexForId(parameterId)),
      captionText(caption),
      accentRole(role)
{
    addAndMakeVisible(slider);
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 84, 18);
    slider.setRepaintsOnMouseActivity(true);
    slider.setScrollWheelEnabled(true);
    slider.setPopupDisplayEnabled(false, false, nullptr);
    slider.onDragStart = [this] { if (onHintLock) onHintLock(index); };
    slider.onDragEnd = [this] { if (onHintLock) onHintLock(0); };

    if (auto* parameter = audioProcessor.apvts.getParameter(parameterId))
    {
        const double fallback = parameter->convertFrom0to1(parameter->getDefaultValue());
        slider.setDoubleClickReturnValue(true, fallback);
    }

    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, parameterId, slider);

    const int parameterIndex = index;
    slider.textFromValueFunction = [parameterIndex](double value) {
        return formatValue(parameterIndex, value);
    };
    slider.valueFromTextFunction = [parameterIndex](const juce::String& valueText) {
        return parseValue(parameterIndex, valueText);
    };
    slider.updateText();

    slider.onContextMenu = [this] { showMenu(); };

    applyPalette();
}

void KnobCell::applyPalette()
{
    slider.setColour(juce::Slider::rotarySliderFillColourId, sdui::accentFor(accentRole));
    slider.getProperties().set("accented", accentRole != sdui::AccentRole::neutral);
    slider.setColour(juce::Slider::textBoxTextColourId, sdui::text());
    slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    slider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
}

void KnobCell::lookAndFeelChanged()
{
    applyPalette();
    repaint();
}

bool KnobCell::hitsHint(juce::Point<int> position) const
{
    if (getLocalBounds().removeFromTop(13).contains(position))
        return true;

    auto bounds = slider.getBounds();
    const int textHeight = juce::jlimit(15, 24, slider.getTextBoxHeight());
    const auto textBox = bounds.removeFromBottom(textHeight);
    if (textBox.contains(position))
        return true;

    const int side = juce::jmin(bounds.getWidth(), bounds.getHeight());
    const auto rotary = bounds.withSizeKeepingCentre(side, juce::jmax(8, side));
    const float radius = static_cast<float>(side) * 0.36f + 4.0f;
    return (position.toFloat() - rotary.getCentre().toFloat()).getDistanceFromOrigin() <= radius;
}

void KnobCell::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop(20);
    slider.setBounds(area.reduced(1, 0));
}

void KnobCell::paint(juce::Graphics& g)
{
    g.setColour(learning ? sdui::amber() : sdui::dim());
    g.setFont(sdui::trackedFont(9.0f, 1.5f));
    g.drawFittedText(captionText, getLocalBounds().withTrimmedTop(8).removeFromTop(13),
                     juce::Justification::centred, 1);

    if (learning)
    {
        g.setColour(sdui::amber().withAlpha(0.85f));
        g.drawRoundedRectangle(slider.getBounds().toFloat().reduced(0.5f), 7.0f, 1.2f);
    }
}

void KnobCell::setLearning(bool shouldLearn)
{
    if (learning == shouldLearn)
        return;
    learning = shouldLearn;
    repaint();
}

void KnobCell::tick()
{
    setLearning(audioProcessor.learnTarget() == index);
}


void KnobCell::showMenu()
{
    const int cc = audioProcessor.ccForParameter(index);
    juce::PopupMenu menu;
    menu.addSectionHeader(juce::String(subdoom::parameterName(index)));
    menu.addItem(1, "MIDI Learn");
    menu.addItem(2, cc >= 0 ? "Clear MIDI (" + juce::String(cc) + ")" : "Clear MIDI", cc >= 0);
    menu.addSeparator();
    menu.addItem(3, "Reset to default");

    juce::Component::SafePointer<KnobCell> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&slider),
                       [safe](int result) {
                           if (safe == nullptr || result == 0)
                               return;
                           if (result == 1)
                           {
                               safe->audioProcessor.beginLearn(safe->index);
                               safe->setLearning(true);
                           }
                           else if (result == 2)
                           {
                               safe->audioProcessor.clearMapping(safe->index);
                           }
                           else if (result == 3)
                           {
                               if (auto* parameter = safe->audioProcessor.apvts.getParameter(
                                       subdoom::parameterId(safe->index)))
                                   parameter->setValueNotifyingHost(parameter->getDefaultValue());
                           }
                       });
}

SwitchButton::SwitchButton()
    : juce::Button("switch")
{
    setClickingTogglesState(true);
    setWantsKeyboardFocus(false);
}

void SwitchButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    const auto area = getLocalBounds().toFloat();
    const float width = area.getWidth();
    const bool on = getToggleState();

    // Recessed trough: darker at the top, with an inner shadow and a bottom
    const bool darkTrough = sdui::darkMode();
    juce::ColourGradient trough(darkTrough ? sdui::well().darker(0.40f)
                                           : sdui::well().darker(0.10f),
                                area.getX(), area.getY(),
                                darkTrough ? sdui::well().brighter(0.10f)
                                           : sdui::well().brighter(0.06f),
                                area.getX(), area.getBottom(), false);
    g.setGradientFill(trough);
    g.fillRoundedRectangle(area, width * 0.5f);

    juce::Path troughShape;
    troughShape.addRoundedRectangle(area, width * 0.5f);
    g.saveState();
    g.reduceClipRegion(troughShape);

    juce::ColourGradient shade(juce::Colours::black.withAlpha(darkTrough ? 0.45f : 0.10f),
                               area.getX(), area.getY(),
                               juce::Colours::transparentBlack, area.getX(),
                               area.getY() + width * 0.55f, false);
    g.setGradientFill(shade);
    g.fillRect(area);

    juce::ColourGradient lift(juce::Colours::white.withAlpha(0.10f), area.getX(),
                              area.getBottom(), juce::Colours::transparentBlack, area.getX(),
                              area.getBottom() - width * 0.45f, false);
    g.setGradientFill(lift);
    g.fillRect(area);
    g.restoreState();

    // Thin dark rim so the slot keeps its edge against the panel.
    g.setColour(darkTrough ? sdui::line().withAlpha(0.70f)
                           : sdui::faint().withAlpha(0.60f));
    g.drawRoundedRectangle(area.reduced(0.5f), width * 0.5f, 1.0f);

    const float radius = width * 0.5f - 3.0f;
    const float centreY = on ? area.getY() + width * 0.5f : area.getBottom() - width * 0.5f;
    const juce::Rectangle<float> puck(area.getCentreX() - radius, centreY - radius,
                                      radius * 2.0f, radius * 2.0f);

    // Clip to the trough so the glow cannot spill past its rounded end.
    juce::Path trackShape;
    trackShape.addRoundedRectangle(area, width * 0.5f);
    g.saveState();
    g.reduceClipRegion(trackShape);

    juce::Path puckPath;
    puckPath.addEllipse(puck);
    juce::DropShadow(juce::Colours::black.withAlpha(sdui::darkMode() ? 0.45f : 0.18f), 6, { 0, 2 })
        .drawForPath(g, puckPath);

    if (on)
    {
        // Filled puck: the on state is obvious at a glance. Same disc and the
        // same edge as the off state, so the thumb does not read as shrinking.
        juce::ColourGradient lit(sdui::amber().brighter(0.35f), puck.getX(), puck.getY(),
                                 sdui::amber().darker(0.25f), puck.getX(), puck.getBottom(), false);
        g.setGradientFill(lit);
        g.fillEllipse(puck);

        g.setColour(juce::Colours::white.withAlpha(sdui::darkMode() ? 0.16f : 0.45f));
        g.drawEllipse(puck.reduced(0.8f), 1.0f);
    }
    else
    {
        juce::ColourGradient body(sdui::panelHigh().brighter(0.12f), puck.getX(), puck.getY(),
                                  sdui::panel().darker(0.20f), puck.getX(), puck.getBottom(), false);
        g.setGradientFill(body);
        g.fillEllipse(puck);

        g.setColour(juce::Colours::white.withAlpha(sdui::darkMode() ? 0.16f : 0.45f));
        g.drawEllipse(puck.reduced(0.8f), 1.0f);

        if (highlighted || down)
        {
            g.setColour(sdui::dim().withAlpha(0.5f));
            g.drawEllipse(puck.reduced(0.7f), 1.0f);
        }
    }

    g.restoreState();
}
void SwitchButton::mouseDown(const juce::MouseEvent& event)
{
    if (event.mods.isRightButtonDown())
    {
        if (onContextMenu)
            onContextMenu();
        return;
    }
    juce::Button::mouseDown(event);
}

SwitchCell::SwitchCell(SubdoomAudioProcessor& processor, const char* parameterId,
                       const juce::String& caption)
    : audioProcessor(processor),
      index(sdui::parameterIndexForId(parameterId)),
      captionText(caption)
{
    addAndMakeVisible(button);
    button.onContextMenu = [this] { showMenu(); };
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        audioProcessor.apvts, parameterId, button);
}

void SwitchCell::resized()
{
    // Short pill: a little closer to the caption than before, stopping above
    // the ON/OFF value so the text stays visible.
    auto area = getLocalBounds();
    area.removeFromTop(22);
    area.removeFromBottom(16);
    const int height = juce::jlimit(40, 58, area.getHeight());
    button.setBounds(area.withSizeKeepingCentre(24, height));
}

void SwitchCell::paint(juce::Graphics& g)
{
    g.setColour(learning ? sdui::amber() : sdui::dim());
    g.setFont(sdui::trackedFont(9.0f, 1.5f));
    g.drawFittedText(captionText, getLocalBounds().withTrimmedTop(8).removeFromTop(13),
                     juce::Justification::centred, 1);

    const bool on = button.getToggleState();
    g.setColour(on ? sdui::amber() : sdui::faint());
    g.setFont(sdui::font(13.0f, true));
    g.drawText(on ? "ON" : "OFF", getLocalBounds().removeFromBottom(16),
               juce::Justification::centred);

    if (learning)
    {
        g.setColour(sdui::amber().withAlpha(0.85f));
        g.drawRoundedRectangle(button.getBounds().toFloat().expanded(4.0f), 9.0f, 1.2f);
    }
}

void SwitchCell::setLearning(bool shouldLearn)
{
    if (learning == shouldLearn)
        return;
    learning = shouldLearn;
    repaint();
}

void SwitchCell::tick()
{
    setLearning(audioProcessor.learnTarget() == index);
}

bool SwitchCell::hitsHint(juce::Point<int> position) const
{
    if (getLocalBounds().removeFromTop(13).contains(position))
        return true;
    if (button.getBounds().expanded(3).contains(position))
        return true;
    return getLocalBounds().removeFromBottom(14).contains(position);
}

void SwitchCell::showMenu()
{
    const int cc = audioProcessor.ccForParameter(index);
    juce::PopupMenu menu;
    menu.addSectionHeader(juce::String(subdoom::parameterName(index)));
    menu.addItem(1, "MIDI Learn");
    menu.addItem(2, cc >= 0 ? "Clear MIDI (" + juce::String(cc) + ")" : "Clear MIDI", cc >= 0);
    menu.addSeparator();
    menu.addItem(3, "Reset to default");

    juce::Component::SafePointer<SwitchCell> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&button),
                       [safe](int result) {
                           if (safe == nullptr || result == 0)
                               return;
                           if (result == 1)
                           {
                               safe->audioProcessor.beginLearn(safe->index);
                               safe->setLearning(true);
                           }
                           else if (result == 2)
                           {
                               safe->audioProcessor.clearMapping(safe->index);
                           }
                           else if (result == 3)
                           {
                               if (auto* parameter = safe->audioProcessor.apvts.getParameter(
                                       subdoom::parameterId(safe->index)))
                                   parameter->setValueNotifyingHost(parameter->getDefaultValue());
                           }
                       });
}

ThemeButton::ThemeButton()
    : juce::Button("theme")
{
}

namespace {

// A disc with a bite out of its top right, as one closed path. Cutting it as a
// path keeps the crescent independent of the colour the button is filled with,
// which is translucent now.
juce::Path crescentPath(juce::Point<float> centre, float radius)
{
    const juce::Point<float> bite { centre.x + radius * 0.65f, centre.y - radius * 0.45f };
    const float biteRadius = radius * 0.975f;
    const auto offset = bite - centre;
    const float distance = offset.getDistanceFromOrigin();
    const float axis = std::atan2(offset.y, offset.x);
    const float outer = std::acos(juce::jlimit(
        -1.0f, 1.0f,
        (distance * distance + radius * radius - biteRadius * biteRadius)
            / (2.0f * distance * radius)));
    const float inner = std::acos(juce::jlimit(
        -1.0f, 1.0f,
        (distance * distance + biteRadius * biteRadius - radius * radius)
            / (2.0f * distance * biteRadius)));

    constexpr int steps = 24;
    const auto onCircle = [](juce::Point<float> origin, float r, float angle) {
        return origin + juce::Point<float>(std::cos(angle), std::sin(angle)) * r;
    };

    juce::Path path;
    for (int step = 0; step <= steps; ++step)
    {
        // Outer edge: the part of the disc the bite does not reach, the long way.
        const float sweep = juce::MathConstants<float>::twoPi - 2.0f * outer;
        const float angle = axis + outer + sweep * static_cast<float>(step) / steps;
        const auto point = onCircle(centre, radius, angle);
        if (step == 0)
            path.startNewSubPath(point);
        else
            path.lineTo(point);
    }
    for (int step = 0; step <= steps; ++step)
    {
        // Inner edge: the part of the bite that falls inside the disc, from the
        // far intersection back to the near one.
        const float angle = axis + juce::MathConstants<float>::pi + inner
            - 2.0f * inner * static_cast<float>(step) / steps;
        path.lineTo(onCircle(bite, biteRadius, angle));
    }
    path.closeSubPath();
    return path;
}

} // namespace

void ThemeButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    const auto area = getLocalBounds().toFloat().reduced(1.0f);

    // The same fill as the pills beside it. At full card opacity the light theme
    // made this a lighter well sitting in the row rather than a button.
    g.setColour((highlighted || down) ? sdui::panelHigh() : sdui::panel().withAlpha(0.55f));
    g.fillRoundedRectangle(area, 4.0f);
    g.setColour(sdui::line());
    g.drawRoundedRectangle(area, 4.0f, 1.0f);

    const float radius = juce::jmin(area.getWidth(), area.getHeight()) * 0.22f;
    const float cx = area.getCentreX();
    const float cy = area.getCentreY();

    switch (sdui::theme())
    {
        case sdui::Theme::light:
        {
            // Sun.
            g.setColour(sdui::text());
            g.fillEllipse(cx - radius * 0.62f, cy - radius * 0.62f, radius * 1.24f,
                          radius * 1.24f);
            for (int ray = 0; ray < 8; ++ray)
            {
                const float angle = juce::MathConstants<float>::twoPi
                    * static_cast<float>(ray) / 8.0f;
                const float inner = radius * 1.05f;
                const float outer = radius * 1.55f;
                g.drawLine(cx + std::cos(angle) * inner, cy + std::sin(angle) * inner,
                           cx + std::cos(angle) * outer, cy + std::sin(angle) * outer, 1.2f);
            }
            break;
        }
        case sdui::Theme::dark:
        {
            g.setColour(sdui::text());
            g.fillPath(crescentPath({ cx, cy }, radius));
            break;
        }
    }
}

void HintCard::setHint(const juce::String& section, const juce::String& title,
                       const juce::String& body)
{
    if (section == sectionText && title == titleText && body == bodyText)
        return;
    sectionText = section;
    titleText = title;
    bodyText = body;
    repaint();
}

void HintCard::paint(juce::Graphics& g)
{
    // The card behind this is drawn in the editor backdrop.
    auto inner = getLocalBounds().reduced(18, 14);
    inner.removeFromTop(6);

    if (titleText.isNotEmpty())
    {
        g.setColour(sdui::text());
        g.setFont(sdui::font(15.0f, true));
        g.drawFittedText(titleText, inner.removeFromTop(20), juce::Justification::centredLeft, 1);
        inner.removeFromTop(7);
    }

    g.setColour(sdui::dim());
    g.setFont(sdui::font(11.0f));
    g.drawFittedText(bodyText, inner, juce::Justification::topLeft, 4, 1.0f);
}

ScopePanel::ScopePanel(SubdoomAudioProcessor& processor)
    : audioProcessor(processor)
{
}

void ScopePanel::resample(const subdoom::Processor::ScopeRing& data, int write, int span,
                          std::vector<float>& target, int width) const
{
    constexpr int ring = subdoom::Processor::kScopeSize;
    target.resize(static_cast<std::size_t>(juce::jmax(1, width)));
    const int columns = static_cast<int>(target.size());
    for (int i = 0; i < columns; ++i)
    {
        const float position = static_cast<float>(i) * static_cast<float>(span - 1)
            / static_cast<float>(juce::jmax(1, columns - 1));
        const int base = static_cast<int>(position);
        const float fraction = position - static_cast<float>(base);
        const int read0 = (write - base + ring) % ring;
        const int read1 = (write - base - 1 + ring) % ring;
        const float sample0 = data[static_cast<std::size_t>(read0)].load(
            std::memory_order_relaxed);
        const float sample1 = data[static_cast<std::size_t>(read1)].load(
            std::memory_order_relaxed);
        target[static_cast<std::size_t>(i)] = sample0 * (1.0f - fraction) + sample1 * fraction;
    }
}

void ScopePanel::paint(juce::Graphics& g)
{
    auto inner = getLocalBounds().reduced(18, 12);
    sdui::drawPanel(g, inner, "SCOPE", sdui::line());
    inner.removeFromTop(14);

    const auto plot = inner.toFloat();
    sdui::fillWell(g, plot, 4.0f);

    for (int line = 1; line < 4; ++line)
    {
        const int y = inner.getY() + inner.getHeight() * line / 4;
        g.setColour(sdui::darkMode() ? sdui::line().withAlpha(0.34f)
                                     : sdui::faint().withAlpha(0.26f));
        g.drawHorizontalLine(y, static_cast<float>(inner.getX() + 1),
                             static_cast<float>(inner.getRight() - 1));
    }
    g.setColour(sdui::darkMode() ? sdui::line().withAlpha(0.85f)
                                 : sdui::faint().withAlpha(0.45f));
    g.drawHorizontalLine(inner.getCentreY(), static_cast<float>(inner.getX() + 1),
                         static_cast<float>(inner.getRight() - 1));

    const int width = juce::jmax(2, inner.getWidth());
    constexpr int span = 640;
    const auto& engine = audioProcessor.engine();
    resample(engine.inputScopeBuffer(), engine.inputScopeWriteIndex(), span, inputWave, width);
    resample(engine.scopeBuffer(), engine.scopeWriteIndex(), span, outputWave, width);

    const float midY = static_cast<float>(inner.getCentreY());
    const float top = static_cast<float>(inner.getY() + 1);
    const float scale = static_cast<float>(inner.getHeight()) * 0.42f;
    const float x0 = static_cast<float>(inner.getX());

    juce::Path inputPath;
    juce::Path outputPath;
    for (int i = 0; i < width; ++i)
    {
        const float x = x0 + static_cast<float>(i);
        const float inY = juce::jlimit(top, top + scale * 2.0f,
                                       midY - inputWave[static_cast<std::size_t>(i)] * scale);
        const float outY = juce::jlimit(top, top + scale * 2.0f,
                                        midY - outputWave[static_cast<std::size_t>(i)] * scale);
        if (i == 0)
        {
            inputPath.startNewSubPath(x, inY);
            outputPath.startNewSubPath(x, outY);
        }
        else
        {
            inputPath.lineTo(x, inY);
            outputPath.lineTo(x, outY);
        }
    }

    g.setColour(sdui::faint().withAlpha(0.50f));
    g.strokePath(inputPath, juce::PathStrokeType(1.0f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    g.setColour(sdui::amber());
    g.strokePath(outputPath, juce::PathStrokeType(1.3f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));

    sdui::strokeWell(g, plot, 4.0f);
}

SpectrumPanel::SpectrumPanel(SubdoomAudioProcessor& processor)
    : audioProcessor(processor)
{
}

void SpectrumPanel::paint(juce::Graphics& g)
{
    auto inner = getLocalBounds().reduced(18, 12);
    sdui::drawPanel(g, inner, "SPECTRUM", sdui::line());
    inner.removeFromTop(14);

    auto axis = inner.removeFromBottom(12);
    const auto plot = inner.toFloat();
    sdui::fillWell(g, plot, 4.0f);

    for (int line = 1; line < 4; ++line)
    {
        const int y = inner.getY() + inner.getHeight() * line / 4;
        g.setColour(sdui::darkMode() ? sdui::line().withAlpha(0.34f)
                                     : sdui::faint().withAlpha(0.26f));
        g.drawHorizontalLine(y, static_cast<float>(inner.getX() + 1),
                             static_cast<float>(inner.getRight() - 1));
    }

    const auto bands = audioProcessor.engine().spectrum();
    const int count = static_cast<int>(bands.size());
    if (count < 2 || inner.getWidth() < 8)
        return;

    smoothed.resize(static_cast<std::size_t>(count));
    float loudest = 0.0f;
    for (int band = 0; band < count; ++band)
    {
        const float db = juce::Decibels::gainToDecibels(
            bands[static_cast<std::size_t>(band)], -72.0f);
        const float amount = juce::jlimit(0.0f, 1.0f, (db + 72.0f) / 72.0f);
        auto& value = smoothed[static_cast<std::size_t>(band)];
        value += (amount - value) * 0.35f;
        loudest = juce::jmax(loudest, value);
    }

    const int steps = juce::jmax(16, count * 6);
    std::vector<juce::Point<float>> points;
    points.reserve(static_cast<std::size_t>(steps));
    for (int i = 0; i < steps; ++i)
    {
        const float position = static_cast<float>(i) * static_cast<float>(count - 1)
            / static_cast<float>(steps - 1);
        const int low = static_cast<int>(position);
        const int high = juce::jmin(count - 1, low + 1);
        const float fraction = position - static_cast<float>(low);
        const float amount = smoothed[static_cast<std::size_t>(low)] * (1.0f - fraction)
            + smoothed[static_cast<std::size_t>(high)] * fraction;
        points.push_back({ plot.getX() + plot.getWidth() * static_cast<float>(i)
                               / static_cast<float>(steps - 1),
                           plot.getBottom() - 1.5f
                               - amount * (plot.getHeight() - 4.0f) });
    }

    if (loudest > 0.02f)
    {
    juce::Path areaPath;
    areaPath.startNewSubPath(points.front().x, plot.getBottom());
    for (const auto& point : points)
        areaPath.lineTo(point);
    areaPath.lineTo(points.back().x, plot.getBottom());
    areaPath.closeSubPath();

    juce::ColourGradient fill(sdui::amber().withAlpha(0.50f), 0.0f, plot.getY(),
                              sdui::blue().withAlpha(0.04f), 0.0f, plot.getBottom(), false);
    g.setGradientFill(fill);
    g.fillPath(areaPath);

    juce::Path linePath;
    linePath.startNewSubPath(points.front());
    for (std::size_t i = 1; i < points.size(); ++i)
        linePath.lineTo(points[i]);
    g.setColour(sdui::amber());
    g.strokePath(linePath, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    }

    // Log positions of the analyser's real edges. Evenly spaced labels would
    // claim 10 kHz at the right edge of a display that ends at 8 kHz.
    struct FrequencyMark
    {
        const char* text;
        double hz;
    };
    static const FrequencyMark marks[] = {
        { "30", 30.0 },
        { "100", 100.0 },
        { "300", 300.0 },
        { "1k", 1000.0 },
        { "3k", 3000.0 },
        { "8k", 8000.0 },
    };
    static_assert(subdoom::ScopeMeter::kSpectrumLowHz == 30.0);
    static_assert(subdoom::ScopeMeter::kSpectrumHighHz == 8000.0);
    const double lowHz = subdoom::ScopeMeter::kSpectrumLowHz;
    const double highHz = subdoom::ScopeMeter::kSpectrumHighHz;
    const double span = std::log(highHz / lowHz);
    g.setFont(sdui::font(8.5f));
    for (const auto& mark : marks)
    {
        const float position = static_cast<float>(std::log(mark.hz / lowHz) / span);
        const float x = plot.getX() + plot.getWidth() * position;
        constexpr int labelWidth = 36;
        const int labelX = juce::jlimit(juce::roundToInt(plot.getX()),
                                         juce::roundToInt(plot.getRight()) - labelWidth,
                                         juce::roundToInt(x) - labelWidth / 2);
        g.setColour(sdui::faint());
        g.drawText(mark.text, labelX, axis.getY(), labelWidth, axis.getHeight(),
                   juce::Justification::centred);
        g.setColour(sdui::line().withAlpha(0.8f));
        g.drawVerticalLine(juce::roundToInt(juce::jlimit(plot.getX(), plot.getRight(), x)),
                           plot.getBottom() - 4.0f, plot.getBottom());
    }

    sdui::strokeWell(g, plot, 4.0f);
}
TunerPanel::TunerPanel(SubdoomAudioProcessor& processor)
    : audioProcessor(processor)
{
}

void TunerPanel::paint(juce::Graphics& g)
{
    auto inner = getLocalBounds().reduced(18, 12);
    sdui::drawPanel(g, inner, "TUNER", sdui::line());
    inner.removeFromTop(18);

    const auto well = inner;
    sdui::fillWell(g, well.toFloat(), 4.0f);

    auto content = well.reduced(16, 10);
    auto noteBlock = content.removeFromLeft(40);
    content.removeFromLeft(6);
    auto rulerArea = content;

    const subdoom::TunerReadout reading = audioProcessor.engine().tuner();
    juce::Colour noteColour = sdui::faint();
    juce::String noteText = "--";

    if (reading.active && reading.midiNote >= 0)
    {
        noteText = juce::String(subdoom::noteName(reading.noteIndex))
            + juce::String(reading.octave);
        noteColour = std::abs(reading.cents) <= 7.0 ? sdui::green() : sdui::red();
    }

    g.setColour(noteColour);
    g.setFont(sdui::font(30.0f, true));
    g.drawFittedText(noteText, noteBlock, juce::Justification::centredLeft, 1);
    // Ruler to the right of the note block.
    const float left = static_cast<float>(rulerArea.getX());
    const float rightEdge = static_cast<float>(rulerArea.getRight());
    const float centreX = (left + rightEdge) * 0.5f;
    const float y = static_cast<float>(rulerArea.getCentreY()) + 2.0f;

    // The hairline disappeared against the flat well, so the ruler is drawn in
    // the same colour as the scope's centre line.
    g.setColour(sdui::darkMode() ? sdui::line().withAlpha(0.85f)
                                 : sdui::faint().withAlpha(0.55f));
    g.drawLine(left, y, rightEdge, y, 2.0f);

    g.setColour(sdui::faint());
    for (int tick = -2; tick <= 2; ++tick)
    {
        const float x = centreX + static_cast<float>(tick) * (rightEdge - left) * 0.22f;
        const float reach = tick == 0 ? 7.0f : 4.0f;
        g.drawLine(x, y - reach, x, y + reach, tick == 0 ? 2.0f : 1.5f);
    }

    if (reading.active)
    {
        const float amount = juce::jlimit(-1.0f, 1.0f, static_cast<float>(reading.cents / 50.0));
        const float x = centreX + amount * (rightEdge - left) * 0.44f;
        g.setColour(noteColour);
        g.drawLine(x, y - 9.0f, x, y + 9.0f, 3.0f);
    }

    sdui::strokeWell(g, well.toFloat(), 4.0f);
}
LevelMeter::LevelMeter(SubdoomAudioProcessor& processor)
    : audioProcessor(processor)
{
    setInterceptsMouseClicks(false, false);
}

void LevelMeter::tick()
{
    // The processor accumulates peak and mean square per block and hands the window
    // over here. The old path read the decimated scope ring, which under-reads
    // transients and raced the audio thread.
    float rawPeak = 0.0f;
    float rawRms = 0.0f;
    audioProcessor.consumeLevels(rawPeak, rawRms);

    if (rawPeak >= 1.0f && !clipped)
    {
        clipped = true;
        clipUntil = juce::Time::getMillisecondCounter() + 1800;
        repaint();
    }
    else if (clipped && juce::Time::getMillisecondCounter() > clipUntil && rawPeak < 1.0f)
    {
        clipped = false;
        repaint();
    }

    // A little ballistics so the bars do not flicker at 30 Hz.
    const float nextPeak = juce::jmax(rawPeak, peak * 0.90f);
    const float nextRms = juce::jmax(rawRms, rms * 0.86f);
    if (std::abs(nextPeak - peak) > 0.0005f || std::abs(nextRms - rms) > 0.0005f)
    {
        peak = nextPeak;
        rms = nextRms;
        repaint();
    }
}

void LevelMeter::paint(juce::Graphics& g)
{
    auto area = getLocalBounds();
    auto labels = area.removeFromLeft(32);
    auto lampBlock = area.removeFromRight(14);
    area.removeFromRight(6);

    g.setColour(sdui::dim());
    g.setFont(sdui::trackedFont(7.5f, 1.2f, true));
    g.drawText("PEAK", labels.removeFromTop(labels.getHeight() / 2),
               juce::Justification::centredLeft);
    g.drawText("RMS", labels, juce::Justification::centredLeft);

    auto rmsArea = area.removeFromBottom(11).toFloat();
    area.removeFromBottom(4);
    auto peakArea = area.toFloat();

    const auto dbToFraction = [](float level) {
        const float db = juce::Decibels::gainToDecibels(level, -60.0f);
        return juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
    };

    const auto drawTrack = [&g, &dbToFraction](juce::Rectangle<float> rect, float level,
                                               juce::Colour colour) {
        sdui::fillWell(g, rect, 3.0f);
        juce::Path clipPath;
        clipPath.addRoundedRectangle(rect, 3.0f);
        g.saveState();
        g.reduceClipRegion(clipPath);
        const float width = rect.getWidth() * dbToFraction(level);
        if (width > 0.5f)
        {
            juce::ColourGradient fill(colour, rect.getX(), rect.getY(),
                                      colour.withAlpha(0.55f), rect.getX() + width,
                                      rect.getY(), false);
            g.setGradientFill(fill);
            g.fillRect(rect.withWidth(width));
        }
        g.restoreState();
        sdui::strokeWell(g, rect, 3.0f);
    };

    drawTrack(peakArea, peak, clipped ? sdui::red() : sdui::amber());
    drawTrack(rmsArea, rms, sdui::green().withAlpha(0.9f));

    // Scale ticks across both tracks.
    for (const float fraction : { 0.25f, 0.5f, 0.75f })
    {
        const float x = peakArea.getX() + peakArea.getWidth() * fraction;
        g.setColour(sdui::faint().withAlpha(sdui::darkMode() ? 0.35f : 0.26f));
        g.drawVerticalLine(juce::roundToInt(x), peakArea.getY() + 1.0f, rmsArea.getBottom() - 1.0f);
    }

    // Clip lamp. The socket is drawn whether or not it is lit, so the unlit
    // state reads as a lamp that is off rather than as a gap in the row.
    auto lamp = lampBlock.removeFromLeft(12).withSizeKeepingCentre(11, 11).toFloat();
    g.setColour(sdui::darkMode() ? sdui::well().brighter(0.10f) : sdui::well().darker(0.12f));
    g.fillRoundedRectangle(lamp, 2.5f);
    g.setColour(sdui::darkMode() ? sdui::line().withAlpha(0.95f)
                                 : sdui::faint().withAlpha(0.85f));
    g.drawRoundedRectangle(lamp.reduced(0.5f), 2.5f, 1.0f);

    if (clipped)
    {
        g.setColour(sdui::red().withAlpha(0.30f));
        g.fillRoundedRectangle(lamp.expanded(3.0f), 4.0f);
        g.setColour(sdui::red());
        g.fillRoundedRectangle(lamp, 2.5f);
    }

}

InputLevelMeter::InputLevelMeter(SubdoomAudioProcessor& processor)
    : audioProcessor(processor)
{
    setInterceptsMouseClicks(false, false);
}

juce::Colour InputLevelMeter::zoneColour(float peak) const
{
    const float db = juce::Decibels::gainToDecibels(peak, -120.0f);
    if (db < kInputFloorDb)
        return sdui::faint();
    if (db < kInputWindowLowDb)
        return sdui::blue();
    if (db <= kInputWindowHighDb)
        return sdui::green();
    return sdui::red();
}

void InputLevelMeter::tick()
{
    const float pre = audioProcessor.engine().takeInputPrePeak();
    const float post = audioProcessor.engine().takeInputPostPeak();
    const float align = static_cast<float>(subdoom::dbToGain(subdoom::Processor::kInputAlignDb));
    const float referred = post / align;
    const auto now = juce::Time::getMillisecondCounter();

    if (referred >= held)
    {
        held = referred;
        holdUntil = now + 300;
    }
    else if (now >= holdUntil)
    {
        held *= 0.84f;
    }

    // -1 dBFS on the way in. Trim can pad this back into the window afterwards.
    const float clipGain = juce::Decibels::decibelsToGain(-1.0f);
    if (pre >= clipGain)
    {
        clipped = true;
        clipUntil = now + 1800;
    }
    else if (clipped && now > clipUntil)
    {
        clipped = false;
    }

    if (isShowing())
        repaint();
}

void InputLevelMeter::paint(juce::Graphics& g)
{
    auto area = getLocalBounds();
    auto lampColumn = area.removeFromRight(36);
    area.removeFromRight(8);

    auto caption = area.removeFromTop(juce::jmin(13, area.getHeight() / 3));
    g.setColour(sdui::dim());
    g.setFont(sdui::trackedFont(7.5f, 1.2f, true));
    g.drawText("INPUT", caption, juce::Justification::centredLeft);

    auto labels = area.removeFromBottom(juce::jmin(12, area.getHeight() / 3));
    auto track = area.toFloat().reduced(0.0f, 1.0f);
    if (track.getWidth() < 8.0f || track.getHeight() < 4.0f)
        return;

    const auto dbToX = [&track](float db) {
        const float fraction = juce::jlimit(0.0f, 1.0f, (db - kInputFloorDb) / (0.0f - kInputFloorDb));
        return track.getX() + track.getWidth() * fraction;
    };

    sdui::fillWell(g, track, 3.0f);

    juce::Path clipPath;
    clipPath.addRoundedRectangle(track, 3.0f);
    g.saveState();
    g.reduceClipRegion(clipPath);

    const float windowLeft = dbToX(kInputWindowLowDb);
    const float windowRight = dbToX(kInputWindowHighDb);
    g.setColour(sdui::green().withAlpha(sdui::darkMode() ? 0.22f : 0.28f));
    g.fillRect(windowLeft, track.getY(), windowRight - windowLeft, track.getHeight());

    g.setColour(sdui::green().withAlpha(0.70f));
    g.drawVerticalLine(juce::roundToInt(windowLeft), track.getY() + 1.0f, track.getBottom() - 1.0f);
    g.drawVerticalLine(juce::roundToInt(windowRight), track.getY() + 1.0f, track.getBottom() - 1.0f);

    const float db = juce::Decibels::gainToDecibels(held, kInputFloorDb);
    const float mark = dbToX(juce::jlimit(kInputFloorDb, 0.0f, db));
    g.setColour(zoneColour(held));
    g.fillRect(mark - 1.25f, track.getY() + 1.0f, 2.5f, track.getHeight() - 2.0f);
    g.restoreState();

    sdui::strokeWell(g, track, 3.0f);

    g.setColour(sdui::faint());
    g.setFont(sdui::font(8.0f));
    const auto drawLabel = [&g, &labels](float x, const char* text) {
        g.drawText(text, juce::roundToInt(x) - 16, labels.getY(), 32, labels.getHeight(),
                   juce::Justification::centred);
    };
    drawLabel(windowLeft, "-29");
    drawLabel(windowRight, "-19");
    drawLabel(track.getRight() - 8.0f, "0");

    g.setFont(sdui::font(8.0f));
    g.setColour(clipped ? sdui::red() : sdui::dim());
    g.drawText("CLIP", lampColumn.getX(), caption.getY(), lampColumn.getWidth(), caption.getHeight(),
               juce::Justification::centred);

    auto lamp = juce::Rectangle<float>(static_cast<float>(lampColumn.getX()),
                                       track.getCentreY() - 5.5f,
                                       static_cast<float>(lampColumn.getWidth()), 11.0f)
                    .withSizeKeepingCentre(11.0f, 11.0f);
    g.setColour(sdui::darkMode() ? sdui::well().brighter(0.10f) : sdui::well().darker(0.12f));
    g.fillRoundedRectangle(lamp, 2.5f);
    g.setColour(sdui::darkMode() ? sdui::line().withAlpha(0.95f) : sdui::faint().withAlpha(0.85f));
    g.drawRoundedRectangle(lamp.reduced(0.5f), 2.5f, 1.0f);
    if (clipped)
    {
        g.setColour(sdui::red().withAlpha(0.30f));
        g.fillRoundedRectangle(lamp.expanded(3.0f), 4.0f);
        g.setColour(sdui::red());
        g.fillRoundedRectangle(lamp, 2.5f);
    }
}

void Led::paint(juce::Graphics& g)
{
    auto dot = getLocalBounds().toFloat().withSizeKeepingCentre(7.0f, 7.0f);
    if (glow > 0.01f)
    {
        g.setColour(colour.withAlpha(0.22f * glow));
        g.fillEllipse(dot.expanded(3.0f + 3.0f * glow));
    }
    g.setColour(colour.withAlpha(0.30f + 0.70f * glow));
    g.fillEllipse(dot);
    g.setColour(sdui::line());
    g.drawEllipse(dot.reduced(0.5f), 1.0f);
}

void Led::setLedColour(juce::Colour newColour)
{
    if (colour == newColour)
        return;
    colour = newColour;
    repaint();
}

void Led::flash()
{
    glow = 1.0f;
    repaint();
}

void Led::tick()
{
    if (glow <= 0.01f)
        return;
    glow *= 0.86f;
    repaint();
}

void PresetBarButton::mouseWheelMove(const juce::MouseEvent& event,
                                     const juce::MouseWheelDetails& wheel)
{
    if (onStep)
    {
        // A trackpad sends many small deltas for one gesture and a mouse wheel about
        // one unit per notch. Stepping per event walked the whole library on a flick,
        // so accumulate to a notch and stop at one step per notch.
        wheelSteps = juce::jlimit(-2.0f, 2.0f, wheelSteps + wheel.deltaY);
        if (std::abs(wheelSteps) >= 1.0f)
        {
            onStep(wheelSteps > 0.0f ? 1 : -1);
            wheelSteps = 0.0f;
        }
        return;
    }
    juce::TextButton::mouseWheelMove(event, wheel);
}

PresetBrowser::PresetBrowser(SubdoomAudioProcessor& processor)
    : audioProcessor(processor)
{
    setInterceptsMouseClicks(true, true);

    addAndMakeVisible(search);
    styleEditor(search);
    search.setTextToShowWhenEmpty("Search presets", sdui::faint());
    search.setJustification(juce::Justification::centredLeft);
    search.onTextChange = [this] { refresh(); };

    addAndMakeVisible(categoryBox);
    categoryBox.onChange = [this] { refresh(); };

    addAndMakeVisible(pcEditor);
    styleEditor(pcEditor);
    pcEditor.setTextToShowWhenEmpty("---", sdui::faint());
    pcEditor.setJustification(juce::Justification::centred);
    pcEditor.setInputRestrictions(3, "0123456789");
    pcEditor.onReturnKey = [this] { commitProgramCode(); };
    pcEditor.onEscapeKey = [this] { syncProgramEditor(true); };
    pcEditor.onFocusLost = [this] { commitProgramCode(); };

    addAndMakeVisible(list);
    list.setModel(this);
    list.setRowHeight(26);
    list.setColour(juce::ListBox::backgroundColourId, sdui::well());
    list.setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
    list.setOutlineThickness(1);

    struct ButtonSpec
    {
        juce::TextButton* button;
        juce::Colour accent;
        std::function<void()> action;
    };

    const ButtonSpec specs[] = {
        { &loadButton, sdui::amber(), [this] { loadSelected(); } },
        { &saveAsButton, sdui::amber(), [this] { showSaveAs(); } },
        { &renameButton, sdui::steel(), [this] { showRename(); } },
        { &deleteButton, sdui::red(), [this] { deleteSelected(); } },
        { &folderButton, sdui::steel(), [this] {
          // Open the folder and show what is in it, rather than its parent with the
          // folder selected.
          const auto folder = audioProcessor.presetFolder();
          if (!folder.startAsProcess())
              folder.revealToUser();
      } },
        { &rescanButton, sdui::steel(), [this] {
              audioProcessor.refreshPresets();
              refresh();
              if (onLibraryChanged)
                  onLibraryChanged();
          } },
        { &closeButton, sdui::steel(), [this] { if (onClose) onClose(); } },
        { &commitButton, sdui::amber(), [this] { commitInline(); } },
        { &cancelButton, sdui::steel(), [this] { cancelInline(); } },
    };

    for (const auto& spec : specs)
    {
        addAndMakeVisible(*spec.button);
        styleButton(*spec.button, spec.accent);
        spec.button->onClick = spec.action;
    }

    addAndMakeVisible(nameEditor);
    addAndMakeVisible(categoryEditor);
    addAndMakeVisible(notesEditor);
    styleEditor(nameEditor);
    styleEditor(categoryEditor);
    styleEditor(notesEditor);
    nameEditor.setTextToShowWhenEmpty("Preset name", sdui::faint());
    categoryEditor.setTextToShowWhenEmpty("Category", sdui::faint());
    notesEditor.setTextToShowWhenEmpty("Notes (optional)", sdui::faint());

    nameEditor.onReturnKey = [this] { commitInline(); };
    categoryEditor.onReturnKey = [this] { commitInline(); };
    notesEditor.onReturnKey = [this] { commitInline(); };
    nameEditor.onEscapeKey = [this] { cancelInline(); };
    categoryEditor.onEscapeKey = [this] { cancelInline(); };
    notesEditor.onEscapeKey = [this] { cancelInline(); };
    nameEditor.onTextChange = [this] { overwriteArmed = false; };

    addAndMakeVisible(statusLabel);
    statusLabel.setFont(sdui::font(11.0f));
    statusLabel.setColour(juce::Label::textColourId, sdui::dim());
    statusLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setText({}, juce::dontSendNotification);

    rebuildCategories();
    updateEditVisibility();
    refresh();
}

void PresetBrowser::rebuildCategories()
{
    const juce::StringArray categories = audioProcessor.presets().categories();
    if (categories == knownCategories)
        return;

    // Ids are assigned from the sorted list, so a new category shifts them.
    // Remember the name, not the id that used to mean it.
    const juce::String previous = categoryBox.getSelectedId() > 1 ? categoryBox.getText()
                                                                  : juce::String();
    knownCategories = categories;
    categoryBox.clear(juce::dontSendNotification);
    categoryBox.addItem("All categories", 1);
    for (int index = 0; index < categories.size(); ++index)
        categoryBox.addItem(categories[index], index + 2);

    int selectedId = 1;
    if (previous.isNotEmpty())
    {
        const int found = categories.indexOf(previous, true);
        if (found >= 0)
            selectedId = found + 2;
    }
    categoryBox.setSelectedId(selectedId, juce::dontSendNotification);
}

void PresetBrowser::lookAndFeelChanged()
{
    for (auto* editor : { &search, &pcEditor, &nameEditor, &categoryEditor, &notesEditor })
        styleEditor(*editor);

    // Each action keeps the accent it was given at construction: a theme change must
    // not turn DELETE amber, and the colours must not drift from one switch to the next.
    for (auto* button : { &loadButton, &saveAsButton, &commitButton })
        styleButton(*button, sdui::amber());
    for (auto* button : { &renameButton, &folderButton, &rescanButton, &closeButton,
                          &cancelButton })
        styleButton(*button, sdui::steel());
    styleButton(deleteButton, sdui::red());

    list.setColour(juce::ListBox::backgroundColourId, sdui::well());
    list.setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
    list.repaint();
    repaint();
}

void PresetBrowser::refresh()
{
    // The browser is constructed before the saved theme is applied, so the palette
    // is re-read here every time it opens.
    list.setColour(juce::ListBox::backgroundColourId, sdui::well());
    rebuildCategories();

    const bool reveal = preferredSelection >= 0;
    int keepSelected = reveal ? preferredSelection : selectedEntry();
    preferredSelection = -1;

    if (reveal && keepSelected >= 0)
    {
        const auto& entry = audioProcessor.presets().entry(keepSelected);
        const juce::String queryNow = search.getText().trim();
        const int categoryIdNow = categoryBox.getSelectedId();
        const juce::String categoryNow = categoryIdNow > 1 ? categoryBox.getText()
                                                           : juce::String();
        const bool hiddenBySearch = queryNow.isNotEmpty()
            && !entry.name.containsIgnoreCase(queryNow);
        const bool hiddenByCategory = categoryNow.isNotEmpty()
            && !entry.category.equalsIgnoreCase(categoryNow);
        if (entry.name.isNotEmpty() && (hiddenBySearch || hiddenByCategory))
        {
            search.setText({}, juce::dontSendNotification);
            categoryBox.setSelectedId(1, juce::dontSendNotification);
        }
    }

    const juce::String query = search.getText().trim();
    const int categoryId = categoryBox.getSelectedId();
    // getText() is the selected item. Item ids start at 1, so id - 2 is the
    // previous row ("All categories" when the first real category is selected).
    const juce::String category = categoryId > 1 ? categoryBox.getText() : juce::String();

    const int count = audioProcessor.presets().size();
    visibleEntries.clear();
    visibleEntries.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index)
    {
        const auto& entry = audioProcessor.presets().entry(index);
        if (!query.isEmpty() && !entry.name.containsIgnoreCase(query))
            continue;
        if (category.isNotEmpty() && !entry.category.equalsIgnoreCase(category))
            continue;
        visibleEntries.push_back(index);
    }

    list.updateContent();

    // Keep the row the user picked, then fall back to the loaded preset.
    const auto rowFor = [this](int entryIndex) {
        for (int index = 0; index < static_cast<int>(visibleEntries.size()); ++index)
            if (visibleEntries[static_cast<std::size_t>(index)] == entryIndex)
                return index;
        return -1;
    };

    int row = keepSelected >= 0 ? rowFor(keepSelected) : -1;
    if (row < 0)
        row = rowFor(audioProcessor.currentPresetIndex());

    if (row >= 0)
        list.selectRow(row);
    else
        list.deselectAllRows();

    deleteArmed = false;
    deleteButton.setButtonText("DELETE");
    syncProgramEditor(false);
    repaint();
}

int PresetBrowser::getNumRows()
{
    return static_cast<int>(visibleEntries.size());
}

void PresetBrowser::paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                                     bool rowIsSelected)
{
    if (row < 0 || row >= static_cast<int>(visibleEntries.size()))
        return;

    const int index = visibleEntries[static_cast<std::size_t>(row)];
    const auto& entry = audioProcessor.presets().entry(index);

    g.fillAll(sdui::well());
    if (rowIsSelected)
    {
        g.setColour(sdui::amber().withAlpha(0.14f));
        g.fillRoundedRectangle(1.0f, 1.0f, static_cast<float>(width - 2),
                               static_cast<float>(height - 2), 3.0f);
    }
    else if (row % 2 == 1)
    {
        g.setColour(sdui::panel().withAlpha(0.35f));
        g.fillRect(1, 1, width - 2, height - 2);
    }

    if (entry.factory)
        sdui::drawChip(g, juce::Rectangle<int>(8, (height - 13) / 2, 50, 13), "FACTORY",
                       sdui::blue(), false);
    else
        sdui::drawChip(g, juce::Rectangle<int>(8, (height - 13) / 2, 36, 13), "USER",
                       sdui::green(), false);

    g.setColour(rowIsSelected ? sdui::text() : sdui::text().withAlpha(0.88f));
    g.setFont(sdui::font(12.5f, true));
    g.drawText(entry.name, 66, 0, juce::jmax(0, width - 270), height,
               juce::Justification::centredLeft);

    const int code = audioProcessor.presets().programCode(index);
    g.setColour(code < 0 ? sdui::faint() : sdui::dim());
    g.setFont(sdui::font(10.5f));
    g.drawText(code < 0 ? "---" : juce::String(code).paddedLeft('0', 3),
               width - 196, 0, 44, height, juce::Justification::centredRight);

    g.setColour(sdui::dim());
    g.setFont(sdui::font(10.5f));
    g.drawText(entry.category, width - 148, 0, 140, height, juce::Justification::centredRight);
}

void PresetBrowser::selectedRowsChanged(int row)
{
    selectedRow = row;
    deleteArmed = false;
    deleteButton.setButtonText("DELETE");
    if (pcEditor.hasKeyboardFocus(true) && programEditIndex >= 0
        && programEditIndex != selectedEntry())
        commitProgramCode();
    syncProgramEditor(false);
}

void PresetBrowser::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    if (row < 0 || row >= static_cast<int>(visibleEntries.size()))
        return;
    selectedRow = row;
    loadSelected();
}

bool PresetBrowser::handleOverlayKey(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        if (mode != EditorMode::none)
            cancelInline();
        else if (onClose)
            onClose();
        return true;
    }

    if (key == juce::KeyPress::returnKey)
    {
        if (mode != EditorMode::none)
            commitInline();
        else
            loadSelected();
        return true;
    }

    return false;
}

void PresetBrowser::paint(juce::Graphics& g)
{
    auto area = getLocalBounds();
    sdui::drawFloatingCard(g, area);

    const auto titleArea = area.reduced(16).removeFromTop(20);
    g.setColour(sdui::text());
    g.setFont(sdui::trackedFont(13.0f, 2.6f, true));
    g.drawText("PRESETS", titleArea, juce::Justification::centredLeft);

    int factoryCount = 0;
    for (int index = 0; index < audioProcessor.presets().size(); ++index)
        if (audioProcessor.presets().entry(index).factory)
            ++factoryCount;
    const int userCount = audioProcessor.presets().size() - factoryCount;

    g.setColour(sdui::faint());
    g.setFont(sdui::trackedFont(9.0f, 1.2f));
    g.drawText(juce::String(factoryCount) + " FACTORY   " + juce::String(userCount) + " USER",
               titleArea.withTrimmedRight(80), juce::Justification::centredRight);

    g.setColour(sdui::dim());
    g.setFont(sdui::font(11.0f));
    g.drawFittedText(hoverText, hintRowBounds, juce::Justification::centredLeft, 1);
}

void PresetBrowser::pollHoverHint()
{
    struct Item { juce::Component* component; const char* text; };
    const Item items[] = {
        { &search, "Filter presets by name." },
        { &pcEditor, "Program change for the selected preset. Blank (---) skips PC switching." },
        { &categoryBox, "Show one category only." },
        { &list, "Click to select, double-click to load." },
        { &loadButton, "Load the selected preset." },
        { &saveAsButton, "Save the current sound as a new preset." },
        { &renameButton, "Rename the selected user preset." },
        { &deleteButton, "Delete the selected user preset. Press twice to confirm." },
        { &folderButton, "Open the preset folder on disk." },
        { &rescanButton, "Reload the library from disk." },
        { &closeButton, "Close the browser." },
        { &nameEditor, "Preset name." },
        { &categoryEditor, "Category shown in the list." },
        { &notesEditor, "Notes, shown when hovering the preset bar." },
        { &commitButton, "Save the preset." },
        { &cancelButton, "Cancel." },
    };

    const auto mouse = getMouseXYRelative();
    juce::String next = "Hover a control to see what it does.";
    for (const auto& item : items)
        if (item.component->isVisible() && item.component->getBounds().contains(mouse))
            next = item.text;

    if (next == hoverText)
        return;
    hoverText = next;
    repaint(hintRowBounds);
}

void PresetBrowser::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto title = area.removeFromTop(20);
    closeButton.setBounds(title.removeFromRight(66).withSizeKeepingCentre(66, 20));

    area.removeFromTop(8);
    auto searchRow = area.removeFromTop(26);
    categoryBox.setBounds(searchRow.removeFromRight(150));
    searchRow.removeFromRight(6);
    pcEditor.setBounds(searchRow.removeFromRight(52));
    searchRow.removeFromRight(8);
    search.setBounds(searchRow);

    area.removeFromTop(8);
    hintRowBounds = area.removeFromBottom(18);
    area.removeFromBottom(2);
    statusLabel.setBounds(area.removeFromBottom(16));
    area.removeFromBottom(6);

    auto editRow = area.removeFromBottom(26);
    area.removeFromBottom(6);
    auto buttonRow = area.removeFromBottom(26);
    area.removeFromBottom(8);

    list.setBounds(area);

    auto placeRow = [](juce::Rectangle<int> row,
                       std::initializer_list<juce::TextButton*> buttons) {
        const int gap = 6;
        const int count = static_cast<int>(buttons.size());
        const int width = (row.getWidth() - gap * (count - 1)) / count;
        int index = 0;
        for (auto* button : buttons)
        {
            if (index == count - 1)
                button->setBounds(row);
            else
            {
                button->setBounds(row.removeFromLeft(width));
                row.removeFromLeft(gap);
            }
            ++index;
        }
    };

    placeRow(buttonRow, { &loadButton, &saveAsButton, &renameButton, &deleteButton,
                          &folderButton, &rescanButton });

    auto inlineArea = editRow;
    cancelButton.setBounds(inlineArea.removeFromRight(66));
    inlineArea.removeFromRight(5);
    commitButton.setBounds(inlineArea.removeFromRight(58));
    inlineArea.removeFromRight(6);
    const int inlineWidth = inlineArea.getWidth();
    nameEditor.setBounds(inlineArea.removeFromLeft(inlineWidth * 38 / 100));
    inlineArea.removeFromLeft(5);
    categoryEditor.setBounds(inlineArea.removeFromLeft(inlineWidth * 24 / 100));
    inlineArea.removeFromLeft(5);
    notesEditor.setBounds(inlineArea);

    updateEditVisibility();
}

void PresetBrowser::updateEditVisibility()
{
    const bool editing = mode != EditorMode::none;
    nameEditor.setVisible(editing);
    categoryEditor.setVisible(mode == EditorMode::saveAs);
    notesEditor.setVisible(mode == EditorMode::saveAs);
    commitButton.setVisible(editing);
    cancelButton.setVisible(editing);

    loadButton.setVisible(!editing);
    saveAsButton.setVisible(!editing);
    renameButton.setVisible(!editing);
    deleteButton.setVisible(!editing);
    folderButton.setVisible(!editing);
    rescanButton.setVisible(!editing);

    commitButton.setButtonText(mode == EditorMode::rename ? "RENAME" : "SAVE");
}

void PresetBrowser::syncProgramEditor(bool force)
{
    if (committingProgram && !force)
        return;
    const int index = selectedEntry();
    if (!force && pcEditor.hasKeyboardFocus(true) && programEditIndex == index)
        return;

    fillingProgramEditor = true;
    programEditIndex = index;
    pcEditor.setEnabled(index >= 0);
    if (index < 0)
        pcEditor.setText({}, juce::dontSendNotification);
    else
    {
        const int code = audioProcessor.presets().programCode(index);
        pcEditor.setText(code < 0 ? juce::String() : juce::String(code),
                         juce::dontSendNotification);
    }
    fillingProgramEditor = false;
}

void PresetBrowser::commitProgramCode()
{
    if (fillingProgramEditor || committingProgram)
        return;

    const int index = programEditIndex;
    if (index < 0 || index >= audioProcessor.presets().size())
        return;

    const juce::String text = pcEditor.getText().trim();
    int code = -1;
    if (text.isNotEmpty())
    {
        if (!text.containsOnly("0123456789"))
        {
            setStatus("PC is 0-127, or blank", true);
            syncProgramEditor(true);
            return;
        }
        code = text.getIntValue();
        if (code > 127)
        {
            setStatus("PC is 0-127, or blank", true);
            syncProgramEditor(true);
            return;
        }
    }

    if (audioProcessor.presets().programCode(index) == code)
        return;

    committingProgram = true;
    juce::String error;
    const bool saved = audioProcessor.setPresetProgram(index, code, error);
    committingProgram = false;
    if (!saved)
    {
        setStatus(error.isEmpty() ? "Could not set the PC" : error, true);
        syncProgramEditor(true);
        return;
    }

    setStatus(code < 0 ? "PC cleared" : "PC " + juce::String(code), false);
    refresh();
    syncProgramEditor(true);
}

int PresetBrowser::selectedEntry() const
{
    if (selectedRow < 0 || selectedRow >= static_cast<int>(visibleEntries.size()))
        return -1;
    return visibleEntries[static_cast<std::size_t>(selectedRow)];
}

void PresetBrowser::loadSelected()
{
    const int index = selectedEntry();
    if (index < 0)
    {
        setStatus("Select a preset first", true);
        return;
    }
    audioProcessor.loadPreset(index);
    setStatus("Loaded " + audioProcessor.currentPresetName(), false);
    if (onLibraryChanged)
        onLibraryChanged();
    refresh();
}

void PresetBrowser::showSaveAs()
{
    mode = EditorMode::saveAs;
    overwriteArmed = false;

    const int index = selectedEntry();
    if (index >= 0)
    {
        const auto& entry = audioProcessor.presets().entry(index);
        nameEditor.setText(entry.name, false);
        categoryEditor.setText(entry.category, false);
        notesEditor.setText(entry.notes, false);
    }
    else
    {
        nameEditor.setText(audioProcessor.currentPresetName(), false);
        categoryEditor.setText(audioProcessor.currentPresetCategory(), false);
        notesEditor.setText(audioProcessor.currentPresetNotes(), false);
    }

    nameEditor.selectAll();
    updateEditVisibility();
    resized();
    nameEditor.grabKeyboardFocus();
    setStatus("Name the preset, then press Save. Existing names are not overwritten by accident.",
              false);
}

void PresetBrowser::showRename()
{
    const int index = selectedEntry();
    if (index < 0)
    {
        setStatus("Select a preset first", true);
        return;
    }
    if (audioProcessor.presets().entry(index).factory)
    {
        setStatus("Factory presets cannot be renamed; use Save As", true);
        return;
    }

    mode = EditorMode::rename;
    overwriteArmed = false;
    nameEditor.setText(audioProcessor.presets().entry(index).name, false);
    nameEditor.selectAll();
    updateEditVisibility();
    resized();
    nameEditor.grabKeyboardFocus();
}

void PresetBrowser::commitInline()
{
    juce::String error;

    if (mode == EditorMode::rename)
    {
        const int index = selectedEntry();
        if (index < 0)
        {
            setStatus("Select a preset first", true);
            return;
        }
        if (!audioProcessor.renamePreset(index, nameEditor.getText(), error))
        {
            setStatus(error, true);
            return;
        }
        // The stored name is sanitised, so the text that was typed can miss the row.
        const juce::String stored = juce::String(subdoom::sanitisePresetText(
            nameEditor.getText().trim().toStdString(), 64));
        const int renamed = audioProcessor.presets().indexOfName(stored, false);
        preferredSelection = renamed >= 0 ? renamed : audioProcessor.currentPresetIndex();
        setStatus("Renamed", false);
    }
    else
    {
        if (!audioProcessor.savePresetCopy(nameEditor.getText(), categoryEditor.getText(),
                                           notesEditor.getText(), overwriteArmed, error))
        {
            if (error.containsIgnoreCase("already exists"))
            {
                overwriteArmed = true;
                setStatus(error + " - press SAVE again to replace it", true);
            }
            else
            {
                setStatus(error, true);
            }
            return;
        }
        // The row that was selected is still the one LOAD would use. Point it at
        // the preset that was just written, and scroll that row into view.
        preferredSelection = audioProcessor.currentPresetIndex();
        setStatus("Saved " + audioProcessor.currentPresetName(), false);
    }

    mode = EditorMode::none;
    overwriteArmed = false;
    updateEditVisibility();
    resized();
    if (onLibraryChanged)
        onLibraryChanged();
    refresh();
}

void PresetBrowser::cancelInline()
{
    mode = EditorMode::none;
    overwriteArmed = false;
    updateEditVisibility();
    resized();
    setStatus("Cancelled", false);
}

void PresetBrowser::deleteSelected()
{
    const int index = selectedEntry();
    if (index < 0)
    {
        setStatus("Select a preset first", true);
        return;
    }
    if (audioProcessor.presets().entry(index).factory)
    {
        setStatus("Factory presets cannot be deleted", true);
        return;
    }
    if (!deleteArmed)
    {
        deleteArmed = true;
        deleteButton.setButtonText("CONFIRM");
        setStatus("Press CONFIRM to delete " + audioProcessor.presets().entry(index).name, true);
        return;
    }

    deleteArmed = false;
    deleteButton.setButtonText("DELETE");

    juce::String error;
    if (!audioProcessor.deletePreset(index, error))
    {
        setStatus(error, true);
        return;
    }
    setStatus("Deleted", false);
    if (onLibraryChanged)
        onLibraryChanged();
    refresh();
}

void PresetBrowser::setStatus(const juce::String& message, bool isError)
{
    statusLabel.setText(message, juce::dontSendNotification);
    statusLabel.setColour(juce::Label::textColourId,
                          isError ? sdui::red() : sdui::dim());
}

MidiPage::MidiPage(SubdoomAudioProcessor& processor)
    : audioProcessor(processor)
{
    setInterceptsMouseClicks(true, true);

    addAndMakeVisible(channelLabel);
    channelLabel.setText("CHANNEL", juce::dontSendNotification);
    channelLabel.setFont(sdui::trackedFont(9.0f, 1.4f, true));
    channelLabel.setColour(juce::Label::textColourId, sdui::dim());
    channelLabel.setJustificationType(juce::Justification::centredLeft);

    addAndMakeVisible(programLabel);
    programLabel.setText("PROGRAM CHANGE", juce::dontSendNotification);
    programLabel.setFont(sdui::trackedFont(9.0f, 1.4f, true));
    programLabel.setColour(juce::Label::textColourId, sdui::dim());
    programLabel.setJustificationType(juce::Justification::centredLeft);

    addAndMakeVisible(channelBox);
    channelBox.addItem("Any", 1);
    for (int channel = 1; channel <= 16; ++channel)
        channelBox.addItem(juce::String(channel), channel + 1);
    channelBox.setSelectedId(1, juce::dontSendNotification);
    channelBox.onChange = [this] {
        audioProcessor.setMidiChannel(channelBox.getSelectedId() - 1);
        refresh();
    };

    struct SimpleButton
    {
        juce::TextButton* button;
        juce::Colour accent;
        std::function<void()> action;
    };

    const SimpleButton buttons[] = {
        { &programChange, sdui::green(), [this] {
              audioProcessor.setProgramChangeEnabled(programChange.getToggleState());
              refresh();
          } },
        { &clearAll, sdui::red(), [this] {
              audioProcessor.clearAllMappings();
              refresh();
          } },
        { &closeButton, sdui::steel(), [this] { if (onClose) onClose(); } },
    };

    for (const auto& spec : buttons)
    {
        addAndMakeVisible(*spec.button);
        styleButton(*spec.button, spec.accent);
        spec.button->onClick = spec.action;
        spec.button->getProperties().set("strong", true);
    }
    programChange.setClickingTogglesState(true);

    for (int parameter = 1; parameter <= subdoom::kParameterCount; ++parameter)
        buildRow(parameter, juce::String(subdoom::parameterName(parameter)));
    buildRow(SubdoomAudioProcessor::kPresetPrevTarget, "PRESET PREV");
    buildRow(SubdoomAudioProcessor::kPresetNextTarget, "PRESET NEXT");

    refresh();
}

void MidiPage::buildRow(int target, const juce::String& label)
{
    Row row;
    row.target = target;
    row.name = std::make_unique<juce::Label>();
    row.name->setText(label, juce::dontSendNotification);
    row.name->setFont(sdui::font(11.5f));
    row.name->setColour(juce::Label::textColourId, sdui::text());
    row.name->setJustificationType(juce::Justification::centredLeft);

    row.mapping = std::make_unique<juce::Label>();
    row.mapping->setText("--", juce::dontSendNotification);
    row.mapping->setFont(sdui::font(11.0f));
    row.mapping->setColour(juce::Label::textColourId, sdui::faint());
    row.mapping->setJustificationType(juce::Justification::centredRight);

    // A link, not a button: a column of pills reads as a wall of chrome.
    row.learn = std::make_unique<juce::TextButton>("LEARN");
    row.learn->setColour(juce::TextButton::buttonOnColourId, sdui::amber());
    row.learn->getProperties().set("link", true);
    row.learn->setMouseCursor(juce::MouseCursor::PointingHandCursor);
    row.learn->onClick = [this, target] {
        audioProcessor.beginLearn(target);
        refresh();
    };

    // The clear action is an icon: there is no room for another word here.
    row.clear = std::make_unique<juce::TextButton>(juce::String::charToString(0x00d7));
    row.clear->setColour(juce::TextButton::buttonOnColourId, sdui::red());
    row.clear->getProperties().set("link", true);
    row.clear->getProperties().set("icon", true);
    row.clear->setMouseCursor(juce::MouseCursor::PointingHandCursor);
    row.clear->onClick = [this, target] {
        audioProcessor.clearMapping(target);
        refresh();
    };

    addAndMakeVisible(*row.name);
    addAndMakeVisible(*row.mapping);
    addAndMakeVisible(*row.learn);
    addAndMakeVisible(*row.clear);
    rows.push_back(std::move(row));
}

void MidiPage::paint(juce::Graphics& g)
{
    auto area = getLocalBounds();
    sdui::drawFloatingCard(g, area);

    auto inner = area.reduced(16);
    auto title = inner.removeFromTop(20);
    g.setColour(sdui::text());
    g.setFont(sdui::trackedFont(13.0f, 2.6f, true));
    g.drawText("MIDI CONTROL", title, juce::Justification::centredLeft);

    // One line at the bottom: the hover hint. The mapped-count status used to sit
    // on the same row and the two drew over each other.
    g.setColour(sdui::dim());
    g.setFont(sdui::font(11.0f));
    g.drawFittedText(hoverText, hintRowBounds, juce::Justification::centredLeft, 1);
}

void MidiPage::pollHoverHint()
{
    const auto mouse = getMouseXYRelative();
    juce::String next = "Hover a control to see what it does.";

    if (channelBox.getBounds().contains(mouse))
        next = "Only respond on this MIDI channel. Any listens to all of them.";
    else if (programChange.getBounds().contains(mouse)
             || programLabel.getBounds().contains(mouse))
        next = "When on, a MIDI program change loads the preset assigned that PC. A blank PC is skipped.";
    else if (clearAll.getBounds().contains(mouse))
        next = "Remove every mapping.";
    else if (closeButton.getBounds().contains(mouse))
        next = "Close the MIDI page.";
    else
    {
        for (auto& row : rows)
        {
            if (row.learn->getBounds().contains(mouse))
            {
                next = "Press, then move a knob or pedal to bind it.";
                break;
            }
            if (row.clear->getBounds().contains(mouse))
            {
                next = "Forget this mapping.";
                break;
            }
            if (row.mapping->getBounds().contains(mouse))
            {
                next = "The controller number currently bound to this control.";
                break;
            }
            if (row.name->getBounds().contains(mouse))
            {
                next = "The parameter this row maps.";
                break;
            }
        }
    }

    if (next == hoverText)
        return;
    hoverText = next;
    repaint(hintRowBounds);
}

void MidiPage::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto title = area.removeFromTop(20);
    closeButton.setBounds(title.removeFromRight(66).withSizeKeepingCentre(66, 20));

    area.removeFromTop(8);
    auto top = area.removeFromTop(26);
    channelLabel.setBounds(top.removeFromLeft(58));
    channelBox.setBounds(top.removeFromLeft(86));
    top.removeFromLeft(16);
    programLabel.setBounds(top.removeFromLeft(104));
    top.removeFromLeft(2);
    programChange.setBounds(top.removeFromLeft(52));
    top.removeFromLeft(10);
    clearAll.setBounds(top.removeFromLeft(84));

    area.removeFromTop(10);
    hintRowBounds = area.removeFromBottom(20);
    area.removeFromBottom(2);
    area.removeFromBottom(16);

    const int rowCount = static_cast<int>(rows.size());
    const int rowsPerColumn = (rowCount + 1) / 2;
    const int rowHeight = juce::jmax(22, area.getHeight() / rowsPerColumn);
    const int gap = 16;
    const int columnWidth = (area.getWidth() - gap) / 2;

    for (int slot = 0; slot < rowCount; ++slot)
    {
        const int column = slot / rowsPerColumn;
        const int row = slot % rowsPerColumn;
        juce::Rectangle<int> cell(area.getX() + column * (columnWidth + gap),
                                  area.getY() + row * rowHeight, columnWidth,
                                  juce::jmin(rowHeight, area.getBottom() - (area.getY() + row * rowHeight)));

        auto line = cell.reduced(0, 1);
        auto& entry = rows[static_cast<std::size_t>(slot)];
        entry.clear->setBounds(line.removeFromRight(22));
        line.removeFromRight(5);
        entry.learn->setBounds(line.removeFromRight(56));
        line.removeFromRight(6);
        entry.mapping->setBounds(line.removeFromRight(52));
        entry.name->setBounds(line);
    }
}

void MidiPage::lookAndFeelChanged()
{
    styleButton(programChange, sdui::green());
    styleButton(clearAll, sdui::red());
    styleButton(closeButton, sdui::steel());
    channelLabel.setColour(juce::Label::textColourId, sdui::dim());
    programLabel.setColour(juce::Label::textColourId, sdui::dim());
    for (auto& row : rows)
    {
        row.name->setColour(juce::Label::textColourId, sdui::text());
        row.mapping->setColour(juce::Label::textColourId, sdui::faint());
        row.learn->setColour(juce::TextButton::buttonOnColourId, sdui::amber());
        row.clear->setColour(juce::TextButton::buttonOnColourId, sdui::red());
    }
    repaint();
}

void MidiPage::refresh()
{
    channelBox.setSelectedId(audioProcessor.midiChannel() + 1, juce::dontSendNotification);

    // The label names the setting, the pill carries its value.
    const bool programOn = audioProcessor.programChangeEnabled();
    programChange.setToggleState(programOn, juce::dontSendNotification);
    programChange.setButtonText(programOn ? "ON" : "OFF");

    const int learning = audioProcessor.learnTarget();

    for (auto& row : rows)
    {
        const bool bound = audioProcessor.ccForControl(row.target) >= 0;

        row.mapping->setText(audioProcessor.mappingLabel(row.target),
                             juce::dontSendNotification);
        row.mapping->setColour(juce::Label::textColourId,
                               bound ? sdui::amber() : sdui::faint());
        row.learn->setButtonText(learning == row.target ? "WAIT" : "LEARN");
        row.learn->setToggleState(learning == row.target, juce::dontSendNotification);
        row.clear->setEnabled(bound);
    }

    repaint();
}

SubdoomAudioProcessorEditor::SubdoomAudioProcessorEditor(SubdoomAudioProcessor& processor)
    : AudioProcessorEditor(&processor),
      audioProcessor(processor),
      scope(processor),
      spectrum(processor),
      tuner(processor),
      inputLevel(processor),
      levelMeter(processor),
      browser(processor),
      midiPage(processor)
{
    // Restore the saved theme before any child reads the palette. Older sessions
    // stored a bool at ui.dark; read that when ui.theme is absent. Neither
    // property means the dark default.
    {
        const int saved = static_cast<int>(
            audioProcessor.apvts.state.getProperty("ui.theme", -1));
        if (saved >= 0)
            sdui::setTheme(saved >= 1 ? sdui::Theme::dark : sdui::Theme::light);
        else
            sdui::setTheme(static_cast<bool>(
                               audioProcessor.apvts.state.getProperty("ui.dark", true))
                               ? sdui::Theme::dark
                               : sdui::Theme::light);
    }

    setLookAndFeel(&lookAndFeel);
    lookAndFeel.updateColours();
    // Children are constructed before the saved theme is restored, so their cached
    // palette colours are from the dark default until this broadcast reaches them.
    sendLookAndFeelChange();
    setResizable(true, true);
    setResizeLimits(1000, 640, 1800, 1200);
    setOpaque(true);
    setWantsKeyboardFocus(true);

    for (const auto& spec : kControlSpecs)
    {
        if (spec.isSwitch)
        {
            auto cell = std::make_unique<SwitchCell>(audioProcessor, spec.id, spec.caption);
            addAndMakeVisible(*cell);
            switchCells.push_back(cell.get());
            cells.push_back(std::move(cell));
        }
        else
        {
            auto cell = std::make_unique<KnobCell>(audioProcessor, spec.id, spec.caption,
                                                   spec.role);
            cell->onHintLock = [this](int parameter) {
                hintLock = parameter;
                updateHint();
            };
            addAndMakeVisible(*cell);
            knobCells.push_back(cell.get());
            cells.push_back(std::move(cell));
        }
    }

    addAndMakeVisible(scope);
    addAndMakeVisible(spectrum);
    addAndMakeVisible(tuner);
    addAndMakeVisible(inputLevel);
    addAndMakeVisible(levelMeter);
    addAndMakeVisible(hint);
    addAndMakeVisible(midiLed);
    addAndMakeVisible(presetBar);
    addAndMakeVisible(prevButton);
    addAndMakeVisible(nextButton);
    addAndMakeVisible(saveButton);
    addAndMakeVisible(revertButton);
    addAndMakeVisible(themeButton);
    addAndMakeVisible(compareButton);
    addAndMakeVisible(midiButton);
    addAndMakeVisible(developerLink);

    styleButton(prevButton, sdui::steel());
    styleButton(nextButton, sdui::steel());
    styleButton(saveButton, sdui::amber());
    styleButton(revertButton, sdui::steel());
    styleButton(compareButton, sdui::green());
    styleButton(midiButton, sdui::blue());
    styleButton(presetBar, sdui::amber());
    presetBar.getProperties().set("menu", true);

    developerLink.setButtonText("Distant Nebula");
    developerLink.setURL(juce::URL("https://distantnebula.com/"));
    developerLink.setFont(sdui::trackedFont(9.0f, 1.5f), false);
    developerLink.setJustificationType(juce::Justification::centredLeft);
    updateLink.setFont(sdui::trackedFont(9.0f, 1.2f), false);
    updateLink.setJustificationType(juce::Justification::centredRight);
    updateLink.setColour(juce::HyperlinkButton::textColourId, sdui::amber());
    addChildComponent(updateLink);

    midiLed.setLedColour(sdui::amber());

    prevButton.onClick = [this] {
        audioProcessor.stepPreset(-1);
        refreshPresetBar();
    };
    nextButton.onClick = [this] {
        audioProcessor.stepPreset(1);
        refreshPresetBar();
    };

    const auto presetStepMenu = [this](StepButton& button, int target, const juce::String& title) {
        button.onContextMenu = [this, &button, target, title] {
            const int cc = audioProcessor.ccForControl(target);
            juce::PopupMenu menu;
            menu.addSectionHeader(title);
            menu.addItem(1, "MIDI Learn");
            menu.addItem(2, cc >= 0 ? "Clear MIDI (" + juce::String(cc) + ")" : "Clear MIDI",
                         cc >= 0);

            juce::Component::SafePointer<SubdoomAudioProcessorEditor> safe(this);
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&button),
                               [safe, target](int result) {
                                   if (safe == nullptr || result == 0)
                                       return;
                                   if (result == 1)
                                       safe->audioProcessor.beginLearn(target);
                                   else if (result == 2)
                                       safe->audioProcessor.clearMapping(target);
                               });
        };
    };
    presetStepMenu(prevButton, SubdoomAudioProcessor::kPresetPrevTarget, "Previous preset");
    presetStepMenu(nextButton, SubdoomAudioProcessor::kPresetNextTarget, "Next preset");
    presetBar.onClick = [this] {
        audioProcessor.refreshPresets();
        browser.refresh();
        openOverlay(browser);
    };
    presetBar.onStep = [this](int direction) {
        audioProcessor.stepPreset(direction);
        refreshPresetBar();
    };
    saveButton.onClick = [this] {
        if (audioProcessor.currentPresetIsFactory())
        {
            browser.showSaveAs();
            openOverlay(browser);
            return;
        }
        juce::String error;
        if (!audioProcessor.saveCurrentPreset(audioProcessor.currentPresetNotes(), error))
        {
            browser.showSaveAs();
            openOverlay(browser);
        }
        refreshPresetBar();
    };
    revertButton.onClick = [this] {
        audioProcessor.revertPreset();
        refreshPresetBar();
    };
    themeButton.onClick = [this] {
        applyTheme(sdui::theme() == sdui::Theme::light ? sdui::Theme::dark
                                                       : sdui::Theme::light);
    };
    compareButton.onClick = [this] {
        audioProcessor.toggleCompare();
        refreshPresetBar();
    };
    midiButton.onClick = [this] {
        midiPage.refresh();
        openOverlay(midiPage);
    };

    browser.onClose = [this] { closeOverlay(); };
    browser.onLibraryChanged = [this] { refreshPresetBar(); };
    midiPage.onClose = [this] { closeOverlay(); };

    addChildComponent(browser);
    addChildComponent(midiPage);

    // The overlays were built before the saved theme was restored, and the first
    // broadcast ran before they were children. Repeat it now that they are, so
    // their labels are not left with the light palette's ink.
    sendLookAndFeelChange();

    setSize(1100, 680);
    refreshPresetBar();
    startTimerHz(30);

    // Owner, repo, and version: SUBDOOM_GITHUB_* and SUBDOOM_VERSION_STRING
    // in CMakeLists.txt. The version definition follows project() VERSION.
    juce::Component::SafePointer<SubdoomAudioProcessorEditor> self(this);
    releaseChecker.check(
        { SUBDOOM_GITHUB_OWNER, SUBDOOM_GITHUB_REPO, SUBDOOM_VERSION_STRING,
          "Subdoom/" SUBDOOM_VERSION_STRING },
        juce::Component::SafePointer<juce::Component>(this),
        [self](const ReleaseInfo& info) {
            if (self != nullptr)
                self->showAvailableUpdate(info);
        });
}

SubdoomAudioProcessorEditor::~SubdoomAudioProcessorEditor()
{
    releaseChecker.cancel();
    stopTimer();
    knobCells.clear();
    switchCells.clear();
    cells.clear();
    setLookAndFeel(nullptr);
}

void SubdoomAudioProcessorEditor::layoutGroup(juce::Rectangle<int> area, const int* indices,
                                              int count, int rows, int columns) const
{
    auto inner = area;
    inner.removeFromTop(17);

    const int gap = 4;
    const int cellWidth = (inner.getWidth() - gap * (columns - 1)) / columns;
    const int cellHeight = (inner.getHeight() - gap * (rows - 1)) / rows;

    for (int index = 0; index < count; ++index)
    {
        const int row = index / columns;
        const int column = index % columns;
        const int x = inner.getX() + column * (cellWidth + gap);
        const int y = inner.getY() + row * (cellHeight + gap);
        const int parameter = indices[index];
        if (parameter >= 1 && parameter <= static_cast<int>(cells.size()))
            cells[static_cast<std::size_t>(parameter - 1)]->setBounds(x, y, cellWidth, cellHeight);
    }
}

void SubdoomAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(16);
    headerArea = area.removeFromTop(52);
    area.removeFromTop(14);
    bodyArea = area;

    const int gap = 14;
    const int rightWidth = juce::jlimit(300, 400, bodyArea.getWidth() * 34 / 100);
    rightArea = bodyArea.removeFromRight(rightWidth);
    bodyArea.removeFromRight(gap);

    // One card holds all three knob groups; headings and rules divide it inside.
    controlsArea = bodyArea;
    auto inner = controlsArea.reduced(14, 12);
    const int groupGap = 10;
    frontArea = inner.removeFromTop(inner.getHeight() * 45 / 100);
    inner.removeFromTop(groupGap);
    shapeArea = inner.removeFromTop((inner.getHeight() - groupGap) * 54 / 100);
    inner.removeFromTop(groupGap);
    outArea = inner;

    // Right column: tuner, meters, spectrum, scope, then the hint card.
    auto right = rightArea;
    hint.setBounds(right.removeFromBottom(122));
    right.removeFromBottom(gap);
    meterArea = right;

    auto column = meterArea;
    tuner.setBounds(column.removeFromTop(152));
    column.removeFromTop(12);
    inputLevel.setBounds(column.removeFromTop(42).reduced(18, 0));
    column.removeFromTop(8);
    levelMeter.setBounds(column.removeFromTop(28).reduced(18, 0));
    column.removeFromTop(14);
    spectrum.setBounds(column.removeFromTop(column.getHeight() * 46 / 100));
    column.removeFromTop(14);
    scope.setBounds(column);

    auto header = headerArea;
    const int wordmarkCap = updateLink.isVisible() ? 260 : 200;
    wordmarkArea = header.removeFromLeft(juce::jmin(wordmarkCap, header.getWidth() / 4));
    const int makerX = wordmarkArea.getX() + kWordmarkMark + 10;
    const int makerY = wordmarkArea.getY() + 22;
    const int makerRight = makerX + wordmarkArea.getWidth() - kWordmarkMark;
    if (updateLink.isVisible())
    {
        constexpr int updateWidth = 52;
        updateLink.setBounds(makerRight - updateWidth, makerY, updateWidth, 14);
        developerLink.setBounds(makerX, makerY,
                                juce::jmax(0, updateLink.getX() - 6 - makerX), 14);
    }
    else
    {
        developerLink.setBounds(makerX, makerY, juce::jmax(0, makerRight - makerX), 14);
    }

    const int midiWidth = 58;
    const int compareWidth = 40;
    midiButton.setBounds(header.removeFromRight(midiWidth).withSizeKeepingCentre(midiWidth, 26));
    header.removeFromRight(6);
    midiLed.setBounds(header.removeFromRight(14).withSizeKeepingCentre(10, 10));
    header.removeFromRight(12);
    compareButton.setBounds(
        header.removeFromRight(compareWidth).withSizeKeepingCentre(compareWidth, 26));
    header.removeFromRight(8);
    themeButton.setBounds(header.removeFromRight(28).withSizeKeepingCentre(28, 26));
    header.removeFromRight(12);

    const int saveWidth = 54;
    const int trailing = saveWidth + 6 + 64 + 14;
    const int barWidth = juce::jlimit(150, 300, header.getWidth() - trailing - 68);

    auto cluster = header.removeFromLeft(28 + 6 + barWidth + 6 + 28);
    prevButton.setBounds(cluster.removeFromLeft(28).withSizeKeepingCentre(28, 26));
    cluster.removeFromLeft(6);
    presetBar.setBounds(cluster.removeFromLeft(barWidth).withSizeKeepingCentre(barWidth, 28));
    cluster.removeFromLeft(6);
    nextButton.setBounds(cluster.removeFromLeft(28).withSizeKeepingCentre(28, 26));

    header.removeFromLeft(12);
    saveButton.setBounds(header.removeFromLeft(saveWidth).withSizeKeepingCentre(saveWidth, 26));
    header.removeFromLeft(6);
    revertButton.setBounds(header.removeFromLeft(64).withSizeKeepingCentre(64, 26));

    layoutGroup(frontArea, kFrontIds, 10, 2, 5);
    layoutGroup(shapeArea, kShapeIds, 6, 1, 6);
    layoutGroup(outArea, kOutIds, 5, 1, 5);

    const int browserWidth = juce::jmin(760, getWidth() - 48);
    const int browserHeight = juce::jmin(540, getHeight() - 48);
    browser.setBounds(juce::Rectangle<int>(browserWidth, browserHeight)
                          .withCentre(getLocalBounds().getCentre()));

    const int midiWidthPage = juce::jmin(820, getWidth() - 48);
    const int midiHeightPage = juce::jmin(540, getHeight() - 48);
    midiPage.setBounds(juce::Rectangle<int>(midiWidthPage, midiHeightPage)
                           .withCentre(getLocalBounds().getCentre()));

    // Rebuilding the whole backdrop on every resize event made a live resize crawl.
    // The first build is immediate so the editor never opens flat; after that the
    // timer rebuilds at most once a frame and paint scales the stale image.
    if (backdrop.isValid())
        backdropDirty = true;
    else
        buildBackdrop();
}

void SubdoomAudioProcessorEditor::showAvailableUpdate(const ReleaseInfo& info)
{
    if (!info.updateAvailable || !info.htmlUrl.startsWith("https://"))
        return;

    availableRelease = info.tag;
    updateLink.setButtonText("Update");
    updateLink.setURL(juce::URL(info.htmlUrl));
    updateLink.setFont(sdui::trackedFont(9.0f, 1.2f), false);
    updateLink.setJustificationType(juce::Justification::centredRight);
    updateLink.setColour(juce::HyperlinkButton::textColourId, sdui::amber());
    updateLink.setVisible(true);
    resized();
}

void SubdoomAudioProcessorEditor::applyTheme(sdui::Theme newTheme)
{
    if (sdui::theme() == newTheme)
        return;

    sdui::setTheme(newTheme);
    audioProcessor.apvts.state.setProperty("ui.theme", static_cast<int>(newTheme), nullptr);
    audioProcessor.apvts.state.removeProperty("ui.dark", nullptr);

    midiLed.setLedColour(sdui::amber());
    lookAndFeel.updateColours();
    sendLookAndFeelChange();
    updateLink.setColour(juce::HyperlinkButton::textColourId, sdui::amber());

    backdropDirty = false;
    buildBackdrop();
    repaint();
}

void SubdoomAudioProcessorEditor::updateHint()
{
    // While a knob is being dragged the hint stays on that control, rather than
    // following the pointer around.
    if (hintLock > 0)
    {
        hint.setHint({}, subdoom::parameterName(hintLock), helpForParameter(hintLock));
        return;
    }

    const auto section = [this](juce::Rectangle<int> area, const char* title,
                                const char* body) {
        if (!area.contains(getMouseXYRelative()))
            return false;
        hint.setHint({}, title, body);
        return true;
    };

    if (overlay != nullptr)
    {
        hint.setHint({}, {}, "Hover over a control for more details.");
        return;
    }

    const auto mouse = getMouseXYRelative();
    if (!getLocalBounds().contains(mouse))
    {
        hint.setHint({}, {}, "Hover over a control for more details.");
        return;
    }

    const auto over = [this, mouse](const juce::Component& component) {
        return component.isVisible() && component.getBounds().contains(mouse);
    };

    if (over(presetBar))
    {
        const juce::String notes = audioProcessor.currentPresetNotes();
        hint.setHint({}, audioProcessor.currentPresetName(),
                     notes.isEmpty()
                         ? "Click to browse presets. Scroll to step through them."
                         : notes);
        return;
    }
    if (over(prevButton))
    {
        hint.setHint({}, "Previous preset",
                     "Steps back one preset; wraps at the start. Right-click to MIDI learn.");
        return;
    }
    if (over(nextButton))
    {
        hint.setHint({}, "Next preset",
                     "Steps forward one preset; wraps at the end. Right-click to MIDI learn.");
        return;
    }
    if (over(saveButton))
    {
        hint.setHint({}, "Save",
                     "Writes your edits into the current user preset. On a factory preset it "
                     "opens Save As.");
        return;
    }
    if (over(revertButton))
    {
        hint.setHint({}, "Revert", "Discards edits and reloads the preset you had.");
        return;
    }
    if (over(themeButton))
    {
        const char* name = sdui::theme() == sdui::Theme::light ? "Light" : "Dark";
        hint.setHint({}, juce::String("Theme: ") + name,
                     "Click to switch between Light and Dark. The choice is stored in the project.");
        return;
    }
    if (over(compareButton))
    {
        hint.setHint({}, "A / B",
                     "Stores the current sound and flips to the other slot, so you can compare "
                     "two settings.");
        return;
    }
    if (over(midiButton))
    {
        hint.setHint({}, "MIDI",
                     "Opens the MIDI map: channel, program change, and learn.");
        return;
    }
    if (over(updateLink))
    {
        hint.setHint({}, "Update",
                     "Version " + availableRelease
                         + " is available. Opens the release page in your browser.");
        return;
    }
    if (over(developerLink))
    {
        hint.setHint({}, "Distant Nebula", "Opens the maker's site in your browser.");
        return;
    }

    if (over(tuner))
    {
        hint.setHint({}, "Tuner",
                     "The note being played. Green when it is in tune, red when sharp or flat.");
        return;
    }
    if (over(spectrum))
    {
        hint.setHint({}, "Spectrum",
                     "Energy in each band, 30 Hz to 8 kHz, output.");
        return;
    }
    if (over(scope))
    {
        hint.setHint({}, "Scope",
                     "The output waveform. The faint trace behind it is the input, so you can see what the plugin did.");
        return;
    }
    if (over(inputLevel))
    {
        hint.setHint({}, "Input level",
                     "Held peak after Trim, read as the level a direct bass arrived at. Trim at 0 "
                     "keeps a -24 dBFS bass in the green window, from -29 to -19 dB. Blue is colder "
                     "and red is hotter. The lamp means the signal came in within 1 dB of full "
                     "scale, before the trim.");
        return;
    }
    if (over(levelMeter))
    {
        hint.setHint({}, "Levels",
                     "Peak and RMS of the output on a 60 dB scale. The lamp lights when the output clips.");
        return;
    }

    for (int index = 0; index < static_cast<int>(cells.size()); ++index)
    {
        const auto* cell = cells[static_cast<std::size_t>(index)].get();
        if (cell == nullptr || !cell->isVisible())
            continue;
        if (!cell->hitsHint(cell->getLocalPoint(this, mouse)))
            continue;
        const int parameter = index + 1;
        hint.setHint({}, subdoom::parameterName(parameter), helpForParameter(parameter));
        return;
    }

    if (section(frontArea, "Front",
                "Input level, the leveler, the split, and the drive on each side of it."))
        return;
    if (section(shapeArea, "Shape",
                "Body EQ, the harmonic lift, the high cut, and the parallel grit."))
        return;
    if (section(outArea, "Out", "Room, master, the limiter, and the tuner mute."))
        return;
    if (section(tuner.getBounds(), "Tuner",
                "Listens to the dry bass before any drive. Green is in tune, red is more than "
                "seven cents off."))
        return;
    if (section(scope.getBounds(), "Scope",
                "The output waveform. The faint trace behind it is the input."))
        return;

    hint.setHint({}, {}, "Hover over a control for more details.");
}

void SubdoomAudioProcessorEditor::buildBackdrop()
{
    const int width = juce::jmax(1, getWidth());
    const int height = juce::jmax(1, getHeight());
    backdrop = juce::Image(juce::Image::RGB, width, height, true);

    // Soft colour behind everything: painted small and scaled up, so the blobs
    // read as light on the canvas rather than as shapes.
    const int lowWidth = juce::jmax(4, width / 5);
    const int lowHeight = juce::jmax(4, height / 5);
    juce::Image blobs(juce::Image::RGB, lowWidth, lowHeight, true);
    {
        juce::Graphics b(blobs);
        b.fillAll(sdui::ink());

        const auto blob = [&b](float cx, float cy, float radius, juce::Colour colour) {
            juce::ColourGradient gradient(colour, cx, cy, colour.withAlpha(0.0f),
                                          cx + radius, cy, true);
            b.setGradientFill(gradient);
            b.fillAll();
        };

        const auto w = static_cast<float>(lowWidth);
        const auto h = static_cast<float>(lowHeight);
        const float strength = sdui::darkMode() ? 0.92f : 1.0f;
        // Centres chosen so the wash sweeps under the cards, not just the edges.
        blob(w * 0.10f, h * 0.06f, w * 0.62f, sdui::amber().withAlpha(0.42f * strength));
        blob(w * 0.52f, h * 1.04f, w * 0.60f, sdui::green().withAlpha(0.36f * strength));
        blob(w * 1.02f, h * 0.40f, w * 0.56f, sdui::blue().withAlpha(0.32f * strength));
        blob(w * 0.45f, h * 0.48f, w * 0.85f, sdui::steel().withAlpha(0.10f * strength));
    }

    juce::Graphics g(backdrop);
    g.setImageResamplingQuality(juce::Graphics::mediumResamplingQuality);
    g.drawImage(blobs, 0, 0, width, height, 0, 0, lowWidth, lowHeight);

    // Cards: controls block, meter column, hint card.
    sdui::drawCard(g, controlsArea);
    sdui::drawCard(g, meterArea);
    sdui::drawCard(g, hint.getBounds());

    // The tuner and the scope each carry their own well, so no divider here.

    // Wordmark: a rust square and ink letters. The maker link under the name is a
    // child button, so it is not part of this cached image.
    g.setColour(sdui::amber());
    g.fillRect(wordmarkArea.getX(), wordmarkArea.getY() + 8, kWordmarkMark, kWordmarkMark);
    g.setColour(sdui::text());
    g.setFont(sdui::trackedFont(21.0f, 1.0f, true));
    g.drawText("SUBDOOM", wordmarkArea.getX() + kWordmarkMark + 10, wordmarkArea.getY() + 1,
               wordmarkArea.getWidth() - kWordmarkMark, 28, juce::Justification::centredLeft);

    sdui::drawPanel(g, frontArea, "FRONT", sdui::line());
    sdui::drawPanel(g, shapeArea, "SHAPE", sdui::line());
    sdui::drawPanel(g, outArea, "OUT", sdui::line());

    // A rule under each heading instead of a box around each group. The hairline
    // colour disappears against the card, so the rule is drawn in the secondary
    // text colour: present at a glance in both themes, still below the heading.
    g.setColour(sdui::dim().withAlpha(sdui::darkMode() ? 0.55f : 0.70f));
    for (const auto* group : { &frontArea, &shapeArea, &outArea })
        g.drawHorizontalLine(group->getY() + 13, static_cast<float>(group->getX()),
                             static_cast<float>(group->getRight()));
}

void SubdoomAudioProcessorEditor::paint(juce::Graphics& g)
{
    if (backdrop.isValid())
        g.drawImage(backdrop, 0, 0, getWidth(), getHeight(), 0, 0, backdrop.getWidth(),
                    backdrop.getHeight());
    else
        g.fillAll(sdui::ink());

    if (overlay != nullptr)
    {
        // A scrim darkens in both themes, so it is not a palette colour.
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.fillRect(getLocalBounds());
    }
}

void SubdoomAudioProcessorEditor::openOverlay(juce::Component& overlayToShow)
{
    // Sheets come up and go away at once. The fade was long enough to feel like
    // a wait, and the scrim already says the page is blocked.
    if (overlay == &overlayToShow)
        return;

    if (overlay != nullptr)
        overlay->setVisible(false);

    overlay = &overlayToShow;
    overlay->setAlpha(1.0f);
    overlay->setVisible(true);
    overlay->toFront(false);
    repaint();
}

void SubdoomAudioProcessorEditor::closeOverlay()
{
    if (overlay == nullptr)
        return;
    overlay->setVisible(false);
    overlay = nullptr;
    repaint();
}

void SubdoomAudioProcessorEditor::mouseDown(const juce::MouseEvent& event)
{
    if (overlay != nullptr && !overlay->getBounds().contains(event.getPosition()))
        closeOverlay();
}

bool SubdoomAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    if (overlay == &browser && browser.handleOverlayKey(key))
        return true;

    if (overlay != nullptr
        && (key == juce::KeyPress::escapeKey || key == juce::KeyPress::returnKey))
    {
        closeOverlay();
        return true;
    }
    return false;
}

void SubdoomAudioProcessorEditor::refreshPresetBar()
{
    const juce::String name = audioProcessor.currentPresetName();
    const bool dirty = audioProcessor.presetIsDirty();
    const bool factory = audioProcessor.currentPresetIsFactory();

    if (name != shownPresetName || dirty != shownDirty || factory != shownFactory)
    {
        shownPresetName = name;
        shownDirty = dirty;
        shownFactory = factory;
        presetBar.setButtonText(dirty ? name + "  *" : name);
        saveButton.setEnabled(audioProcessor.saveIsAvailable());
        revertButton.setEnabled(dirty);
    }

    const bool compareB = audioProcessor.compareSlot() == 1;
    if (compareB != shownCompare)
    {
        shownCompare = compareB;
        compareButton.setButtonText(compareB ? "B" : "A");
        compareButton.setToggleState(compareB, juce::dontSendNotification);
    }

    const int learning = audioProcessor.learnTarget();
    midiButton.setToggleState(learning >= 0, juce::dontSendNotification);
    const auto paintStep = [](juce::TextButton& button, bool armed) {
        button.setColour(juce::TextButton::buttonColourId, armed ? sdui::amber() : sdui::panel());
    };
    paintStep(prevButton, learning == SubdoomAudioProcessor::kPresetPrevTarget);
    paintStep(nextButton, learning == SubdoomAudioProcessor::kPresetNextTarget);
}

void SubdoomAudioProcessorEditor::timerCallback()
{
    audioProcessor.servicePresetHold();

    // Rebuilt at most once a frame instead of once per resize event.
    if (backdropDirty)
    {
        backdropDirty = false;
        buildBackdrop();
        repaint();
    }

    // Pitch analysis is the heaviest thing the editor does; a 10 Hz readout is
    // plenty for a tuner and takes two thirds of the cost out of the message thread.
    if (++tunerTick >= 3)
    {
        tunerTick = 0;
        audioProcessor.engine().pollTuner();
        tuner.repaint();
    }

    scope.repaint();
    spectrum.repaint();
    inputLevel.tick();
    levelMeter.tick();

    for (auto* knob : knobCells)
        knob->tick();
    for (auto* toggle : switchCells)
        toggle->tick();

    const int activity = audioProcessor.midiActivity();
    if (activity != shownActivity)
    {
        shownActivity = activity;
        midiLed.flash();
    }
    midiLed.tick();

    refreshPresetBar();
    updateHint();
    if (browser.isVisible())
        browser.pollHoverHint();

    if (midiPage.isVisible())
    {
        midiPage.pollHoverHint();

        // Refresh only when a mapping, the learn target or MIDI traffic changed.
        // Refreshing every frame repainted the whole sheet 30 times a second.
        const int learn = audioProcessor.learnTarget();
        if (learn != shownLearn || activity != shownMidiActivity)
        {
            shownLearn = learn;
            shownMidiActivity = activity;
            midiPage.refresh();
        }
    }
}

void SubdoomAudioProcessorEditor::parentHierarchyChanged()
{
    if (audioProcessor.wrapperType != juce::AudioProcessor::wrapperType_Standalone)
        return;
    if (fittedToScreen || getPeer() == nullptr)
        return;
    fittedToScreen = true;

    auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(getScreenBounds());
    if (display == nullptr)
        return;

    const auto desk = display->userBounds.toNearestInt();
    if (desk.isEmpty())
        return;

    // The standalone wrapper owns the window: a title bar, its notification
    // strip, and the Options button sit outside the editor. Leave room for that
    // chrome and let the wrapper size the window from the editor instead of
    // setting peer bounds here, which would clip the bottom of the editor.
    constexpr int chrome = 130;
    const int availableWidth = juce::jmax(960, desk.getWidth() - chrome);
    const int availableHeight = juce::jmax(600, desk.getHeight() - chrome);
    const int nextWidth = juce::jmin(getWidth(), availableWidth);
    const int nextHeight = juce::jmin(getHeight(), availableHeight);
    if (nextWidth != getWidth() || nextHeight != getHeight())
        setSize(nextWidth, nextHeight);
}
