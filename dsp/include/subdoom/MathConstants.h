#pragma once

#include <algorithm>
#include <cmath>

namespace subdoom {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kDenorm = 1.0e-24;
inline constexpr int kParameterCount = 21;
inline constexpr int kFactoryPresetCount = 10;

inline double clamp(double value, double lo, double hi) noexcept
{
    return std::max(lo, std::min(hi, value));
}

inline double dbToGain(double db) noexcept
{
    return std::pow(10.0, db / 20.0);
}

inline double gainToDb(double gain) noexcept
{
    return 20.0 * std::log10(std::max(gain, 1.0e-12));
}

inline int asInt(double value) noexcept
{
    return static_cast<int>(std::floor(value + 0.5));
}

inline bool asOn(double value) noexcept
{
    return value > 0.5;
}

} // namespace subdoom
