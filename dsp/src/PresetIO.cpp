#include "subdoom/PresetIO.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace subdoom {
namespace {

std::string trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool isBlankOrComment(const std::string& line)
{
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos)
        return true;
    return line[first] == '#';
}

// Parameter ids never contain '=', so a preset value may.
bool splitKeyValue(const std::string& line, std::string& key, std::string& value)
{
    const auto equals = line.find('=');
    if (equals == std::string::npos)
        return false;
    key = trim(line.substr(0, equals));
    value = trim(line.substr(equals + 1));
    return !key.empty();
}

int parameterIndexForId(const std::string& id)
{
    for (int index = 1; index <= kParameterCount; ++index)
        if (id == parameterId(index))
            return index;
    return 0;
}

bool parseNumber(const std::string& text, double& value)
{
    if (text.empty())
        return false;
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str())
        return false;
    if (!std::isfinite(parsed))
        return false;
    const std::string rest = trim(std::string(end));
    if (!rest.empty())
        return false;
    value = parsed;
    return true;
}

std::string formatValue(double value)
{
    char buffer[64] {};
    std::snprintf(buffer, sizeof(buffer), "%.10g", value);
    return buffer;
}

bool parseHeader(const std::string& line, PresetParseReport& report)
{
    const std::size_t headerLength = std::char_traits<char>::length(kPresetHeader);
    if (line.compare(0, headerLength, kPresetHeader) != 0
        || (line.size() > headerLength && line[headerLength] != ' '
            && line[headerLength] != '\t'))
    {
        report.result = PresetParseResult::notPreset;
        report.detail = "missing subdoom-preset header";
        return false;
    }

    const std::string version = trim(line.substr(headerLength));
    double parsed = 0.0;
    if (!parseNumber(version, parsed) || parsed < 1.0)
    {
        report.result = PresetParseResult::unsupportedVersion;
        report.detail = "unreadable format version";
        return false;
    }
    if (parsed > static_cast<double>(kPresetFormatVersion))
    {
        report.result = PresetParseResult::unsupportedVersion;
        report.detail = "preset was written by a newer Subdoom";
        return false;
    }
    return true;
}

} // namespace

std::string sanitisePresetText(const std::string& text, std::size_t maxLength)
{
    std::string clean;
    clean.reserve(std::min(text.size(), maxLength));
    for (const char character : text)
    {
        if (clean.size() >= maxLength)
            break;
        const unsigned char code = static_cast<unsigned char>(character);
        if (code == '\r' || code == '\n' || code == '\t')
            clean.push_back(' ');
        else if (code >= 0x20)
            clean.push_back(character);
    }
    return trim(clean);
}

std::string writePresetText(const PresetDocument& document)
{
    PresetDocument safe;
    safe.params = document.params;
    safe.params.clampIntoRange();
    safe.name = sanitisePresetText(document.name, 64);
    safe.category = sanitisePresetText(document.category, 32);
    safe.notes = sanitisePresetText(document.notes, 480);
    safe.created = sanitisePresetText(document.created, 40);
    if (safe.name.empty())
        safe.name = "UNTITLED";
    if (safe.category.empty())
        safe.category = "User";

    std::string text = std::string(kPresetHeader) + " " + std::to_string(kPresetFormatVersion) + "\n";
    text += "name=" + safe.name + "\n";
    text += "category=" + safe.category + "\n";
    const int program = document.program >= 0 && document.program <= 127 ? document.program : -1;
    text += "pc=" + (program < 0 ? std::string("-") : std::to_string(program)) + "\n";
    if (!safe.created.empty())
        text += "created=" + safe.created + "\n";
    if (!safe.notes.empty())
        text += "notes=" + safe.notes + "\n";

    for (int index = 1; index <= kParameterCount; ++index)
    {
        // The host hands back float32 values, so snap to the parameter step
        // before printing: 0.84, not 0.8399999738. A file that diffs is a file
        // a user can read.
        const ParameterRange range = parameterRange(index);
        double value = safe.params.at(index);
        if (range.step > 0.0)
            value = range.min + std::round((value - range.min) / range.step) * range.step;
        value = clamp(value, range.min, range.max);
        text += std::string(parameterId(index)) + "=" + formatValue(value) + "\n";
    }

    return text;
}

PresetParseReport readPresetText(const std::string& text, PresetDocument& document)
{
    PresetParseReport report;
    document = PresetDocument {};

    bool haveHeader = false;

    std::size_t at = 0;
    while (at <= text.size())
    {
        const std::size_t end = text.find('\n', at);
        std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        at = end == std::string::npos ? text.size() + 1 : end + 1;

        if (isBlankOrComment(line))
            continue;

        if (!haveHeader)
        {
            haveHeader = true;
            if (!parseHeader(trim(line), report))
                return report;
            continue;
        }

        std::string key;
        std::string value;
        if (!splitKeyValue(line, key, value))
        {
            ++report.ignoredValues;
            continue;
        }

        if (key == "name")
        {
            document.name = sanitisePresetText(value, 64);
        }
        else if (key == "category")
        {
            document.category = sanitisePresetText(value, 32);
        }
        else if (key == "notes")
        {
            document.notes = sanitisePresetText(value, 480);
        }
        else if (key == "created")
        {
            document.created = sanitisePresetText(value, 40);
        }
        else if (key == "pc")
        {
            // Missing and blank both stay at -1. A bad value is ignored, same as
            // any other value this build does not understand.
            if (value == "-" || value.empty())
            {
                document.program = -1;
            }
            else
            {
                double parsed = 0.0;
                if (!parseNumber(value, parsed))
                {
                    ++report.ignoredValues;
                }
                else
                {
                    const int code = static_cast<int>(std::lround(parsed));
                    if (code < 0 || code > 127)
                        ++report.ignoredValues;
                    else
                        document.program = code;
                }
            }
        }
        else if (const int index = parameterIndexForId(key); index != 0)
        {
            double parsed = 0.0;
            if (parseNumber(value, parsed))
                document.params.at(index) = parsed;
            else
                ++report.ignoredValues;
        }
        else
        {
            ++report.unknownKeys;
        }
    }

    if (!haveHeader)
    {
        report.result = PresetParseResult::empty;
        report.detail = "no preset content";
        return report;
    }

    document.params.clampIntoRange();
    if (document.name.empty())
        document.name = "UNTITLED";
    if (document.category.empty())
        document.category = "User";

    report.result = PresetParseResult::ok;
    return report;
}

} // namespace subdoom
