# Parameters {#parameters}

Parameters are how a user tunes your mode. You declare them once, and the device renders the mode's param page, remembers the values across power cycles, and hands them to your code. Most modes need nothing more than this, and no custom UI at all.

## Declaring parameters

Each parameter is a C++ object you declare once, then list in @ref OP_MODE_PARAMS. This is the full parameter block from the declarative template:

```cpp
inline constexpr op::params::Numeric Transpose{
    .name = "Transpose", .min = -12, .max = 12, .default_ = 0,
};
inline constexpr op::params::Bool Enable{
    .name = "Enable", .default_ = true,
};
inline constexpr op::params::Enum Channel{
    .name = "Channel",
    .options = {"All", "Ch 1", "Ch 2", "Ch 3", /* ... */ "Ch 16"},
    .default_ = 0,
};

OP_MODE_PARAMS(Transpose, Enable, Channel);
```

The C++ name on the left (`Transpose`) is what your code uses. The `.name` string is the label the firmware shows on screen. A mode with no parameters uses @ref OP_MODE_NO_PARAMS instead.

## Parameter kinds

| Kind | Use it for |
|------|-----------|
| @ref op::params::Bool "Bool" | An on/off toggle |
| @ref op::params::Numeric "Numeric" | A number in a range, with optional decimal places |
| @ref op::params::Enum "Enum" | One choice from a fixed list of labels |
| @ref op::params::List "List" | A multi-select set of labels |
| @ref op::params::Range "Range" | A low/high pair of numbers |
| @ref op::params::NoteRange "NoteRange" | A low/high pair shown as MIDI note names |
| @ref op::params::FilePicker "FilePicker" | A file chosen from the mode's storage folder |
| @ref op::params::Action "Action" | A button that runs one of the mode's functions, holding no value |

The device renders and handles each kind on the param page for you. Reading a `FilePicker`'s chosen file is in @ref storage-and-files.

## Reading a parameter

Read a value with the typed `param<>` accessor, using the parameter's C++ name:

```cpp
if (param<Enable>()) {                 // Bool reads as a truth value
    const int32_t shift = param<Transpose>();   // Numeric reads as its value
    // ...
}
```

`param<>` returns the live value, the same one the user sees on screen. You can read parameters in `init` and in `process`. When you have only a parameter's slot index rather than its C++ name, the raw @ref OperatorApi::get_param_value "get_param_value" reads it by index.

## Changing a parameter from code

Write a value with @ref OperatorApi::set_param_value "set_param_value". The change is published to the param page within a frame. This is mainly how a custom UI edits parameters in response to input, see @ref custom-ui.

## Action buttons {#action-params}

Most parameters hold a value the user tunes. An @ref op::params::Action "Action" holds none. It appears as a button on the param page, and selecting it runs one of the mode's functions. This is how a mode puts a command, clearing a recorded clip or firing a device query, right alongside its ordinary parameters.

An Action is declared by naming the function it runs:

```cpp
void clear_clip() { /* ... */ }

inline constexpr op::params::Action Clear{
    .name      = "Clear Clip",
    .confirm   = "Erase all clips?",
    .on_select = &clear_clip,
};

OP_MODE_PARAMS(Clear);
```

The callback runs in the mode's own context, so it reads and changes the mode's state and sends MIDI just as `process()` does. For an output mode loaded on several outputs, it acts on the instance whose button was selected.

Confirmation is opt-in. Left unset, `.confirm` lets the callback run the moment the button is selected. Holding a short question, it puts a yes/no prompt carrying that question first and the callback runs only on a yes. A destructive command is worth guarding that way, while a harmless one runs straight away.

## How parameters persist

Each mode has its own settings file on the device, `/extras/{mode name}/config.opc`. The device writes your parameter values there when the user leaves the mode's param page, and reads them back when the mode loads. A value that was never changed falls back to the `.default_` you declared. So a user's settings survive a power cycle, and a fresh install starts from your defaults.

An output mode can run as several instances at once, one per assigned output. Each instance keeps its own value for any parameter you mark `.is_per_instance = true`, and all of the instances' values live side by side in that one `config.opc`. Parameters without `.is_per_instance` are shared across every instance. You do not track any of this yourself: the firmware keys each instance's values by its output for you.

## Grouping parameters

For a group of ungated parameters, @ref op::params::make_section "make_section" inlines them under a heading:

```cpp
inline constexpr op::params::Bool    On   { .name = "On" };
inline constexpr op::params::Numeric Rate { .name = "Rate", .min = 1, .max = 32, .default_ = 4 };

OP_MODE_PARAMS(Bpm, Bypass, make_section("LFO", On, Rate));
```

`Bpm` and `Bypass` stay top-level, while `On` and `Rate` appear grouped under "LFO". When a grouped parameter also needs conditional visibility, use the `.parent` form shown next instead.

## Conditional visibility

Show or hide a parameter based on another parameter's live value with a `.visible_when` gate. The `delay` sample (`samples/delay/main.cpp`) uses one `Sync` toggle to swap which timing parameters appear:

```cpp
inline constexpr op::params::Bool Sync{ .name = "Sync", .default_ = true };

// Rate shows only while Sync is on:
inline constexpr op::params::Enum Rate{
    .name = "Rate", .options = {"1/4", "1/8", "1/16"}, .default_ = 1,
    .visible_when = {.watch = Sync, .op = op::params::Op::Eq, .value = 1},
};

// Time shows only while Sync is off:
inline constexpr op::params::Numeric Time{
    .name = "Time", .min = 1, .max = 2000, .default_ = 250,
    .visible_when = {.watch = Sync, .op = op::params::Op::Eq, .value = 0},
};

OP_MODE_PARAMS(Sync, Rate, Time);
```

In the gate, `.watch` is the parameter to test (by object, no `&`), the comparison is one of `Op::Eq`, `Op::Neq`, `Op::Gt`, `Op::Gte`, `Op::Lt`, or `Op::Lte` (see @ref op::modes::op_params::Op "Op"), and `.value` is what to compare against.

Gated parameters work inside a section too. Declare a named @ref op::params::Section "Section", give each parameter `.parent = <section>` alongside its `.visible_when`, and they appear and disappear within the group. The `cc-lfo` sample (`samples/cc-lfo/main.cpp`) reveals each LFO's controls only once its source is set, exactly this way.

@note @ref op::params::make_section "make_section" builds a group from inline, unnamed children, which cannot be gated. To conditionally show a parameter inside a section, declare it flat with `.parent = <section>`, as above.

## Where these render

The device draws all of this as a standard param page and handles navigation, editing, and persistence for you. That is why most modes are declarative-only and never draw anything themselves. When you need a display a list of parameters cannot express, see @ref custom-ui.
