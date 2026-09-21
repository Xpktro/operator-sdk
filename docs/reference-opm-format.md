# .opm Format Reference {#reference-opm-format}

The on-disk reference for an Operator mode. If you want to inspect, sign, or repackage a `.opm` file without treating the packager as a black box, this page has the exact byte layout. If you are just writing a mode, you can skip it, because `op_add_mode()` produces a valid file for you.

An `.opm` file is the on-flash binary representation of a mode. It is produced by the SDK's packaging step (run for you by `op_add_mode()`) and loaded at runtime by the firmware mode loader. This document is the single authoritative source for the header, the parameter table, and the flag bits.

## File structure

```
+--------------------------+ 0
|  148-byte OpmHeader      |   Seven logical groups
|   Group 1: Identity      |
|   Group 2: Section Layout|
|   Group 3: Relocations   |
|   Group 4: Entry Points  |
|   Group 5: Param Tables  |
|   Group 6: Name          |
|   Group 7: Reserved      |
+--------------------------+ 148
|  .text (code, XIP)       | text_offset, text_size
+--------------------------+
|  .rodata (read-only data)| rodata_offset, rodata_size
+--------------------------+
|  .got (relocation table) | got_offset, got_size
+--------------------------+
|  .data (initial RAM)     | data_offset, data_size
+--------------------------+
|  (0..3 bytes alignment)  | pad so reloc_offset is 4-byte aligned
+--------------------------+
|  R_ARM_RELATIVE uint32[] | reloc_offset, reloc_count * 4
+--------------------------+ EOF
```

The header is always first and always exactly 148 bytes. Section offsets are absolute (from the start of the file), so any section can be read directly. Unused sections declare `_size == 0` and are omitted from the file body. The total file size must not exceed the maximum mode size (256 KiB by default).

## OpmHeader field reference

The `OpmHeader` struct is defined in `operator_sdk/abi/mode_format.h` and enforced by `static_assert(sizeof(OpmHeader) == 148)`. It is packed, little-endian. Offsets below are decimal.

### Group 1: Identity (offset 0..15, 16 bytes)

| Offset | Width | Field | Notes |
|--------|-------|-------|-------|
| 0 | 4 | `magic` | `0x4F 0x50 0x4D 0x31` ("OPM1") |
| 4 | 4 | `header_size` | 148 (little-endian) |
| 8 | 2 | `sdk_version` | e.g. `0x0001` for v1 |
| 10 | 1 | `mode_type` | 0 = global, 1 = output |
| 11 | 1 | `flags` | Bit field, see "Flag bits" |
| 12 | 2 | `config_schema_ver` | Increment on `kParams` break |
| 14 | 1 | `param_count` | May be 0 |
| 15 | 1 | `on_select_count` | Entries in the Action `on_select` callback table. 0 when the mode declares no Action parameters |

### Group 2: Section Layout (offset 16..51, 36 bytes)

| Offset | Width | Field | Notes |
|--------|-------|-------|-------|
| 16 | 4 | `text_offset` | Absolute, typically 148 |
| 20 | 4 | `text_size` | Bytes of `.text` |
| 24 | 4 | `rodata_offset` | Absolute, 0 if `.rodata` empty |
| 28 | 4 | `rodata_size` | Bytes, 0 if `.rodata` empty |
| 32 | 4 | `got_offset` | Absolute |
| 36 | 4 | `got_size` | Bytes (multiple of 4) |
| 40 | 4 | `data_offset` | Absolute |
| 44 | 4 | `data_size` | Bytes of initial `.data` |
| 48 | 4 | `bss_size` | Zero-initialized RAM bytes |

### Group 3: Relocations (offset 52..59, 8 bytes)

| Offset | Width | Field | Notes |
|--------|-------|-------|-------|
| 52 | 4 | `reloc_offset` | File offset of the `R_ARM_RELATIVE` `uint32[]` table (4-byte aligned, 0 if `reloc_count == 0`) |
| 56 | 4 | `reloc_count` | Number of `uint32` entries |

### Group 4: Entry Points (offset 60..91, 32 bytes)

| Offset | Width | Field | Notes |
|--------|-------|-------|-------|
| 60 | 4 | `init_offset` | `mode_init` within `.text` |
| 64 | 4 | `process_offset` | `mode_process` within `.text` |
| 68 | 4 | `destroy_offset` | `mode_destroy` within `.text` |
| 72 | 4 | `migrate_offset` | `mode_migrate_config` within `.text`, 0 if unused |
| 76 | 4 | `ui_render_offset` | `mode_ui_render` within `.text`, 0 if declarative-only |
| 80 | 4 | `ui_gesture_offset` | `mode_ui_gesture` within `.text`, 0 if declarative-only |
| 84 | 4 | `on_sysex_offset` | `mode_on_sysex` within `.text`, 0 if the mode exports no SysEx handler |
| 88 | 4 | `on_select_table_offset` | Link address of the Action `on_select` callback table, read as the RAM base plus this offset rather than as a `.text` offset. 0 when the mode declares no Action parameters |

### Group 5: Parameter Tables (offset 92..107, 16 bytes)

| Offset | Width | Field | Notes |
|--------|-------|-------|-------|
| 92 | 4 | `param_table_offset` | 0 if `param_count == 0` |
| 96 | 4 | `param_table_size` | `param_count * 32` |
| 100 | 4 | `param_strings_offset` | 0 if `param_count == 0` |
| 104 | 4 | `param_strings_size` | Bytes of the NUL-separated label blob |

### Group 6: Name (offset 108..139, 32 bytes)

| Offset | Width | Field | Notes |
|--------|-------|-------|-------|
| 108 | 32 | `name[32]` | ASCII, NUL-terminated, no `/`, backslash, or `.` |

### Group 7: Reserved (offset 140..147, 8 bytes)

| Offset | Width | Field | Notes |
|--------|-------|-------|-------|
| 140 | 8 | `_reserved_tail` | Must be zero, reserved for future use |

## Flag bits {#flag-bits}

| Bit | Mask | Constant | Meaning |
|-----|------|----------|---------|
| 0 | `0x01` | `kOpmFlagHandlesSysex` | The mode actively inspects, consumes, or generates system-exclusive messages. It may call `send_sysex`, and inbound SysEx reaches its `on_sysex` handler. |
| 1 | `0x02` | `kOpmFlagCustomUiFullScreen` | Custom UI suppresses the status bar. The mode owns the full 128x64 frame. |
| 2 | `0x04` | `kOpmFlagGeneratesClock` | Mode emits 24 PPQ clock and is eligible when the user sets Clock Source to `Mode`. Valid on a global mode. Only one such mode may load at a time. |

Unknown bits are reserved and must be zero. A header carrying a bit outside the mask above is refused at load.

`op_add_mode()` writes this byte. It takes the flags by name, and the names compose, so `FLAGS FULLSCREEN_UI GENERATES_CLOCK` is a clock-generating mode that draws the whole screen. It refuses `GENERATES_CLOCK` on `TYPE output` at configure time, and the packaging step refuses the same combination, so the rule is reported where the `FLAGS` line can be edited.

## ParamSpec schema

The `ParamSpec` struct is defined in `operator_sdk/abi/mode_param.h`. It is packed to 32 bytes, and the size is `static_assert`ed to catch accidental layout changes. One `ParamSpec` entry describes one declarative parameter. The packed array lives at `param_table_offset` and the display strings live at `param_strings_offset`.

| Offset | Width | Field | Meaning |
|--------|-------|-------|---------|
| 0 | 2 | `name_offset` | Byte offset into `kParamStrings` for the display name |
| 2 | 2 | `options_offset` | Byte offset to enum labels (Enum/List) or the filter-ext string (FilePicker), 0 = absent |
| 4 | 1 | `kind` | `ParamKind` enum |
| 5 | 1 | `is_per_instance` | Output-mode only: 1 = stored per instance, 0 = shared across instances |
| 6 | 1 | `decimal_places` | 0..3. Rendering divides the int32 value by 10^n |
| 7 | 1 | `option_count` | For Enum/List: number of options. 0 otherwise |
| 8 | 4 | `min_value` | Inclusive min |
| 12 | 4 | `max_value` | Inclusive max (the maximum filename length for FilePicker) |
| 16 | 4 | `default_value` | Used when `.opc` is absent or on a schema reset |
| 20 | 4 | `step_value` | Numeric increment per detent, 1 for non-numeric kinds |
| 24 | 1 | `value_slot_count` | 1 for scalar kinds, 2 for `Range` / `NoteRange` |
| 25 | 1 | `parent_param_id` | 0xFF = top-level, else the index of a Section parent |
| 26 | 1 | `visible_when_param_id` | Op plus a sentinel byte (0xFF always-visible, 0xFE always-hidden) |
| 27 | 1 | `visible_when_value` | Signed compare target (-128..127) |
| 28 | 1 | `visible_when_watcher` | Watcher ParamSpec index 0..255 |
| 29 | 1 | `on_select_index` | Action only: the entry it runs in the `on_select` callback table, and it must be below `on_select_count`. 0 for every other kind |
| 30 | 2 | `_reserved_tail[2]` | Must be zero, reserved for future use |

`ParamKind` values:

| Value | Name | Slots | Notes |
|-------|------|-------|-------|
| 0 | `Bool` | 1 | Rendered as checkmark or empty box |
| 1 | `Numeric` | 1 | Honors `decimal_places` and `step_value` |
| 2 | `Enum` | 1 | `options_offset` points at null-separated labels |
| 3 | `List` | 1 | Multi-select mask, label format same as Enum |
| 4 | `Range` | 2 | Low at slot i, high at slot i+1 |
| 5 | `NoteRange` | 2 | Same shape as Range, renders as MIDI note names |
| 6 | `Note` | 1 | A single note number, rendered with a note-name label such as C4 |
| 7 | `FilePicker` | 1 | The stored value is an internal offset, so a picked file is read through `param<FilePickerSpec>()`, which yields a `FilePickerValue { char filename[N+1]; bool empty(); }`, or through `api->get_param_filename` for the raw shim |
| 8 | `Section` | 0 | A container that holds no value and is not persisted. Its `parent_param_id` may point at another Section to nest |
| 9 | `Action` | 0 | A button that runs a callback, optionally behind a confirmation prompt taken from `options_offset`. Holds no value and is not persisted |

The parameter kinds are covered in @ref parameters. How to read a FilePicker's chosen file is in @ref storage-and-files.

## Validation invariants

A `.opm` is rejected unless all of the following hold:

1. `magic == 0x4F504D31` and `header_size == 148`.
2. `sdk_version <= kOperatorSdkVersion` (firmware refuses newer modes).
3. `mode_type <= 1`.
4. Every bit set in `flags` is one of the flag bits listed above.
5. `mode_type` is global whenever the clock-generating bit is set.
6. `text_size + rodata_size + got_size + data_size + bss_size <= kMaxModeBinarySize`. All five allocatable sections are in the sum, because the limit covers the RAM a loaded mode occupies, not only the bytes stored in the file.
7. Each `<section>_offset + <section>_size` fits within `sizeof(OpmHeader) + kMaxModeBinarySize`.
8. `param_count <= kMaxParamsPerMode`.
9. If `param_table_offset != 0`, then `param_table_size == param_count * sizeof(ParamSpec)`.
10. Every nonzero `.text`-relative entry point (`init_offset`, `process_offset`, `destroy_offset`, `migrate_offset`, `ui_render_offset`, `ui_gesture_offset`, `on_sysex_offset`) falls within `[text_offset, text_offset + text_size)`. Zero means the mode exports nothing at that entry point.
11. `on_select_count <= kMaxParamsPerMode`; `on_select_table_offset` and `on_select_count` are both zero or both nonzero; and when nonzero, the table lies wholly inside the mode's `.rodata`.
12. `name[0] != '\0'`, `name[31] == '\0'`, and no byte in `name` is `/`, backslash, or `.`.

A file that fails any of these is reported to the user and the load is aborted.

## How the file is produced

You do not normally build a `.opm` by hand. `op_add_mode()` runs the SDK's packaging step as part of the build, turning your compiled mode into the layout above. It is deterministic, so the same input always produces a byte-identical `.opm`. If a build fails while packaging, the common causes and their fixes are in [Debugging](debugging.md).

## See also

- [ABI Reference](reference-abi.md): the runtime `OperatorApi` function table and the gesture ABI.
- @ref per-instance-memory "The Mode Mindset": why your mutable state stays private to each instance.
- [Mode Lifecycle](mode-lifecycle.md): how a mode is loaded and called.
