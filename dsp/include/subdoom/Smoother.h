#pragma once

#include <algorithm>
#include <cmath>

namespace subdoom {

struct OnePoleChase
{
    double value = 0.0;
    double coeff = 1.0;

    static double coeffFor(double seconds, double sampleRate) noexcept
    {
        const double samples = std::max(1.0, seconds * std::max(8000.0, sampleRate));
        return 1.0 - std::exp(-1.0 / samples);
    }

    void setCoeff(double next) noexcept { coeff = next; }

    void snap(double target) noexcept { value = target; }

    double next(double target) noexcept
    {
        value += coeff * (target - value);
        return value;
    }
};

} // namespace subdoom
