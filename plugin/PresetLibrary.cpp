#include "PresetLibrary.h"

#include <algorithm>
#include <utility>

namespace {

constexpr juce::int64 kMaxPresetBytes = 256 * 1024;
constexpr const char* kFactoryProgramMap = "program-map.txt";

juce::String describeParse(const subdoom::PresetParseReport& report)
{
    switch (report.result)
    {
        case subdoom::PresetParseResult::ok: return {};
        case subdoom::PresetParseResult::notPreset: return "Not a Subdoom preset";
        case subdoom::PresetParseResult::unsupportedVersion:
            return "Preset version is newer than this build";
        case subdoom::PresetParseResult::empty: return "Preset file is empty";
    }
    return "Unreadable preset";
}

} // namespace

PresetLibrary::PresetLibrary(juce::File rootFolder)
    : folder (rootFolder.getFullPathName().isEmpty() ? userFolder() : std::move(rootFolder))
{
    reload();
}

juce::File PresetLibrary::userFolder()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Subdoom")
        .getChildFile("Presets");
}

juce::String PresetLibrary::safeFileName(const juce::String& name)
{
    juce::String token = name.trim();
    token = token.retainCharacters(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_+().");
    token = token.replaceCharacter(' ', '-');

    while (token.contains("--"))
        token = token.replace("--", "-");

    token = token.trimCharactersAtStart("-.");
    token = token.trimCharactersAtEnd("-. ");
    if (token.length() > 64)
        token = token.substring(0, 64).trimCharactersAtEnd("-. ");
    if (token.isEmpty())
        token = "preset";
    return token;
}

juce::File PresetLibrary::fileFor(const juce::String& name) const
{
    return folder.getChildFile(safeFileName(name) + subdoom::kPresetExtension);
}

void PresetLibrary::reload()
{
    entries.clear();
    entries.reserve(static_cast<std::size_t>(subdoom::kFactoryPresetCount) + 16);

    for (int index = 0; index < subdoom::kFactoryPresetCount; ++index)
    {
        Entry entry;
        entry.name = juce::String(subdoom::presetName(index));
        entry.category = juce::String(subdoom::presetCategory(index));
        entry.params = subdoom::factoryPreset(index);
        entry.factory = true;
        entry.factorySlot = index;
        entry.program = defaultFactoryProgram(index);
        entries.push_back(std::move(entry));
    }

    if (folder.isDirectory())
    {
        juce::Array<juce::File> files;
        folder.findChildFiles(files, juce::File::findFiles, false,
                              juce::String("*") + subdoom::kPresetExtension);

        for (const auto& file : files)
        {
            Entry entry;
            juce::String error;
            if (readFile(file, entry, error))
                entries.push_back(std::move(entry));
        }
    }

    std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
        const int byName = left.name.compareIgnoreCase(right.name);
        if (byName != 0)
            return byName < 0;
        if (left.factory != right.factory)
            return left.factory;
        return left.factorySlot < right.factorySlot;
    });

    applyFactoryProgramMap();
}

int PresetLibrary::indexOfFactorySlot(int slot) const
{
    if (slot < 0)
        return -1;
    for (int index = 0; index < size(); ++index)
    {
        const Entry& entry = entries[static_cast<std::size_t>(index)];
        if (entry.factory && entry.factorySlot == slot)
            return index;
    }
    return -1;
}

int PresetLibrary::defaultFactoryProgram(int index) noexcept
{
    return index >= 0 && index <= 127 ? index : -1;
}

int PresetLibrary::programCode(int index) const
{
    if (index < 0 || index >= size())
        return -1;
    return entries[static_cast<std::size_t>(index)].program;
}

int PresetLibrary::indexForProgram(int code) const
{
    if (code < 0 || code > 127)
        return -1;
    for (int index = 0; index < size(); ++index)
        if (entries[static_cast<std::size_t>(index)].program == code)
            return index;
    return -1;
}

void PresetLibrary::applyFactoryProgramMap()
{
    const juce::File file = folder.getChildFile(kFactoryProgramMap);
    if (!file.existsAsFile())
        return;

    const juce::StringArray lines = juce::StringArray::fromLines(file.loadFileAsString());
    for (const auto& raw : lines)
    {
        const juce::String line = raw.trim();
        if (line.isEmpty() || line.startsWithChar('#'))
            continue;
        const int eq = line.indexOfChar('=');
        if (eq <= 0)
            continue;

        const int slot = line.substring(0, eq).trim().getIntValue();
        const int index = indexOfFactorySlot(slot);
        if (index < 0)
            continue;

        const juce::String value = line.substring(eq + 1).trim();
        int code = -1;
        if (value != "-" && value.isNotEmpty())
        {
            if (!value.containsOnly("0123456789"))
                continue;
            code = value.getIntValue();
            if (code > 127)
                continue;
        }
        entries[static_cast<std::size_t>(index)].program = code;
    }
}

bool PresetLibrary::saveFactoryProgramMap(juce::String& error)
{
    juce::String text;
    text << "# Subdoom factory program changes. factory=pc, or factory=- for none.\n";
    bool any = false;
    for (int slot = 0; slot < subdoom::kFactoryPresetCount; ++slot)
    {
        const int index = indexOfFactorySlot(slot);
        if (index < 0)
            continue;
        const int code = entries[static_cast<std::size_t>(index)].program;
        if (code == defaultFactoryProgram(slot))
            continue;
        any = true;
        text << slot << "=" << (code < 0 ? "-" : juce::String(code)) << "\n";
    }

    const juce::File file = folder.getChildFile(kFactoryProgramMap);
    if (!any)
    {
        if (file.existsAsFile() && !file.deleteFile())
        {
            error = "Cannot update " + file.getFullPathName();
            return false;
        }
        return true;
    }

    const juce::File parent = file.getParentDirectory();
    if (!parent.isDirectory())
    {
        const juce::Result created = parent.createDirectory();
        if (created.failed())
        {
            error = "Cannot create " + parent.getFullPathName() + ": " + created.getErrorMessage();
            return false;
        }
    }
    if (!file.replaceWithText(text, false, false, "\n"))
    {
        error = "Cannot write " + file.getFullPathName();
        return false;
    }
    return true;
}

bool PresetLibrary::setProgramCode(int index, int code, juce::String& error)
{
    error.clear();
    if (index < 0 || index >= size())
    {
        error = "No preset selected";
        return false;
    }
    if (code < -1 || code > 127)
    {
        error = "PC is 0-127, or blank";
        return false;
    }
    if (entries[static_cast<std::size_t>(index)].program == code)
        return true;

    std::vector<int> changed;
    if (code >= 0)
    {
        for (int other = 0; other < size(); ++other)
        {
            if (other == index)
                continue;
            if (entries[static_cast<std::size_t>(other)].program == code)
            {
                entries[static_cast<std::size_t>(other)].program = -1;
                changed.push_back(other);
            }
        }
    }

    entries[static_cast<std::size_t>(index)].program = code;
    changed.push_back(index);

    for (const int item : changed)
    {
        const Entry& entry = entries[static_cast<std::size_t>(item)];
        if (entry.factory)
            continue;
        if (!writeFile(entry.file, entry.name, entry.category, entry.notes,
                       entry.params, entry.program, error))
            return false;
    }
    return saveFactoryProgramMap(error);
}

const PresetLibrary::Entry& PresetLibrary::entry(int index) const
{
    if (index < 0 || index >= size())
    {
        static const Entry empty;
        return empty;
    }
    return entries[static_cast<std::size_t>(index)];
}

int PresetLibrary::indexOfName(const juce::String& name, bool factory) const
{
    for (int index = 0; index < size(); ++index)
        if (entries[static_cast<std::size_t>(index)].factory == factory
            && entries[static_cast<std::size_t>(index)].name.equalsIgnoreCase(name))
            return index;
    return -1;
}

int PresetLibrary::indexOfFile(const juce::File& file) const
{
    for (int index = 0; index < size(); ++index)
    {
        const auto& entry = entries[static_cast<std::size_t>(index)];
        if (!entry.factory && entry.file == file)
            return index;
    }
    return -1;
}

juce::StringArray PresetLibrary::categories() const
{
    juce::StringArray result;
    for (const auto& entry : entries)
        if (!result.contains(entry.category, true))
            result.add(entry.category);
    result.sort(true);
    return result;
}

bool PresetLibrary::writeFile(const juce::File& file,
                              const juce::String& name,
                              const juce::String& category,
                              const juce::String& notes,
                              const subdoom::Parameters& params,
                              int program,
                              juce::String& error)
{
    error.clear();
    const auto parent = file.getParentDirectory();
    if (!parent.isDirectory())
    {
        const juce::Result created = parent.createDirectory();
        if (created.failed())
        {
            error = "Cannot create " + parent.getFullPathName() + ": " + created.getErrorMessage();
            return false;
        }
    }

    subdoom::PresetDocument document;
    document.name = name.trim().toStdString();
    document.category = category.trim().toStdString();
    document.notes = notes.toStdString();
    document.created = juce::Time::getCurrentTime().toISO8601(true).toStdString();
    document.params = params;
    document.program = program >= 0 && program <= 127 ? program : -1;

    const std::string text = subdoom::writePresetText(document);
    if (!file.replaceWithText(juce::String(text), false, false, "\n"))
    {
        error = "Cannot write " + file.getFullPathName();
        return false;
    }
    return true;
}

bool PresetLibrary::readFile(const juce::File& file, Entry& entry, juce::String& error)
{
    error.clear();
    if (!file.existsAsFile())
    {
        error = "Missing file " + file.getFullPathName();
        return false;
    }
    if (file.getSize() > kMaxPresetBytes)
    {
        error = file.getFileName() + " is too large for a preset";
        return false;
    }

    subdoom::PresetDocument document;
    const subdoom::PresetParseReport report =
        subdoom::readPresetText(file.loadFileAsString().toStdString(), document);
    if (!report.ok())
    {
        error = file.getFileName() + ": " + describeParse(report);
        return false;
    }

    entry.name = juce::String(document.name);
    entry.category = juce::String(document.category);
    entry.notes = juce::String(document.notes);
    entry.params = document.params;
    entry.program = document.program;
    entry.factory = false;
    entry.file = file;
    return true;
}

bool PresetLibrary::saveUser(const juce::String& name,
                             const juce::String& category,
                             const juce::String& notes,
                             const subdoom::Parameters& params,
                             bool allowOverwrite,
                             int& savedIndex,
                             juce::String& error)
{
    error.clear();
    savedIndex = -1;

    const juce::String trimmed = name.trim();
    if (trimmed.isEmpty())
    {
        error = "Give the preset a name";
        return false;
    }

    const juce::File file = fileFor(trimmed);
    if (file.existsAsFile() && !allowOverwrite)
    {
        error = "A preset called " + trimmed + " already exists";
        return false;
    }
    int program = -1;
    if (file.existsAsFile())
    {
        Entry existing;
        juce::String ignored;
        if (readFile(file, existing, ignored))
            program = existing.program;
    }
    if (!writeFile(file, trimmed, category.trim().isEmpty() ? "User" : category, notes, params,
                   program, error))
        return false;

    reload();

    savedIndex = indexOfFile(file);
    if (savedIndex < 0)
        savedIndex = indexOfName(
            juce::String(subdoom::sanitisePresetText(trimmed.toStdString(), 64)), false);
    if (savedIndex < 0)
    {
        error = "Could not read " + file.getFileName() + " back after saving";
        return false;
    }
    return true;
}

bool PresetLibrary::overwrite(int index, const juce::String& notes,
                              const subdoom::Parameters& params, juce::String& error)
{
    error.clear();
    if (index < 0 || index >= size())
    {
        error = "No preset selected";
        return false;
    }
    const Entry& target = entries[static_cast<std::size_t>(index)];
    if (target.factory)
    {
        error = "Factory presets cannot be overwritten; save a copy instead";
        return false;
    }
    const juce::File file = target.file;
    const juce::String storedName = target.name;
    const juce::String storedCategory = target.category;
    const int program = target.program;
    if (!writeFile(file, storedName, storedCategory, notes, params, program, error))
        return false;

    reload();
    if (indexOfFile(file) < 0)
    {
        error = "Could not read " + file.getFileName() + " back after saving";
        return false;
    }
    return true;
}

bool PresetLibrary::renameUser(int index, const juce::String& newName, int& renamedIndex,
                               juce::String& error)
{
    error.clear();
    renamedIndex = index;

    if (index < 0 || index >= size())
    {
        error = "No preset selected";
        return false;
    }
    const Entry& target = entries[static_cast<std::size_t>(index)];
    if (target.factory)
    {
        error = "Factory presets cannot be renamed";
        return false;
    }

    const juce::String trimmed = newName.trim();
    if (trimmed.isEmpty())
    {
        error = "Give the preset a name";
        return false;
    }
    if (trimmed.equalsIgnoreCase(target.name))
        return true;

    const juce::File destination = fileFor(trimmed);
    if (destination.existsAsFile() && destination != target.file)
    {
        error = "A preset called " + trimmed + " already exists";
        return false;
    }

    // Move first, then rewrite the document at its new path. Writing the new name
    // into the old file first left a half-renamed preset behind if the move failed:
    // the filename still said one thing and the file's own name field another.
    if (destination != target.file && !target.file.moveFileTo(destination))
    {
        error = "Cannot rename " + target.file.getFileName();
        return false;
    }

    if (!writeFile(destination, trimmed, target.category, target.notes, target.params,
                   target.program, error))
    {
        // Put the file back under its old name so a failed write does not rename it.
        if (destination != target.file)
            destination.moveFileTo(target.file);
        return false;
    }

    reload();

    renamedIndex = indexOfFile(destination);
    if (renamedIndex < 0)
        renamedIndex = indexOfName(
            juce::String(subdoom::sanitisePresetText(trimmed.toStdString(), 64)), false);
    return true;
}

bool PresetLibrary::removeUser(int index, juce::String& error)
{
    error.clear();
    if (index < 0 || index >= size())
    {
        error = "No preset selected";
        return false;
    }
    const Entry& target = entries[static_cast<std::size_t>(index)];
    if (target.factory)
    {
        error = "Factory presets cannot be deleted";
        return false;
    }
    if (!target.file.deleteFile())
    {
        error = "Cannot delete " + target.file.getFileName();
        return false;
    }

    reload();
    return true;
}

