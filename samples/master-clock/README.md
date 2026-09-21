# Master Clock

Master Clock turns the Operator into a MIDI clock generator. Once it is the
device's clock source it sends MIDI Timing Clock out of every output at the tempo
it is set to, so a synth, a drum machine, or a DAW downstream all run to the
Operator's tempo.

The clock runs at 24 pulses per quarter note, the MIDI standard, and goes to all
8 physical outputs and USB at once. Loading the mode makes it available under
Settings, Clock Source. Should the clock ever fall behind, it sends at most a
quarter note's worth of catching up, so the worst a stall costs is a moment of
drift. Unloading the mode disarms the clock.

## Controls

| Control | Range | Default | What it does |
| ------- | ----- | ------- | ------------ |
| BPM     | 20.00 to 300.00 | 120.00 | The tempo, in beats per minute. |
| Bypass  | Off, On | Off | On stops the clock. Turning it back off picks up from the current moment, so the pulses that went by while it was on stay behind it and the clock simply carries on. |

## Build and flash

From the SDK root:

```bash
cd samples/master-clock
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../cmake/operator-mode-toolchain.cmake
cmake --build build
```

This produces `build/master_clock.opm`. Copy it into `/global/` on the
Operator's USB drive, then reboot to load it.
