#pragma once

#include "subdoom/BiquadFilter.h"
#include "subdoom/FirstOrderFilters.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace subdoom {

struct TunerReadout
{
    bool active = false;
    double hz = 0.0;
    double cents = 0.0;
    int midiNote = -1;
    int noteIndex = -1;
    int octave = 0;
};

inline const char* noteName(int index) noexcept
{
    switch (index)
    {
        case 0: return "C";
        case 1: return "C#";
        case 2: return "D";
        case 3: return "D#";
        case 4: return "E";
        case 5: return "F";
        case 6: return "F#";
        case 7: return "G";
        case 8: return "G#";
        case 9: return "A";
        case 10: return "A#";
        case 11: return "B";
        default: return "--";
    }
}

// The audio thread only decimates. analyse() runs from the UI timer or a test.
class Tuner
{
public:
    static constexpr int kRing = 4096;
    static constexpr int kWindow = 896;
    static constexpr int kMaxTau = 560;

    // Written by the audio thread, read by analyse() on the message thread. Relaxed
    // atomics make that read well defined; a pitch window tolerates a sample that is
    // one push old, so no stronger ordering is needed.
    using SharedSample = std::atomic<double>;
    using Ring = std::array<SharedSample, kRing>;
    static_assert(std::atomic<double>::is_always_lock_free,
                  "the tuner ring must not take a lock on the audio thread");

    static_assert(kWindow >= 830, "low-note window covers two periods of A#0");
    static_assert(kWindow + kMaxTau < kRing, "analysis span fits in the ring");
    static_assert((kRing & (kRing - 1)) == 0, "ring length stays a power of two");

    void reset() noexcept
    {
        fullHp.reset();
        fullLp.reset();
        lowHp.reset();
        lowLpA.reset();
        lowLpB.reset();
        for (auto& sample : fullRing)
            sample.store(0.0, std::memory_order_relaxed);
        for (auto& sample : lowRing)
            sample.store(0.0, std::memory_order_relaxed);
        centsHist.fill(0.0);
        write.store(0, std::memory_order_relaxed);
        decimCount = 0;
        filled.store(0, std::memory_order_relaxed);
        env.store(0.0, std::memory_order_relaxed);
        readout = {};
        candidate = -1;
        candidateCount = 0;
        histIndex = 0;
        histCount = 0;
        silenceCount = 0;
        smoothCents = 0.0;
        preparedRate = 0.0;
        decimatedRate = 12000.0;
        decim = 4;
    }

    void setSampleRate(double sampleRate) noexcept
    {
        const double rate = std::max(8000.0, sampleRate);
        if (std::abs(rate - preparedRate) < 0.5)
            return;
        preparedRate = rate;
        decim = std::max(1, static_cast<int>(std::lround(rate / 12000.0)));
        decimatedRate = rate / static_cast<double>(decim);
        const BiquadCoeffs fullCut = makeLowpass(1600.0, 0.7071, rate);
        fullLpCoeff = fullCut;
        const BiquadCoeffs lowCut = makeLowpass(90.0, 0.7071, rate);
        lowLpCoeff = lowCut;
        fullHpA = rcHpCoeff(15.0, rate);
        lowHpA = rcHpCoeff(15.0, rate);
        fullHp.reset();
        fullLp.reset();
        lowHp.reset();
        lowLpA.reset();
        lowLpB.reset();
    }

    void feed(double mono) noexcept
    {
        if (preparedRate < 1000.0)
            setSampleRate(48000.0);
        const double full = fullLp.process(fullHp.process(mono, fullHpA), fullLpCoeff);
        const double lowOnce = lowLpA.process(lowHp.process(mono, lowHpA), lowLpCoeff);
        const double low = lowLpB.process(lowOnce, lowLpCoeff);
        env.store(env.load(std::memory_order_relaxed) * 0.9992 + std::abs(mono) * 0.0008,
                  std::memory_order_relaxed);
        if (++decimCount < decim)
            return;
        decimCount = 0;
        const int at = write.load(std::memory_order_relaxed);
        fullRing[static_cast<std::size_t>(at)].store(full, std::memory_order_relaxed);
        lowRing[static_cast<std::size_t>(at)].store(low, std::memory_order_relaxed);
        write.store((at + 1) & (kRing - 1), std::memory_order_relaxed);
        const int count = filled.load(std::memory_order_relaxed);
        if (count < kRing)
            filled.store(count + 1, std::memory_order_relaxed);
    }

    double envelope() const noexcept { return env.load(std::memory_order_relaxed); }

    TunerReadout reading() const noexcept
    {
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const uint32_t ticket = sequence.load(std::memory_order_acquire);
            if ((ticket & 1u) != 0u)
                continue;
            const TunerReadout copy = readout;
            if (sequence.load(std::memory_order_acquire) == ticket)
                return copy;
        }
        return {};
    }

    void noteSilence() noexcept
    {
        sequence.fetch_add(1u, std::memory_order_release);
        // The editor polls analysis at 10 Hz, so four calls is the old ~0.4 s hold.
        if (++silenceCount > 4)
        {
            readout.active = false;
            readout.midiNote = -1;
            readout.noteIndex = -1;
            readout.cents = 0.0;
            candidate = -1;
            candidateCount = 0;
            smoothCents = 0.0;
        }
        sequence.fetch_add(1u, std::memory_order_release);
    }

    void analyse() noexcept
    {
        sequence.fetch_add(1u, std::memory_order_release);
        silenceCount = 0;
        if (filled.load(std::memory_order_relaxed) < kWindow + kMaxTau)
        {
            sequence.fetch_add(1u, std::memory_order_release);
            return;
        }

        const int origin = wrap(write.load(std::memory_order_relaxed) - kWindow - kMaxTau);
        const double lowMean = meanAbs(lowRing, origin, kWindow);
        const double fullMean = meanAbs(fullRing, origin, kWindow);
        const auto lowHit = search(lowRing, origin, tauSamples(120.0), tauSamples(25.0), 0.22);
        const auto fullHit = search(fullRing, origin, tauSamples(500.0), tauSamples(25.0), 0.20);

        const bool lowLoud = lowMean > 0.008 && lowMean > fullMean * 0.35;
        const bool lowOk = lowHit.ok && lowLoud && lowHit.hz >= 25.0 && lowHit.hz <= 120.0;
        const bool fullIsOctave = lowOk && fullHit.ok && std::abs(fullHit.hz - lowHit.hz * 2.0) < lowHit.hz * 0.08;
        const bool fullAgrees = lowOk && fullHit.ok && std::abs(fullHit.hz - lowHit.hz) < lowHit.hz * 0.04;

        PitchHit chosen = fullHit;
        if (lowOk && (!fullHit.ok || fullIsOctave || fullAgrees))
            chosen = lowHit;
        if (!chosen.ok || chosen.hz <= 25.0 || chosen.hz >= 500.0)
        {
            sequence.fetch_add(1u, std::memory_order_release);
            return;
        }

        publish(chosen.hz);
        sequence.fetch_add(1u, std::memory_order_release);
    }

private:
    struct PitchHit
    {
        bool ok = false;
        double hz = 0.0;
    };

    int wrap(int index) const noexcept
    {
        int wrapped = index % kRing;
        if (wrapped < 0)
            wrapped += kRing;
        return wrapped;
    }

    int tauSamples(double hz) const noexcept
    {
        if (decimatedRate < 1000.0 || hz <= 1.0)
            return 32;
        int tau = static_cast<int>(std::lround(decimatedRate / hz));
        if (tau < 4)
            tau = 4;
        if (tau > kMaxTau)
            tau = kMaxTau;
        return tau;
    }

    double difference(const Ring& source, int origin, int tau, int window) const noexcept
    {
        double diff = 0.0;
        for (int j = 0; j < window; ++j)
        {
            const double a = source[static_cast<std::size_t>(wrap(origin + j))].load(
                std::memory_order_relaxed);
            const double b = source[static_cast<std::size_t>(wrap(origin + j + tau))].load(
                std::memory_order_relaxed);
            diff += std::abs(a - b);
        }
        return diff;
    }

    double meanAbs(const Ring& source, int origin, int window) const noexcept
    {
        double sum = 0.0;
        for (int j = 0; j < window; ++j)
            sum += std::abs(source[static_cast<std::size_t>(wrap(origin + j))].load(
                std::memory_order_relaxed));
        return sum / static_cast<double>(window);
    }

    PitchHit search(const Ring& source, int origin, int tauMin, int tauMax,
                    double threshold) const noexcept
    {
        if (tauMin < 2)
            tauMin = 2;
        if (tauMax > kMaxTau - 1)
            tauMax = kMaxTau - 1;
        if (tauMax <= tauMin)
            return {};

        double running = 0.0;
        int bestTau = -1;
        double bestNorm = 1.0;
        for (int tau = 1; tau <= tauMax && bestTau < 0; ++tau)
        {
            const double diff = difference(source, origin, tau, kWindow);
            running += diff;
            const double norm = running > 0.0 ? diff / (running / static_cast<double>(tau)) : 1.0;
            if (tau < tauMin || norm >= threshold)
                continue;

            bestTau = tau;
            bestNorm = norm;
            while (bestTau + 1 <= tauMax)
            {
                const double nextDiff = difference(source, origin, bestTau + 1, kWindow);
                const double nextRunning = running + nextDiff;
                const double nextNorm = nextRunning > 0.0
                    ? nextDiff / (nextRunning / static_cast<double>(bestTau + 1))
                    : 1.0;
                if (nextNorm < bestNorm)
                {
                    ++bestTau;
                    running = nextRunning;
                    bestNorm = nextNorm;
                }
                else
                {
                    break;
                }
            }
        }

        if (bestTau < tauMin)
            return {};

        const double y1 = difference(source, origin, bestTau - 1, kWindow);
        const double y2 = difference(source, origin, bestTau, kWindow);
        const double y3 = bestTau + 1 <= tauMax ? difference(source, origin, bestTau + 1, kWindow) : y2;
        const double denom = 2.0 * (y1 - 2.0 * y2 + y3);
        double delta = std::abs(denom) > 0.00001 ? (y1 - y3) / denom : 0.0;
        delta = clamp(delta, -1.0, 1.0);
        const double tauSub = static_cast<double>(bestTau) + delta;
        if (tauSub <= 1.0)
            return {};
        const double hz = decimatedRate / tauSub;
        if (hz <= 25.0 || hz >= 520.0)
            return {};
        return { true, hz };
    }

    void publish(double hz) noexcept
    {
        const double midi = 69.0 + 12.0 * (std::log(hz / 440.0) / std::log(2.0));
        int note = static_cast<int>(std::floor(midi + 0.5));
        if (note < 0)
            note = 0;
        if (note > 127)
            note = 127;
        int semitone = note % 12;
        if (semitone < 0)
            semitone += 12;
        const int octave = static_cast<int>(std::floor(static_cast<double>(note) / 12.0)) - 1;
        const double rawCents = (midi - static_cast<double>(note)) * 100.0;

        if (note == candidate)
            ++candidateCount;
        else
        {
            candidate = note;
            candidateCount = 1;
        }

        if (candidateCount >= 2 || readout.midiNote < 0)
        {
            if (readout.midiNote != note)
            {
                histCount = 0;
                histIndex = 0;
                smoothCents = rawCents;
            }
            readout.midiNote = note;
            readout.noteIndex = semitone;
            readout.octave = octave;
        }

        centsHist[static_cast<std::size_t>(histIndex)] = rawCents;
        histIndex = (histIndex + 1) % 4;
        histCount = std::min(4, histCount + 1);
        double sum = 0.0;
        for (int i = 0; i < histCount; ++i)
            sum += centsHist[static_cast<std::size_t>(i)];
        const double average = sum / static_cast<double>(histCount);
        smoothCents = smoothCents * 0.65 + average * 0.35;

        readout.active = true;
        readout.hz = hz;
        readout.cents = smoothCents;
    }

    OnePoleHp fullHp;
    Biquad fullLp;
    OnePoleHp lowHp;
    Biquad lowLpA;
    Biquad lowLpB;
    BiquadCoeffs fullLpCoeff {};
    BiquadCoeffs lowLpCoeff {};
    double fullHpA = 0.99;
    double lowHpA = 0.99;
    Ring fullRing {};
    Ring lowRing {};
    std::array<double, 4> centsHist {};
    std::atomic<int> write { 0 };
    int decimCount = 0;
    int decim = 4;
    std::atomic<int> filled { 0 };
    std::atomic<double> env { 0.0 };
    double preparedRate = 0.0;
    double decimatedRate = 12000.0;
    double smoothCents = 0.0;
    int candidate = -1;
    int candidateCount = 0;
    int histIndex = 0;
    int histCount = 0;
    int silenceCount = 0;
    TunerReadout readout;
    std::atomic<uint32_t> sequence { 0 };
};

} // namespace subdoom
