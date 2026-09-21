# Operator SDK

The Operator SDK lets you build MIDI processing and UI modes that the Operator device loads dynamically from its `/global/` and `/output/` folders. A mode is a small, position-independent ARM binary that plugs into the device's real-time MIDI pipeline and talks to the firmware through a stable, append-only function-table ABI. You never rebuild or reflash the firmware to add one. You drop a `.opm` file onto the device and load it.

New here? Start with [Getting Started](getting-started.md). To understand the device before writing code, read [How Operator Works](how-operator-works.md).

**Start here**

- @subpage getting-started (install the toolchain and build your first mode)

**Concepts**

- @subpage how-operator-works (the two cores, the MIDI signal chain, global vs output modes)
- @subpage mode-lifecycle (the hooks, and how a mode becomes a live callback)

**How-to**

- @subpage processing-midi (read, transform, drop, and generate MIDI: the core of a mode)
- @subpage parameters (declaring parameters, reading them, and how they persist)
- @subpage custom-ui (drawing your own screen and handling input)
- @subpage timing-and-clock (a smooth position from the clock, and generating or consuming it)
- @subpage storage-and-files (reading files, the FilePicker)
- @subpage the-mode-mindset (the constraints that shape every mode, and why)
- @subpage debugging (the on-device MIDI monitor, the Log Monitor, and common build failures)
- @subpage testing (test your mode's logic on your computer, no hardware needed)

**Reference**

- @subpage reference-abi (the `OperatorApi` function table and the gesture ABI)
- @subpage reference-opm-format (the `.opm` header, parameter table, and flags)
- @subpage reference-build (prerequisites, `op_add_mode()`, the toolchain files)

The per-symbol API reference generated from the headers is under **Namespaces**, **Classes**, and **Files** in the sidebar.
