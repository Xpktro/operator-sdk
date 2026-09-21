# CC LFO

Four independent low-frequency oscillators that emit Control Change (CC)
messages. Each one points at any CC number (filter cutoff, an effect depth,
anything that listens to CC) and sweeps that value automatically, in a shape and
at a rate of its own.

Shown as LFO 1 through LFO 4, they each modulate a different CC at their own rate
and shape. Each starts Off and turns on when its Source is set to Free (a fixed
speed in Hz) or Sync (locked to the incoming clock), at which point the rest of
its controls appear. An LFO sends a message only when its value actually changes,
and the mode keeps the combined output rate in check so a busy set of LFOs never
floods the MIDI bus.

With Source set to Sync, an LFO stays locked to the incoming clock and follows
tempo changes. Starting or stopping the transport re-anchors the sweep to the
downbeat, so a slow multi-bar shape restarts cleanly from bar 1. With Source set
to Free it runs at its Hz rate regardless of any clock. Setting an LFO's Source
back to Off hides its other rows and silences it, without touching the other
three.

## Controls

Each LFO's Source comes first. The remaining rows appear once Source is Free or
Sync, and which rate row shows depends on that choice.

| Control   | Options / Range | Default | What it does |
| --------- | --------------- | ------- | ------------ |
| Source    | Off, Free, Sync | Off | Turns the LFO on and picks its clock. Off is silent, Free runs at a fixed speed in Hz, Sync locks to the incoming clock. |
| CC        | 0..127 | 1-4 (per LFO) | Which Control Change number this LFO sends. |
| Shape     | Sine, Triangle, Saw, Inv Saw, Square, S&H | Sine | The modulation waveform. S&H (sample and hold) jumps to a new random value once per cycle. |
| Low       | 0..127 | 0 | The bottom of the output range. |
| High      | 0..127 | 127 | The top of the output range. The waveform sweeps between Low and High. |
| Rate Hz   | 0.01 to 20.00 Hz | 1.00 Hz | Speed when Source is Free. |
| Rate Beat | 16 down to 1/32T | 1/4 | Speed when Source is Sync, as a musical division. Whole numbers are measures, fractions are note values, and "T" is a triplet. One full sweep spans that division, so "1/4" is one sweep per beat and "16" is one slow sweep across 16 bars. |

## Build and flash

From the SDK root:

```bash
cd samples/cc-lfo
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../cmake/operator-mode-toolchain.cmake
cmake --build build
```

This produces `build/cc_lfo.opm`. Copy it into `/output/` on the Operator's USB
drive, then reboot and load it on any output from the mode browser.
