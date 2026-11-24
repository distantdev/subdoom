#pragma once

#include "subdoom/BiquadFilter.h"
#include "subdoom/Dynamics.h"
#include "subdoom/FirstOrderFilters.h"
#include "subdoom/Parameters.h"
#include "subdoom/Room.h"
#include "subdoom/ScopeMeter.h"
#include "subdoom/Smoother.h"
#include "subdoom/Tuner.h"

#include <array>
#include <atomic>

namespace subdoom {

struct BlockMetrics
{
    double peak = 0.0;
    double sum = 0.0;
    double sumSquares = 0.0;
    int samples = 0;
    int nonFinite = 0;

    void accumulate(double left, double right) noexcept
    {
        if (!std::isfinite(left) || !std::isfinite(right))
        {
            ++nonFinite;
            return;
        }
        peak = std::max(peak, std::max(std::abs(left), std::abs(right)));
        sum += 0.5 * (left + right);
        sumSquares += 0.5 * (left * left + right * right);
        ++samples;
    }

    double peakDbfs() const noexcept
    {
        return samples > 0 ? gainToDb(peak) : -120.0;
    }
};

struct Coefficients
{
    double sampleRate = 48000.0;
    BiquadCoeffs subHp {};
    double gateAttack = 0.01;
    double gateRelease = 0.001;
    double gateHold = 0.01;
    double gateOpen = 0.01;
    double levelAttack = 0.001;
    double levelRelease = 0.0002;
    double levelHold = 0.01;
    double splitA0 = 0.01;
    double splitB1 = 0.99;
    double octLpA0 = 0.02;
    double octLpB1 = 0.98;
    double octAvgA0 = 0.001;
    double octAvgB1 = 0.999;
    double octHpA = 0.99;
    double octOutA0 = 0.02;
    double octOutB1 = 0.98;
    double highHpA = 0.99;
    double sagAttack = 0.01;
    double sagRelease = 0.0002;
    double gainSmooth = 1.0;
    BiquadCoeffs thump {};
    BiquadCoeffs harm2 {};
    BiquadCoeffs harm3 {};
    BiquadCoeffs growl {};
    BiquadCoeffs honk {};
    BiquadCoeffs grit {};
    BiquadCoeffs fuzzBody {};
    BiquadCoeffs cut {};
    double limAttack = 0.1;
    double limRelease = 0.001;
    double fadeStep = 0.002;
    double muteStep = 0.002;
};

class Processor
{
public:
    static constexpr int kScopeSize = ScopeMeter::kScopeRing;
    static constexpr double kAsymmetry = 0.62;
    static constexpr double kOctaveGain = 2.4;
    // Gain range into the two clippers. The reference keeps a drive knob about
    // tone. The low path can square the fundamental. The high path squares the
    // note's harmonics, and only those: the scrape bypasses the clipper.
    static constexpr double kLowDriveMax = 22.0;
    static constexpr double kHighDriveMax = 64.0;
    static constexpr double kDriveReference = 0.25;
    // Trim at 0 lines a -24 dBFS peak up onto kDriveReference. A direct bass
    // sits there; the old -12 dBFS target meant the trim knob had to do it.
    static constexpr double kInputAlignDb = 12.0;
    // What the high-path clipper is allowed to see. Finger noise sits above
    // this; the residual is mixed back clean so the high cut is not the only
    // thing keeping the scrape down.
    static constexpr double kFuzzBodyHz = 1400.0;
    // The voiced curve holds through the factory range. Past the knee the
    // gain keeps climbing, up to this many times kHighDriveMax at full scale,
    // so a quiet harmonic can square without opening the cut.
    static constexpr double kHighDriveKnee = 0.62;
    static constexpr double kHighDriveTop = 3.0;
    // Under the note. Two Butterworth sections, so an octave below the corner
    // is cone travel and A0 at 27.5 Hz still passes.
    static constexpr double kSubsonicHz = 20.0;
    // Second and third of A0 (27.5 Hz). One knob, both bells.
    static constexpr double kHarmonic2Hz = 55.0;
    static constexpr double kHarmonic3Hz = 82.5;
    static constexpr double kHarmonicQ = 2.5;

    Processor();

    using ScopeRing = ScopeMeter::ScopeRing;

    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;
    void setParameters(const Parameters& next);

    // Snapshot of the parameters the engine is using. Not thread-safe: call it from
    // the thread driving process(), which is what the tests do. The editor reads its
    // values through the APVTS instead.
    Parameters parameters() const noexcept { return params; }

    void requestPreset(const Parameters& next) noexcept;
    bool consumePresetApplied() noexcept;

    void process(double* left, double* right, int numSamples, BlockMetrics* metrics = nullptr);
    void process(float* left, float* right, int numSamples, BlockMetrics* metrics = nullptr);

    void pollTuner() noexcept;
    TunerReadout tuner() const noexcept { return pitch.reading(); }

    int scopeWriteIndex() const noexcept { return meter.outputWriteIndex(); }
    const ScopeRing& scopeBuffer() const noexcept { return meter.outputBuffer(); }
    int inputScopeWriteIndex() const noexcept { return meter.inputWriteIndex(); }
    const ScopeRing& inputScopeBuffer() const noexcept { return meter.inputBuffer(); }
    std::array<float, ScopeMeter::kSpectrumBands> spectrum() const noexcept { return meter.spectrum(); }

    // Hottest sample since the last take. Pre-trim is the signal as it arrived.
    // Post-trim is after the trim gain and before the 20 Hz highpass. The take clears it.
    float takeInputPrePeak() noexcept { return meter.takePreTrimPeak(); }
    float takeInputPostPeak() noexcept { return meter.takePostTrimPeak(); }

private:
    struct OctaveFold
    {
        OnePoleLp source;
        OnePoleLp average;
        OnePoleHp hp;
        OnePoleLp lowpass;

        void reset() noexcept
        {
            source.reset();
            average.reset();
            hp.reset();
            lowpass.reset();
        }

        double process(double x, const Coefficients& c) noexcept
        {
            const double src = source.process(x, c.octLpA0, c.octLpB1);
            const double mag = std::abs(src);
            const double avg = average.process(mag, c.octAvgA0, c.octAvgB1);
            double fold = mag - avg;
            fold = hp.process(fold, c.octHpA);
            return lowpass.process(fold, c.octOutA0, c.octOutB1);
        }
    };

    // Linear targets for the four dB controls, resolved once per block so the
    // sample loop never calls pow(). The chases still smooth them per sample.
    struct GainTargets
    {
        double trim = 1.0;
        double low = 1.0;
        double high = 1.0;
        double master = 1.0;
    };

    static GainTargets gainTargetsFor(const Parameters& params) noexcept;

    // Coefficient interpolation. A biquad that receives a new coefficient set at a
    // block boundary steps the filter; ramping the five coefficients across the
    // block makes an automated band or filter edge sweep instead. settled ramps cost
    // one predictable branch per sample.
    struct CoeffRamp
    {
        BiquadCoeffs value {};
        BiquadCoeffs target {};
        BiquadCoeffs step {};
        int remaining = 0;

        void snap(const BiquadCoeffs& next) noexcept;
        void aim(const BiquadCoeffs& next, int samples) noexcept;
        void advance() noexcept;
    };

    struct ValueRamp
    {
        double value = 0.0;
        double target = 0.0;
        double step = 0.0;
        int remaining = 0;

        void snap(double next) noexcept;
        void aim(double next, int samples) noexcept;
        void advance() noexcept;
    };

    void updateCoefficients();
    void aimRamps(int numSamples) noexcept;
    void snapSmoothers() noexcept;
    void resetSignalPath() noexcept;
    void installStagedPreset() noexcept;
    void pullPreset() noexcept;
    void processSample(double& left, double& right, const GainTargets& gains) noexcept;

    Parameters params;
    Parameters staged;
    Coefficients coeffs;

    OnePoleChase trimChase;
    OnePoleChase sustainChase;
    OnePoleChase lowDriveChase;
    OnePoleChase highDriveChase;
    OnePoleChase lowGainChase;
    OnePoleChase highGainChase;
    OnePoleChase octaveChase;
    OnePoleChase sagChase;
    OnePoleChase gritChase;
    OnePoleChase roomMixChase;
    OnePoleChase roomDecayChase;
    OnePoleChase masterChase;

    Biquad subHpA;
    Biquad subHpB;
    OnePoleLp split;
    OnePoleHp highHp;
    NoiseGate gate;
    SlowLeveler leveler;
    OctaveFold octave;
    HighSag sag;
    Biquad thump;
    Biquad harm2;
    Biquad harm3;
    Biquad growl;
    Biquad honk;
    Biquad grit;
    Biquad fuzzBodyA;
    Biquad fuzzBodyB;
    Biquad cutA;
    Biquad cutB;
    CoeffRamp thumpRamp;
    CoeffRamp harm2Ramp;
    CoeffRamp harm3Ramp;
    CoeffRamp growlRamp;
    CoeffRamp honkRamp;
    CoeffRamp cutRamp;
    ValueRamp splitRamp;
    Room room;
    OutputLimiter limiter;
    Tuner pitch;
    ScopeMeter meter;

    int fadeState = 0;
    double blockPrePeak = 0.0;
    double blockPostPeak = 0.0;
    double fadeGain = 1.0;
    double muteGain = 1.0;
    bool stagedValid = false;

    // Two mailbox slots and a copy handshake. The writer only ever fills the slot
    // that is not published, so the reader can copy the published slot without a
    // lock; presetCopyBusy closes the gap where a second write could otherwise
    // reuse the slot the reader is still copying.
    std::array<Parameters, 2> presetMailboxes {};
    std::atomic<int> publishedPreset { -1 };
    std::atomic<bool> presetCopyBusy { false };
    std::atomic<bool> presetApplied { false };
    std::atomic_flag presetWriteBusy {};
};

} // namespace subdoom
