#include "subdoom/Processor.h"

#include "subdoom/Saturation.h"

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86)
#include <immintrin.h>
#define SUBDOOM_HAS_MXCSR 1
#endif

#include <cmath>

namespace subdoom {
namespace {

struct DenormalGuard
{
#if defined(SUBDOOM_HAS_MXCSR)
    unsigned int mxcsr;
    DenormalGuard() noexcept
        : mxcsr(_mm_getcsr())
    {
        _mm_setcsr(mxcsr | 0x8040u);
    }
    ~DenormalGuard() noexcept { _mm_setcsr(mxcsr); }
#else
    DenormalGuard() noexcept = default;
#endif
};

} // namespace

Processor::Processor()
{
    prepare(48000.0, 512);
}

void Processor::prepare(double sampleRate, int)
{
    coeffs.sampleRate = std::max(8000.0, sampleRate);
    room.prepare(coeffs.sampleRate);
    pitch.setSampleRate(coeffs.sampleRate);
    meter.prepare(coeffs.sampleRate);
    updateCoefficients();
    reset();
}

void Processor::updateCoefficients()
{
    const double sr = coeffs.sampleRate;
    const double splitPole = rcLpPole(params.splitHz, sr);
    const double octPole = rcLpPole(180.0, sr);
    const double avgPole = rcLpPole(6.0, sr);
    const double outPole = rcLpPole(220.0, sr);

    coeffs.subHp = makeHighpass(kSubsonicHz, 0.7071, sr);
    coeffs.gateAttack = OnePoleChase::coeffFor(0.020, sr);
    coeffs.gateRelease = OnePoleChase::coeffFor(0.400, sr);
    coeffs.gateHold = OnePoleChase::coeffFor(0.030, sr);
    coeffs.gateOpen = OnePoleChase::coeffFor(0.030, sr);
    coeffs.levelAttack = OnePoleChase::coeffFor(0.080, sr);
    coeffs.levelRelease = OnePoleChase::coeffFor(0.500, sr);
    coeffs.levelHold = OnePoleChase::coeffFor(0.025, sr);
    coeffs.splitA0 = 1.0 - splitPole;
    coeffs.splitB1 = splitPole;
    coeffs.octLpA0 = 1.0 - octPole;
    coeffs.octLpB1 = octPole;
    coeffs.octAvgA0 = 1.0 - avgPole;
    coeffs.octAvgB1 = avgPole;
    coeffs.octHpA = rcHpCoeff(35.0, sr);
    coeffs.octOutA0 = 1.0 - outPole;
    coeffs.octOutB1 = outPole;
    coeffs.highHpA = rcHpCoeff(15.0, sr);
    coeffs.sagAttack = OnePoleChase::coeffFor(0.012, sr);
    coeffs.sagRelease = OnePoleChase::coeffFor(0.420, sr);
    coeffs.gainSmooth = OnePoleChase::coeffFor(0.020, sr);
    coeffs.thump = makePeaking(58.0, 0.9, params.thumpDb, sr);
    coeffs.harm2 = makePeaking(kHarmonic2Hz, kHarmonicQ, params.harmDb, sr);
    coeffs.harm3 = makePeaking(kHarmonic3Hz, kHarmonicQ, params.harmDb, sr);
    coeffs.growl = makePeaking(240.0, 0.8, params.growlDb, sr);
    coeffs.honk = makePeaking(750.0, 1.1, params.honkDb, sr);
    coeffs.grit = makeBandpass(1400.0, 2.0, sr);
    coeffs.fuzzBody = makeLowpass(kFuzzBodyHz, 0.7071, sr);
    coeffs.cut = makeLowpass(params.highCutHz, 0.7071, sr);
    coeffs.limAttack = OnePoleChase::coeffFor(0.0015, sr);
    coeffs.limRelease = OnePoleChase::coeffFor(0.050, sr);
    coeffs.fadeStep = 1.0 / std::max(1.0, 0.012 * sr);
    coeffs.muteStep = 1.0 / std::max(1.0, 0.015 * sr);

    trimChase.setCoeff(coeffs.gainSmooth);
    sustainChase.setCoeff(coeffs.gainSmooth);
    lowDriveChase.setCoeff(coeffs.gainSmooth);
    highDriveChase.setCoeff(coeffs.gainSmooth);
    lowGainChase.setCoeff(coeffs.gainSmooth);
    highGainChase.setCoeff(coeffs.gainSmooth);
    octaveChase.setCoeff(coeffs.gainSmooth);
    sagChase.setCoeff(coeffs.gainSmooth);
    gritChase.setCoeff(coeffs.gainSmooth);
    roomMixChase.setCoeff(coeffs.gainSmooth);
    roomDecayChase.setCoeff(coeffs.gainSmooth);
    masterChase.setCoeff(coeffs.gainSmooth);
}

Processor::GainTargets Processor::gainTargetsFor(const Parameters& params) noexcept
{
    return { dbToGain(params.inputTrim + kInputAlignDb), dbToGain(params.lowDb),
             dbToGain(params.highDb), dbToGain(params.masterDb) };
}

namespace {
constexpr BiquadCoeffs kZeroCoeffs { 0.0, 0.0, 0.0, 0.0, 0.0 };
}

void Processor::CoeffRamp::snap(const BiquadCoeffs& next) noexcept
{
    value = target = next;
    step = kZeroCoeffs;
    remaining = 0;
}

void Processor::CoeffRamp::aim(const BiquadCoeffs& next, int samples) noexcept
{
    target = next;
    if (value.b0 == target.b0 && value.b1 == target.b1 && value.b2 == target.b2
        && value.a1 == target.a1 && value.a2 == target.a2)
    {
        step = kZeroCoeffs;
        remaining = 0;
        return;
    }

    remaining = samples > 1 ? samples : 1;
    const double inverse = 1.0 / static_cast<double>(remaining);
    step.b0 = (target.b0 - value.b0) * inverse;
    step.b1 = (target.b1 - value.b1) * inverse;
    step.b2 = (target.b2 - value.b2) * inverse;
    step.a1 = (target.a1 - value.a1) * inverse;
    step.a2 = (target.a2 - value.a2) * inverse;
}

void Processor::CoeffRamp::advance() noexcept
{
    if (remaining <= 0)
        return;

    if (--remaining == 0)
    {
        value = target;
        step = kZeroCoeffs;
        return;
    }

    value.b0 += step.b0;
    value.b1 += step.b1;
    value.b2 += step.b2;
    value.a1 += step.a1;
    value.a2 += step.a2;
}

void Processor::ValueRamp::snap(double next) noexcept
{
    value = target = next;
    step = 0.0;
    remaining = 0;
}

void Processor::ValueRamp::aim(double next, int samples) noexcept
{
    target = next;
    if (value == target)
    {
        step = 0.0;
        remaining = 0;
        return;
    }

    remaining = samples > 1 ? samples : 1;
    step = (target - value) / static_cast<double>(remaining);
}

void Processor::ValueRamp::advance() noexcept
{
    if (remaining <= 0)
        return;

    if (--remaining == 0)
    {
        value = target;
        step = 0.0;
        return;
    }

    value += step;
}

void Processor::aimRamps(int numSamples) noexcept
{
    thumpRamp.aim(coeffs.thump, numSamples);
    harm2Ramp.aim(coeffs.harm2, numSamples);
    harm3Ramp.aim(coeffs.harm3, numSamples);
    growlRamp.aim(coeffs.growl, numSamples);
    honkRamp.aim(coeffs.honk, numSamples);
    cutRamp.aim(coeffs.cut, numSamples);
    splitRamp.aim(coeffs.splitB1, numSamples);
}

void Processor::snapSmoothers() noexcept
{
    trimChase.snap(dbToGain(params.inputTrim + kInputAlignDb));
    sustainChase.snap(params.sustain);
    lowDriveChase.snap(params.lowDrive);
    highDriveChase.snap(params.highDrive);
    lowGainChase.snap(dbToGain(params.lowDb));
    highGainChase.snap(dbToGain(params.highDb));
    octaveChase.snap(params.octave);
    sagChase.snap(params.sag);
    gritChase.snap(params.grit);
    roomMixChase.snap(params.roomMix);
    roomDecayChase.snap(params.roomDecay);
    masterChase.snap(dbToGain(params.masterDb));
}

void Processor::resetSignalPath() noexcept
{
    subHpA.reset();
    subHpB.reset();
    split.reset();
    highHp.reset();
    gate.reset(asOn(params.gate));
    leveler.reset();
    octave.reset();
    sag.reset();
    thump.reset();
    harm2.reset();
    harm3.reset();
    growl.reset();
    honk.reset();
    grit.reset();
    fuzzBodyA.reset();
    fuzzBodyB.reset();
    cutA.reset();
    cutB.reset();
    room.reset();
    limiter.reset();
    snapSmoothers();
    thumpRamp.snap(coeffs.thump);
    harm2Ramp.snap(coeffs.harm2);
    harm3Ramp.snap(coeffs.harm3);
    growlRamp.snap(coeffs.growl);
    honkRamp.snap(coeffs.honk);
    cutRamp.snap(coeffs.cut);
    splitRamp.snap(coeffs.splitB1);
}

void Processor::reset() noexcept
{
    resetSignalPath();
    pitch.reset();
    pitch.setSampleRate(coeffs.sampleRate);
    meter.reset();
    fadeState = 0;
    fadeGain = 1.0;
    muteGain = asOn(params.tunerMute) ? 0.0 : 1.0;
    stagedValid = false;
    publishedPreset.store(-1, std::memory_order_relaxed);
    presetApplied.store(false, std::memory_order_relaxed);
}

void Processor::setParameters(const Parameters& next)
{
    // The host hands the same values back every block; rebuilding ~25 transcendental
    // coefficients for them is pure waste. A real change is at least one parameter
    // step, so the preset tolerance is the right gate.
    if (parametersMatch(params, next))
        return;

    params = next;
    params.clampIntoRange();
    updateCoefficients();
}

void Processor::requestPreset(const Parameters& next) noexcept
{
    while (presetWriteBusy.test_and_set(std::memory_order_acquire))
    {
    }

    // Wait for an in-flight copy to finish. Once it has, the published slot is the
    // only one the reader could have been holding, and the other slot is free.
    while (presetCopyBusy.load(std::memory_order_acquire))
    {
    }

    const int current = publishedPreset.load(std::memory_order_acquire);
    const int slot = current == 0 ? 1 : 0;
    presetMailboxes[static_cast<std::size_t>(slot)] = next;
    publishedPreset.store(slot, std::memory_order_release);
    presetWriteBusy.clear(std::memory_order_release);
}

bool Processor::consumePresetApplied() noexcept
{
    return presetApplied.exchange(false, std::memory_order_acq_rel);
}

void Processor::installStagedPreset() noexcept
{
    params = staged;
    params.clampIntoRange();
    stagedValid = false;
    updateCoefficients();
    resetSignalPath();
    presetApplied.store(true, std::memory_order_release);
}

void Processor::pullPreset() noexcept
{
    // Announce the copy before reading the slot, so a write that starts after this
    // point either targets the other slot or waits for the copy to finish.
    presetCopyBusy.store(true, std::memory_order_release);

    int published = publishedPreset.load(std::memory_order_acquire);
    if (published >= 0)
        staged = presetMailboxes[static_cast<std::size_t>(published)];

    presetCopyBusy.store(false, std::memory_order_release);

    if (published < 0)
        return;

    stagedValid = true;
    if (fadeState != 1)
        fadeState = 1;
    publishedPreset.compare_exchange_strong(published, -1, std::memory_order_acq_rel);
}

void Processor::pollTuner() noexcept
{
    if (pitch.envelope() > 0.003)
        pitch.analyse();
    else
        pitch.noteSilence();
}

void Processor::processSample(double& left, double& right, const GainTargets& gains) noexcept
{
    if (!std::isfinite(left) || !std::isfinite(right))
    {
        left = 0.0;
        right = 0.0;
    }

    blockPrePeak = std::max(blockPrePeak, std::max(std::abs(left), std::abs(right)));
    const double trim = trimChase.next(gains.trim);
    double mono = 0.5 * (left + right) * trim;
    blockPostPeak = std::max(blockPostPeak, std::abs(mono));
    mono = subHpA.process(mono, coeffs.subHp);
    mono = subHpB.process(mono, coeffs.subHp);

    // Dry, after trim, before the drives. Mute does not touch this.
    const double dryMono = mono;
    pitch.feed(mono);

    const double sustain = sustainChase.next(clamp(params.sustain, 0.0, 1.0));
    mono = gate.process(mono, asOn(params.gate), coeffs.gateAttack, coeffs.gateRelease,
                        coeffs.gateHold, coeffs.gateOpen);
    mono = leveler.process(mono, sustain, coeffs.levelAttack, coeffs.levelRelease, coeffs.levelHold);

    const double folded = octave.process(mono, coeffs);
    splitRamp.advance();
    const double low = split.process(mono, 1.0 - splitRamp.value, splitRamp.value);
    const double highIn = mono - low;

    const double lowDrive = lowDriveChase.next(clamp(params.lowDrive, 0.0, 1.0));
    const double highDrive = highDriveChase.next(clamp(params.highDrive, 0.0, 1.0));
    const double lowGain = lowGainChase.next(gains.low);
    const double highGain = highGainChase.next(gains.high);
    const double lowPush = driveGain(lowDrive, kLowDriveMax);
    const double highPush = driveGain(highDrive, kHighDriveMax);
    const double opened = clamp((highDrive - kHighDriveKnee) / (1.0 - kHighDriveKnee), 0.0, 1.0);
    const double knee = opened * opened;
    const double bodyPush = highPush * (1.0 + knee * (kHighDriveTop - 1.0));
    const double lowMakeup = saturateCompensation(lowPush, kDriveReference);
    const double highMakeup = asymCompensation(bodyPush, kAsymmetry, kDriveReference);

    const double lowOut = saturate(low * lowPush) / lowMakeup * lowGain;
    // Clip the note. The residual above the body lowpass is finger noise and
    // pick scrape; it sums back at unity so drive does not lift it.
    double body = fuzzBodyA.process(highIn, coeffs.fuzzBody);
    body = fuzzBodyB.process(body, coeffs.fuzzBody);
    const double air = highIn - body;
    double high = asymClip(body * bodyPush, kAsymmetry) / highMakeup + air;
    high = highHp.process(high, coeffs.highHpA) * highGain;

    const double octaveAmount = octaveChase.next(clamp(params.octave, 0.0, 1.0));
    const double sagAmount = sagChase.next(clamp(params.sag, 0.0, 1.0));
    const double sagGain = sag.gain(high, sagAmount, coeffs.sagAttack, coeffs.sagRelease);
    high = (high + folded * octaveAmount * kOctaveGain) * sagGain;

    double sum = lowOut + high;
    thumpRamp.advance();
    sum = thump.process(sum, thumpRamp.value);
    harm2Ramp.advance();
    sum = harm2.process(sum, harm2Ramp.value);
    harm3Ramp.advance();
    sum = harm3.process(sum, harm3Ramp.value);
    growlRamp.advance();
    sum = growl.process(sum, growlRamp.value);
    honkRamp.advance();
    sum = honk.process(sum, honkRamp.value);

    const double gritAmount = gritChase.next(clamp(params.grit, 0.0, 1.0));
    const double band = grit.process(sum, coeffs.grit);
    sum += hardClip(band * 5.5) * gritAmount * 0.42;

    cutRamp.advance();
    sum = cutA.process(sum, cutRamp.value);
    sum = cutB.process(sum, cutRamp.value);

    const double roomMix = roomMixChase.next(clamp(params.roomMix, 0.0, 1.0));
    const double roomDecay = roomDecayChase.next(clamp(params.roomDecay, 0.0, 1.0));
    sum = room.process(sum, roomMix, roomDecay);

    const double master = masterChase.next(gains.master);
    double out = saturate(sum * master);
    out = limiter.process(out, asOn(params.limiter), coeffs.limAttack, coeffs.limRelease);

    if (fadeState == 1)
    {
        fadeGain -= coeffs.fadeStep;
        if (fadeGain <= 0.0)
        {
            fadeGain = 0.0;
            if (stagedValid)
                installStagedPreset();
            fadeState = 2;
        }
    }
    else if (fadeState == 2)
    {
        fadeGain += coeffs.fadeStep;
        if (fadeGain >= 1.0)
        {
            fadeGain = 1.0;
            fadeState = 0;
        }
    }

    if (asOn(params.tunerMute))
        muteGain = std::max(0.0, muteGain - coeffs.muteStep);
    else
        muteGain = std::min(1.0, muteGain + coeffs.muteStep);

    out *= fadeGain * muteGain;
    // Never hand the host a non-finite sample, even if something upstream produced one.
    out = std::isfinite(out) ? clamp(out, -2.0, 2.0) : 0.0;

    left = out;
    right = out;
    meter.push(static_cast<float>(dryMono), static_cast<float>(out));
}

void Processor::process(double* left, double* right, int numSamples, BlockMetrics* metrics)
{
    if (left == nullptr || right == nullptr || numSamples <= 0)
        return;

    const DenormalGuard denormals;
    blockPrePeak = 0.0;
    blockPostPeak = 0.0;
    const GainTargets gains = gainTargetsFor(params);
    pullPreset();
    aimRamps(numSamples);
    thump.heal();
    harm2.heal();
    harm3.heal();
    growl.heal();
    honk.heal();
    grit.heal();
    fuzzBodyA.heal();
    fuzzBodyB.heal();
    cutA.heal();
    cutB.heal();
    subHpA.heal();
    subHpB.heal();

    for (int i = 0; i < numSamples; ++i)
    {
        double sampleL = left[i];
        double sampleR = right[i];
        processSample(sampleL, sampleR, gains);
        if (metrics != nullptr)
            metrics->accumulate(sampleL, sampleR);
        left[i] = sampleL;
        right[i] = sampleR;
    }
    meter.publishInputLevel(static_cast<float>(blockPrePeak), static_cast<float>(blockPostPeak));
}

void Processor::process(float* left, float* right, int numSamples, BlockMetrics* metrics)
{
    if (left == nullptr || right == nullptr || numSamples <= 0)
        return;

    const DenormalGuard denormals;
    blockPrePeak = 0.0;
    blockPostPeak = 0.0;
    const bool aliased = left == right;
    const GainTargets gains = gainTargetsFor(params);
    pullPreset();
    aimRamps(numSamples);
    thump.heal();
    harm2.heal();
    harm3.heal();
    growl.heal();
    honk.heal();
    grit.heal();
    fuzzBodyA.heal();
    fuzzBodyB.heal();
    cutA.heal();
    cutB.heal();
    subHpA.heal();
    subHpB.heal();

    for (int i = 0; i < numSamples; ++i)
    {
        double sampleL = static_cast<double>(left[i]);
        double sampleR = static_cast<double>(right[i]);
        processSample(sampleL, sampleR, gains);
        if (metrics != nullptr)
            metrics->accumulate(sampleL, sampleR);
        const float out = static_cast<float>(sampleL);
        if (aliased)
            left[i] = out;
        else
        {
            left[i] = out;
            right[i] = static_cast<float>(sampleR);
        }
    }
    meter.publishInputLevel(static_cast<float>(blockPrePeak), static_cast<float>(blockPostPeak));
}

} // namespace subdoom
