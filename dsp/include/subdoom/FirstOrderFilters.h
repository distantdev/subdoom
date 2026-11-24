#pragma once

#include "subdoom/MathConstants.h"

namespace subdoom {

inline double rcLpPole(double fc, double sampleRate) noexcept
{
    const double rate = std::max(8000.0, sampleRate);
    const double freq = clamp(fc, 1.0, rate * 0.45);
    return std::exp(-2.0 * kPi * freq / rate);
}

inline double rcHpCoeff(double fc, double sampleRate) noexcept
{
    const double rate = std::max(8000.0, sampleRate);
    const double freq = clamp(fc, 1.0, rate * 0.45);
    return 1.0 / (1.0 + 2.0 * kPi * freq / rate);
}

struct OnePoleLp
{
    double z = 0.0;

    void reset() noexcept { z = 0.0; }

    double process(double x, double a0, double b1) noexcept
    {
        z = a0 * x + b1 * z + kDenorm;
        return z;
    }
};

struct OnePoleHp
{
    double y = 0.0;
    double prev = 0.0;

    void reset() noexcept
    {
        y = 0.0;
        prev = 0.0;
    }

    double process(double x, double a) noexcept
    {
        y = a * (y + x - prev) + kDenorm;
        prev = x;
        return y;
    }
};

} // namespace subdoom
