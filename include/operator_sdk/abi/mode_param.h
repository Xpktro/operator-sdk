#pragma once
/// @file mode_param.h
/// @brief Declarative parameter schema embedded in a .opm mode binary.
///
/// A mode declares its parameters once as a data table of ParamSpec entries.
/// The firmware reads that table to render a generic settings screen, persist
/// the values, and move values between cores, so a mode gets a full parameter
/// UI without writing any UI code. ParamSpec is packed and size-locked for ABI
/// stability, and every value is stored as an int32_t.

#include <cstdint>

#include "config_generated.h"  // op::config::kMaxParamValuesPerMode
#include "modes/mode_api.h"    // extern "C" context for kOperatorSdkVersion

// ABI-side parameter encoding shared by the firmware and SDK.
namespace op::modes {

// Widget kind for a declarative parameter.
// Bool, Numeric, Enum, and List are scalar and occupy one value slot.
// Range and NoteRange occupy two consecutive value slots (low then high).
// Note is a scalar single note number that occupies one value slot and renders
// with a note-name label (for example C4).
// FilePicker is scalar and stores a filename reference (see ParamSpec). The
// FilePicker entry reuses options_offset to point at an extension filter
// string (for example ".scl") and max_value as the maximum filename length.
// Section is a pure container that holds no value and is not persisted, and
// its parent_param_id may point at another Section to nest arbitrarily.
// Action holds no value and is not persisted. It renders as a button that runs
// a callback, optionally behind a confirmation prompt.
enum class ParamKind : uint8_t {
    Bool       = 0,
    Numeric    = 1,
    Enum       = 2,
    List       = 3,  // MultiSelect in UI terms
    Range      = 4,
    NoteRange  = 5,
    Note       = 6,
    FilePicker = 7,
    Section    = 8,  // pure container; value_slot_count=0; not persisted.
                     // parent_param_id may point at another Section (arbitrary nesting).
    Action     = 9,  // runs a callback. value_slot_count=0, not persisted.
                     // options_offset holds an optional confirm prompt.
};

// One parameter's declaration, packed to a fixed 32 bytes.
// Strings (the display name and any option labels) are referenced by byte
// offset into the mode's string table rather than stored inline. Every value
// is an int32_t. The size is checked with a static_assert so an accidental
// layout change is caught at compile time.
struct __attribute__((packed)) ParamSpec {
    uint16_t name_offset;       // Into mode's string table (0 permitted for unnamed)
    uint16_t options_offset;    // Into string table; 0 = no options
    uint8_t  kind;              // ParamKind
    uint8_t  is_per_instance;   // output modes only (boolean 0/1)
    uint8_t  decimal_places;    // 0..3; rendering divides by 10^n (loader soft-clamps)
    uint8_t  option_count;      // Number of options for Enum / List (0 otherwise)
    int32_t  min_value;         // Inclusive min
    int32_t  max_value;         // Inclusive max
    int32_t  default_value;     // Used when .opc absent or on schema reset
    int32_t  step_value;        // Numeric step; 1 for non-numeric kinds
    // Range and NoteRange occupy two consecutive value slots (low then high);
    // every scalar kind occupies one. An unset (0) field is normalized by the
    // loader. The sum across all specs must not exceed kMaxParamValuesPerMode.
    uint8_t  value_slot_count;          // byte 24
    // 0xFF = top-level, otherwise an index into the mode's ParamSpec array, and
    // the referenced entry must have kind == Section (the loader checks this).
    uint8_t  parent_param_id;           // byte 25
    // Visibility operator plus sentinel. 0xFF = always visible, 0xFE = always
    // hidden. Otherwise the high 3 bits hold the compare op (0=Eq, 1=Neq,
    // 2=Gte, 3=Lte, 4=Gt, 5=Lt; 6 and 7 are reserved and fall back to visible).
    // Test the whole byte against the sentinels before unpacking the op.
    uint8_t  visible_when_param_id;     // byte 26
    // Signed compare target, range -128..+127.
    int8_t   visible_when_value;        // byte 27
    // The watcher's ParamSpec index, 0..255. Only meaningful when
    // visible_when_param_id is not a sentinel.
    uint8_t  visible_when_watcher;      // byte 28
    // Holds an Action's on_select callback index and is zero for every
    // other kind.
    uint8_t  on_select_index;           // byte 29
    // Reserved. Must stay at zero.
    uint8_t  _reserved_tail[2];         // bytes 30..31
};
static_assert(sizeof(ParamSpec) == 32, "ParamSpec must be 32 bytes");

// visible_when packing and evaluation helpers, shared so the firmware and SDK
// agree on the encoding. Each field and helper below documents its own bits.

inline constexpr uint8_t kVisibleAlways = 0xFF;
// 0xFE is the always-hidden sentinel: a state-only parameter that is persisted
// but never drawn. Like 0xFF it is tested against the whole byte before the op
// is unpacked, and the watcher byte is ignored when a sentinel is present.
inline constexpr uint8_t kVisibleNever = 0xFE;

namespace op_params {  // avoid clashing with the SDK's op::params namespace
/// @brief Comparison for a parameter's `visible_when` gate.
///
/// The gate compares the watched parameter's current value against the gate's
/// target value using one of these operators. Written at the call site through
/// the `op::params::Op` alias, for example `op::params::Op::Eq`.
enum class Op : uint8_t {
    Eq  = 0,   ///< Visible while the watched value equals the target.
    Neq = 1,   ///< Visible while the watched value differs from the target.
    Gte = 2,   ///< Visible while the watched value is greater than or equal to the target.
    Lte = 3,   ///< Visible while the watched value is less than or equal to the target.
    Gt  = 4,   ///< Visible while the watched value is greater than the target.
    Lt  = 5,   ///< Visible while the watched value is less than the target.
    // 6, 7 reserved. is_visible() falls back to visible for unknown ops.
};
}  // namespace op_params

// Pack a compare op into the high 3 bits of the visible_when_param_id byte.
inline constexpr uint8_t pack_visible_when(op_params::Op op) {
    return static_cast<uint8_t>(static_cast<uint8_t>(op) << 5);
}

// Read the watcher index (0..255) from a spec. Only meaningful when
// visible_when_param_id is not a sentinel.
inline constexpr uint8_t unpack_watcher_id(const ParamSpec& spec) {
    return spec.visible_when_watcher;
}

// Unpack the compare op (0..7) from a packed byte.
inline constexpr uint8_t unpack_op_code(uint8_t packed) {
    return static_cast<uint8_t>((packed >> 5) & 0x07u);
}

// Evaluate visibility. The sentinels are tested first. Reserved op codes fall
// back to visible.
inline constexpr bool is_visible(uint8_t packed_byte,
                                 int32_t watcher_value,
                                 int8_t compare_value) {
    if (packed_byte == kVisibleAlways) return true;
    if (packed_byte == kVisibleNever) return false;
    const uint8_t op_code = unpack_op_code(packed_byte);
    const int32_t lhs = watcher_value;
    const int32_t rhs = static_cast<int32_t>(compare_value);
    switch (op_code) {
        case 0: return lhs == rhs;   // Eq
        case 1: return lhs != rhs;   // Neq
        case 2: return lhs >= rhs;   // Gte
        case 3: return lhs <= rhs;   // Lte
        case 4: return lhs >  rhs;   // Gt
        case 5: return lhs <  rhs;   // Lt
        default: return true;        // reserved (6,7) fall back to visible
    }
}

}  // namespace op::modes
