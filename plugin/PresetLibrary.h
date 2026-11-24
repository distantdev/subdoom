#pragma once

#include "subdoom/Parameters.h"
#include "subdoom/PresetIO.h"

#include <juce_core/juce_core.h>

#include <vector>

// Factory presets are compiled in. User presets are text files the plugin owns,
// scanned from the per-user preset folder. Nothing here touches the audio thread:
// the processor treats the list as message-thread state and publishes a sound
// through the DSP mailbox.
class PresetLibrary
{
public:
    struct Entry
    {
        juce::String name;
        juce::String category;
        juce::String notes;
        subdoom::Parameters params;
        bool factory = false;
        juce::File file;
        // Compiled factory slot, or -1 for a user preset. program-map.txt keys
        // this, which stays put when the list is sorted by name.
        int factorySlot = -1;
        // -1 is blank: MIDI program change skips it. Next and previous still visit it.
        int program = -1;
    };

    // The folder is a parameter so tests can point a library at a temp directory.
    // The plugin uses the per-user folder.
    explicit PresetLibrary(juce::File rootFolder = {});

    void reload();

    int size() const noexcept { return static_cast<int>(entries.size()); }
    const Entry& entry(int index) const;

    // -1 when nothing matches.
    int indexOfName(const juce::String& name, bool factory) const;
    int indexOfFile(const juce::File& file) const;

    // -1 when the preset is blank or the index is out of range.
    int programCode(int index) const;
    // Library index for a MIDI program number, or -1 when nothing is assigned.
    int indexForProgram(int code) const;
    // code is 0-127, or -1 to clear. A code can belong to only one preset.
    bool setProgramCode(int index, int code, juce::String& error);

    juce::StringArray categories() const;

    static juce::File userFolder();
    static juce::String safeFileName(const juce::String& name);

    // Writes one preset document. Creates the parent folder first; reports every
    // failure instead of failing silently.
    static bool writeFile(const juce::File& file,
                          const juce::String& name,
                          const juce::String& category,
                          const juce::String& notes,
                          const subdoom::Parameters& params,
                          int program,
                          juce::String& error);
    static bool readFile(const juce::File& file, Entry& entry, juce::String& error);

    // Saves into the library folder. Refuses to overwrite unless allowOverwrite.
    // savedIndex receives the entry the written file produced: the stored name is
    // sanitised and capped, so it is not necessarily the name that was asked for.
    bool saveUser(const juce::String& name,
                  const juce::String& category,
                  const juce::String& notes,
                  const subdoom::Parameters& params,
                  bool allowOverwrite,
                  int& savedIndex,
                  juce::String& error);

    // Overwrites the file behind a user entry, keeping its identity.
    bool overwrite(int index, const juce::String& notes,
                   const subdoom::Parameters& params, juce::String& error);

    bool renameUser(int index, const juce::String& newName, int& renamedIndex, juce::String& error);
    bool removeUser(int index, juce::String& error);

private:
    static int defaultFactoryProgram(int slot) noexcept;
    int indexOfFactorySlot(int slot) const;
    void applyFactoryProgramMap();
    bool saveFactoryProgramMap(juce::String& error);

    juce::File fileFor(const juce::String& name) const;

    juce::File folder;
    std::vector<Entry> entries;
};
