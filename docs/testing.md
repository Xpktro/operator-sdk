# Testing {#testing}

You can test a mode's logic on your computer, with no hardware. The SDK ships a mock of the device that you drive from a plain C++ unit test: feed it MIDI and timing, run your mode, and check what comes out.

## What the harness gives you

`operator_sdk_sim` is a host-side test harness. It provides a mock @ref OperatorApi, the same function table your mode calls on the device, backed by in-memory state: an incoming and outgoing MIDI queue, a virtual clock, a framebuffer, and parameter storage. Your mode's own `main.cpp` compiles unchanged and links against it. There is no device and no on-screen simulator, just your mode's code exercised in a test.

## The shape of a test

Tests use doctest, which the harness bundles. A test file registers the mode under test, then each `TEST_CASE` drives it:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include <operator_sdk_sim.h>
#include <operator_sdk.h>

OP_MODE_UNDER_TEST();   // bind this test to your mode's exported hooks

TEST_CASE("transpose shifts note-ons up a fifth") {
    op::sim::reset_state();
    const OperatorApi* api = op::sim::get_api();
    mode_init(api);

    // Set a parameter by slot (its position in OP_MODE_PARAMS). Transpose = +7.
    api->set_param_value(/*Transpose=*/0, 7);

    // Feed one note-on: status 0x90, note 60, velocity 100, 3 bytes long.
    OpMidiMessage note{ .status = 0x90, .data1 = 60, .data2 = 100, .port = 0, .length = 3 };
    mode_process(&note, 1, api->get_tick());

    // A transform edits the batch in place.
    CHECK(note.data1 == 67);

    mode_destroy();
}
```

## The building blocks

- `op::sim::reset_state()` clears the mock between tests.
- `op::sim::get_api()` returns the mock @ref OperatorApi to hand to `mode_init`.
- `mode_init`, `mode_process`, and `mode_destroy` are your mode's hooks, called directly. `OP_MODE_UNDER_TEST()` makes them, and your `kParams` table, visible to the test.
- **Parameters:** set values with @ref OperatorApi::set_param_value "set_param_value" by slot index (their order in @ref OP_MODE_PARAMS). `reset_state()` zeroes the store, so to start from your declared defaults call `op::sim::apply_param_defaults(kParams, kParamCount)` after `reset_state()` and before `mode_init`.
- **Incoming MIDI:** build an @ref OpMidiMessage batch and pass it to `mode_process`, or enqueue with `op::sim::queue_incoming_midi`.
- **Outgoing MIDI:** read what your mode emitted with `op::sim::drain_outgoing_midi(out, buf, max)`.
- **Timing:** drive the clock with `op::sim::advance_tick`, `op::sim::set_pulse_count`, or `op::sim::set_beat_position` before calling `mode_process`. See @ref timing-and-clock.
- **Custom UI:** call `ui_render` and read the pixels with `op::sim::capture_framebuffer`, and deliver input with `ui_gesture(control, gesture, type, value)`.
- **Files:** seed a file with `op::sim::seed_extras_file(mode, name, data, size)` and a FilePicker choice with `op::sim::pick_filename(slot, name)`.
- **Outputs:** flip an output on or off with `op::sim::set_output_active(out, active)` to simulate an unavailable output.

## Common scenarios

**Start from your declared defaults.** `reset_state()` zeroes the parameter store, so seed the real defaults to exercise a mode the way a user would first meet it:

```cpp
op::sim::reset_state();
op::sim::apply_param_defaults(kParams, kParamCount);   // after reset, before init
const OperatorApi* api = op::sim::get_api();
mode_init(api);
```

**A filter that drops messages.** Feed a message the mode should remove, run `process`, and check it was dropped (a dropped message has `status == 0`):

```cpp
OpMidiMessage cc{ .status = 0xB0, .data1 = 1, .data2 = 64, .port = 0, .length = 3 };
mode_process(&cc, 1, api->get_tick());
CHECK(cc.status == 0);   // the mode dropped it
```

**A generator driven by the clock.** Advance the virtual clock, run `process`, then drain what the mode emitted:

```cpp
op::sim::set_pulse_count(24);              // one beat in
mode_process(nullptr, 0, api->get_tick());

OpMidiMessage out[8];
const auto n = op::sim::drain_outgoing_midi(/*out=*/0, out, 8);
CHECK(n >= 1);
CHECK((out[0].status & 0xF0) == 0x90);     // it fired a note-on
```

For a clock-generating mode, assert the tempo it programmed with `op::sim::get_last_clock_period_us()`.

**A custom UI.** Render a frame and check pixels, or push an input event and check the effect:

```cpp
ui_render();
uint8_t fb[op::sim::kFramebufferBytes];
op::sim::capture_framebuffer(fb);          // assert on the drawn pixels

// A right-control rotation, meant as "change":
ui_gesture(/*control=*/1, op::Gesture::Rotate, op::GestureType::Change, +1);
CHECK(api->get_param_value(0) == /* expected new value */);
```

**A file-reading mode.** Seed both the file's contents and the user's FilePicker choice, then let the mode read it:

```cpp
const uint8_t song[] = { /* .mid bytes */ };
op::sim::seed_extras_file("my_mode", "song.mid", song, sizeof song);
mode_init(api);
op::sim::pick_filename(/*FilePicker slot=*/0, "song.mid");
mode_process(nullptr, 0, api->get_tick());   // now param<FilePicker>() resolves the file
```

**An unavailable output.** Turn an output off (for example USB when USB MIDI is not active) and check the mode routes around it:

```cpp
op::sim::set_output_active(/*USB=*/8, false);
// ... run the mode; assert it did not rely on the dead output ...
```

## Wiring the build

A test is an ordinary executable that compiles your mode's source as a library and links the harness. Every sample follows this pattern, so the simplest starting point is to copy a `samples/*/tests/CMakeLists.txt`. The essence is:

```cmake
add_library(my_mode_host STATIC ../main.cpp)
target_include_directories(my_mode_host PUBLIC ${SDK_INCLUDE_DIR})

add_executable(test_my_mode test_my_mode.cpp)
target_link_libraries(test_my_mode PRIVATE my_mode_host operator_sdk_sim)
```

Register each `TEST_CASE` with `add_test` so `ctest` runs it.

## Test behavior, not internals

Drive your mode through its real hooks and assert on what a user would observe: the MIDI it emits, the pixels it draws, the parameters it changes. Do not reach into the mode's private state, and do not add an export just so a test can see something. If a test seems to need one, the behavior it is checking should usually be observable through the ABI instead. A random generator is the case people reach for a hook on, but the sim starts the tick at zero, so a mode that seeds from the tick already runs the same way every time and needs no seam.
