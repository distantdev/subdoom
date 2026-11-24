#pragma once

#include "subdoom/MathConstants.h"

namespace subdoom {

// Slow open, slower close. Off leaves the signal alone.
// The hold bridges zero crossings so a low note is not chopped twice a cycle.
struct NoiseGate
{
    double hold = 0.0;
    double env = 1.0;

    void reset(bool enabled) noexcept
    {
        hold = 0.0;
        env = enabled ? 0.0 : 1.0;
    }

    double process(double x, bool enabled, double attack, double release, double holdRelease, double openCoeff) noexcept
    {
        const double peak = std::abs(x);
        if (peak > hold)
            hold = peak;
        else
            hold += holdRelease * (peak - hold);

        if (!enabled)
        {
            env += openCoeff * (1.0 - env);
            return x * env;
        }

        const double target = hold > 0.0025 ? 1.0 : 0.0;
        const double coeff = target > env ? attack : release;
        env += coeff * (target - env);
        return x * env;
    }
};

// Evens a long note. The envelope is slow enough that a pick still gets through.
// A steady -12 dBFS sine sits on the target and stays near unity.
struct SlowLeveler
{
    double hold = 0.0;
    double env = 0.0;

    void reset() noexcept
    {
        hold = 0.0;
        env = 0.0;
    }

    double process(double x, double sustain, double attack, double release, double holdRelease) noexcept
    {
        const double peak = std::abs(x);
        if (peak > hold)
            hold = peak;
        else
            hold += holdRelease * (peak - hold);

        const double coeff = hold > env ? attack : release;
        env += coeff * (hold - env);

        const double depth = clamp(sustain, 0.0, 1.0) * 0.42;
        const double sense = std::max(env, 0.008);
        double gain = std::pow(0.25 / sense, depth);
        gain = clamp(gain, 0.55, 1.85);
        const double open = clamp(env / 0.004, 0.0, 1.0);
        gain = 1.0 + (gain - 1.0) * open;
        return x * gain;
    }
};

// Ducks the high path after a loud note. The low path is not its detector.
struct HighSag
{
    double env = 0.0;

    void reset() noexcept { env = 0.0; }

    double gain(double detector, double amount, double attack, double release) noexcept
    {
        const double peak = std::abs(detector);
        const double coeff = peak > env ? attack : release;
        env += coeff * (peak - env);
        return 1.0 / (1.0 + clamp(amount, 0.0, 1.0) * 2.8 * env);
    }
};

struct OutputLimiter
{
    double gain = 1.0;

    void reset() noexcept { gain = 1.0; }

    double process(double x, bool enabled, double attack, double release) noexcept
    {
        constexpr double ceiling = 0.8912509381337456; // -1 dBFS
        if (!enabled)
        {
            gain += release * (1.0 - gain);
            return x;
        }
        const double peak = std::abs(x);
        const double target = peak > ceiling ? (ceiling / peak) : 1.0;
        const double coeff = target < gain ? attack : release;
        gain += coeff * (target - gain);
        x *= gain;
        return clamp(x, -ceiling, ceiling);
    }
};

} // namespace subdoom
