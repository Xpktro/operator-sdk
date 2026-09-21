# Euclidean Sequencer

A four-track note sequencer that builds its patterns from Euclidean rhythms. Each
track divides the bar into a number of steps and spreads a number of hits as evenly
as it can around them. It runs on an output and plays one note per track,
defaulting to 36 (kick), 38 (snare), 40 (closed hat) and 42 (open hat), which line
up with the General MIDI drum map.

A track spreads its Events hits across its Steps positions, so 3 hits over 8 steps
gives the classic triplet `x . . x . . x .`. Offset rotates that pattern around
the bar without changing its shape. The four tracks are independent, so different
step counts layer into polyrhythms, say 5 against 7 against 3 against 4. On each
step a track releases its previous note and, when the step carries a hit, plays a
new one, and all four stay locked to the incoming clock.

The mode carries its own two-screen display. The main screen shows all four tracks
at once, each as a row of dots with a playhead sweeping across as the bar plays,
and the selected row outlined. The detail screen edits a single track, showing its
trigger row alongside its Steps, Events, and Offset values.

## Controls

Each track carries the same three parameters.

| Control | Range | Default | What it does |
| ------- | ----- | ------- | ------------ |
| Steps   | 1 to 32 | 16 | How many equal slots the bar is divided into. |
| Events  | 0 to 32 | 4 | How many hits to place across those steps. 0 is silent, and a value at or above Steps fills every step. |
| Offset  | 0 to 31 | 0 | Rotates the pattern, moving where the hits land. |

The two screens respond to gestures, so the controls follow whichever input map is
active.

| Screen | Gesture | Action |
| ------ | ------- | ------ |
| Main   | Scroll | Move the row selection. |
| Main   | Enter  | Open the selected track's detail screen. |
| Detail | Scroll | Move between Steps, Events, and Offset. |
| Detail | Change | Change the selected value. |
| Detail | Enter  | Reset the track to Steps 16, Events 4, Offset 0. |
| Detail | Back   | Go back to the main screen. |

## Build and flash

From the SDK root:

```bash
cd samples/euclidean-sequencer
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../cmake/operator-mode-toolchain.cmake
cmake --build build
```

This produces `build/euclidean_sequencer.opm`. Copy it into `/output/` on the
Operator's USB drive, then reboot and load it on any output from the mode
browser.
