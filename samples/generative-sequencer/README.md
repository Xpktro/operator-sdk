# Generative Sequencer

The generative sequencer plays a stream of random notes in time with the clock.
On every 16th-note step it rolls against Probability, and on a pass it picks a
note at random from Note Range, holds it for a random length from Length Range,
and sends it to the chosen output. Left running, it turns out an ever-changing
melodic or percussive line.

The sequencer follows the device clock, which keeps it in time with whatever is
driving the tempo, and it waits on the step it is on while that clock is stopped.
Internal Clock hands it its own tempo instead, at the BPM set beside it.

A note is released once its length has run out, whatever later steps do. Notes
go out at a fixed velocity on channel 1. Up to sixteen can sound at once, and
while all sixteen are held a step passes in silence and leaves them to ring out.

With Output set to All the notes go round the outputs in turn, passing over any
that are inactive, which is usually USB while USB MIDI is off. Choosing a single
output sends every note there, and that choice holds even when the output cannot
play, so the sequencer falls silent until it can.

## Controls

| Control        | Range | Default | What it does |
| -------------- | ----- | ------- | ------------ |
| Note Range     | 0 to 127 | 36 to 127 | The lowest and highest note the random pick chooses between. |
| Length Range   | 10 to 2000 ms | 100 to 2000 ms | The shortest and longest a note is held, picked afresh for each one. |
| Internal Clock | Off, On | Off | Off follows the device clock, On runs the sequencer at its own tempo. |
| Internal BPM   | 20.00 to 300.00 | 120.00 | The tempo while Internal Clock is on. Hidden otherwise. |
| Probability    | 0 to 100 | 50 | The chance, as a percentage, that a step plays a note. 0 is silent and 100 plays every step. |
| Output         | All, Out 1 to Out 8, USB | All | Where the notes go. All sends them round every output in turn, and a single choice sends them all to that one. |

## Build and flash

From the SDK root:

```bash
cd samples/generative-sequencer
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../cmake/operator-mode-toolchain.cmake
cmake --build build
```

This produces `build/generative_sequencer.opm`. Copy it into `/global/` on the
Operator's USB drive, then reboot to load it.
