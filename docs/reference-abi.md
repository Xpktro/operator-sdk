# ABI Reference {#reference-abi}

The lookup table for everything your mode calls at runtime. The `OperatorApi` is a versioned struct of function pointers, and a mode resolves an entry by its position, so the table only ever grows and existing entries keep their slot. Your custom UI also receives a flat gesture event. The on-disk binary layout (`OpmHeader`, `ParamSpec`) lives in [.opm Format](reference-opm-format.md).

A mode compiled against SDK version N continues to load on firmware version M where M is greater than or equal to N, for the lifetime of the product. See [ABI versioning](#abi-versioning) below for the exact rules.

## `OperatorApi` function table

Defined in `operator_sdk/abi/mode_api.h`. SDK v1.0 populates 28 function pointers in the order listed. The index is the 0-based slot inside the struct. Reordering or removing any entry would make modes un-loadable. Each name links to its full declaration.

| Idx | Function | Purpose |
|-----|----------|---------|
| 0 | @ref OperatorApi::send_midi "send_midi" | Queue a 1 to 3 byte MIDI message for output, where the modes loaded after this one can act on it. `out` selects the output for a global mode (0..7 = OUT 1..8, 8 = USB) and is ignored for an output mode, which reaches its bound output. The output takes the message only if it passes that kind of message. Clock (`0xF8`) goes to the output on its own and joins no mode's batch. |
| 1 | @ref OperatorApi::send_midi_direct "send_midi_direct" | Queue a 1 to 3 byte MIDI message straight to the output, skipping the modes loaded after this one. Output selection and the message-type rule match `send_midi`. Suits a timing reference or a tap reporting what an output is doing. |
| 2 | @ref OperatorApi::send_midi_from "send_midi_from" | Queue a 1 to 3 byte MIDI message caused by one the mode was handed. It joins the traffic on the input named by the first argument (0..3 = IN 1..4, 4 = USB), so it is routed the way its cause is routed. Global modes only, refused for an output mode. |
| 3 | @ref OperatorApi::send_sysex "send_sysex" | Queue a SysEx message the mode generates, or one fragment of it. Output scoping matches `send_midi`: an output mode's argument is ignored and its bound output is used, a global mode names the output (0..7 = OUT 1..8, 8 = USB). Returns the fragment's byte count, `kOutputBusy` (-2) when the output cannot take it this pass, `kOutputTooLarge` (-3) when the fragment can never fit, or -1 for an invalid output, an output not set to pass system-exclusive, or a mode not allowed to send SysEx. |
| 4 | @ref OperatorApi::output_active "output_active" | Returns true only if `out` can currently emit MIDI: in range, not user-disabled, and physically present (USB output activates only while USB MIDI is active). Mirrors `send_midi`'s drop behavior, so a mode can skip dead or disabled outputs instead of emitting into the void. |
| 5 | @ref OperatorApi::get_tick "get_tick" | Monotonic microsecond counter since boot. |
| 6 | @ref OperatorApi::get_pulse_count "get_pulse_count" | Monotonic-since-boot 24 PPQ pulse counter. Advances when the active clock source emits a clock message. |
| 7 | @ref OperatorApi::set_clock_period_us "set_clock_period_us" | Program the internal clock generator. Effective only when the caller is a clock-gen mode and Clock Source is set to Mode. |
| 8 | @ref OperatorApi::storage_read "storage_read" | Read up to `size` bytes from `/extras/{mode}/<path>`, returning bytes read or a negative error. |
| 9 | @ref OperatorApi::storage_file_open "storage_file_open" | Open a handle for streaming reads under `/extras/{mode}/<path>`. |
| 10 | @ref OperatorApi::storage_file_read "storage_file_read" | Read up to `size` bytes from a handle. 0 = EOF, -1 = error. |
| 11 | @ref OperatorApi::storage_file_seek "storage_file_seek" | Reposition a handle's read cursor. |
| 12 | @ref OperatorApi::storage_file_close "storage_file_close" | Close a streaming handle. |
| 13 | @ref OperatorApi::storage_file_open_write "storage_file_open_write" | Open a handle for streaming writes under `/extras/{mode}/<path>`. `open_mode` is `kStorageOpenTruncate` (create or overwrite from empty) or `kStorageOpenOverwrite` (overwrite in place). Returns a handle, -1 on error, or -2 (`kStorageBusy`). |
| 14 | @ref OperatorApi::storage_file_write "storage_file_write" | Write `size` bytes to a write handle. Returns bytes written, -1 on error, -2 (`kStorageBusy`) when busy, or -3 (`kStorageNoSpace`) when the store is full. |
| 15 | @ref OperatorApi::storage_delete "storage_delete" | Delete `/extras/{mode}/<path>`. Returns 0 on success, -1 on error, or -2 (`kStorageBusy`). |
| 16 | @ref OperatorApi::storage_enumerate "storage_enumerate" | List the mode's stored files into `out` as NUL-separated names, filtered by an optional extension `pattern`. Returns the entry count, -1 on error, or -2 (`kStorageBusy`). |
| 17 | @ref OperatorApi::draw_rect "draw_rect" | Draw a 1-pixel outline rectangle. |
| 18 | @ref OperatorApi::fill_rect "fill_rect" | Draw a filled rectangle with ON or OFF pixels. |
| 19 | @ref OperatorApi::draw_text "draw_text" | Render text in the 5x7 standard font. |
| 20 | @ref OperatorApi::draw_text_large "draw_text_large" | Render text in the 8x12 large font. |
| 21 | @ref OperatorApi::draw_bitmap "draw_bitmap" | Blit a packed 1-bit bitmap. The packing is described in @ref custom-ui. |
| 22 | @ref OperatorApi::set_pixel "set_pixel" | Turn a single pixel ON or OFF. Out-of-range coordinates are discarded. |
| 23 | @ref OperatorApi::log "log" | Emit a log line. Level is one of 0 = debug, 1 = info, 2 = warn, 3 = error. Visible and filterable in the Monitor view. |
| 24 | @ref OperatorApi::get_param_value "get_param_value" | Read the current int32 value for a declarative param. |
| 25 | @ref OperatorApi::set_param_value "set_param_value" | Write a declarative param value. Core 0 publishes it within one frame. |
| 26 | @ref OperatorApi::get_param_filename "get_param_filename" | Resolve a FilePicker param to a NUL-terminated filename. Returns bytes written (excluding NUL), 0 for "no file picked yet", -1 on validation error, and -2 (`kStorageBusy`) when busy. Prefer the typed `param<FilePickerSpec>()` accessor from `<operator_sdk/params.h>`. |
| 27 | @ref OperatorApi::get_clock_state "get_clock_state" | Returns one of `OpClockState`: `kClockStateActive`, `kClockStateNoClock`, or `kClockStateMode`. |

Every function is documented on the @ref OperatorApi struct page, which carries the per-parameter and return-value notes from the header's `///` comments. Your editor shows the same on hover.

## Custom-UI gesture ABI

A custom-UI mode's `mode_ui_gesture` export is a 4-argument flat event handler:

```c
void mode_ui_gesture(uint8_t encoder_id, uint8_t gesture,
                     uint8_t gesture_type, int16_t value);
```

- `encoder_id` selects the physical control: `kEncoderLeft` (0) or `kEncoderRight` (1).
- `gesture` is `kGestureRotate` (0, signed `value`), `kGestureShortPress` (1), or `kGestureLongPress` (2).
- `gesture_type` is the firmware's input-mapped intent: `kGestureTypeNone` (0), `kGestureTypeScroll` (1), `kGestureTypeChange` (2), `kGestureTypeEnter` (3), or `kGestureTypeBack` (4). Modes can use `gesture_type` to respect the firmware input-map behavior, or use `encoder_id` and `gesture` for raw, map-independent behavior (for example an X/Y "etch-a-sketch").
- `value` is the signed rotation step (with acceleration applied) for rotations, or 0 for presses.

Long-presses are reserved by the firmware and modes NEVER handle them (on either the physical fields or `gesture_type`): `kGestureTypeExitUi` (6, exit the custom UI) and `kGestureTypeEnterParams` (5, open the Parameters page). The layout zones and reserved gestures for custom UIs are covered in @ref custom-ui.

## ABI versioning

Every new SDK release bumps `kOperatorSdkVersion` when the function table or header gains new fields. The rules below are absolute when releasing new versions:

- **Append only.** New function pointers go at the end of `OperatorApi`. New fields go into a reserved tail region of `OpmHeader`.
- **Never reorder.** The ordering of existing fields does not change.
- **Never remove.** Deprecated functions stay at their original offset. The firmware may return a sentinel for no-op behavior, but the slot is not recycled.
- **Never change types.** A signature does not change even if both the old and new types are the same size.

Firmware validates `sdk_version` on load and refuses modes newer than it understands. Modes older than the firmware always load, because the unused appended functions are simply never called by the mode.

## Compatibility matrix

| SDK Version | Firmware Versions That Load It | Notes |
|-------------|--------------------------------|-------|
| v1.0.0 | v1.0.0 | Initial release. |

## See also

- [.opm Format](reference-opm-format.md): the `OpmHeader`, `ParamSpec`, flag bits, and packaging.
- @ref custom-ui shows the drawing primitives and gesture handling in context.
- [Storage and Files](storage-and-files.md): using the storage entries safely from Core 1.
