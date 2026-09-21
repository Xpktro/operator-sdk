# Custom UI Mode Template

A scaffold to copy when starting a mode that draws its own screen. It owns every
pixel of the display while it is open.

The example is a counter, and it is there to be replaced. It draws a value in the
large font, centered, with a hint row beneath it in the small font. Change counts it
up and down, and Enter puts it back to zero.

The mode branches on the gesture type, never on the encoder the gesture arrived
from. The active input map decides which encoder and which press make a Change or an
Enter, so a mode that reads the type works under any of them. Long-press is spoken
for on both encoders, one leaving the custom UI and the other opening the mode's
Parameters page, so a mode is sent rotates and short presses.

## Make it yours

- **Draw something else.** `draw_rect`, `fill_rect`, `set_pixel` and `draw_bitmap`
  build up bars, grids and graphics, and the canvas arrives cleared every frame. See
  [Custom UI](../docs/custom-ui.md).
- **Carry parameters too.** Declare them the way the declarative template does and
  put `OP_MODE_PARAMS` where `OP_MODE_NO_PARAMS` stands. The device gives them a page
  of their own, reached from the screen with a long press.
- **Follow the clock.** `op::api->get_pulse_count()` steps with the beat, and
  `op::sdk::ClockInterpolator` in `<operator_sdk/timing.h>` reads a smooth position
  between one pulse and the next, which is what a moving playhead wants. See
  [Timing and clock](../docs/timing-and-clock.md).
- **Keep the status bar.** The `FLAGS FULLSCREEN_UI` in `CMakeLists.txt` is what
  gives the mode the whole screen. Dropping it leaves the status bar in place, and
  the mode draws beneath it.

## Build and flash

```bash
cd template-custom-ui
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../cmake/operator-mode-toolchain.cmake
cmake --build build
```

This produces `build/my_ui_mode.opm`. Copy it into `/global/` on the Operator's USB
drive, then reboot to load it.

To name the mode, replace `my_ui_mode` in `CMakeLists.txt`, in both `project()` and
`op_add_mode()`.
