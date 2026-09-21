# Mode Lifecycle {#mode-lifecycle}

When each part of your mode runs, on which core, and how you declare them.

## Declaring your hooks

You write ordinary functions and hand them to the SDK's registration macros. The macros add `extern "C"` wrappers and the exported names the device needs. A minimal mode is just this:

```cpp
#include <operator_sdk.h>
using namespace op;

void init()    { /* set up state */ }
void process(OpMidiMessage* msgs, uint8_t count, uint32_t tick) { /* your work */ }
void destroy() { /* clean up */ }

OP_MODE_NO_PARAMS();                        // or OP_MODE_PARAMS(...) to declare params
OP_MODE_REGISTER(init, process, destroy);
```

The function names above (`init`, `process`, `destroy`) are a convention these docs follow for clarity, and any names will do. @ref OP_MODE_REGISTER takes the three core functions, and an optional fourth, a SysEx handler (see @ref receiving-sysex). @ref OP_MODE_PARAMS (or @ref OP_MODE_NO_PARAMS for a mode with none) declares the mode's parameters and makes @ref op::api available. A custom UI adds @ref OP_MODE_REGISTER_UI with the `ui_render` and `ui_gesture` functions.

A fuller mode, with a parameter and a custom UI, uses all three macros:

```cpp
inline constexpr op::params::Numeric Depth{ .name = "Depth", .min = 0, .max = 127, .default_ = 64 };

void init()      { /* set up state */ }
void process(OpMidiMessage* msgs, uint8_t count, uint32_t tick) { /* your work */ }
void destroy()   { /* clean up */ }
void ui_render() { /* draw your screen */ }
void ui_gesture(uint8_t control, op::Gesture gesture, op::GestureType type, int16_t value) { /* handle input */ }

OP_MODE_PARAMS(Depth);
OP_MODE_REGISTER(init, process, destroy);
OP_MODE_REGISTER_UI(ui_render, ui_gesture);
```

Declaring parameters is covered in @ref parameters, and drawing plus input in @ref custom-ui.

## The hooks

| Hook       | Thread | Called                                                          | When you write it                             |
| ---------- | ------ | --------------------------------------------------------------- | --------------------------------------------- |
| init       | Core 0 | Once, right after your mode loads                               | Always                                        |
| process    | Core 1 | Every pass of the real-time loop, whether or not MIDI arrived   | Always. This is where your mode does its work |
| destroy    | Core 0 | Once, right before your mode unloads                            | Always                                        |
| ui_render  | Core 0 | Every display frame, while your mode owns the screen            | Only if you draw a custom UI                  |
| ui_gesture | Core 0 | On each input event your custom UI receives                     | Only if you handle input in a custom UI       |
| on_sysex   | Core 1 | On each inbound SysEx fragment, if the mode declares `HANDLES_SYSEX` | Only to inspect, consume, or generate SysEx   |

## What each hook looks like

**init** runs once when your mode loads. @ref op::api is already assigned, so you can read parameters and cache state here.

```cpp
void init();
```

**process** is the hot path, called on the real-time core on every pass of its loop, whether or not any MIDI arrived (see @ref core-1-cycle). This is where a mode reads the incoming messages and decides what leaves. See @ref processing-midi.

```cpp
void process(OpMidiMessage* msgs, uint8_t count, uint32_t tick);
```

- `msgs` is the writable batch of @ref OpMidiMessage values that arrived on this pass.
- `count` is how many messages are in the batch. A batch of zero is normal and common, because the loop runs far faster than MIDI does.
- `tick` is a microsecond timestamp for this pass.

**destroy** runs once when your mode unloads. Release anything you set up in init.

```cpp
void destroy();
```

**ui_render** draws your custom UI, once per display frame while your mode owns the screen. The drawing calls are in @ref custom-ui.

```cpp
void ui_render();
```

**ui_gesture** receives one input event at a time for a custom UI. Reading and responding to these is covered in @ref custom-ui.

```cpp
void ui_gesture(uint8_t control, op::Gesture gesture, op::GestureType type, int16_t value);
```

- `control` is which physical control the event came from (0 = left, 1 = right).
- `gesture` is the raw event, an @ref op::Gesture (rotate, short press, long press).
- `type` is the input-mapped intent, an @ref op::GestureType (scroll, change, enter, back).
- `value` is the signed step for a rotation, or 0 for a press.

**on_sysex** receives inbound SysEx on the real-time core, only when the mode declares the `HANDLES_SYSEX` flag and passes a handler to @ref OP_MODE_REGISTER third, beside `process`. Handling it is covered in @ref receiving-sysex.

```cpp
bool on_sysex(uint8_t port, const uint8_t* data, uint16_t len, uint8_t flags);
```

Returning `true` takes the message, so it is not sent on. Returning `false` lets it continue to the routed outputs.

A mode that omits `ui_render` and `ui_gesture` is **declarative-only**: the firmware renders its parameter screen for you. That is the recommended default. See @ref parameters.

## Threading rules

- **init** may read @ref op::api but should not send MIDI yet. The real-time core has not started processing this slot.
- **process** is bounded, with no blocking and no allocation. It runs on every pass of the real-time loop, for every loaded mode, and a mode that repeatedly overruns its per-call time budget is disabled at runtime. If you need file data, read it in init and cache it. See @ref the-mode-mindset.
- **on_sysex** runs on the real-time core, only when SysEx arrives, and shares instance state with process. It carries the same speed requirement.
- **ui_render** and **ui_gesture** share Core 0 with the firmware UI. They see a stable snapshot of parameter values published each frame, the same values the real-time core reads through @ref OperatorApi::get_param_value.
- **destroy** runs after the real-time core has stopped dispatching into your slot, so it is safe to tear down state. If your mode programs the clock generator, `destroy` has to disarm it (see @ref timing-and-clock).

## How a mode is loaded

The firmware owns the whole lifecycle of your `.opm`, and you do not manage any of it. In short:

1. It finds your mode in `/global/` or `/output/` and reads its name for the picker.
2. When you load it, the firmware checks the file is valid and built for a compatible SDK version, then places it in a free slot and sets up its parameters (from a saved `.opc` if one exists, otherwise from your declared defaults).
3. It calls your init once on Core 0. From there on, the real-time core calls process on every pass of its loop, whether or not any MIDI arrived. If your mode has a custom UI, Core 0 also calls ui_render each frame and delivers input events to ui_gesture.
4. Loading a mode that is already loaded does nothing. Unloading calls destroy and returns the slot.

Nothing here allocates memory while your mode runs.
