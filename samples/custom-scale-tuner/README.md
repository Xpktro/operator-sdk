# Custom Scale Tuner

Retunes incoming notes to a microtonal scale supplied as a Scala (`.scl`) file
and sends the retuned notes to the output it runs on. Each note is mapped to the
nearest degree of the scale and nudged to its exact pitch with a pitch-bend
message, so the mode can play any tuning in any synth that is bend-capable.

Scala `.scl` files placed in `/extras/custom_scale_tuner/` on the Operator's USB
drive appear in a file picker. For every note, the mode finds the closest scale
degree and emits that note plus the pitch bend that lands it on the true microtonal
pitch. Incoming pitch-bend wheel movements are combined with the scale tuning,
so held notes bend as expected without losing their tuning. When two keys retune to
the same note it sounds once and holds until the last of them is released. When the
scale file is missing or malformed the mode passes MIDI through unchanged rather
than going silent.

With MPE on, concurrent notes spread across member channels (2..16), one note
per channel, so each note bends independently. Switching MPE while notes are
held can leave some hanging until they are retriggered, so it is best changed
between phrases. Non-note messages (CC, program change, aftertouch, clock,
and so on) pass through to the output untouched.

## Controls

| Control      | Range | Default | What it does |
| ------------ | ----- | ------- | ------------ |
| Scale File   | any `.scl` file | none | The microtonal scale to tune to. Until one is picked the mode passes MIDI through untouched. |
| Root Key     | 0..127 | 69 (A4) | The key that anchors the scale's base degree (1/1). |
| Root Freq Hz | 20.00 to 480.00 Hz | 440.00 | The frequency of the root key. |
| Bend Range   | 1 to 48 semitones | 2 | Matches the pitch-bend range the downstream synth is set to, so the retune bend lands correctly. The mode sends no setup messages, so both sides must be set to agree. |
| MPE          | Off, On | Off | On spreads concurrent notes across MPE member channels (2..16), one note per channel, so each note can bend independently. |

## Build and flash

From the SDK root:

```bash
cd samples/custom-scale-tuner
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../cmake/operator-mode-toolchain.cmake
cmake --build build
```

This produces `build/custom_scale_tuner.opm`. Copy it into `/output/` on the
Operator's USB drive, then reboot and load it on the output to retune.
