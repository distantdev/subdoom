#include "subdoom/PresetIO.h"
#include "subdoom/Processor.h"
#include "subdoom/Saturation.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

// -24 dBFS. Trim at 0 aligns this onto the internal -12 dBFS drive reference.
constexpr double kNominal = 0.063095734448019324;
constexpr double kASharp0 = 29.14;

struct Rendered
{
    std::vector<double> samples;
    int nonFinite = 0;
    double peak = 0.0;
};

template <typename Signal>
Rendered render(const subdoom::Parameters& params, double sampleRate, int total, Signal signal)
{
    subdoom::Processor processor;
    constexpr int kBlock = 256;
    processor.prepare(sampleRate, kBlock);
    processor.setParameters(params);
    processor.reset();

    Rendered out;
    out.samples.assign(static_cast<std::size_t>(total), 0.0);
    std::vector<double> left(kBlock);
    std::vector<double> right(kBlock);
    int frame = 0;
    while (frame < total)
    {
        const int count = std::min(kBlock, total - frame);
        for (int i = 0; i < count; ++i)
        {
            const double sample = signal(frame + i, sampleRate);
            left[static_cast<std::size_t>(i)] = sample;
            right[static_cast<std::size_t>(i)] = sample;
        }
        processor.process(left.data(), right.data(), count, nullptr);
        for (int i = 0; i < count; ++i)
        {
            const double sample = left[static_cast<std::size_t>(i)];
            if (!std::isfinite(sample) || !std::isfinite(right[static_cast<std::size_t>(i)]))
                ++out.nonFinite;
            else
                out.peak = std::max(out.peak, std::abs(sample));
            out.samples[static_cast<std::size_t>(frame + i)] = sample;
        }
        frame += count;
    }
    return out;
}

double magnitude(const std::vector<double>& samples, int start, int count, double freq, double sampleRate)
{
    double real = 0.0;
    double imag = 0.0;
    const double w = 2.0 * subdoom::kPi * freq / sampleRate;
    const double n = static_cast<double>(std::max(1, count - 1));
    for (int i = 0; i < count; ++i)
    {
        const double window = 0.5 * (1.0 - std::cos(2.0 * subdoom::kPi * static_cast<double>(i) / n));
        const double sample = samples[static_cast<std::size_t>(start + i)] * window;
        real += sample * std::cos(w * static_cast<double>(i));
        imag -= sample * std::sin(w * static_cast<double>(i));
    }
    return 4.0 * std::sqrt(real * real + imag * imag) / static_cast<double>(count);
}

double highPassedRms(const std::vector<double>& samples, int start, int count, double sampleRate)
{
    const double a = 1.0 / (1.0 + 2.0 * subdoom::kPi * 3000.0 / sampleRate);
    double y1 = 0.0;
    double y2 = 0.0;
    double prev1 = 0.0;
    double prev2 = 0.0;
    double energy = 0.0;
    int used = 0;
    const int skip = count / 5;
    for (int i = 0; i < count; ++i)
    {
        const double x = samples[static_cast<std::size_t>(start + i)];
        y1 = a * (y1 + x - prev1);
        prev1 = x;
        y2 = a * (y2 + y1 - prev2);
        prev2 = y1;
        if (i >= skip)
        {
            energy += y2 * y2;
            ++used;
        }
    }
    if (used <= 0)
        return 0.0;
    return std::sqrt(energy / static_cast<double>(used));
}

double noiseSample(int index)
{
    const uint32_t n = static_cast<uint32_t>(index) * 1664525u + 1013904223u;
    return ((static_cast<double>(n >> 8) / static_cast<double>(1u << 24)) * 2.0 - 1.0) * 0.25;
}

subdoom::Parameters flatChain()
{
    subdoom::Parameters params;
    params.lowDrive = 0.0;
    params.highDrive = 0.0;
    params.octave = 0.0;
    params.sag = 0.0;
    params.sustain = 0.0;
    params.thumpDb = 0.0;
    params.harmDb = 0.0;
    params.growlDb = 0.0;
    params.honkDb = 0.0;
    params.grit = 0.0;
    params.roomMix = 0.0;
    params.masterDb = 0.0;
    params.limiter = 0.0;
    return params;
}

int presetNamed(const char* name)
{
    for (int i = 0; i < subdoom::kFactoryPresetCount; ++i)
        if (subdoom::presetName(i) == name)
            return i;
    return -1;
}

void tuneNote(double freq, int midi, int octave, int noteIndex)
{
    subdoom::Parameters params;
    constexpr double sr = 48000.0;
    constexpr int total = static_cast<int>(sr * 2.6);
    subdoom::Processor processor;
    processor.prepare(sr, 256);
    processor.setParameters(params);
    processor.reset();

    constexpr int kBlock = 256;
    std::vector<double> left(kBlock);
    std::vector<double> right(kBlock);
    int sincePoll = 0;
    for (int frame = 0; frame < total; frame += kBlock)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            const double t = static_cast<double>(frame + i) / sr;
            const double sample = kNominal * std::sin(2.0 * subdoom::kPi * freq * t);
            left[static_cast<std::size_t>(i)] = sample;
            right[static_cast<std::size_t>(i)] = sample;
        }
        processor.process(left.data(), right.data(), kBlock, nullptr);
        sincePoll += kBlock;
        if (sincePoll >= 2048)
        {
            processor.pollTuner();
            sincePoll = 0;
        }
    }
    processor.pollTuner();
    const subdoom::TunerReadout read = processor.tuner();
    INFO(freq << " Hz -> " << read.hz << " midi " << read.midiNote << " cents " << read.cents);
    REQUIRE(read.active);
    REQUIRE(read.midiNote == midi);
    REQUIRE(read.octave == octave);
    REQUIRE(read.noteIndex == noteIndex);
    REQUIRE(std::abs(read.cents) <= 8.0);
}

} // namespace

TEST_CASE("input level keeps the hottest peak until it is read", "[dsp]")
{
    subdoom::Processor processor;
    subdoom::Parameters params;
    params.inputTrim = -6.0;
    processor.setParameters(params);
    processor.prepare(48000.0, 128);
    processor.reset();

    std::vector<double> left(128, 0.5);
    std::vector<double> right(128, 0.25);
    processor.process(left.data(), right.data(), 128);

    const double trim = subdoom::dbToGain(-6.0 + subdoom::Processor::kInputAlignDb);
    const double post = 0.5 * (0.5 + 0.25) * trim;
    REQUIRE(std::abs(processor.takeInputPrePeak() - 0.5f) < 1.0e-6f);
    REQUIRE(std::abs(static_cast<double>(processor.takeInputPostPeak()) - post) < 1.0e-5);
    REQUIRE(processor.takeInputPrePeak() == 0.0f);
    REQUIRE(processor.takeInputPostPeak() == 0.0f);

    std::fill(left.begin(), left.end(), 1.0);
    std::fill(right.begin(), right.end(), 0.2);
    processor.process(left.data(), right.data(), 128);
    std::fill(left.begin(), left.end(), 0.05);
    std::fill(right.begin(), right.end(), 0.05);
    processor.process(left.data(), right.data(), 128);
    REQUIRE(std::abs(processor.takeInputPrePeak() - 1.0f) < 1.0e-6f);
}

TEST_CASE("parameter ids, names, ranges, and defaults", "[params]")
{
    struct Row
    {
        const char* id;
        const char* name;
        double min;
        double max;
        double step;
        double value;
    };
    const Row rows[subdoom::kParameterCount] = {
        { "inputTrim", "Trim", -24.0, 12.0, 0.1, 0.0 },
        { "gate", "Gate", 0.0, 1.0, 1.0, 0.0 },
        { "sustain", "Sustain", 0.0, 1.0, 0.01, 0.45 },
        { "splitHz", "Split", 70.0, 180.0, 1.0, 100.0 },
        { "sag", "Sag", 0.0, 1.0, 0.01, 0.15 },
        { "lowDrive", "Low Drive", 0.0, 1.0, 0.01, 0.15 },
        { "lowDb", "Low", -12.0, 12.0, 0.1, 0.0 },
        { "highDrive", "High Drive", 0.0, 1.0, 0.01, 0.45 },
        { "highDb", "High", -12.0, 12.0, 0.1, 0.0 },
        { "octave", "Octave", 0.0, 1.0, 0.01, 0.0 },
        { "thumpDb", "Thump", -8.0, 12.0, 0.1, 3.0 },
        { "harmDb", "Harmonics", 0.0, 12.0, 0.1, 0.0 },
        { "growlDb", "Growl", -10.0, 10.0, 0.1, 1.0 },
        { "honkDb", "Honk", -10.0, 8.0, 0.1, 0.0 },
        { "highCutHz", "Cut", 700.0, 6500.0, 10.0, 2400.0 },
        { "grit", "Grit", 0.0, 1.0, 0.01, 0.0 },
        { "roomMix", "Room", 0.0, 1.0, 0.01, 0.08 },
        { "roomDecay", "Decay", 0.0, 1.0, 0.01, 0.35 },
        { "masterDb", "Master", -24.0, 6.0, 0.1, -6.0 },
        { "limiter", "Limit", 0.0, 1.0, 1.0, 1.0 },
        { "tunerMute", "Mute", 0.0, 1.0, 1.0, 0.0 },
    };

    REQUIRE(subdoom::kParameterCount == 21);
    const subdoom::Parameters defaults;
    for (int i = 0; i < subdoom::kParameterCount; ++i)
    {
        const int index = i + 1;
        const subdoom::ParameterRange range = subdoom::parameterRange(index);
        INFO(rows[i].id);
        REQUIRE(std::string(subdoom::parameterId(index)) == rows[i].id);
        REQUIRE(std::string(subdoom::parameterName(index)) == rows[i].name);
        REQUIRE(range.min == rows[i].min);
        REQUIRE(range.max == rows[i].max);
        REQUIRE(range.step == rows[i].step);
        REQUIRE(defaults.at(index) == rows[i].value);
    }
}

TEST_CASE("factory preset names", "[presets]")
{
    REQUIRE(subdoom::kFactoryPresetCount == 10);
    const char* names[10] = {
        "LOW MASS", "WOOL HYMN", "RED SUN", "FUNERAL WALL", "DRAGON LUNG",
        "SERPENT", "GIZA", "MARROW", "EMPRESS", "CAVEMAN",
    };
    for (int i = 0; i < 10; ++i)
    {
        REQUIRE(subdoom::presetName(i) == names[i]);
        const subdoom::Parameters preset = subdoom::factoryPreset(i);
        for (int p = 1; p <= subdoom::kParameterCount; ++p)
        {
            const subdoom::ParameterRange range = subdoom::parameterRange(p);
            REQUIRE(preset.at(p) >= range.min);
            REQUIRE(preset.at(p) <= range.max);
        }
        REQUIRE(preset.limiter == 1.0);
    }
}

TEST_CASE("silence and finite audio on every preset", "[finite]")
{
    const double rates[] = { 44100.0, 48000.0, 96000.0 };
    for (double rate : rates)
    {
        const int silenceN = static_cast<int>(rate * 0.35);
        const int otherN = static_cast<int>(rate * 0.45);
        for (int preset = 0; preset < subdoom::kFactoryPresetCount; ++preset)
        {
            const subdoom::Parameters params = subdoom::factoryPreset(preset);
            INFO(subdoom::presetName(preset) << " @ " << rate);

            const Rendered quiet = render(params, rate, silenceN, [](int, double) { return 0.0; });
            REQUIRE(quiet.nonFinite == 0);
            REQUIRE(subdoom::gainToDb(quiet.peak) < -100.0);

            const Rendered impulse = render(params, rate, otherN, [](int index, double) {
                return index == 0 ? 1.0 : 0.0;
            });
            REQUIRE(impulse.nonFinite == 0);

            const Rendered noise = render(params, rate, otherN, [](int index, double) {
                return noiseSample(index);
            });
            REQUIRE(noise.nonFinite == 0);

            const Rendered low = render(params, rate, otherN, [](int index, double sampleRate) {
                const double t = static_cast<double>(index) / sampleRate;
                return kNominal * std::sin(2.0 * subdoom::kPi * kASharp0 * t);
            });
            REQUIRE(low.nonFinite == 0);
        }
    }
}

TEST_CASE("root beats the third harmonic on the heavy presets", "[fundamental]")
{
    constexpr double sr = 48000.0;
    constexpr int warmup = static_cast<int>(sr * 0.45);
    constexpr int body = static_cast<int>(sr * 1.0);
    const int presets[] = { presetNamed("FUNERAL WALL"), presetNamed("CAVEMAN") };
    for (int index : presets)
    {
        REQUIRE(index >= 0);
        const Rendered audio = render(subdoom::factoryPreset(index), sr, warmup + body,
                                       [](int i, double sampleRate) {
                                           const double t = static_cast<double>(i) / sampleRate;
                                           return kNominal * std::sin(2.0 * subdoom::kPi * kASharp0 * t);
                                       });
        REQUIRE(audio.nonFinite == 0);
        const double root = magnitude(audio.samples, warmup, body, kASharp0, sr);
        const double third = magnitude(audio.samples, warmup, body, kASharp0 * 3.0, sr);
        INFO(subdoom::presetName(index) << " root " << root << " third " << third);
        REQUIRE(root >= third * 2.0);
    }
}

TEST_CASE("split keeps the low sine cleaner than the high sine", "[split]")
{
    constexpr double sr = 48000.0;
    constexpr int warmup = static_cast<int>(sr * 0.4);
    constexpr int body = static_cast<int>(sr * 1.0);

    subdoom::Parameters driven;
    driven.lowDrive = 0.0;
    driven.highDrive = 1.0;
    driven.octave = 0.0;
    subdoom::Parameters clean = driven;
    clean.highDrive = 0.0;

    const auto lowTone = [](int i, double sampleRate) {
        const double t = static_cast<double>(i) / sampleRate;
        return kNominal * std::sin(2.0 * subdoom::kPi * 30.0 * t);
    };
    const Rendered drivenLow = render(driven, sr, warmup + body, lowTone);
    const Rendered cleanLow = render(clean, sr, warmup + body, lowTone);
    const double drivenMag = magnitude(drivenLow.samples, warmup, body, 30.0, sr);
    const double cleanMag = magnitude(cleanLow.samples, warmup, body, 30.0, sr);
    const double fundamentalDb = 20.0 * std::log10(drivenMag / std::max(cleanMag, 1.0e-12));
    INFO("30 Hz fundamental shift " << fundamentalDb << " dB");
    REQUIRE(std::abs(fundamentalDb) <= 6.0);

    const auto highTone = [](int i, double sampleRate) {
        const double t = static_cast<double>(i) / sampleRate;
        return kNominal * std::sin(2.0 * subdoom::kPi * 220.0 * t);
    };
    const Rendered high = render(driven, sr, warmup + body, highTone);
    const double fund = magnitude(high.samples, warmup, body, 220.0, sr);
    const double third = magnitude(high.samples, warmup, body, 660.0, sr);
    INFO("220 Hz fund " << fund << " third " << third);
    REQUIRE(third > fund * 0.08);
}

TEST_CASE("high drive fuzzes under the cut and does not lift the scrape", "[drive]")
{
    constexpr double sr = 48000.0;
    constexpr int warmup = static_cast<int>(sr * 0.45);
    constexpr int body = static_cast<int>(sr * 1.0);

    subdoom::Parameters driven = flatChain();
    driven.highDrive = 1.0;
    driven.highCutHz = 1600.0;
    subdoom::Parameters clean = driven;
    clean.highDrive = 0.0;

    const auto note = [](int i, double sampleRate) {
        const double t = static_cast<double>(i) / sampleRate;
        return kNominal * std::sin(2.0 * subdoom::kPi * 400.0 * t);
    };
    const Rendered fuzz = render(driven, sr, warmup + body, note);
    const Rendered dry = render(clean, sr, warmup + body, note);
    const double third = magnitude(fuzz.samples, warmup, body, 1200.0, sr);
    const double fund = magnitude(fuzz.samples, warmup, body, 400.0, sr);
    const double thirdDry = magnitude(dry.samples, warmup, body, 1200.0, sr);
    INFO("400 Hz fund " << fund << " third " << third << " dry third " << thirdDry);
    REQUIRE(fuzz.nonFinite == 0);
    REQUIRE(third > fund * 0.15);
    REQUIRE(third > thirdDry * 4.0);

    // Scrape sits above the clipper's lowpass, so the drive must not lift it.
    subdoom::Parameters bright = flatChain();
    bright.highDrive = 1.0;
    bright.highCutHz = 6500.0;
    subdoom::Parameters brightClean = bright;
    brightClean.highDrive = 0.0;
    const auto scrape = [](int i, double sampleRate) {
        const double t = static_cast<double>(i) / sampleRate;
        return kNominal * std::sin(2.0 * subdoom::kPi * 5000.0 * t);
    };
    const Rendered noisy = render(bright, sr, warmup + body, scrape);
    const Rendered quiet = render(brightClean, sr, warmup + body, scrape);
    const double scrapeOn = magnitude(noisy.samples, warmup, body, 5000.0, sr);
    const double scrapeOff = magnitude(quiet.samples, warmup, body, 5000.0, sr);
    const double scrapeDb = 20.0 * std::log10(scrapeOn / std::max(scrapeOff, 1.0e-12));
    INFO("5 kHz shift " << scrapeDb << " dB");
    REQUIRE(noisy.nonFinite == 0);
    REQUIRE(std::abs(scrapeDb) <= 3.0);
}

TEST_CASE("octave fold raises the second harmonic of A#0", "[octave]")
{
    constexpr double sr = 48000.0;
    constexpr int warmup = static_cast<int>(sr * 0.4);
    constexpr int body = static_cast<int>(sr * 1.0);
    subdoom::Parameters off;
    subdoom::Parameters on = off;
    on.octave = 1.0;
    const auto tone = [](int i, double sampleRate) {
        const double t = static_cast<double>(i) / sampleRate;
        return kNominal * std::sin(2.0 * subdoom::kPi * kASharp0 * t);
    };
    const Rendered dry = render(off, sr, warmup + body, tone);
    const Rendered wet = render(on, sr, warmup + body, tone);
    const double rootOff = magnitude(dry.samples, warmup, body, kASharp0, sr);
    const double rootOn = magnitude(wet.samples, warmup, body, kASharp0, sr);
    const double octOff = magnitude(dry.samples, warmup, body, 58.27, sr);
    const double octOn = magnitude(wet.samples, warmup, body, 58.27, sr);
    const double rootDb = 20.0 * std::log10(rootOn / std::max(rootOff, 1.0e-12));
    const double octDb = 20.0 * std::log10(std::max(octOn, 1.0e-12) / std::max(octOff, 1.0e-12));
    INFO("root shift " << rootDb << " dB, octave rise " << octDb << " dB");
    REQUIRE(std::abs(rootDb) <= 2.0);
    REQUIRE(octDb >= 12.0);
}

TEST_CASE("harmonic boost lifts the second and third of A0", "[harmonics]")
{
    constexpr double sr = 48000.0;
    constexpr double a0 = 27.5;
    constexpr int warmup = static_cast<int>(sr * 0.45);
    constexpr int body = static_cast<int>(sr * 1.0);

    // A string already carries these partials. The bells only lift what is there.
    const auto tone = [](int i, double sampleRate) {
        const double t = static_cast<double>(i) / sampleRate;
        const double fundamental = std::sin(2.0 * subdoom::kPi * a0 * t);
        const double second = std::sin(2.0 * subdoom::kPi * (a0 * 2.0) * t);
        const double third = std::sin(2.0 * subdoom::kPi * (a0 * 3.0) * t);
        return 0.025118864315095801 * (fundamental + 0.5 * second + 0.35 * third);
    };

    subdoom::Parameters off = flatChain();
    subdoom::Parameters on = off;
    on.harmDb = 12.0;

    const Rendered dry = render(off, sr, warmup + body, tone);
    const Rendered wet = render(on, sr, warmup + body, tone);
    REQUIRE(dry.nonFinite == 0);
    REQUIRE(wet.nonFinite == 0);

    const double secondDb = 20.0 * std::log10(
        magnitude(wet.samples, warmup, body, a0 * 2.0, sr)
        / std::max(magnitude(dry.samples, warmup, body, a0 * 2.0, sr), 1.0e-12));
    const double thirdDb = 20.0 * std::log10(
        magnitude(wet.samples, warmup, body, a0 * 3.0, sr)
        / std::max(magnitude(dry.samples, warmup, body, a0 * 3.0, sr), 1.0e-12));
    const double rootDb = 20.0 * std::log10(
        magnitude(wet.samples, warmup, body, a0, sr)
        / std::max(magnitude(dry.samples, warmup, body, a0, sr), 1.0e-12));
    INFO("second " << secondDb << " dB, third " << thirdDb << " dB, root " << rootDb << " dB");
    REQUIRE(secondDb >= 8.0);
    REQUIRE(thirdDb >= 8.0);
    REQUIRE(std::abs(rootDb) <= 3.0);
}

TEST_CASE("subsonic highpass cuts energy that only moves a speaker", "[subsonic]")
{
    constexpr double sr = 48000.0;
    constexpr int warmup = static_cast<int>(sr * 0.5);
    constexpr int body = static_cast<int>(sr * 1.0);
    const subdoom::Parameters params = flatChain();

    const auto tone = [](double freq) {
        return [freq](int i, double sampleRate) {
            const double t = static_cast<double>(i) / sampleRate;
            return kNominal * std::sin(2.0 * subdoom::kPi * freq * t);
        };
    };

    const Rendered note = render(params, sr, warmup + body, tone(27.5));
    const Rendered mid = render(params, sr, warmup + body, tone(100.0));
    const Rendered rumble = render(params, sr, warmup + body, tone(10.0));
    REQUIRE(note.nonFinite == 0);
    REQUIRE(rumble.nonFinite == 0);

    const double noteMag = magnitude(note.samples, warmup, body, 27.5, sr);
    const double midMag = magnitude(mid.samples, warmup, body, 100.0, sr);
    const double rumbleMag = magnitude(rumble.samples, warmup, body, 10.0, sr);
    const double noteDb = 20.0 * std::log10(noteMag / std::max(midMag, 1.0e-12));
    const double rumbleDb = 20.0 * std::log10(std::max(rumbleMag, 1.0e-12) / std::max(noteMag, 1.0e-12));
    INFO("27.5 Hz vs 100 Hz " << noteDb << " dB, 10 Hz vs 27.5 Hz " << rumbleDb << " dB");
    REQUIRE(noteDb >= -4.0);
    REQUIRE(rumbleDb <= -18.0);
}

TEST_CASE("high cut drops energy above 3 kHz", "[cut]")
{
    constexpr double sr = 48000.0;
    constexpr int total = static_cast<int>(sr * 0.8);
    subdoom::Parameters dark;
    dark.highCutHz = 1000.0;
    dark.roomMix = 0.0;
    dark.grit = 0.0;
    subdoom::Parameters bright = dark;
    bright.highCutHz = 5000.0;
    const auto noise = [](int i, double) { return noiseSample(i); };
    const Rendered lowCut = render(dark, sr, total, noise);
    const Rendered highCut = render(bright, sr, total, noise);
    const int start = static_cast<int>(sr * 0.2);
    const double darkRms = highPassedRms(lowCut.samples, start, total - start, sr);
    const double brightRms = highPassedRms(highCut.samples, start, total - start, sr);
    INFO("1 kHz cut rms " << darkRms << " 5 kHz cut rms " << brightRms);
    REQUIRE(darkRms < brightRms * 0.35);
}

TEST_CASE("an automated cut sweeps across the block", "[automation]")
{
    constexpr double sr = 48000.0;
    constexpr int block = 256;
    constexpr double hz = 5000.0;

    // A clean chain: the cut is then the only thing deciding how much 5 kHz survives.
    subdoom::Parameters params;
    params.highCutHz = 700.0;
    params.highDrive = 0.0;
    params.lowDrive = 0.0;
    params.sag = 0.0;
    params.octave = 0.0;
    params.grit = 0.0;
    params.roomMix = 0.0;
    params.sustain = 0.0;
    params.gate = 0.0;
    params.thumpDb = 0.0;
    params.growlDb = 0.0;
    params.honkDb = 0.0;
    params.masterDb = 0.0;

    subdoom::Processor processor;
    processor.prepare(sr, block);
    processor.setParameters(params);
    processor.reset();

    std::vector<double> left(static_cast<std::size_t>(block));
    std::vector<double> right(static_cast<std::size_t>(block));
    const auto renderBlock = [&processor, &left, &right](int frame) {
        for (int i = 0; i < block; ++i)
        {
            const double t = static_cast<double>(frame + i) / sr;
            const double sample = kNominal * std::sin(2.0 * subdoom::kPi * hz * t);
            left[static_cast<std::size_t>(i)] = sample;
            right[static_cast<std::size_t>(i)] = sample;
        }
        processor.process(left.data(), right.data(), block, nullptr);
    };

    int frame = 0;
    for (; frame < static_cast<int>(sr * 0.5); frame += block)
        renderBlock(frame);

    // Open the cut wide. The coefficients have to travel across the next block, so the
    // start of it still sounds like the old setting; an unsmoothed jump would open the
    // filter on the first sample and the whole block would measure the same.
    params.highCutHz = 6500.0;
    processor.setParameters(params);
    renderBlock(frame);

    const auto rms = [&left](int start, int count) {
        double energy = 0.0;
        for (int i = 0; i < count; ++i)
            energy += left[static_cast<std::size_t>(start + i)] * left[static_cast<std::size_t>(start + i)];
        return std::sqrt(energy / static_cast<double>(count));
    };

    const double early = rms(0, 32);
    const double late = rms(block - 32, 32);
    INFO("early " << early << " late " << late);
    REQUIRE(late > early * 3.0);
}

TEST_CASE("factory presets sit in the loudness window", "[loudness]")
{
    constexpr double sr = 48000.0;
    constexpr int total = static_cast<int>(sr * 1.1);
    constexpr int skip = static_cast<int>(sr * 0.35);
    for (int i = 0; i < subdoom::kFactoryPresetCount; ++i)
    {
        const Rendered audio = render(subdoom::factoryPreset(i), sr, total, [](int index, double sampleRate) {
            const double t = static_cast<double>(index) / sampleRate;
            return kNominal * std::sin(2.0 * subdoom::kPi * 32.70 * t);
        });
        double peak = 0.0;
        for (int n = skip; n < total; ++n)
            peak = std::max(peak, std::abs(audio.samples[static_cast<std::size_t>(n)]));
        const double peakDb = subdoom::gainToDb(peak);
        INFO(subdoom::presetName(i) << " peak " << peakDb << " dBFS");
        REQUIRE(audio.nonFinite == 0);
        REQUIRE(peakDb >= -14.0);
        REQUIRE(peakDb <= -6.0);
    }
}

TEST_CASE("tuner locks low B and the G string", "[tuner]")
{
    tuneNote(27.50, 21, 0, 9);
    tuneNote(kASharp0, 22, 0, 10);
    tuneNote(30.87, 23, 0, 11);
    tuneNote(32.70, 24, 1, 0);
    tuneNote(196.00, 55, 3, 7);
    tuneNote(392.00, 67, 4, 7);
}

TEST_CASE("preset text round-trips every factory preset", "[presetio]")
{
    for (int i = 0; i < subdoom::kFactoryPresetCount; ++i)
    {
        subdoom::PresetDocument document;
        document.name = subdoom::presetName(i);
        document.category = subdoom::presetCategory(i);
        document.created = "2026-11-13T01:00:00Z";
        document.notes = "hold the root, = deliberately";
        document.params = subdoom::factoryPreset(i);

        const subdoom::PresetDocument readBack = [&] {
            subdoom::PresetDocument parsed;
            const subdoom::PresetParseReport report =
                subdoom::readPresetText(subdoom::writePresetText(document), parsed);
            INFO(subdoom::presetName(i));
            REQUIRE(report.ok());
            REQUIRE(report.unknownKeys == 0);
            REQUIRE(report.ignoredValues == 0);
            return parsed;
        }();

        INFO(subdoom::presetName(i));
        REQUIRE(readBack.name == document.name);
        REQUIRE(readBack.category == document.category);
        REQUIRE(readBack.created == document.created);
        REQUIRE(readBack.notes == document.notes);
        for (int p = 1; p <= subdoom::kParameterCount; ++p)
            REQUIRE(std::abs(readBack.params.at(p) - document.params.at(p)) <= 1.0e-9);
    }
}

TEST_CASE("preset text tolerates hand edits", "[presetio]")
{
    const std::string text =
        "# written by hand\r\n"
        "\r\n"
        "subdoom-preset 1\r\n"
        "name=  TEST KIT \r\n"
        "notes=\r\n"
        "futureKey=7\r\n"
        "lowDrive=nonsense\r\n"
        "splitHz=999\r\n"
        "highDb=-99\r\n"
        "junk line without equals\r\n"
        "thumpDb=4.5\r\n";

    subdoom::PresetDocument document;
    const subdoom::PresetParseReport report = subdoom::readPresetText(text, document);

    REQUIRE(report.ok());
    REQUIRE(report.unknownKeys == 1);
    REQUIRE(report.ignoredValues == 2);
    REQUIRE(document.name == "TEST KIT");
    REQUIRE(document.category == "User");
    REQUIRE(document.notes.empty());
    REQUIRE(document.params.splitHz == 180.0);
    REQUIRE(document.params.highDb == -12.0);
    REQUIRE(document.params.thumpDb == 4.5);

    const subdoom::Parameters defaults;
    REQUIRE(document.params.lowDrive == defaults.lowDrive);
}

TEST_CASE("preset text rejects foreign and newer files", "[presetio]")
{
    subdoom::PresetDocument document;
    REQUIRE(subdoom::readPresetText("", document).result == subdoom::PresetParseResult::empty);
    REQUIRE(subdoom::readPresetText("   \n\n", document).result == subdoom::PresetParseResult::empty);
    REQUIRE(subdoom::readPresetText("hello there", document).result
            == subdoom::PresetParseResult::notPreset);
    REQUIRE(subdoom::readPresetText("subdoom-presetX 1\n", document).result
            == subdoom::PresetParseResult::notPreset);
    REQUIRE(subdoom::readPresetText("subdoom-preset 2\nname=X\n", document).result
            == subdoom::PresetParseResult::unsupportedVersion);
    REQUIRE(subdoom::readPresetText("subdoom-preset zz\n", document).result
            == subdoom::PresetParseResult::unsupportedVersion);
    REQUIRE(document.name == "UNTITLED");
}

TEST_CASE("preset writing clamps and flattens text", "[presetio]")
{
    subdoom::PresetDocument document;
    document.name = "line\nbreak\tname";
    document.category = "";
    document.params.splitHz = 500.0;
    document.params.inputTrim = -80.0;

    const std::string text = subdoom::writePresetText(document);
    REQUIRE(text.find("name=line break name\n") != std::string::npos);
    REQUIRE(text.find("category=User") != std::string::npos);

    subdoom::PresetDocument readBack;
    REQUIRE(subdoom::readPresetText(text, readBack).ok());
    REQUIRE(readBack.params.splitHz == 180.0);
    REQUIRE(readBack.params.inputTrim == -24.0);
}

TEST_CASE("preset writing snaps host float values to the step", "[presetio]")
{
    subdoom::PresetDocument document;
    document.name = "SNAPPED";
    // What the host actually hands back for a 0.01 parameter.
    document.params.sustain = 0.8399999738;
    document.params.splitHz = 100.0000001;

    const std::string text = subdoom::writePresetText(document);
    REQUIRE(text.find("sustain=0.84\n") != std::string::npos);
    REQUIRE(text.find("splitHz=100\n") != std::string::npos);
}

TEST_CASE("switches are exactly the three boolean parameters", "[params]")
{
    int switches = 0;
    for (int index = 1; index <= subdoom::kParameterCount; ++index)
    {
        if (!subdoom::parameterIsSwitch(index))
            continue;
        ++switches;
        REQUIRE(subdoom::parameterRange(index).min == 0.0);
        REQUIRE(subdoom::parameterRange(index).max == 1.0);
        REQUIRE(subdoom::parameterRange(index).step >= 1.0);
    }

    REQUIRE(switches == 3);
    REQUIRE(subdoom::parameterKind(2) == subdoom::ParameterKind::toggle);
    REQUIRE(subdoom::parameterKind(20) == subdoom::ParameterKind::toggle);
    REQUIRE(subdoom::parameterKind(21) == subdoom::ParameterKind::toggle);
    REQUIRE(subdoom::parameterKind(19) == subdoom::ParameterKind::continuous);
    REQUIRE(subdoom::kHostParameterOrder[0] == 19);
    REQUIRE(subdoom::parameterIsSwitch(2));    // gate
    REQUIRE(subdoom::parameterIsSwitch(20));   // limiter
    REQUIRE(subdoom::parameterIsSwitch(21));   // mute
    REQUIRE_FALSE(subdoom::parameterIsSwitch(3));  // sustain is continuous 0..1
    REQUIRE_FALSE(subdoom::parameterIsSwitch(4));  // split steps in whole Hz
    REQUIRE_FALSE(subdoom::parameterIsSwitch(15)); // cut steps in 10 Hz
}

TEST_CASE("factory presets carry a browsing category", "[presets]")
{
    for (int index = 0; index < subdoom::kFactoryPresetCount; ++index)
    {
        INFO(subdoom::presetName(index));
        const std::string category = subdoom::presetCategory(index);
        REQUIRE(category.size() >= 3);
        REQUIRE(category.size() <= 16);
    }
}

namespace {

int peakBand(const std::array<float, subdoom::ScopeMeter::kSpectrumBands>& bands)
{
    int best = 0;
    for (int index = 1; index < static_cast<int>(bands.size()); ++index)
        if (bands[static_cast<std::size_t>(index)] > bands[static_cast<std::size_t>(best)])
            best = index;
    return best;
}

std::array<float, subdoom::ScopeMeter::kSpectrumBands> spectrumOfTone(double hz)
{
    constexpr double rate = 48000.0;
    subdoom::ScopeMeter meter;
    meter.prepare(rate);

    const int total = static_cast<int>(rate);
    for (int frame = 0; frame < total; ++frame)
    {
        const double t = static_cast<double>(frame) / rate;
        const float value = static_cast<float>(kNominal * std::sin(2.0 * subdoom::kPi * hz * t));
        meter.push(0.0f, value);
    }
    return meter.spectrum();
}

} // namespace

TEST_CASE("the meter spectrum follows the played fundamental", "[meter]")
{
    const auto low = spectrumOfTone(58.0);
    const auto high = spectrumOfTone(1000.0);

    const int lowPeak = peakBand(low);
    const int highPeak = peakBand(high);
    INFO("low peak band " << lowPeak << ", high peak band " << highPeak);

    REQUIRE(low[static_cast<std::size_t>(lowPeak)] > 1.0e-3f);
    REQUIRE(high[static_cast<std::size_t>(highPeak)] > 1.0e-3f);
    REQUIRE(lowPeak < 9);
    REQUIRE(highPeak > 12);

    for (const float band : low)
        REQUIRE(std::isfinite(band));
    for (const float band : high)
        REQUIRE(std::isfinite(band));
}

TEST_CASE("scope rings advance and reset", "[meter]")
{
    subdoom::ScopeMeter meter;
    meter.prepare(48000.0);

    constexpr int pushes = 600;
    for (int index = 0; index < pushes; ++index)
        meter.push(0.25f, (index % 2 == 0) ? 0.5f : -0.5f);

    constexpr int expectedWrites = pushes / 6;
    REQUIRE(meter.inputWriteIndex() == expectedWrites);
    REQUIRE(meter.outputWriteIndex() == expectedWrites);
    REQUIRE(meter.inputBuffer()[static_cast<std::size_t>(expectedWrites - 1)].load()
            == 0.25f);
    REQUIRE(meter.outputBuffer()[static_cast<std::size_t>(expectedWrites - 1)].load()
            == -0.5f);

    meter.reset();
    REQUIRE(meter.inputWriteIndex() == 0);
    REQUIRE(meter.outputWriteIndex() == 0);
    for (const auto& sample : meter.outputBuffer())
        REQUIRE(sample.load() == 0.0f);
}




namespace {

// How much the drive stage changes a bass-like signal relative to the same
// preset with both drives at zero. A dB or two is a clean preset; the heavy
// ones have to move the waveform substantially or the name is a lie.
double driveChangeForPreset(int preset)
{
    const subdoom::Parameters params = subdoom::factoryPreset(preset);
    subdoom::Parameters clean = params;
    clean.lowDrive = 0.0;
    clean.highDrive = 0.0;

    const auto bass = [](int index, double rate) {
        const double t = static_cast<double>(index) / rate;
        double value = 0.0;
        for (int harmonic = 1; harmonic <= 8; ++harmonic)
            value += std::sin(2.0 * subdoom::kPi * 58.0 * harmonic * t) / harmonic;
        return kNominal * value / 1.6;
    };

    const Rendered driven = render(params, 48000.0, 48000, bass);
    const Rendered dry = render(clean, 48000.0, 48000, bass);

    const int start = 24000;
    const int count = 22050;
    double difference = 0.0;
    double reference = 0.0;
    for (int index = 0; index < count; ++index)
    {
        const double a = driven.samples[static_cast<std::size_t>(start + index)];
        const double b = dry.samples[static_cast<std::size_t>(start + index)];
        difference += (a - b) * (a - b);
        reference += b * b;
    }
    return std::sqrt(difference / std::max(1.0e-12, reference));
}

} // namespace

TEST_CASE("the heavy presets are driven and the clean ones are not", "[drive]")
{
    // Funeral Wall, Dragon Lung, Serpent, Caveman.
    const int heavy[] = { 3, 4, 5, 9 };
    for (const int preset : heavy)
    {
        INFO(subdoom::presetName(preset));
        REQUIRE(driveChangeForPreset(preset) >= 0.60);
    }

    // Giza is the clean fundamental with a soft octave; Red Sun is the punchy one.
    INFO("GIZA");
    REQUIRE(driveChangeForPreset(6) <= 0.10);
    INFO("RED SUN");
    REQUIRE(driveChangeForPreset(2) <= 0.30);
}