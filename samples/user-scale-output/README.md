# User Scale (Output)

User Scale snaps incoming notes to a scale. A note in the scale passes through, and
a note outside it moves up to the next one that is in, carrying into the octave
above when the current one has none left. The scale is one of twenty-three presets
or one drawn key by key on the keyboard, and with every key turned off only
Transpose moves the notes. This is the per-output version, so each output running it
holds a scale of its own and two outputs can play in different keys at once.

Root is the note the scale starts on, so a major scale with a Root of D is D major,
and Transpose shifts the retuned note by up to three octaves. The screen shows the
scale name, a one-octave keyboard, and a row carrying Root and Transpose. A dot
marks every note in the scale, a ring marks the key under the cursor, and editing a
key while a preset is showing copies it to a custom scale. The keyboard follows
Root, so the lit keys are the notes that sound, and Back flips it to the shape of
the scale itself, drawn from C. When two keys land on the same note it sounds once
and keeps ringing until the last of them is released, so a legato line holds
together.

## Controls

| Control   | Range | Default | What it does |
| --------- | ----- | ------- | ------------ |
| Root      | C to B | C | The note the scale starts on. |
| Transpose | -36 to +36 | 0 | Shifts the retuned note up or down in semitones. |

Each output running the mode keeps its own Root, Transpose and scale. They are set
on the screen, which responds to gestures, so the controls follow whichever input
map is active.

| Gesture | Action |
| ------- | ------ |
| Scroll  | Move between the scale name, the keyboard, Root and Transpose. |
| Change  | Cycle the scale, move the key cursor, or change Root or Transpose. |
| Enter   | Turn the key under the cursor on or off, or put the selected field back to its default. |
| Back    | On the keyboard, flip between the notes that sound and the shape of the scale. |

## Build and flash

From the SDK root:

```bash
cd samples/user-scale-output
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../cmake/operator-mode-toolchain.cmake
cmake --build build
```

This produces `build/user_scale.opm`. Copy it into `/output/` on the Operator's
USB drive, then reboot and load it on the outputs to retune.
