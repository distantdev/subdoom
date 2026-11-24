#pragma once

#include "subdoom/MathConstants.h"

namespace subdoom {

struct BiquadCoeffs
{
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

struct Biquad
{
    double x1 = 0.0;
    double x2 = 0.0;
    double y1 = 0.0;
    double y2 = 0.0;

    void reset() noexcept
    {
        x1 = x2 = y1 = y2 = 0.0;
    }

    void heal() noexcept
    {
        if (!std::isfinite(y1) || !std::isfinite(y2) || std::abs(y1) > 8.0 || std::abs(y2) > 8.0)
            reset();
    }

    double process(double x, const BiquadCoeffs& c) noexcept
    {
        const double y = c.b0 * x + c.b1 * x1 + c.b2 * x2 - c.a1 * y1 - c.a2 * y2 + kDenorm;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

inline BiquadCoeffs makePeaking(double fc, double q, double gainDb, double sampleRate) noexcept
{
    const double rate = std::max(8000.0, sampleRate);
    const double freq = clamp(fc, 10.0, rate * 0.45);
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * freq / rate;
    const double cosw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * std::max(0.2, q));
    const double a0Inv = 1.0 / (1.0 + alpha / A);
    BiquadCoeffs c;
    c.b0 = (1.0 + alpha * A) * a0Inv;
    c.b1 = (-2.0 * cosw) * a0Inv;
    c.b2 = (1.0 - alpha * A) * a0Inv;
    c.a1 = c.b1;
    c.a2 = (1.0 - alpha / A) * a0Inv;
    return c;
}

inline BiquadCoeffs makeBandpass(double fc, double q, double sampleRate) noexcept
{
    const double rate = std::max(8000.0, sampleRate);
    const double freq = clamp(fc, 10.0, rate * 0.45);
    const double w0 = 2.0 * kPi * freq / rate;
    const double cosw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * clamp(q, 0.2, 18.0));
    const double a0Inv = 1.0 / (1.0 + alpha);
    BiquadCoeffs c;
    c.b0 = alpha * a0Inv;
    c.b1 = 0.0;
    c.b2 = -c.b0;
    c.a1 = (-2.0 * cosw) * a0Inv;
    c.a2 = (1.0 - alpha) * a0Inv;
    return c;
}

inline BiquadCoeffs makeHighpass(double fc, double q, double sampleRate) noexcept
{
    const double rate = std::max(8000.0, sampleRate);
    const double freq = clamp(fc, 10.0, rate * 0.45);
    const double w0 = 2.0 * kPi * freq / rate;
    const double cosw = std::cos(w0);
    const double sinw = std::sin(w0);
    const double alpha = sinw / (2.0 * std::max(0.2, q));
    const double a0Inv = 1.0 / (1.0 + alpha);
    BiquadCoeffs c;
    c.b1 = -(1.0 + cosw) * a0Inv;
    c.b0 = c.b1 * -0.5;
    c.b2 = c.b0;
    c.a1 = (-2.0 * cosw) * a0Inv;
    c.a2 = (1.0 - alpha) * a0Inv;
    return c;
}

inline BiquadCoeffs makeLowpass(double fc, double q, double sampleRate) noexcept
{
    const double rate = std::max(8000.0, sampleRate);
    const double freq = clamp(fc, 10.0, rate * 0.45);
    const double w0 = 2.0 * kPi * freq / rate;
    const double cosw = std::cos(w0);
    const double sinw = std::sin(w0);
    const double alpha = sinw / (2.0 * std::max(0.2, q));
    const double a0Inv = 1.0 / (1.0 + alpha);
    BiquadCoeffs c;
    c.b1 = (1.0 - cosw) * a0Inv;
    c.b0 = c.b1 * 0.5;
    c.b2 = c.b0;
    c.a1 = (-2.0 * cosw) * a0Inv;
    c.a2 = (1.0 - alpha) * a0Inv;
    return c;
}

} // namespace subdoom
