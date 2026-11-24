#pragma once

#include "subdoom/FirstOrderFilters.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace subdoom {

// Single-producer rings for the editor. The audio thread writes the sample and then
// publishes the next index with release; the message thread acquires the index and
// may see a sample being overwritten, never a torn index. The samples themselves
// are relaxed atomics: on x86 that is a plain move, and it keeps a display read that
// races a write formally well defined. No locks and no allocation either side.
struct ScopeMeter
{
    static constexpr int kScopeRing = 2048;
    static constexpr int kSpectrumBands = 24;
    // Geometric band edges. The spectrum axis has to use these: a 50 Hz-10 kHz
    // ruler would put 10 kHz on a display that ends at 8 kHz.
    static constexpr double kSpectrumLowHz = 30.0;
    static constexpr double kSpectrumHighHz = 8000.0;

    using SharedSample = std::atomic<float>;
    using ScopeRing = std::array<SharedSample, kScopeRing>;
    using SpectrumBands = std::array<SharedSample, kSpectrumBands>;
    static_assert(std::atomic<float>::is_always_lock_free,
                  "the scope meter must not take a lock on the audio thread");

    void prepare(double sampleRate) noexcept
    {
        const double sr = std::max(8000.0, sampleRate);
        // The bands run at the audio rate: a decimated analyser would move
        // every cutoff with it, and the low end is the whole point here.
        analysisDecay = 1.0 - std::exp(-1.0 / (0.09 * sr));

        // One lowpass edge per band boundary, spaced geometrically across the
        // displayed range.
        for (int edge = 0; edge <= kSpectrumBands; ++edge)
        {
            const double t = static_cast<double>(edge) / static_cast<double>(kSpectrumBands);
            const double freq = kSpectrumLowHz * std::pow(kSpectrumHighHz / kSpectrumLowHz, t);
            edges[static_cast<std::size_t>(edge)] = rcLpPole(freq, sr);
        }
        reset();
    }

    void reset() noexcept
    {
        scopeDecim = 0;
        for (auto& sample : outputScope)
            sample.store(0.0f, std::memory_order_relaxed);
        for (auto& sample : inputScope)
            sample.store(0.0f, std::memory_order_relaxed);
        for (auto& stage : states)
            stage = 0.0;
        for (auto& band : bands)
            band.store(0.0f, std::memory_order_relaxed);
        outputScopeIndex.store(0, std::memory_order_relaxed);
        inputScopeIndex.store(0, std::memory_order_relaxed);
        preTrimPeak.store(0.0f, std::memory_order_relaxed);
        postTrimPeak.store(0.0f, std::memory_order_relaxed);
    }

    // Block peaks since the UI last took them. The take clears the slot.
    void publishInputLevel(float preTrim, float postTrim) noexcept
    {
        raisePeak(preTrimPeak, preTrim);
        raisePeak(postTrimPeak, postTrim);
    }

    float takePreTrimPeak() noexcept
    {
        return preTrimPeak.exchange(0.0f, std::memory_order_acq_rel);
    }

    float takePostTrimPeak() noexcept
    {
        return postTrimPeak.exchange(0.0f, std::memory_order_acq_rel);
    }

    // Called once per sample from the audio thread.
    void push(float dryInput, float output) noexcept
    {
        if (++scopeDecim >= kScopeDecimation)
        {
            scopeDecim = 0;
            writeRing(outputScope, outputScopeIndex, output);
            writeRing(inputScope, inputScopeIndex, dryInput);
        }

        analyse(output);
    }

    int outputWriteIndex() const noexcept
    {
        return outputScopeIndex.load(std::memory_order_acquire);
    }

    int inputWriteIndex() const noexcept
    {
        return inputScopeIndex.load(std::memory_order_acquire);
    }

    const ScopeRing& outputBuffer() const noexcept { return outputScope; }
    const ScopeRing& inputBuffer() const noexcept { return inputScope; }

    std::array<float, kSpectrumBands> spectrum() const noexcept
    {
        std::array<float, kSpectrumBands> copy {};
        for (int band = 0; band < kSpectrumBands; ++band)
            copy[static_cast<std::size_t>(band)] = bands[static_cast<std::size_t>(band)].load(
                std::memory_order_relaxed);
        return copy;
    }

private:
    static constexpr int kScopeDecimation = 6;

    static void writeRing(ScopeRing& ring, std::atomic<int>& index, float sample) noexcept
    {
        const int at = index.load(std::memory_order_relaxed);
        ring[static_cast<std::size_t>(at)].store(sample, std::memory_order_relaxed);
        index.store((at + 1) % kScopeRing, std::memory_order_release);
    }

    void analyse(float sample) noexcept
    {
        // One lowpass per band edge, all fed the same sample. The difference
        // between neighbouring outputs is that band's energy: a first-order
        // band split that costs one multiply-add per band.
        double previous = 0.0;
        for (int edge = 0; edge <= kSpectrumBands; ++edge)
        {
            const std::size_t k = static_cast<std::size_t>(edge);
            const double pole = edges[k];
            states[k] = states[k] * pole + static_cast<double>(sample) * (1.0 - pole);

            if (edge > 0)
            {
                const double band = std::abs(states[k] - previous);
                SharedSample& stored = bands[k - 1];
                const double current = static_cast<double>(stored.load(std::memory_order_relaxed));
                const double rate = band > current ? 0.55 : analysisDecay;
                stored.store(static_cast<float>(current + (band - current) * rate),
                             std::memory_order_relaxed);
            }
            previous = states[k];
        }
    }

    std::array<double, kSpectrumBands + 1> edges {};
    std::array<double, kSpectrumBands + 1> states {};
    SpectrumBands bands {};

    ScopeRing outputScope {};
    ScopeRing inputScope {};
    std::atomic<int> outputScopeIndex { 0 };
    std::atomic<int> inputScopeIndex { 0 };
    double analysisDecay = 0.01;
    int scopeDecim = 0;
    std::atomic<float> preTrimPeak { 0.0f };
    std::atomic<float> postTrimPeak { 0.0f };

    static void raisePeak(std::atomic<float>& slot, float sample) noexcept
    {
        float previous = slot.load(std::memory_order_relaxed);
        while (sample > previous
               && !slot.compare_exchange_weak(previous, sample, std::memory_order_release,
                                              std::memory_order_relaxed))
        {
        }
    }
};

} // namespace subdoom
