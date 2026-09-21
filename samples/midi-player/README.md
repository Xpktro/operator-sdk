# MIDI Player

MIDI Player plays Standard MIDI Files from the device. It runs on an output,
sending a song's notes out of it, in time either with the song's own tempo or
with the device clock. It passes incoming MIDI along untouched, since it only
ever plays the file.

The file is streamed as it plays, so a long song costs the same memory as a short
one and files of up to 16 MB play as readily as small ones. Pausing, stopping, or reaching the end silences whatever notes are still
sounding, so nothing is left hanging, and stopping winds the song back to its
beginning. Pausing keeps its place and resuming carries on from it.

The mode carries its own screen. It shows the name of the song, the time elapsed
and the time it runs to, the clock the song is following, and two buttons, one
that plays and pauses and one that stops.

Songs go in `/extras/midi_player/` on the Operator's USB drive, where the MIDI
File control lists them. Single-track and multi-track files both play. The rarer
kind that gives each track its own independent timing does not, since it has no
single tempo to follow.

## Controls

| Control      | Options | Default | What it does |
| ------------ | ------- | ------- | ------------ |
| Clock Source | File, Ext. | File | File plays the song at its own tempo. Ext. follows the device clock, so a tempo change upstream carries the song with it. |
| MIDI File    | any `.mid` file | none | The song to play. |

The screen responds to gestures, so the controls follow whichever input map is
active.

| Gesture | Action |
| ------- | ------ |
| Scroll  | Move between the play and pause button and the stop button. |
| Enter   | Press the selected button. |
| Change  | Switch the Clock Source. |

## Build and flash

From the SDK root:

```bash
cd samples/midi-player
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../cmake/operator-mode-toolchain.cmake
cmake --build build
```

This produces `build/midi_player.opm`. Copy it into `/output/` on the Operator's
USB drive, then reboot and load it on any output from the mode browser.
