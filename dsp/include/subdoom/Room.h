#pragma once

#include "subdoom/FirstOrderFilters.h"

#include <vector>

namespace subdoom {

// One short mono room. Four damped combs into an allpass. No modes, no shimmer.
class Room
{
public:
    void prepare(double sampleRate)
    {
        sampleRate = std::max(8000.0, sampleRate);
        const double scale = sampleRate / 48000.0;
        const int seeds[kCombs] = { 1321, 1657, 2011, 2371 };
        for (int i = 0; i < kCombs; ++i)
        {
            int length = static_cast<int>(std::lround(static_cast<double>(seeds[i]) * scale));
            length = std::max(32, std::min(length, kMaxDelay - 1));
            combs[static_cast<std::size_t>(i)].assign(length);
        }
        int allpassLength = static_cast<int>(std::lround(347.0 * scale));
        allpassLength = std::max(16, std::min(allpassLength, kMaxDelay - 1));
        allpass.assign(allpassLength);
        hpCoeff = rcHpCoeff(160.0, sampleRate);
    }

    void reset() noexcept
    {
        for (auto& comb : combs)
            comb.clear();
        allpass.clear();
        wetHp.reset();
    }

    double process(double dry, double mix, double decay) noexcept
    {
        const double feedback = clamp(0.30 + clamp(decay, 0.0, 1.0) * 0.60, 0.0, 0.90);
        double wet = 0.0;
        for (auto& comb : combs)
            wet += comb.push(dry, feedback);
        wet *= 0.25;
        wet = allpass.push(wet);
        wet = wetHp.process(wet, hpCoeff);
        return dry + wet * clamp(mix, 0.0, 1.0) * 0.55;
    }

private:
    static constexpr int kCombs = 4;
    static constexpr int kMaxDelay = 16384;

    struct Line
    {
        std::vector<double> buf;
        int index = 0;
        double damp = 0.0;

        void assign(int length)
        {
            if (static_cast<int>(buf.size()) != length)
                buf.assign(static_cast<std::size_t>(length), 0.0);
            clear();
        }

        void clear() noexcept
        {
            for (auto& sample : buf)
                sample = 0.0;
            index = 0;
            damp = 0.0;
        }

        double push(double input, double feedback) noexcept
        {
            if (buf.empty())
                return 0.0;
            const double delayed = buf[static_cast<std::size_t>(index)];
            damp += 0.28 * (delayed - damp);
            buf[static_cast<std::size_t>(index)] = input + damp * feedback + kDenorm;
            index += 1;
            if (index >= static_cast<int>(buf.size()))
                index = 0;
            return delayed;
        }

        double push(double input) noexcept
        {
            if (buf.empty())
                return input;
            constexpr double g = 0.62;
            const double delayed = buf[static_cast<std::size_t>(index)];
            const double y = -g * input + delayed;
            buf[static_cast<std::size_t>(index)] = input + g * y + kDenorm;
            index += 1;
            if (index >= static_cast<int>(buf.size()))
                index = 0;
            return y;
        }
    };

    Line combs[kCombs];
    Line allpass;
    OnePoleHp wetHp;
    double hpCoeff = 0.98;
};

} // namespace subdoom
