# Declarative Mode Template

A scaffold to copy when starting a mode that carries parameters and draws no screen
of its own. The device builds the settings page out of the parameters the mode
declares, so there is no UI code to write.

The example is a note transposer. Every note-on and note-off is shifted by
Transpose and held inside the MIDI note range, Enable turns the shift off without
unloading the mode, and Channel narrows it to a single MIDI channel or lets every
channel through. A Reset Transpose button sits under the three and puts the shift
back at zero. Everything else passes straight along.

## Make it yours

- **Transform something else.** `OpMidiMessage` carries `status`, `data1`, `data2`,
  `port` and `length`, and a message is changed by writing to it in place. A channel
  remap, a velocity curve or a CC filter all take the shape the transpose takes. See
  [Processing MIDI](../docs/processing-midi.md).
- **Add a parameter.** Declare another `constexpr` object beside the ones in
  `main.cpp` and name it in `OP_MODE_PARAMS`. See [Parameters](../docs/parameters.md)
  for the kinds on offer.
- **Run a command from the params page.** `Reset Transpose` is an
  `op::params::Action`, a row that runs one of the mode's functions when it is
  selected. Give one a `.confirm` message and a yes/no prompt carrying that
  message appears first.
- **Send messages of your own.** `op::api->send_midi(...)` can emit notes,
  which is how an echo or a harmonizer is built.

## Build and flash

```bash
cd template-declarative
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../cmake/operator-mode-toolchain.cmake
cmake --build build
```

This produces `build/my_mode.opm`. Copy it into `/global/` on the Operator's USB
drive, then reboot to load it.

To name the mode, replace `my_mode` in `CMakeLists.txt`, in both `project()` and
`op_add_mode()`. `TYPE global` loads the mode once for the device, and `TYPE output`
loads a copy on each output it runs on.
