# Note Probability

Note Probability drops notes at random as they pass through. Probability is the
chance that any one note plays. At 100 every note plays, and the lower it goes
the more of them fall away, so a repeated part thins out and comes back a little
different each time.

Only the notes themselves are dropped. Note-offs pass through, and so do control
changes, pitch bend, program changes, and the clock, so a note that did play is
still released cleanly.

## Controls

| Control     | Range | Default | What it does |
| ----------- | ----- | ------- | ------------ |
| Probability | 0 to 100 | 100 | The chance, as a percentage, that a note sounds. 0 drops every note and 100 plays every note. |

## Build and flash

From the SDK root:

```bash
cd samples/note-probability
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../cmake/operator-mode-toolchain.cmake
cmake --build build
```

This produces `build/note_probability.opm`. Copy it into `/global/` on the
Operator's USB drive, then reboot to load it.
