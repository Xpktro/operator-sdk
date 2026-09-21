# Arpeggiator

The arpeggiator plays a held chord back one note at a time, in the selected
pattern, locked to the clock. It runs on an output, watching incoming notes
and, in their place, sends its own stepped stream.

Held notes themselves are not passed through, the arpeggio replaces them.
Everything else (aftertouch, CC, pitch bend, and so on) passes straight through,
so the downstream synth keeps its full expression. Notes added or removed while
it runs fold into the pattern on the next step, and the arpeggio stops once the
last note is released.

The arpeggio follows the tempo and transport of the incoming clock and stops in
time with whatever is driving it. Changing Rate, Style, or any other parameter
takes effect on the next step.

## Controls

| Control | Range | Default | What it does |
| ------- | ----- | ------- | ------------ |
| Style   | As Played, Up, Down, Up-Down, Down-Up, Converge, Diverge, Random | Up | The order the held notes are played. As Played follows the order they were pressed, Up and Down run through them by pitch, Up-Down and Down-Up bounce between the extremes, Converge and Diverge work from the edges inward or the middle outward, and Random picks freely. |
| Rate    | 1/4, 1/8, 1/16, 1/32, 1/64 | 1/16 | How often a step fires, as a note division of the beat. |
| Triplet | Off, On | Off | Gives the rate a triplet feel (three steps in the space of two). |
| Octave  | -3 to +3 | 0 | Shifts every played note up or down by whole octaves. |
| Fill    | 1 to 100 | 50 | How long each note sounds, as a percentage of the step. 100 holds the note right up to the next step, small values give a short, staccato feel. |
| Chord   | Off, On | Off | Off plays one note per step. On plays the whole chord at every step, moved so the pattern note becomes the lowest voice while the chord's shape is kept. |

## Build and flash

From the SDK root:

```bash
cd samples/arpeggiator
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../cmake/operator-mode-toolchain.cmake
cmake --build build
```

This produces `build/arpeggiator.opm`. Copy it into `/output/` on the
Operator's USB drive, then reboot and load it on any output from the mode
browser.
