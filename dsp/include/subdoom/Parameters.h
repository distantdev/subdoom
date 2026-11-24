#pragma once

#include "subdoom/MathConstants.h"

#include <string>

namespace subdoom {

struct ParameterRange
{
    double min = 0.0;
    double max = 1.0;
    double step = 0.01;
};

// Host order is field order, index 1..kParameterCount. Preset choice is not a field.
struct Parameters
{
    double inputTrim = 0.0;
    double gate = 0.0;
    double sustain = 0.45;
    double splitHz = 100.0;
    double sag = 0.15;
    double lowDrive = 0.15;
    double lowDb = 0.0;
    double highDrive = 0.45;
    double highDb = 0.0;
    double octave = 0.0;
    double thumpDb = 3.0;
    double harmDb = 0.0;
    double growlDb = 1.0;
    double honkDb = 0.0;
    double highCutHz = 2400.0;
    double grit = 0.0;
    double roomMix = 0.08;
    double roomDecay = 0.35;
    double masterDb = -6.0;
    double limiter = 1.0;
    double tunerMute = 0.0;

    double& at(int index) noexcept;
    double at(int index) const noexcept;
    void clampIntoRange() noexcept;
};

inline double Parameters::* const kParameterFields[kParameterCount + 1] = {
    nullptr,
    &Parameters::inputTrim,
    &Parameters::gate,
    &Parameters::sustain,
    &Parameters::splitHz,
    &Parameters::sag,
    &Parameters::lowDrive,
    &Parameters::lowDb,
    &Parameters::highDrive,
    &Parameters::highDb,
    &Parameters::octave,
    &Parameters::thumpDb,
    &Parameters::harmDb,
    &Parameters::growlDb,
    &Parameters::honkDb,
    &Parameters::highCutHz,
    &Parameters::grit,
    &Parameters::roomMix,
    &Parameters::roomDecay,
    &Parameters::masterDb,
    &Parameters::limiter,
    &Parameters::tunerMute,
};

struct ParameterSpec
{
    const char* id;
    const char* name;
    const char* unit;
    ParameterRange range;
};

// One table for everything the host, the preset file, the readouts and the tests
// need from a parameter. Index is the host index 1..kParameterCount.
inline const ParameterSpec kParameterSpecs[kParameterCount] = {
    { "inputTrim", "Trim", "dB", { -24.0, 12.0, 0.1 } },
    { "gate", "Gate", "", { 0.0, 1.0, 1.0 } },
    { "sustain", "Sustain", "", { 0.0, 1.0, 0.01 } },
    { "splitHz", "Split", "Hz", { 70.0, 180.0, 1.0 } },
    { "sag", "Sag", "", { 0.0, 1.0, 0.01 } },
    { "lowDrive", "Low Drive", "", { 0.0, 1.0, 0.01 } },
    { "lowDb", "Low", "dB", { -12.0, 12.0, 0.1 } },
    { "highDrive", "High Drive", "", { 0.0, 1.0, 0.01 } },
    { "highDb", "High", "dB", { -12.0, 12.0, 0.1 } },
    { "octave", "Octave", "", { 0.0, 1.0, 0.01 } },
    { "thumpDb", "Thump", "dB", { -8.0, 12.0, 0.1 } },
    { "harmDb", "Harmonics", "dB", { 0.0, 12.0, 0.1 } },
    { "growlDb", "Growl", "dB", { -10.0, 10.0, 0.1 } },
    { "honkDb", "Honk", "dB", { -10.0, 8.0, 0.1 } },
    { "highCutHz", "Cut", "Hz", { 700.0, 6500.0, 10.0 } },
    { "grit", "Grit", "", { 0.0, 1.0, 0.01 } },
    { "roomMix", "Room", "", { 0.0, 1.0, 0.01 } },
    { "roomDecay", "Decay", "", { 0.0, 1.0, 0.01 } },
    { "masterDb", "Master", "dB", { -24.0, 6.0, 0.1 } },
    { "limiter", "Limit", "", { 0.0, 1.0, 1.0 } },
    { "tunerMute", "Mute", "", { 0.0, 1.0, 1.0 } },
};

inline const ParameterSpec& parameterSpec(int index) noexcept
{
    return kParameterSpecs[(index >= 1 && index <= kParameterCount) ? index - 1 : 0];
}

inline ParameterRange parameterRange(int index) noexcept { return parameterSpec(index).range; }
inline const char* parameterId(int index) noexcept { return parameterSpec(index).id; }
inline const char* parameterName(int index) noexcept { return parameterSpec(index).name; }
inline const char* parameterUnit(int index) noexcept { return parameterSpec(index).unit; }

inline double& Parameters::at(int index) noexcept
{
    return this->*kParameterFields[(index >= 1 && index <= kParameterCount) ? index : 1];
}

inline double Parameters::at(int index) const noexcept
{
    return this->*kParameterFields[(index >= 1 && index <= kParameterCount) ? index : 1];
}

inline void Parameters::clampIntoRange() noexcept
{
    for (int i = 1; i <= kParameterCount; ++i)
    {
        const ParameterRange range = parameterRange(i);
        double value = clamp(at(i), range.min, range.max);
        if (range.step >= 1.0)
            value = std::floor(value + 0.5);
        at(i) = value;
    }
}

inline bool parametersMatch(const Parameters& left, const Parameters& right) noexcept
{
    for (int i = 1; i <= kParameterCount; ++i)
    {
        const double step = std::max(1.0e-6, parameterRange(i).step);
        if (std::abs(left.at(i) - right.at(i)) > step * 0.51)
            return false;
    }
    return true;
}

// Boolean parameters. Not "any parameter whose step is 1": Split and Cut are
// whole numbers too, and treating them as switches breaks their readouts and
// makes a MIDI CC flip them instead of setting a value.
inline bool parameterIsSwitch(int index) noexcept
{
    return index == 2 || index == 20 || index == 21;
}

inline double tailSeconds(const Parameters& params) noexcept
{
    if (params.roomMix <= 0.001)
        return 0.05;
    return std::min(3.0, 0.25 + clamp(params.roomDecay, 0.0, 1.0) * 2.75);
}

std::string presetName(int index);
const char* presetCategory(int index);
Parameters factoryPreset(int index);

} // namespace subdoom
