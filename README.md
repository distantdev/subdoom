# Subdoom

Downtuned bass processor, version 0.1.0, from [Distant Nebula](https://distantnebula.com/).

It is a VST3, a CLAP, and a standalone app for downtuned bass. The low string is the center of the sound. The drive was voiced for a direct bass around -24 dBFS, and the harmonics are placed so a fundamental near low A (about 27 Hz) still reads.

## Controls

**Front** is the note, before the tone.

- **Trim** is the input level. Zero is the level the drive was voiced for. Hotter clips sooner. Colder stays cleaner.
- **Gate** cuts hiss between songs. It is slow enough that a palm mute or a ringing note is not chopped.
- **Sustain** evens long notes and leaves the pick attack. It is a leveler.
- **Split** is where the low path becomes the high path. Leave it near 100 Hz so the low string stays clean.
- **Sag** ducks the high path after a loud note and lets it back slowly. The fundamental does not pump.
- **Low Drive** saturates the fundamental. It squares the note.
- **Low** is the level of that path after the drive.
- **High Drive** drives the harmonics. The fundamental stays on the low path, and finger noise is not in this gain.
- **High** is the level of that path after the clipper.
- **Octave** folds the low band and adds it to the high path. The harmonics are even. It is a fold, so the pitch of the note does not move.

**Shape** is the voice.

- **Thump** is a few dB at 58 Hz. A little goes a long way.
- **Harmonics** lifts the second and third of a low A, so the note still speaks when the fundamental is mostly excursion.
- **Growl** is body around 240 Hz.
- **Honk** is around 750 Hz, to bring the note forward when the drive buries it.
- **Cut** is two lowpasses, 24 dB per octave, with no resonance.
- **Grit** is a parallel clipped band near 1.4 kHz. Pick and fret noise, for the nastier presets.

**Out** is the room and the level.

- **Room** is a short room. The wet path is highpassed so the lows do not pile up.
- **Decay** is how long that room lasts.
- **Master** is the output level, before a fixed saturate and the limiter.
- **Limit** catches peaks at -1 dBFS. Leave it on unless you want the raw output.
- **Mute** ramps the output down while you tune. The tuner keeps listening.

Gate, Limit, and Mute are switches. Everything else is a knob.

## Set the level

Play direct, and leave Trim at 0 if the bass is already near -24 dBFS. The input meter reads the held peak after Trim.

- Green is -29 to -19 dB. That is the window for a -24 dBFS bass with Trim at 0.
- Blue is colder.
- Red is hotter.

The lamp means the bass reached the plugin within 1 dB of full scale, before the trim. A fixed highpass at 20 Hz takes out the subsonic energy under the note.

The tuner is green in tune and red when the note is sharp or flat. Mute does not stop it. The spectrum is the output, from 30 Hz to 8 kHz. The scope is the output, with the input as a faint trace behind it. Peak and RMS are the output, and their lamp lights when the output clips.

## Presets

Ten factory presets are built in. You cannot overwrite them. A star after the name means the sound has been edited.

- **Drone.** LOW MASS, GIZA.
- **Doom.** WOOL HYMN, MARROW, EMPRESS.
- **Stoner.** RED SUN, SERPENT.
- **Sludge.** FUNERAL WALL, DRAGON LUNG, CAVEMAN.

Click the name to open the browser. It lists every preset alphabetically, and the number on a row is that preset's MIDI program. A dash means the preset has no program. Filter by name or category. Load, save as, rename, delete (it asks you to confirm), open the preset folder, or rescan.

The arrows beside the name step through the list and wrap. Scrolling on the name does the same. Right-click an arrow to teach it MIDI.

Save writes the current user preset. On a factory preset, Save opens Save As. Revert drops the edits and reloads the preset you had. Deleting the user preset you are on leaves the sound in place and names it UNSAVED.

A/B keeps two sounds. The button shows A or B. Click it to store the sound you are on and flip to the other. Both come back with the project.

The sound stored in the DAW project is the one that reopens. A preset file is read only when you load it.

User presets are text files in `%APPDATA%\Subdoom\Presets`, one `*.subdoom` file each. A file has a name, a category, optional notes, and one line per control. You can edit one by hand. A control the plugin does not know is ignored. A missing control keeps its default.

The host sees 128 programs. The program slot is the number on the preset, not its place in the list. A program change loads the preset with that number, and sending it again leaves that preset selected. A number with no preset is skipped.

## MIDI

Right-click a knob or a switch: MIDI Learn, or Clear MIDI. A CC already used on another control moves.

Knobs follow the controller. Gate, Limit, and Mute latch: they toggle when the CC crosses 64. The preset arrows fire once when their CC crosses 64.

MIDI opens the whole map, a channel filter (any channel, or one of 1-16), program change, and clear all.

A new project puts the twenty-one controls on CC 20 through CC 40, in the order they appear on the panel. Change any of them from the control or from the MIDI page. The map is stored in the project. Clear all leaves that project with nothing bound.

## Build from source

Windows, CMake 3.25 or newer, and Visual Studio 2026. From this directory:

```
cmake -S . -B build-msvc -G "Visual Studio 18 2026" -A x64
cmake --build build-msvc --config Release
```

Use that Visual Studio generator for `build-msvc`. The build writes:

- `build-msvc\Subdoom_artefacts\Release\VST3\Subdoom.vst3`
- `build-msvc\Subdoom_artefacts\Release\CLAP\Subdoom.clap`
- `build-msvc\Subdoom_artefacts\Release\Standalone\Subdoom.exe`

It does not copy them into a host. Put the VST3 in `C:\Program Files\Common Files\VST3` and the CLAP in `C:\Program Files\Common Files\CLAP`, then rescan. The standalone uses ASIO. Run `Subdoom.exe`.

## License

Subdoom is released under the MIT License. See [LICENSE](LICENSE).

The plugin is built with JUCE under the JUCE licence. JUCE is not part of the MIT grant.
