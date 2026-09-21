# How Operator Works {#how-operator-works}

Operator is a MIDI processor built on a dual-core ARM Cortex-M33 microcontroller. The two cores have a strict division of labor:

- **Core 0 (interface and control).** Drives the OLED display, reads user inputs, services USB, runs the user interface, mode file operations, and loads modes from flash. Everything seen or touched is handled here.
- **Core 1 (real-time MIDI).** Reads the physical and USB MIDI inputs, routes, merges, and filters messages, runs mode code, and writes into MIDI outputs. It runs a free-running loop with bounded, predictable timing and never blocks on display or storage operations.

Modes always run on Core 1, in the middle of that real-time loop. The rest of this page explains what a mode is, where it sits in the MIDI signal chain, and how the two mode types differ.

## What Is a Mode?

A **mode** is a small program that changes how Operator processes MIDI. Modes are loaded at runtime from `.opm` files on the device's flash. You do **not** rebuild or reflash the firmware to add or update one, but load them into the device when booting into USB Drive mode. This is how Operator is extended. The core firmware stays small and proprietary, while modes (written against this public SDK) add new behavior.

A mode can:

- **Generate** MIDI: an arpeggiator, a master clock, an LFO emitting CCs.
- **Transform** MIDI: transpose, re-channel, apply a velocity curve, humanize timing.
- **Filter or merge** MIDI: drop messages, combine streams (only on global modes), gate by note range or channel.

Each mode is a position-independent ARM binary. It never includes firmware-specific code and never deals with the hardware directly. Instead, it talks to the device through a small and stable **function-table ABI** in the form of an @ref OperatorApi instance handed to it at startup and accessible via @ref op::api. This instance is used to send MIDI, read timing and parameters, draw to the display, and read storage. Because the underlying ABI is append-only, a mode compiled against an older SDK will always work on newer firmware. (The ABI is documented in [reference-abi.md](reference-abi.md) and the on-flash binary layout in [reference-opm-format.md](reference-opm-format.md).)

There are two kinds of modes (**global** and **output**), distinguished by _where_ in the signal chain they run. The next section shows that chain, and the section after it covers the two types in detail.

## Where Modes Run: the MIDI Signal Chain

On every pass of the Core 1 loop, MIDI flows through a fixed pipeline, whether or not anything arrived. Modes plug into two points in it:

```
   +------------+    +--------------+    +-------------+    +--------------+    +------------+
   |   Inputs   |    |    Global    |    |  Routing /  |    |    Output    |    |  Outputs   |
in |  Phys x4   |--->|    modes     |--->|   merge /   |--->|    modes     |--->|  Phys x8   | out
-->|  + USB     |    | (whole flow) |    |   filter    |    | (per output) |    |  + USB     |-->
   +------------+    +--------------+    +-------------+    +--------------+    +------------+
                            ^                                       ^
                      see ALL inputs,                        see only messages
                      before routing                         routed to their output

   Arrows carry the 1-to-3-byte batch through each mode's process(). SysEx rides
   the same two stages but reaches a HANDLES_SYSEX mode through on_sysex().
```

1. **Inputs.** The four physical inputs and USB are read and combined into a single snapshot of everything that arrived this cycle. Incoming clock is detected here.
2. **Global modes.** Active global modes see the _entire_ input snapshot, before any routing happens. They can transform, merge, drop, or add messages. This is where generators, master clocks, and cross-output transforms live. Their output feeds routing.
3. **Routing / merge / filter.** The firmware applies the user's routing settings. An output is fed by the inputs assigned to it, and for each of those inputs the user chooses which kinds of message pass and whether the channel is changed on the way. The same message can reach one output untouched, reach another on a different channel, and be turned away by a third. Clock is handled first, then everything else.
4. **Output modes.** For each output that has modes assigned, those modes see _only_ the messages routed to that output, and can transform or drop them. Each output runs its own independent mode chain.
5. **Outputs.** The resulting messages are written to the eight physical outputs and USB.

The two insertion points define the two mode types. A **global mode** runs at step 2 with a whole-device view, and an **output mode** runs at step 4, scoped to a single output. Everything in this pipeline executes on Core 1 within one bounded cycle, with no dynamic allocation and no blocking I/O.

A mode does not only transform what passes through, it can put a message into the chain, and the call it uses decides where that message is inserted. @ref OperatorApi::send_midi_from "send_midi_from" inserts it before step 3, so it is routed exactly like the message that caused it. @ref OperatorApi::send_midi "send_midi" inserts it inside step 3. The mode has already named the output, so there is nothing left to route, but that output still only takes the kinds of message it is set to pass. @ref OperatorApi::send_midi_direct "send_midi_direct" inserts it after step 4, so the modes on that output never see it. The calls are in @ref processing-midi.

A message carries the input it arrived on in its `port` field. One inserted by naming an output arrived on no input, so its `port` reads `0xFF`.

**System-exclusive messages** ride this same chain, but on their own rail. They never appear in the `process()` batch, which carries only 1 to 3 byte messages. A mode that declares the `HANDLES_SYSEX` flag instead receives them through its `on_sysex()` handler at the very same two insertion points: a global mode sees every input's SysEx before routing, and an output mode sees only the SysEx routed to its output. Such a mode reads the message and can take it, in which case nothing is sent on and no later mode in the chain sees it. A message that no mode takes reaches the routed outputs unchanged, whether no SysEx-capable mode is loaded or the ones that are let it by. The handler runs on the same real-time core as `process()` and under the same rules, and the two share the mode's instance state, as `process()` and `ui_gesture` do. Receiving and generating SysEx is covered in @ref receiving-sysex.

## Core 1 cycle {#core-1-cycle}

Core 1 runs a free-running loop and never waits for MIDI to arrive. One pass reads the inputs, runs the global modes, applies routing and merge and filter, runs the output modes, writes the outputs, and starts over.

Every active mode's `process()` is called on every pass of that loop, independent of incoming MIDI messages. The loop runs far faster than MIDI does, so most passes carry no messages at all and a batch of zero messages is the common case.

That fast cadence is what makes generator modes possible. An arpeggiator, an LFO, a master clock, or a step sequencer gets a call on every pass and decides for itself whether enough time has passed to act. A generator mode never waits for an incoming message to wake it up.

It is also what makes `process()` part of the _hot path_. It runs on every pass, for every loaded mode, so it has to be bounded and non-blocking. A mode that repeatedly overruns its per-call time budget is disabled at runtime, keeping slow or hanging modes from dragging the whole loop late.

What a mode does inside a single call is covered in @ref processing-midi, and driving work from the clock is in @ref timing-and-clock.

## Global vs Output Modes

Modes can be built for two execution contexts, chosen at compile time via `op_add_mode(... TYPE global|output ...)`:

- **Global modes** live in `/global/`. One instance per slot, processing the aggregate MIDI flow before output-specific routing (step 2 above). Good for generators, master clocks, and cross-output transforms. Each receives @ref OpMidiMessage batches with their original `port` field intact, addresses any output directly via the `out` argument to @ref OperatorApi::send_midi "send_midi", and can send a message on behalf of an input with @ref OperatorApi::send_midi_from "send_midi_from", which global modes alone can call.
- **Output modes** live in `/output/`. May have **multiple instances**, one per logical output that has been assigned to the mode in the routing UI (step 4 above). Each instance receives only the messages destined for its assigned output and can mutate them, drop them, or pass them through. An output mode's @ref OperatorApi::send_midi "send_midi" emissions are scoped to the instance's bound output regardless of the `out` argument the mode passes (the firmware remaps it to the instance's output). `is_per_instance` on a `ParamSpec` entry declares that a parameter is stored per-instance rather than shared across all of that output mode's instances.

A mode's type is baked into its `OpmHeader` `mode_type`. The firmware refuses to load a `TYPE global` mode from `/output/` (or vice versa).

## What the device remembers

Beyond dropping `.opm` files into `/global/` and `/output/`, you never manage files on the device by hand. The device keeps track of the rest so a user's setup comes back after a power cycle:

- **Which modes are loaded.** When you load a mode into a slot, the device records it in a small manifest file, `/device/modes.opl`. On the next power-up it restores the same modes in the same places.
- **Each mode's settings.** A mode's parameter values are saved in its own file, `/extras/{mode name}/config.opc`, and restored when the mode loads. An output mode with several instances keeps each instance's settings in that same file. How this looks from your side is in @ref parameters.

These files are the device's business, not yours. A mode reads and writes only inside its own `/extras/{mode name}/` folder, through the storage API (see @ref storage-and-files).

## Next

Understanding how your mode is loaded and called is in [Mode Lifecycle](mode-lifecycle.md), and how to think about the on-device constraints is in [The Mode Mindset](the-mode-mindset.md).
