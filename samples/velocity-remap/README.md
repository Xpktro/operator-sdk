# Velocity Remap

Velocity Remap reshapes how hard notes come out. Every note's velocity is bent
through a curve, so a heavy hand can be folded into a gentler range, the distance
between soft and loud can be opened up, or every note can be flattened to one fixed
level. It runs on an output, and only velocities change, so note-offs, CC,
aftertouch, clock and everything else pass straight through.

The screen draws the curve as it is edited, with a dotted line where the last note
landed on it, and the values beside it. At the defaults the curve is a straight line
and velocities come out as they went in. A Minimum above the Maximum flips it, so
soft playing comes out loud.

## Controls

| Control  | Range | Default | What it does |
| -------- | ----- | ------- | ------------ |
| Minimum  | 1 to 127 | 1 | The softest the output can be. Hidden while Constant is on. |
| Maximum  | 1 to 127 | 127 | The loudest the output can be. |
| Tension  | -100 to +100 | 0 | Bends the curve toward the loud end (+) or the soft end (-), so the same playing reads louder or softer throughout. |
| Compand  | -100 to +100 | 0 | Opens up (+) or closes down (-) the spread between soft and loud around the middle. |
| Constant | Off, On | Off | On plays every note at one fixed velocity, the Maximum, however hard it was played. |

The screen responds to gestures, so the controls follow whichever input map is
active.

| Gesture | Action |
| ------- | ------ |
| Scroll  | Move between the four values. |
| Change  | Change the selected value. |
| Enter   | Reset the selected value to its default. |
| Back    | Toggle Constant. |

## Build and flash

From the SDK root:

```bash
cd samples/velocity-remap
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../cmake/operator-mode-toolchain.cmake
cmake --build build
```

This produces `build/velocity_remap.opm`. Copy it into `/output/` on the
Operator's USB drive, then reboot and load it on any output from the mode
browser.
