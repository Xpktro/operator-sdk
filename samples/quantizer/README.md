# Quantizer

Quantizer tightens up timing by snapping notes to a grid as they are played. A
note that lands a little late is held back and let go on the next grid point, so
a loose performance locks to the beat. It runs on an output, and it moves notes
in time and not in pitch. To snap notes to a scale, load User Scale Output.

Only the start of a note is moved. A note played just after a grid point is
already where it wants to be and plays at once, since holding it would only make
it late, and a note played in the second half of a window waits for the grid
point ahead of it. A release always passes straight through, so a note keeps the
length it was played with, and a note tapped and released before its grid point
never sounds at all. Everything that is not a note, a control change or a pitch
bend or the clock, passes through untouched.

The mode carries its own screen. It shows the Division in large text, a bar
marked with the grid and swept by a cursor on the beat, and whether Triplet is
on.

## Controls

| Control  | Options | Default | What it does |
| -------- | ------- | ------- | ------------ |
| Division | 1, 1/2, 1/4, 1/8, 1/16, 1/32, 1/64 | 1/4 | How far apart the grid points are. 1/4 snaps to quarter notes, and smaller values snap tighter. |
| Triplet  | Off, On | Off | On moves the grid points closer together, three where two would go, so 1/8 gives eighth-note triplets. |

The screen responds to gestures, so the controls follow whichever input map is
active.

| Gesture | Action |
| ------- | ------ |
| Scroll  | Cycle the Division. |
| Change  | Toggle Triplet. |

## Build and flash

From the SDK root:

```bash
cd samples/quantizer
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../cmake/operator-mode-toolchain.cmake
cmake --build build
```

This produces `build/quantizer.opm`. Copy it into `/output/` on the Operator's
USB drive, then reboot and load it on any output from the mode browser.
