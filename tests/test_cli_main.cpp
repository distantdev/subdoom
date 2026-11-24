#include "subdoom/Processor.h"
#include "subdoom/Saturation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Options
{
    double sampleRate = 48000.0;
    int blockSize = 512;
    int blocks = 32;
    std::string signal = "impulse";
    int preset = 0;
    bool json = false;
    bool selfTest = false;
    bool loudness = false;
};

bool parseDouble(const std::string& text, double& value) noexcept
{
    try
    {
        std::size_t used = 0;
        const double parsed = std::stod(text, &used);
        if (used != text.size() || !std::isfinite(parsed))
            return false;
        value = parsed;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool parseInteger(const std::string& text, int& value) noexcept
{
    try
    {
        std::size_t used = 0;
        const int parsed = std::stoi(text, &used);
        if (used != text.size())
            return false;
        value = parsed;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void printUsage()
{
    std::cerr
        << "subdoom_test_cli --signal impulse|sine|noise|zero [--blocks N] [--block-size N]\n"
        << "                  [--sample-rate Hz] [--preset N] [--json] [--self-test]\n"
        << "       subdoom_test_cli --loudness   (RMS and peak per factory preset)\n";
}

bool parseArgs(int argc, char** argv, Options& options)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc)
            {
                std::cerr << "missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--json")
            options.json = true;
        else if (arg == "--loudness")
            options.loudness = true;
        else if (arg == "--self-test")
            options.selfTest = true;
        else if (arg == "--signal")
        {
            const char* value = need("--signal");
            if (value == nullptr)
                return false;
            if (std::string(value) != "impulse" && std::string(value) != "sine"
                && std::string(value) != "noise" && std::string(value) != "zero")
            {
                std::cerr << "unknown signal " << value << "\n";
                printUsage();
                return false;
            }
            options.signal = value;
        }
        else if (arg == "--sample-rate")
        {
            const char* value = need("--sample-rate");
            if (value == nullptr || !parseDouble(value, options.sampleRate)
                || options.sampleRate < 8000.0 || options.sampleRate > 384000.0)
            {
                std::cerr << "invalid --sample-rate (8000 to 384000)\n";
                return false;
            }
        }
        else if (arg == "--block-size")
        {
            const char* value = need("--block-size");
            if (value == nullptr || !parseInteger(value, options.blockSize)
                || options.blockSize < 1 || options.blockSize > 65536)
            {
                std::cerr << "invalid --block-size (1 to 65536)\n";
                return false;
            }
        }
        else if (arg == "--blocks")
        {
            const char* value = need("--blocks");
            if (value == nullptr || !parseInteger(value, options.blocks)
                || options.blocks < 1 || options.blocks > 1000000)
            {
                std::cerr << "invalid --blocks (1 to 1000000)\n";
                return false;
            }
        }
        else if (arg == "--preset")
        {
            const char* value = need("--preset");
            if (value == nullptr || !parseInteger(value, options.preset)
                || options.preset < -1 || options.preset >= subdoom::kFactoryPresetCount)
            {
                std::cerr << "invalid --preset (-1 for defaults, 0 to "
                          << subdoom::kFactoryPresetCount - 1 << ")\n";
                return false;
            }
        }
        else if (arg == "--help" || arg == "-h")
        {
            printUsage();
            return false;
        }
        else
        {
            std::cerr << "unknown argument " << arg << "\n";
            printUsage();
            return false;
        }
    }
    return true;
}

void renderSignal(const std::string& kind, int index, double sampleRate, double& left, double& right)
{
    if (kind == "zero")
    {
        left = 0.0;
        right = 0.0;
    }
    else if (kind == "impulse")
    {
        left = index == 0 ? 1.0 : 0.0;
        right = left;
    }
    else if (kind == "sine")
    {
        const double phase = 2.0 * subdoom::kPi * 32.70 * static_cast<double>(index) / sampleRate;
        left = 0.063095734448019324 * std::sin(phase);
        right = left;
    }
    else
    {
        const uint32_t n = static_cast<uint32_t>(index) * 1664525u + 1013904223u;
        const double unit = (static_cast<double>(n >> 8) / static_cast<double>(1u << 24)) * 2.0 - 1.0;
        left = unit * 0.25;
        right = left;
    }
}

subdoom::BlockMetrics run(const Options& options, const subdoom::Parameters& params)
{
    subdoom::Processor processor;
    processor.prepare(options.sampleRate, options.blockSize);
    processor.setParameters(params);
    processor.reset();

    subdoom::BlockMetrics metrics;
    std::vector<double> left(static_cast<std::size_t>(options.blockSize));
    std::vector<double> right(static_cast<std::size_t>(options.blockSize));
    int frame = 0;
    for (int block = 0; block < options.blocks; ++block)
    {
        for (int i = 0; i < options.blockSize; ++i, ++frame)
            renderSignal(options.signal, frame, options.sampleRate,
                         left[static_cast<std::size_t>(i)], right[static_cast<std::size_t>(i)]);
        processor.process(left.data(), right.data(), options.blockSize, &metrics);
    }
    return metrics;
}

void printJson(const subdoom::BlockMetrics& metrics)
{
    const bool passed = metrics.nonFinite == 0 && std::isfinite(metrics.peak);
    std::cout << std::fixed;
    std::cout << "{\n"
              << "  \"compiled\": true,\n"
              << "  \"has_nan_inf\": " << (metrics.nonFinite > 0 ? "true" : "false") << ",\n"
              << "  \"metrics\": {\n"
              << "    \"peak_dbfs\": " << metrics.peakDbfs() << ",\n"
              << "    \"non_finite\": " << metrics.nonFinite << ",\n"
              << "    \"passed_audio\": " << (passed ? "true" : "false") << "\n"
              << "  }\n"
              << "}\n";
}

bool expectFinite(const char* name, const subdoom::BlockMetrics& metrics, double peakLimit)
{
    if (metrics.nonFinite != 0 || !std::isfinite(metrics.peak) || metrics.peak > peakLimit)
    {
        std::cerr << "FAIL " << name
                  << " nonfinite=" << metrics.nonFinite
                  << " peak=" << metrics.peak
                  << "\n";
        return false;
    }
    std::cout << "ok " << name << " peak=" << metrics.peak << "\n";
    return true;
}

int selfTest()
{
    int failed = 0;
    if (subdoom::kParameterCount != 21)
    {
        std::cerr << "FAIL parameter count " << subdoom::kParameterCount << "\n";
        ++failed;
    }
    if (subdoom::kFactoryPresetCount != 10 || subdoom::presetName(0) != "LOW MASS"
        || subdoom::presetName(9) != "CAVEMAN")
    {
        std::cerr << "FAIL factory preset list\n";
        ++failed;
    }
    else
    {
        std::cout << "ok parameters and presets\n";
    }

    const double sat = subdoom::saturate(0.25);
    if (!std::isfinite(sat) || !std::isfinite(subdoom::saturate(80.0)) || std::abs(sat - 0.25) > 0.02)
    {
        std::cerr << "FAIL saturate\n";
        ++failed;
    }
    else
    {
        std::cout << "ok saturate\n";
    }

    double parsedRate = 0.0;
    int parsedBlocks = 0;
    if (parseDouble("abc", parsedRate) || parseDouble("", parsedRate)
        || parseDouble("48000x", parsedRate) || parseDouble("nan", parsedRate)
        || !parseDouble("48000", parsedRate) || parsedRate != 48000.0
        || parseInteger("12x", parsedBlocks) || parseInteger("", parsedBlocks)
        || parseInteger("1e3", parsedBlocks) || !parseInteger("32", parsedBlocks)
        || parsedBlocks != 32)
    {
        std::cerr << "FAIL argument parsing\n";
        ++failed;
    }
    else
    {
        std::cout << "ok argument parsing\n";
    }

    Options silence;
    silence.signal = "zero";
    silence.blocks = 48;
    const auto quiet = run(silence, subdoom::factoryPreset(0));
    if (!expectFinite("silence", quiet, 1.0e-5))
        ++failed;
    if (quiet.peakDbfs() >= -100.0)
    {
        std::cerr << "FAIL silence level " << quiet.peakDbfs() << " dBFS\n";
        ++failed;
    }

    Options impulse;
    impulse.signal = "impulse";
    impulse.blocks = 64;
    if (!expectFinite("impulse", run(impulse, subdoom::factoryPreset(3)), 2.0))
        ++failed;

    Options noise;
    noise.signal = "noise";
    noise.blocks = 48;
    if (!expectFinite("noise", run(noise, subdoom::factoryPreset(9)), 2.0))
        ++failed;

    if (failed == 0)
        std::cout << "self-test passed\n";
    return failed == 0 ? 0 : 1;
}

// ITU-R BS.1770 K-weighting for 48 kHz, as a transposed direct form II biquad.
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    double process(double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

struct LoudnessReading
{
    double rmsDb = -120.0;
    double peakDb = -120.0;
    double lufs = -120.0;
};

// A warm bass voice: the fundamental plus four harmonics. Loudness matching has
// to hear the same harmonics the drive and the body EQ add, which a sine cannot.
double bassVoice(int index, double sampleRate, bool plucked)
{
    const double f = 32.70;
    // -12 dB on the whole voice, so the probe peaks near the -24 dBFS target.
    const auto tone = [](double phase) {
        constexpr double align = 0.25118864315095801;
        return align * (0.18 * std::sin(phase) + 0.09 * std::sin(2.0 * phase)
            + 0.05 * std::sin(3.0 * phase) + 0.03 * std::sin(4.0 * phase)
            + 0.02 * std::sin(5.0 * phase));
    };

    const double t = static_cast<double>(index) / sampleRate;
    if (!plucked)
        return tone(2.0 * subdoom::kPi * f * t);

    // One note every 500 ms: 5 ms attack, then a slow decay into the next note.
    constexpr double noteLength = 0.5;
    const double position = std::fmod(t, noteLength);
    const double envelope = std::min(1.0, position / 0.005) * std::exp(-position * 4.0);
    return tone(2.0 * subdoom::kPi * f * position) * envelope;
}

LoudnessReading measureLoudness(const subdoom::Parameters& params, bool plucked)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;
    constexpr double seconds = 3.0;
    constexpr double settleSeconds = 1.0;

    subdoom::Processor processor;
    processor.prepare(sampleRate, blockSize);
    processor.setParameters(params);
    processor.reset();

    std::vector<double> left(static_cast<std::size_t>(blockSize));
    std::vector<double> right(static_cast<std::size_t>(blockSize));
    const int total = static_cast<int>(sampleRate * seconds);
    const int settle = static_cast<int>(sampleRate * settleSeconds);

    Biquad shelf { 1.53512485958697, -2.69169618940638, 1.19839281085285,
                   -1.69065929318241, 0.73248077421585, 0.0, 0.0 };
    Biquad highpass { 1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621, 0.0, 0.0 };

    double sumSquares = 0.0;
    double weightedSquares = 0.0;
    double peak = 0.0;
    int counted = 0;
    int frame = 0;
    while (frame < total)
    {
        const int count = std::min(blockSize, total - frame);
        for (int i = 0; i < count; ++i)
        {
            const double sample = bassVoice(frame + i, sampleRate, plucked);
            left[static_cast<std::size_t>(i)] = sample;
            right[static_cast<std::size_t>(i)] = sample;
        }
        processor.process(left.data(), right.data(), count);

        for (int i = 0; i < count; ++i)
        {
            if (frame + i < settle)
                continue;
            const double sample = left[static_cast<std::size_t>(i)];
            sumSquares += sample * sample;
            const double weighted = highpass.process(shelf.process(sample));
            weightedSquares += weighted * weighted;
            peak = std::max(peak, std::abs(sample));
            ++counted;
        }
        frame += count;
    }

    LoudnessReading reading;
    if (counted > 0)
        reading.rmsDb = subdoom::gainToDb(std::sqrt(sumSquares / counted));
    if (peak > 0.0)
        reading.peakDb = subdoom::gainToDb(peak);
    if (counted > 0)
        reading.lufs = 10.0 * std::log10(weightedSquares / counted + 1.0e-20) - 0.691;
    return reading;
}

int loudnessReport()
{
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "preset          master    held rms   held lufs   pluck rms  pluck lufs"
                 "   held peak\n";
    for (int i = 0; i < subdoom::kFactoryPresetCount; ++i)
    {
        const subdoom::Parameters params = subdoom::factoryPreset(i);
        const LoudnessReading held = measureLoudness(params, false);
        const LoudnessReading plucked = measureLoudness(params, true);
        std::cout << std::left << std::setw(15) << subdoom::presetName(i) << std::right
                  << std::setw(7) << params.masterDb
                  << std::setw(10) << held.rmsDb << std::setw(12) << held.lufs
                  << std::setw(12) << plucked.rmsDb << std::setw(12) << plucked.lufs
                  << std::setw(12) << held.peakDb << "\n";
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parseArgs(argc, argv, options))
        return 1;
    if (options.selfTest)
        return selfTest();
    if (options.loudness)
        return loudnessReport();

    const subdoom::Parameters params = options.preset >= 0
        ? subdoom::factoryPreset(options.preset)
        : subdoom::Parameters {};
    const subdoom::BlockMetrics metrics = run(options, params);
    if (options.json)
        printJson(metrics);
    else
    {
        std::cout << "peak_dbfs " << metrics.peakDbfs()
                  << " non_finite " << metrics.nonFinite << "\n";
    }
    return metrics.nonFinite == 0 && std::isfinite(metrics.peak) ? 0 : 1;
}
