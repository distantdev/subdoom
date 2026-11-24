#include "subdoom/Parameters.h"

namespace subdoom {
namespace {

struct Voice
{
    const char* name;
    double sustain;
    double splitHz;
    double lowDb;
    double lowDrive;
    double highDrive;
    double highDb;
    double octave;
    double sag;
    double thumpDb;
    double harmDb;
    double growlDb;
    double honkDb;
    double highCutHz;
    double grit;
    double roomMix;
    double roomDecay;
    double masterDb;
};

// Master is voiced so the ten presets land on the same perceived level, measured
// as RMS on a held bass note and on a repeated pluck (see --loudness in the test
// CLI). Peak still has to stay inside the loudness window on a C1 sine.
// Drive is voiced against kLowDriveMax / kHighDriveMax. Past the high-drive
// knee the harmonic clip keeps climbing; Funeral Wall, Dragon Lung, and
// Caveman sit in that range. The low path is what squares the fundamental.
const Voice kVoices[kFactoryPresetCount] = {
    { "LOW MASS", 0.84, 100.0, 1.5, 0.10, 0.36, 0.0, 0.0, 0.21, 5.5, 0.0, 3.0, -1.0, 1600.0, 0.0, 0.08, 0.42, -1.1 },
    { "WOOL HYMN", 0.48, 108.0, 1.0, 0.12, 0.30, 0.5, 0.0, 0.08, 2.0, 0.0, 4.5, 1.0, 3500.0, 0.22, 0.06, 0.30, 1.7 },
    { "RED SUN", 0.26, 118.0, 0.5, 0.05, 0.26, 1.0, 0.0, 0.06, 3.5, 0.0, -2.0, 5.5, 4000.0, 0.08, 0.04, 0.22, 2.6 },
    { "FUNERAL WALL", 0.68, 88.0, 0.0, 0.82, 0.88, -1.0, 0.0, 0.41, 4.5, 0.0, 2.0, -2.5, 1200.0, 0.48, 0.0, 0.20, -3.6 },
    { "DRAGON LUNG", 0.52, 82.0, 1.0, 0.58, 0.96, -0.5, 0.0, 0.32, 9.5, 0.0, 1.0, 4.0, 2200.0, 0.58, 0.0, 0.18, -7.1 },
    { "SERPENT", 0.74, 128.0, 0.0, 0.14, 0.60, 0.5, 0.0, 0.06, 2.0, 0.0, 2.0, 5.0, 4500.0, 0.36, 0.05, 0.26, -1.2 },
    { "GIZA", 0.90, 94.0, 2.0, 0.04, 0.16, -1.0, 0.18, 0.05, 7.5, 0.0, -3.5, -1.0, 2000.0, 0.0, 0.14, 0.58, -1.5 },
    { "MARROW", 0.82, 104.0, 1.0, 0.26, 0.40, 0.5, 0.0, 0.13, 4.0, 0.0, 2.0, 3.0, 3400.0, 0.0, 0.16, 0.50, -0.6 },
    { "EMPRESS", 0.62, 100.0, 2.0, 0.08, 0.54, 1.0, 0.0, 0.20, 5.0, 0.0, 3.0, 0.5, 1800.0, 0.0, 0.10, 0.38, -3.1 },
    { "CAVEMAN", 0.64, 78.0, 1.5, 0.74, 0.92, -2.0, 0.0, 0.37, 12.0, 0.0, -1.0, -6.0, 900.0, 0.12, 0.0, 0.15, -8.2 },
};

Parameters fromVoice(const Voice& voice)
{
    Parameters params;
    params.inputTrim = 0.0;
    params.gate = 0.0;
    params.sustain = voice.sustain;
    params.splitHz = voice.splitHz;
    params.lowDb = voice.lowDb;
    params.lowDrive = voice.lowDrive;
    params.highDrive = voice.highDrive;
    params.highDb = voice.highDb;
    params.octave = voice.octave;
    params.sag = voice.sag;
    params.thumpDb = voice.thumpDb;
    params.harmDb = voice.harmDb;
    params.growlDb = voice.growlDb;
    params.honkDb = voice.honkDb;
    params.highCutHz = voice.highCutHz;
    params.grit = voice.grit;
    params.roomMix = voice.roomMix;
    params.roomDecay = voice.roomDecay;
    params.masterDb = voice.masterDb;
    params.limiter = 1.0;
    params.tunerMute = 0.0;
    params.clampIntoRange();
    return params;
}

// Browsing only. The name is the tone; the category groups it for the browser.
const char* kCategories[kFactoryPresetCount] = {
    "Drone", "Doom", "Stoner", "Sludge", "Sludge",
    "Stoner", "Drone", "Doom", "Doom", "Sludge",
};

} // namespace

std::string presetName(int index)
{
    if (index < 0 || index >= kFactoryPresetCount)
        return {};
    return kVoices[index].name;
}

const char* presetCategory(int index)
{
    if (index < 0 || index >= kFactoryPresetCount)
        return "Doom";
    return kCategories[index];
}

Parameters factoryPreset(int index)
{
    if (index < 0 || index >= kFactoryPresetCount)
        return fromVoice(kVoices[0]);
    return fromVoice(kVoices[index]);
}

} // namespace subdoom
