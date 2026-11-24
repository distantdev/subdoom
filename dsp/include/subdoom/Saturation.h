#pragma once

#include "subdoom/MathConstants.h"

namespace subdoom {

// Monotonic asymptotic saturator. Hard-clamps past the wavefold region.
inline double saturate(double x) noexcept
{
    if (x > 4.5)
        return 1.0;
    if (x < -4.5)
        return -1.0;
    const double x2 = x * x;
    return x * (27.0 + x2) / (27.0 + 9.0 * x2);
}

// Symmetric slam. Flattens sooner than saturate and stays finite.
inline double hardClip(double x) noexcept
{
    if (x > 1.0)
        return 1.0;
    if (x < -1.0)
        return -1.0;
    return x;
}

// Asymmetric clipper. Positive peaks fold sooner than negative ones.
// asym is fixed by the caller. It is not a parameter.
inline double asymClip(double x, double asym) noexcept
{
    const double posGain = 1.0 + asym * 0.55;
    const double negGain = 1.0 - asym * 0.35;
    if (x >= 0.0)
        return saturate(x * posGain) / posGain;
    return saturate(x * negGain) / negGain;
}

// Gain into a clipper for a 0..1 drive control: 1.0 leaves the signal alone,
// maxPush is the gain at the top of the knob. Squared so the low half of the
// range stays usable instead of jumping straight to fuzz.
inline double driveGain(double amount, double maxPush) noexcept
{
    const double a = clamp(amount, 0.0, 1.0);
    return 1.0 + a * a * (std::max(1.0, maxPush) - 1.0);
}

// What the clipper does to a signal at the reference level. Dividing by this
// keeps a drive knob about tone rather than volume: signals below the reference
// get lifted, signals above it get squashed, which is what a fuzz stage does.
// Without it, dividing by the drive gain cancels the drive.
inline double saturateCompensation(double gain, double reference) noexcept
{
    return clamp(saturate(reference * gain) / reference, 0.02, 24.0);
}

inline double asymCompensation(double gain, double asym, double reference) noexcept
{
    return clamp(asymClip(reference * gain, asym) / reference, 0.02, 24.0);
}

} // namespace subdoom
