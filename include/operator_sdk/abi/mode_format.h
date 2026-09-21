#pragma once
// mode_format.h: binary layout of the .opm mode file header.
//
// Every .opm mode binary begins with a packed 148 byte OpmHeader. It records
// the magic bytes, the SDK version the mode was built against, the section
// layout, the entry point offsets, and the declarative parameter tables. The
// loader validates this header before running any mode code, so a malformed
// header is rejected rather than executed. The header is packed so its byte
// layout is identical across compilers and platforms.

#include <cstdint>

#include "modes/mode_api.h"  // kOperatorSdkVersion
#include "modes/mode_param.h"  // op::modes::ParamSpec (sizeof for header bounds)
#include "config_generated.h"  // op::config::kMaxModeBinarySize, kMaxParamsPerMode

// Magic value that identifies a .opm file. The bytes spell "OPM1" and are
// stored as a little-endian uint32_t.
constexpr uint32_t kOpmMagic = 0x4F504D31;

// OpmHeader.flags bit: the mode handles SysEx. It gates send_sysex and gates
// inbound delivery to the mode's on_sysex export.
constexpr uint8_t kOpmFlagHandlesSysex       = 0x01;  // bit 0
// OpmHeader.flags bit: the custom UI hides the status bar and draws full screen (128x64).
constexpr uint8_t kOpmFlagCustomUiFullScreen = 0x02;  // bit 1
// OpmHeader.flags bit: the mode generates clock messages when Clock Source is Mode.
// Only one clock generating mode can be loaded at a time.
constexpr uint8_t kOpmFlagGeneratesClock     = 0x04;  // bit 2

// Mask of every flag bit defined in this SDK version. The loader accepts any
// subset of these bits. Bits outside the mask are reserved and must stay 0 in a
// compliant .opm file.
constexpr uint8_t kOpmFlagAllKnownMask =
    kOpmFlagHandlesSysex | kOpmFlagCustomUiFullScreen | kOpmFlagGeneratesClock;

// Mode personality stored in OpmHeader.mode_type.
enum class ModeType : uint8_t {
    Global = 0,
    Output = 1,
};

// Packed 148 byte header at the start of every .opm file. The fields are
// organized into seven logical groups (Identity, Section Layout, Relocations,
// Entry Points, Parameter Tables, Name, Reserved). All multi byte fields are
// little-endian. The Reserved tail lets future SDK versions add fields without
// changing the header size.
struct __attribute__((packed)) OpmHeader {
    // Group 1: Identity (offset 0..15, 16 bytes)
    uint32_t magic;                  // 0x4F504D31 ("OPM1")              [ 0]
    uint32_t header_size;            // Must equal sizeof(OpmHeader)==148 [ 4]
    uint16_t sdk_version;            // SDK version mode was built against[ 8]
    uint8_t  mode_type;              // 0 = global, 1 = output            [10]
    uint8_t  flags;                  // Bit 0 = handles_sysex, bit 1 = UI,
                                     // bit 2 = generates_clock           [11]
    uint16_t config_schema_ver;      // Config schema version      [12]
    uint8_t  param_count;            // Number of persistable parameters  [14]
    uint8_t  on_select_count;        // Entries in the Action .on_select
                                     // callback table (0 = no Action). It
                                     // sits beside param_count because that
                                     // is where the other table length
                                     // lives, and it leaves the 8-byte
                                     // reserved tail whole for the next
                                     // field that needs room.            [15]

    // Group 2: Section Layout (offset 16..51, 36 bytes)
    uint32_t text_offset;            // File offset of .text              [16]
    uint32_t text_size;              // Size of .text                     [20]
    uint32_t rodata_offset;          // file offset of .rodata
                                     // (0 if empty)                      [24]
    uint32_t rodata_size;            // bytes of .rodata
                                     // (0 if empty)                      [28]
    uint32_t got_offset;             // File offset of GOT                [32]
    uint32_t got_size;               // Size of GOT                       [36]
    uint32_t data_offset;            // File offset of .data              [40]
    uint32_t data_size;              // Size of .data                     [44]
    uint32_t bss_size;               // Size of .bss (zero-initialized)   [48]

    // Group 3: Relocations (offset 52..59, 8 bytes)
    uint32_t reloc_offset;           // file offset of
                                     // R_ARM_RELATIVE uint32[] (4-byte
                                     // aligned, 0 if reloc_count == 0)   [52]
    uint32_t reloc_count;            // number of uint32 entries[56]

    // Group 4: Entry Points (offset 60..91, 32 bytes). Every offset here is
    // relative to .text start, with one exception: on_select_table_offset is a
    // link VMA that the loader consumes as ram + offset. A zero offset means
    // the mode provides no such handler.
    uint32_t init_offset;            // Offset of mode_init               [60]
    uint32_t process_offset;         // Offset of mode_process            [64]
    uint32_t destroy_offset;         // Offset of mode_destroy            [68]
    uint32_t migrate_offset;         // Offset of mode_migrate_config
                                     // (0 = not provided)                [72]
    uint32_t ui_render_offset;       // Offset of mode_ui_render
                                     // (0 = mode has no custom UI)       [76]
    uint32_t ui_gesture_offset;      // Offset of mode_ui_gesture
                                     // (0 = mode has no custom UI)       [80]
    uint32_t on_sysex_offset;        // Offset of mode_on_sysex
                                     // (0 = mode has no SysEx handler)    [84]
    uint32_t on_select_table_offset; // Link VMA of the Action on_select
                                     // callback table, consumed as
                                     // ram + offset (0 = no Action)       [88]

    // Group 5: Parameter Tables (offset 92..107, 16 bytes).
    // All four fields are zero when param_count == 0.
    uint32_t param_table_offset;     // File offset of packed ParamSpec
                                     // array                             [92]
    uint32_t param_table_size;       // Bytes, must equal param_count *
                                     // sizeof(ParamSpec)                 [96]
    uint32_t param_strings_offset;   // File offset of kParamStrings blob[100]
    uint32_t param_strings_size;     // Bytes of kParamStrings           [104]

    // Group 6: Name (offset 108..139, 32 bytes)
    char     name[32];               // Null-terminated mode name        [108]

    // Group 7: Reserved (offset 140..147, 8 bytes)
    uint8_t  _reserved_tail[8];      // Must be zero, reserved for future
                                     // use (pad to 148)                 [140]
};

static_assert(sizeof(OpmHeader) == 148,
              "OpmHeader must be exactly 148 bytes "
              "(rodata + R_ARM_RELATIVE locators plus an 8-byte reserved tail "
              "for future field growth).");
