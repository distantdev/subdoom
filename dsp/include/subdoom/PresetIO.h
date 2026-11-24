#pragma once

#include "subdoom/Parameters.h"

#include <string>

namespace subdoom {

// User presets are plain text so they diff, survive a text editor, and never
// need the plugin to read them back. Factory presets stay compiled in.
inline constexpr const char* kPresetExtension = ".subdoom";
inline constexpr const char* kPresetHeader = "subdoom-preset";
inline constexpr int kPresetFormatVersion = 1;

struct PresetDocument
{
    std::string name = "UNTITLED";
    std::string category = "User";
    std::string notes;
    std::string created;
    // -1 leaves this preset out of MIDI program-change switching.
    int program = -1;
    Parameters params;
};

enum class PresetParseResult
{
    ok,
    notPreset,
    unsupportedVersion,
    empty
};

struct PresetParseReport
{
    PresetParseResult result = PresetParseResult::ok;
    int unknownKeys = 0;
    int ignoredValues = 0;
    std::string detail;

    bool ok() const noexcept { return result == PresetParseResult::ok; }
};

std::string writePresetText(const PresetDocument& document);
PresetParseReport readPresetText(const std::string& text, PresetDocument& document);

// Trimmed, newline-free copy of user-supplied text, capped at maxLength bytes.
std::string sanitisePresetText(const std::string& text, std::size_t maxLength);

} // namespace subdoom
