#pragma once
/// @file params.h
/// @brief Declarative parameter-table DSL for Operator SDK modes.
///
/// Describe each parameter as a small, kind-specific struct and pass them all
/// to OP_MODE_PARAMS(). The SDK builds the string table and emits the three
/// ABI globals the firmware reads (kParams, kParamCount, kParamStrings) for
/// you, so you fill in only the fields that matter for each parameter.
///
/// Usage:
///
///     #include <operator_sdk.h>
///     #include <operator_sdk/params.h>
///
///     OP_MODE_PARAMS(
///         op::params::Numeric{.name = "Transpose", .min = -12, .max = 12,
///                             .default_ = 0},
///         op::params::Bool   {.name = "Enable",    .default_ = true},
///         op::params::Enum   {.name = "Channel",
///                             .options  = {"All","Ch 1","Ch 2","Ch 3",
///                                          "Ch 4","Ch 5","Ch 6","Ch 7",
///                                          "Ch 8","Ch 9","Ch 10","Ch 11",
///                                          "Ch 12","Ch 13","Ch 14","Ch 15",
///                                          "Ch 16"},
///                             .default_ = 0}
///     );
///
/// Parameter kinds, each a struct in this namespace:
///   - Numeric    an integer with min/max/step and optional fixed-point display
///   - Bool       an on/off toggle
///   - Enum       one choice from a named list
///   - List       a multi-select set of named options, stored as a bitmask
///   - Note       a single MIDI note, shown as a note name
///   - Range      a low/high integer pair
///   - NoteRange  a low/high MIDI-note pair
///   - FilePicker a file chosen from storage, filtered by extension
///   - Section    a grouping header for the params that follow it
///   - Action     a button that runs one of the mode's functions
///
/// Every kind carries `.name`, an optional `.parent` Section, an optional
/// `.visible_when` gate, and `.is_per_instance`. The kind-specific fields
/// (`.min`, `.options`, `.default_`, and so on) are documented on each struct
/// below. Read a value at run time with `param<Obj>()`, which returns the int32
/// value, a `std::pair` for the Range kinds, or a FilePickerValue for
/// FilePicker.
///
/// This header is opt-in. operator_sdk.h does not transitively include it.

#include <operator_sdk.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <tuple>        // SectionGroup stores children as std::tuple
#include <type_traits>  // std::is_same_v / std::remove_cvref_t in param<>
#include <utility>

#include <config_generated.h>  // op::config::kMaxParamsPerMode

// ---------------------------------------------------------------------------
// Build-injected mode identifiers. Defaulted so this header still compiles
// when included from non-sample translation units (doctest harnesses that
// #include params.h without going through op_add_mode). Under op_add_mode,
// the CMake helper injects the real values via target_compile_definitions
// before the mode TU is compiled.
// ---------------------------------------------------------------------------
#ifndef OP_MODE_NAME
#define OP_MODE_NAME "unknown"
#endif
#ifndef OP_MODE_TYPE_VALUE
#define OP_MODE_TYPE_VALUE 0
#endif
#ifndef OP_MODE_FLAGS
#define OP_MODE_FLAGS 0x00
#endif
#ifndef OP_MODE_CONFIG_SCHEMA_VER
#define OP_MODE_CONFIG_SCHEMA_VER 0
#endif

/// @namespace op::params
/// @brief Declarative parameter DSL for modes: param specs and widget builders.
namespace op::params {

// ---------------------------------------------------------------------------
// Surface the ABI-defined Op enum under op::params so call sites write
// `op::params::Op::Eq`. The byte-identical encoding lives in mode_param.h. The
// sub-namespace `op::modes::op_params` was chosen in the ABI header to avoid
// colliding with this namespace.
// ---------------------------------------------------------------------------
/// @brief Comparison used by a parameter's `visible_when` gate.
///
/// This is the operator field of a `visible_when` gate (see @ref gate). Write
/// it as `Op::Eq`, `Op::Neq`, `Op::Gt`, `Op::Gte`, `Op::Lt`, or `Op::Lte` at
/// the call site. See @ref op::modes::op_params::Op for what each one means.
using Op = ::op::modes::op_params::Op;

// Forward declarations:
//   * `Section`, defined later in this header. `.parent` captures a Section
//     reference via the `ParentRef` wrapper (analogous to `WatcherRef`).
//   * `ParentRef`, a wrapper struct enabling `.parent = Sec` (no `&` required,
//     uniform with `.watch = Obj`). Definition appears after Section because
//     the converting constructor body reads `&section` which requires the
//     complete Section type.
struct Section;
struct ParentRef;

// ---------------------------------------------------------------------------
// Watcher references are object-ref only, uniform with .parent. Any param
// that wants `.visible_when` must be declared at file scope as `inline
// constexpr` (flat form) so this struct can capture a real C++ object
// reference to its watcher.
// Structural `make_section("S", A, B)` remains valid for ungated grouping
// (no .visible_when on any child). Gated params must be flat-form so
// siblings have a named identifier to reference as `.watch = WatcherObj`.
//
// Authoring note: the "no gate" state is expressed by leaving `.watch`
// default-constructed (null `target`). The stage() post-pass treats a null
// `target` as "always visible" and serializes the param's
// visible_when_param_id as kVisibleAlways (0xFF, the always-visible sentinel).
// ---------------------------------------------------------------------------

// ParentRef lets the author write `.parent = Section` (no `&`). The converting
// constructor captures the Section's address. It is the parent-side analogue of
// `WatcherRef`. Default-constructed (`section == nullptr`) encodes "top-level"
// (no parent).
//
// The converting constructor body is defined out-of-line (after Section is
// fully defined) because taking `&sec` requires the complete Section type.
struct ParentRef {
    // Internal pointer: `nullptr` means "top-level" (no parent Section).
    // stage() resolves this captured address to a ParamSpec index (see the
    // resolution note lower in this header).
    const Section* section = nullptr;

    constexpr ParentRef() = default;

    // Converting constructor: capture a Section reference by address.
    // Body defined out-of-line below `struct Section` because `&sec`
    // requires Section to be a complete type at instantiation.
    constexpr ParentRef(const Section& sec);
};

// WatcherRef captures the watcher's address at construction time. The user
// writes `.watch = WatcherObj` (no `&`) and stage() later resolves that
// address to a ParamSpec index. Default-constructed (`target == nullptr`)
// encodes "no gate". A stray non-param reference simply fails to resolve, and
// the compile-fail harness guards the typed `.watch = Obj` surface.
struct WatcherRef {
    // Internal representation: the watcher object's address, captured at
    // construction time. (C++ member fields cannot carry the `constexpr`
    // keyword directly. The field is constexpr-initialized because both the
    // default ctor and the converting ctor are `constexpr`, and every
    // WatcherRef instance lives inside an `inline constexpr` VisibleWhen
    // value whose watcher has static storage duration.)
    const void* target = nullptr;

    constexpr WatcherRef() = default;

    // Capture any kind struct's (or Section's) address by identity at
    // construction time. The watcher must be `inline constexpr` at file
    // scope so `&watcher` is a constant expression.
    template <typename T>
    constexpr WatcherRef(const T& watcher)
        : target(static_cast<const void*>(&watcher)) { }
};

struct VisibleWhen {
    WatcherRef watch = {};  // user writes `.watch = Obj`, captures the watcher's
                            // address (resolved by identity at staging)
    Op op        = Op::Eq;
    int8_t value = 0;
    bool never   = false;  // hidden(): always hidden (serializes 0xFE).
                           // Default false preserves the always-visible
                           // (0xFF) behavior of an unset .visible_when.
};

/// @brief Gate a parameter's visibility on another parameter's current value.
///
/// Call-form shorthand for a conditional `.visible_when`. Use it whenever a
/// param should only appear in certain states, for example hiding an LFO's
/// depth and rate until its source enum is set, or hiding "Maximum" while a
/// "Constant" toggle is on. The call form and the designated-initializer
/// aggregate are exactly equivalent, so pick whichever reads better at the
/// call site:
///
///     .visible_when = gate(Constant, Op::Eq, false)                     // call
///     .visible_when = {.watch = Constant, .op = Op::Eq, .value = false} // aggregate
///
/// Both keep the param visible only while `Constant == false`.
/// @param watcher The other param object to watch (bare reference, no `&`).
/// @param op How to compare the watcher's value (see Op).
/// @param value The value to compare the watcher against.
/// @return A VisibleWhen to assign to a param's `.visible_when` field.
template <typename T> constexpr VisibleWhen gate(const T& watcher, Op op, int8_t value) {
    return VisibleWhen {WatcherRef {watcher}, op, value};
}

/// @brief Mark a state-only param as always hidden.
///
/// A param carrying `.visible_when = hidden()` is persisted across reboots and
/// never drawn on the param page, which suits a value the mode keeps for itself
/// and only needs stored.
/// @return A gate that keeps the param off the param page.
constexpr VisibleWhen hidden() {
    return VisibleWhen {.never = true};
}

// ---------------------------------------------------------------------------
// User-facing parameter descriptors.
// ---------------------------------------------------------------------------
// Each kind carries only the fields that make semantic sense for it, and the
// emitter fills the remaining ABI slots with correct neutral values.
// Designated initializers (C++20) make call-site syntax self-documenting.
// Note: in C++20 strict mode all aggregate initializers must either be all
// designated or none, so pass `.name = "..."` alongside the other fields.
//
// Every kind carries `.parent` (optional Section ref) and `.visible_when`
// (optional sibling gate).

/// @brief An integer parameter with a min, max, step, and optional decimals.
///
/// The most common kind, an integer value in [min, max].
/// Use `.decimal_places` for fixed-point display (the stored value stays an
/// integer, for example store 12000 with 2 decimals to show 120.00).
struct Numeric {
    /// Display name shown in the settings UI.
    std::string_view name;
    /// Minimum value.
    int32_t min;
    /// Maximum value.
    int32_t max;
    /// Initial value.
    int32_t default_ = 0;
    /// Increment between adjacent values.
    int32_t step = 1;
    /// Fixed-point display digits. The stored value stays an integer, so
    /// storing 12000 with 2 places shows "120.00".
    uint8_t decimal_places   = 0;
    uint8_t value_slot_count = 1;
    /// Set to 1 to give each mode instance its own copy of this value (output
    /// modes only; ignored for global modes).
    uint8_t is_per_instance = 0;
    /// Optional Section this parameter nests under.
    ParentRef parent = {};
    /// Optional gate that hides this parameter based on a sibling's value
    /// (see @ref gate).
    VisibleWhen visible_when = {};
};

/// @brief A two-state on/off toggle.
///
/// Renders as a yes/no (or on/off) switch. Read it back with param<>() as a
/// 0 or 1 integer.
struct Bool {
    /// Display name shown in the settings UI.
    std::string_view name;
    /// Initial state.
    bool default_ = false;
    /// Set to 1 to give each mode instance its own copy of this value (output
    /// modes only; ignored for global modes).
    uint8_t is_per_instance = 0;
    /// Optional Section this parameter nests under.
    ParentRef parent = {};
    /// Optional gate that hides this parameter based on a sibling's value
    /// (see @ref gate).
    VisibleWhen visible_when = {};
};

/// @brief A single choice from a fixed list of named options.
///
/// The stored value is the zero-based index of the chosen option. Read it
/// back with param<>() as that index.
struct Enum {
    /// Display name shown in the settings UI.
    std::string_view name;
    /// Option labels in index order (the first is index 0).
    std::initializer_list<std::string_view> options;
    /// Initial option, as a zero-based index into options.
    int32_t default_ = 0;
    /// Set to 1 to give each mode instance its own copy of this value (output
    /// modes only; ignored for global modes).
    uint8_t is_per_instance = 0;
    /// Optional Section this parameter nests under.
    ParentRef parent = {};
    /// Optional gate that hides this parameter based on a sibling's value
    /// (see @ref gate).
    VisibleWhen visible_when = {};
};

/// @brief A multi-select set of named options stored as a bitmask.
///
/// Like Enum but several options can be on at once. The stored value is a
/// bitmap where bit N is set when option N is selected. Set the initial
/// selection with `.default_mask`.
struct List {
    /// Display name shown in the settings UI.
    std::string_view name;
    /// Option labels in index order (the first is index 0).
    std::initializer_list<std::string_view> options;
    /// Initial selection as a bitmask (bit N set means option N is on).
    int32_t default_mask = 0;
    /// Set to 1 to give each mode instance its own copy of this value (output
    /// modes only; ignored for global modes).
    uint8_t is_per_instance = 0;
    /// Optional Section this parameter nests under.
    ParentRef parent = {};
    /// Optional gate that hides this parameter based on a sibling's value
    /// (see @ref gate).
    VisibleWhen visible_when = {};
};

/// @brief A single MIDI note (0..127) shown with a note-name label.
///
/// Edits like a Numeric (dial the note number up or down), but renders as
/// a musical label (for example C4). For a low/high note span, use NoteRange.
struct Note {
    /// Display name shown in the settings UI.
    std::string_view name;
    /// Lowest note number selectable (0..127).
    int32_t min = 0;
    /// Highest note number selectable (0..127).
    int32_t max = 127;
    /// Initial note number (60 is middle C).
    int32_t default_ = 60;
    /// Set to 1 to give each mode instance its own copy of this value (output
    /// modes only; ignored for global modes).
    uint8_t is_per_instance = 0;
    /// Optional Section this parameter nests under.
    ParentRef parent = {};
    /// Optional gate that hides this parameter based on a sibling's value
    /// (see @ref gate).
    VisibleWhen visible_when = {};
};

/// @brief A low/high pair of integers (a span the user dials at both ends).
///
/// Occupies two consecutive value slots. Read it back with param<>() as a
/// std::pair, for example `auto [lo, hi] = param<MyRange>();`.
struct Range {
    /// Display name shown in the settings UI.
    std::string_view name;
    /// Lowest value either end reaches.
    int32_t min;
    /// Highest value either end reaches.
    int32_t max;
    /// Initial low end.
    int32_t default_low;
    /// Initial high end.
    int32_t default_high;
    /// Increment between adjacent values.
    int32_t step = 1;
    /// Fixed-point display digits. The stored values stay integers.
    uint8_t decimal_places = 0;
    /// Set to 1 to give each mode instance its own copy of this value (output
    /// modes only; ignored for global modes).
    uint8_t is_per_instance = 0;
    /// Optional Section this parameter nests under.
    ParentRef parent = {};
    /// Optional gate that hides this parameter based on a sibling's value
    /// (see @ref gate).
    VisibleWhen visible_when = {};
};

/// @brief A low/high pair of MIDI notes shown with note-name labels.
///
/// Like Range but both ends render as note names (see Note). Occupies two
/// value slots and reads back as a std::pair via param<>().
struct NoteRange {
    /// Display name shown in the settings UI.
    std::string_view name;
    /// Lowest note number either end reaches (0..127).
    int32_t min = 0;
    /// Highest note number either end reaches (0..127).
    int32_t max = 127;
    /// Initial low note.
    int32_t default_low = 0;
    /// Initial high note.
    int32_t default_high = 127;
    /// Set to 1 to give each mode instance its own copy of this value (output
    /// modes only; ignored for global modes).
    uint8_t is_per_instance = 0;
    /// Optional Section this parameter nests under.
    ParentRef parent = {};
    /// Optional gate that hides this parameter based on a sibling's value
    /// (see @ref gate).
    VisibleWhen visible_when = {};
};

/// @brief A picker that lets the user choose a file from storage.
///
/// Filters the listing by `.extension` (for example ".scl"). Read it back
/// with param<>(), which returns a FilePickerValue holding the chosen
/// filename. The mode then opens it through the storage ABI.
struct FilePicker {
    /// Display name shown in the settings UI.
    std::string_view name;
    /// Filename filter, for example ".scl". Only matching files are listed.
    std::string_view extension;
    /// Longest filename the picker stores.
    uint8_t max_filename_len = 64;
    /// Set to 1 to give each mode instance its own copy of this value (output
    /// modes only; ignored for global modes).
    uint8_t is_per_instance = 0;
    /// Optional Section this parameter nests under.
    ParentRef parent = {};
    /// Optional gate that hides this parameter based on a sibling's value
    /// (see @ref gate).
    VisibleWhen visible_when = {};
};

// ---------------------------------------------------------------------------
// Typed FilePicker return value.
// ---------------------------------------------------------------------------
// `param<FilePickerSpec>()` returns one of these. The buffer is sized at
// compile time from the spec's max_filename_len (plus one for the NUL), so
// each param gets a right-sized buffer. `empty()` (buf[0] == '\0') reports
// "no file picked yet", matching what the firmware writes.
/// @brief The value returned by param<>() for a FilePicker parameter.
///
/// Holds the chosen filename in a fixed buffer sized from the spec's
/// `max_filename_len`. Call empty() to check whether the user has picked a
/// file yet, then pass `.filename` to the storage ABI to open it.
template <auto const& Spec> struct FilePickerValue {
    char filename[Spec.max_filename_len + 1] {};

    /// @brief True when the user has not picked a file yet.
    /// @return True if the filename buffer is empty.
    constexpr bool empty() const noexcept {
        return filename[0] == '\0';
    }
};

// ---------------------------------------------------------------------------
// Section: pure container kind. Does not carry .visible_when (gating a whole
// Section is not supported), so per-kind VisibleWhen is the only gate surface.
// Section's `.parent` supports arbitrary nesting (Section-in-Section).
// ---------------------------------------------------------------------------
/// @brief A named grouping header for the params that follow it.
///
/// A pure container that organizes the param page into labeled groups. Other
/// params point at it with `.parent = MySection`, and Sections can nest by
/// setting their own `.parent`. A Section holds no value of its own.
struct Section {
    /// Display name shown as the group header.
    std::string_view name;
    /// Optional outer Section to nest this one under.
    ParentRef parent = {};
    /// Not meaningful for a Section, which holds no value. Leave it at 0.
    uint8_t is_per_instance = 0;
};

/// @brief A button that runs one of the mode's functions when selected.
///
/// Appears on the param page as a row labeled `.name` and holds no value of
/// its own. Selecting it runs `.on_select` in the mode's own context, so the
/// function reads and changes the mode's state and sends MIDI just as
/// `process()` does. A `.confirm` holding a short question guards a destructive
/// command, putting a yes/no prompt carrying that question first. Left unset,
/// the function runs the moment the button is selected.
///
///     void clear_clip() { /* ... */ }
///     inline constexpr op::params::Action Clear {
///         .name = "Clear Clip", .confirm = "Erase all clips?",
///         .on_select = &clear_clip};
struct Action {
    /// Display name shown as the button label.
    std::string_view name;
    /// Optional yes/no prompt shown before the function runs.
    const char* confirm = nullptr;
    /// The function to run when the button is selected.
    void (*on_select)() = nullptr;
    /// Optional Section this button nests under.
    ParentRef parent = {};
};

// Out-of-line definition of ParentRef's converting constructor. It stores the
// Section's address. stage() later resolves that address to a ParamSpec index.
constexpr ParentRef::ParentRef(const Section& sec)
    : section(&sec) { }

// Forward declaration of SectionGroup: used by make_section() factory for
// the structural form. Children are stored as a tuple of the original kind
// structs, and emit() walks the tuple and stamps parent_param_id at emit time.
namespace detail {
    template <typename... Children> struct SectionGroup {
        std::string_view name;
        uint8_t is_per_instance = 0;
        ParentRef parent        = {};
        std::tuple<Children...> children {};
    };
}  // namespace detail

/// @brief Build a Section together with its children in one expression.
///
/// The structural form: pass the section name and its child params inline and
/// they are parented to the new Section automatically.
///
///     OP_MODE_PARAMS(make_section("LFO", Bool{.name="On"},
///                                 Numeric{.name="Rate", ...}))
///
/// Children here must stay ungated (leave their `.visible_when` at the
/// default). Inline children have no name a gate watcher could reference, so
/// declare gated params flat-form at file scope and use `.parent` instead.
/// @param name The section's display name.
/// @param children The params to place inside the section.
/// @return A group value to pass straight into OP_MODE_PARAMS().
template <typename... Children>
constexpr detail::SectionGroup<Children...> make_section(std::string_view name, Children... children) {
    return detail::SectionGroup<Children...> {name, /*is_per_instance=*/0, /*parent=*/ParentRef {},
                                              std::tuple<Children...> {std::move(children)...}};
}

// ---------------------------------------------------------------------------
// Internal: oversized staging bundle + per-kind emitters.
// ---------------------------------------------------------------------------

namespace detail {

    // Compile-time staging space. The param cap tracks the firmware's hard
    // limit so the SDK static_asserts below reject exactly what the loader
    // would. The string budget is a local staging bound (no config constant
    // mirrors it yet), sized well above any real mode.
    inline constexpr std::size_t kMaxParams      = op::config::kMaxParamsPerMode;
    inline constexpr std::size_t kMaxStringBytes = 1024;

    struct StagedBundle {
        std::array<::op::modes::ParamSpec, kMaxParams> specs {};
        std::array<char, kMaxStringBytes> strings {};
        // Parallel table mapping specs[i] -> the address of the source
        // declaration object (the `inline constexpr op::params::*` the author
        // wrote) that produced it. Used by resolve_addr_in_bundle to resolve
        // `.parent` / `.visible_when.watch` by object identity.
        // Compile-time only, never emitted into the .opm. The exactly-sized
        // kParams[]/kParamStrings[] extraction lambdas copy out specs[]
        // and strings[] only, so this leaves the ABI shape byte-identical.
        std::array<const void*, kMaxParams> source_addrs {};
        // Action .on_select pointers in declaration order. emit(Action) appends
        // each one and stamps its index into the spec's on_select_index.
        // OP_MODE_PARAMS copies them into the exported kOnSelectCallbacks table.
        std::array<void (*)(), kMaxParams> on_select_callbacks {};
        std::size_t param_count     = 0;
        std::size_t strings_length  = 0;
        std::size_t on_select_count = 0;
    };

    // Append a null-terminated string to the staged blob and return its offset.
    constexpr uint16_t append(StagedBundle& b, std::string_view s) {
        const auto offset = static_cast<uint16_t>(b.strings_length);
        for (char c : s) {
            b.strings[b.strings_length++] = c;
        }
        b.strings[b.strings_length++] = '\0';
        return offset;
    }

    // Push a fresh ParamSpec slot and record the source declaration
    // object's address into the parallel source_addrs[] table at the same
    // index. Every emit() overload routes its slot allocation through here so
    // identity-based `.parent` / `.watch` resolution can later match a captured
    // object address to its ParamSpec index. `src` is the address of the author's
    // `inline constexpr op::params::*` declaration (static storage ->
    // constant-expression address in this constexpr context).
    constexpr ::op::modes::ParamSpec& new_spec(StagedBundle& b, const void* src) {
        const std::size_t i = b.param_count++;
        b.source_addrs[i]   = src;
        return b.specs[i];
    }

    // Explicit ABI-tail initialization shared by every emit() overload. Default
    // visible_when must serialize as the 0xFF sentinel (a zero byte would be
    // indistinguishable from Op::Eq watcher 0). parent_param_id is left at 0xFF
    // here and patched by the stage() post-pass once every spec has been emitted.
    constexpr void init_abi_tail(::op::modes::ParamSpec& s) {
        s.parent_param_id       = 0xFF;
        s.visible_when_param_id = ::op::modes::kVisibleAlways;
        s.visible_when_value    = 0;
        // Dedicated watcher byte. Only read when visible_when_param_id is
        // non-sentinel. The resolver writes the real index explicitly for gated
        // params. 0 is a safe "unused" default under the kVisibleAlways sentinel.
        s.visible_when_watcher = 0;
    }

    // Per-kind ParamSpec emitters. Each appends the label strings it needs to the
    // bundle (recording their offsets) and fills the ParamSpec. Passing the bundle
    // by reference lets one fold expression drive all of them.

    constexpr void emit(StagedBundle& b, const Numeric& p) {
        auto& s            = new_spec(b, &p);
        s.name_offset      = append(b, p.name);
        s.options_offset   = 0;
        s.kind             = static_cast<uint8_t>(::op::modes::ParamKind::Numeric);
        s.is_per_instance  = p.is_per_instance;
        s.decimal_places   = p.decimal_places;
        s.option_count     = 0;
        s.min_value        = p.min;
        s.max_value        = p.max;
        s.default_value    = p.default_;
        s.step_value       = p.step;
        s.value_slot_count = p.value_slot_count;
        init_abi_tail(s);
    }

    constexpr void emit(StagedBundle& b, const Bool& p) {
        auto& s            = new_spec(b, &p);
        s.name_offset      = append(b, p.name);
        s.options_offset   = 0;
        s.kind             = static_cast<uint8_t>(::op::modes::ParamKind::Bool);
        s.is_per_instance  = p.is_per_instance;
        s.decimal_places   = 0;
        s.option_count     = 0;
        s.min_value        = 0;
        s.max_value        = 1;
        s.default_value    = p.default_ ? 1 : 0;
        s.step_value       = 1;
        s.value_slot_count = 1;
        init_abi_tail(s);
    }

    constexpr void emit(StagedBundle& b, const Enum& p) {
        auto& s       = new_spec(b, &p);
        s.name_offset = append(b, p.name);
        // Options append into the shared blob as one contiguous NUL-separated
        // span. options_offset points at the first, and option_count counts them.
        uint16_t first = 0;
        bool first_set = false;
        for (auto opt : p.options) {
            auto off = append(b, opt);
            if (!first_set) {
                first     = off;
                first_set = true;
            }
        }
        s.options_offset   = first;
        s.kind             = static_cast<uint8_t>(::op::modes::ParamKind::Enum);
        s.is_per_instance  = p.is_per_instance;
        s.decimal_places   = 0;
        s.option_count     = static_cast<uint8_t>(p.options.size());
        s.min_value        = 0;
        s.max_value        = p.options.size() > 0 ? static_cast<int32_t>(p.options.size() - 1) : 0;
        s.default_value    = p.default_;
        s.step_value       = 1;
        s.value_slot_count = 1;
        init_abi_tail(s);
    }

    constexpr void emit(StagedBundle& b, const List& p) {
        auto& s        = new_spec(b, &p);
        s.name_offset  = append(b, p.name);
        uint16_t first = 0;
        bool first_set = false;
        for (auto opt : p.options) {
            auto off = append(b, opt);
            if (!first_set) {
                first     = off;
                first_set = true;
            }
        }
        s.options_offset   = first;
        s.kind             = static_cast<uint8_t>(::op::modes::ParamKind::List);
        s.is_per_instance  = p.is_per_instance;
        s.decimal_places   = 0;
        s.option_count     = static_cast<uint8_t>(p.options.size());
        s.min_value        = 0;
        s.max_value        = static_cast<int32_t>((1LL << p.options.size()) - 1);
        s.default_value    = p.default_mask;
        s.step_value       = 1;
        s.value_slot_count = 1;
        init_abi_tail(s);
    }

    constexpr void emit(StagedBundle& b, const Note& p) {
        auto& s            = new_spec(b, &p);
        s.name_offset      = append(b, p.name);
        s.options_offset   = 0;
        s.kind             = static_cast<uint8_t>(::op::modes::ParamKind::Note);
        s.is_per_instance  = p.is_per_instance;
        s.decimal_places   = 0;
        s.option_count     = 0;
        s.min_value        = p.min;
        s.max_value        = p.max;
        s.default_value    = p.default_;
        s.step_value       = 1;
        s.value_slot_count = 1;
        init_abi_tail(s);
    }

    constexpr void emit(StagedBundle& b, const Range& p) {
        auto& s            = new_spec(b, &p);
        s.name_offset      = append(b, p.name);
        s.options_offset   = 0;
        s.kind             = static_cast<uint8_t>(::op::modes::ParamKind::Range);
        s.is_per_instance  = p.is_per_instance;
        s.decimal_places   = p.decimal_places;
        s.option_count     = 0;
        s.min_value        = p.min;
        s.max_value        = p.max;
        s.default_value    = p.default_low;
        s.step_value       = p.step;
        s.value_slot_count = 2;
        init_abi_tail(s);
    }

    constexpr void emit(StagedBundle& b, const NoteRange& p) {
        auto& s            = new_spec(b, &p);
        s.name_offset      = append(b, p.name);
        s.options_offset   = 0;
        s.kind             = static_cast<uint8_t>(::op::modes::ParamKind::NoteRange);
        s.is_per_instance  = p.is_per_instance;
        s.decimal_places   = 0;
        s.option_count     = 0;
        s.min_value        = p.min;
        s.max_value        = p.max;
        s.default_value    = p.default_low;
        s.step_value       = 1;
        s.value_slot_count = 2;
        init_abi_tail(s);
    }

    constexpr void emit(StagedBundle& b, const FilePicker& p) {
        auto& s            = new_spec(b, &p);
        s.name_offset      = append(b, p.name);
        s.options_offset   = append(b, p.extension);  // extension filter
        s.kind             = static_cast<uint8_t>(::op::modes::ParamKind::FilePicker);
        s.is_per_instance  = p.is_per_instance;
        s.decimal_places   = 0;
        s.option_count     = 0;
        s.min_value        = 0;
        s.max_value        = p.max_filename_len;
        s.default_value    = 0;  // byte offset into string ring
        s.step_value       = 1;
        s.value_slot_count = 1;
        init_abi_tail(s);
    }

    // Section: pure container, value_slot_count=0. Section's own
    // visible_when_param_id is always kVisibleAlways (no Section-level gating).
    constexpr void emit(StagedBundle& b, const Section& p) {
        auto& s            = new_spec(b, &p);
        s.name_offset      = append(b, p.name);
        s.options_offset   = 0;
        s.kind             = static_cast<uint8_t>(::op::modes::ParamKind::Section);
        s.is_per_instance  = p.is_per_instance;
        s.decimal_places   = 0;
        s.option_count     = 0;
        s.min_value        = 0;
        s.max_value        = 0;
        s.default_value    = 0;
        s.step_value       = 0;
        s.value_slot_count = 0;  // Sections hold no value slot
        init_abi_tail(s);
    }

    // Action: a button, value_slot_count=0 like Section. The confirm prompt is
    // staged into options_offset, which stays 0 when there is no prompt, so the
    // offset doubles as the confirm flag. The .on_select pointer goes into the
    // bundle's callback table and its index into the spec's on_select_index.
    constexpr void emit(StagedBundle& b, const Action& p) {
        auto& s            = new_spec(b, &p);
        s.name_offset      = append(b, p.name);
        s.options_offset   = p.confirm ? append(b, std::string_view {p.confirm}) : static_cast<uint16_t>(0);
        s.kind             = static_cast<uint8_t>(::op::modes::ParamKind::Action);
        s.is_per_instance  = 0;
        s.decimal_places   = 0;
        s.option_count     = 0;
        s.min_value        = 0;
        s.max_value        = 0;
        s.default_value    = 0;
        s.step_value       = 0;
        s.value_slot_count = 0;  // buttons hold no value slot
        init_abi_tail(s);
        const auto callback_index                  = static_cast<uint8_t>(b.on_select_count);
        b.on_select_callbacks[b.on_select_count++] = p.on_select;
        s.on_select_index                          = callback_index;
    }

    // Structural form helper: emit a SectionGroup inline. The group's Section
    // entry is emitted first (so children can reference it by index), then
    // each child is emitted with its parent_param_id stamped to the Section's
    // specs[] index. Children's own `.parent` refs (if any) are ignored
    // inside a SectionGroup, where the group is the parent.
    //
    // Constexpr-safety: the synthetic `Section sec` and the tuple-copied
    // children have automatic storage, so their addresses are not
    // constant expressions and must not persist in the returned bundle's
    // source_addrs[]. We clear those entries to nullptr after emit(). This is
    // safe because structural-form entries are never identity-resolution
    // targets: a synthesized Section can never be the target of a flat-form
    // `.parent = Obj` (you cannot take its address), and structural children
    // are ungated (no `.watch`) and parented by index here, not by address.
    template <typename... Children> constexpr void emit(StagedBundle& b, const SectionGroup<Children...>& g) {
        // Emit the Section header first. The group's `.parent` ParentRef
        // wrapper is forwarded as-is to the synthesised Section so nested
        // structural sections preserve their outer-Section parent reference.
        const uint8_t section_idx = static_cast<uint8_t>(b.param_count);
        Section sec {g.name, g.parent, g.is_per_instance};
        emit(b, sec);
        // Drop the automatic-storage address new_spec() recorded for `sec`.
        b.source_addrs[section_idx] = nullptr;

        // Emit each child and stamp its parent_param_id to the section index.
        auto emit_child = [&](const auto& child) constexpr {
            const std::size_t i = b.param_count;
            emit(b, child);
            b.specs[i].parent_param_id = section_idx;
            // Drop the automatic-storage address of this tuple-copied child.
            b.source_addrs[i] = nullptr;
        };
        std::apply([&](const auto&... cs) constexpr { (emit_child(cs), ...); }, g.children);
    }

    // ---------------------------------------------------------------------------
    // parent + visible_when resolution via object identity.
    // ---------------------------------------------------------------------------
    // The compile-time resolver takes the captured address of a referenced param
    // object (`.parent.section` for a Section, `.visible_when.watch.target` for a
    // watcher) and matches it to the corresponding ParamSpec array index by
    // pointer equality against the per-spec source_addrs[] table the emitters
    // recorded. There is no name-based fallback: name-match would collapse any
    // references whose targets shared a display name onto the first match.
    //
    // This identity path is shared for both .parent and .visible_when.watch.

    // Count how many specs[] entries a single staged item contributes.
    // Scalar kinds + Section = 1. SectionGroup contributes 1 + children_count.
    // Forward-declared here so resolve_parent_and_watcher can reference it
    // from a lambda without ADL surprises at second-phase template lookup.
    template <typename T> constexpr std::size_t child_spec_count(const T&) {
        return 1;
    }

    template <typename... Children> constexpr std::size_t child_spec_count(const SectionGroup<Children...>&) {
        return 1 + sizeof...(Children);
    }

    // Resolve a captured source-object address to its ParamSpec index by pointer
    // identity. Returns 0xFF when `addr` is null or matches no emitted spec.
    // Called only after every emit(), so every referenced object's address is
    // already recorded in source_addrs[0..param_count).
    constexpr uint8_t resolve_addr_in_bundle(const StagedBundle& b, const void* addr) {
        if (addr == nullptr) return 0xFF;
        for (std::size_t i = 0; i < b.param_count; ++i) {
            if (b.source_addrs[i] == addr) {
                return static_cast<uint8_t>(i);
            }
        }
        return 0xFF;
    }

    // Post-pass resolver: for a single input `p` at emit index `i`, patch
    // specs[i].parent_param_id and visible_when_{param_id,value} using the
    // bundle's now-complete specs[] + source_addrs[].
    //
    // Accepts SectionGroup (no patching needed, children handled at emit
    // time) and any kind struct / Section (patched based on .parent / .visible_when).
    //
    // Both `.parent` (ParentRef wrapper, capturing `const Section*` via the
    // converting ctor) and `.watch` (WatcherRef wrapper, capturing the
    // watcher's address via a converting-constructor template) resolve through
    // the same `resolve_addr_in_bundle` identity lookup against the recorded
    // source_addrs[] table. "No parent" is encoded by `parent.section ==
    // nullptr`, and "no gate" by a null `watch.target` (identity resolution).
    template <typename T>
    constexpr void resolve_parent_and_watcher(StagedBundle& b, std::size_t spec_index, const T& p) {
        if constexpr (requires { p.parent; }) {
            // `p.parent` is a ParentRef wrapper whose `.section` holds the captured
            // `const Section*` (nullptr = top-level / no parent).
            if (p.parent.section != nullptr) {
                const uint8_t idx = resolve_addr_in_bundle(b, static_cast<const void*>(p.parent.section));
                // Missing parent falls through to 0xFF sentinel so loader
                // validation can flag declared-but-not-listed cases. Flat-form
                // typos referencing undeclared objects are caught by C++ name
                // resolution at the tests/compile_fail harness.
                b.specs[spec_index].parent_param_id = idx;
            }
        }
        if constexpr (requires { p.visible_when; }) {
            // Watcher captured at construction time as a `WatcherRef` whose
            // `target` is the watcher object's address. A null `target`
            // (default-constructed WatcherRef) means "no gate".
            //
            // `hidden()` (.never == true) takes precedence and packs the
            // kVisibleNever (0xFE) sentinel, with no watcher to resolve. An unset
            // `.visible_when` hits neither branch and keeps the init_abi_tail
            // default of kVisibleAlways (0xFF), always visible (no regression).
            if (p.visible_when.never) {
                b.specs[spec_index].visible_when_param_id = ::op::modes::kVisibleNever;
            } else if (p.visible_when.watch.target != nullptr) {
                const uint8_t widx = resolve_addr_in_bundle(b, p.visible_when.watch.target);
                // Byte 26 carries the op code only. The full watcher index
                // (0..255, no masking) goes into the dedicated visible_when_watcher
                // byte so watchers beyond index 31 are encodable.
                b.specs[spec_index].visible_when_param_id = ::op::modes::pack_visible_when(p.visible_when.op);
                b.specs[spec_index].visible_when_watcher  = static_cast<uint8_t>(widx);
                b.specs[spec_index].visible_when_value    = p.visible_when.value;
            }
        }
    }

    // Overload for SectionGroup: walk the children tuple and patch each
    // child's ParamSpec via the same resolver.
    template <typename... Children>
    constexpr void resolve_parent_and_watcher(StagedBundle& b,
                                              std::size_t section_spec_index,
                                              const SectionGroup<Children...>& g) {
        (void)section_spec_index;  // group parent handled structurally at emit()
        // Walk the tuple. Each child's spec is at section_spec_index+1+k but
        // the easier approach is to track a running counter via std::apply
        // with a running index lambda that matches emit() order.
        std::size_t child_spec = section_spec_index + 1;
        auto visit             = [&](const auto& child) constexpr {
            resolve_parent_and_watcher(b, child_spec, child);
            child_spec += child_spec_count(child);
        };
        std::apply([&](const auto&... cs) constexpr { (visit(cs), ...); }, g.children);
    }

    // Fold-driven bundle assembly. Runs fully at compile time because every
    // emit() overload is constexpr and the StagedBundle is a literal type.
    // After the emit()-fold, a second fold patches parent_param_id and
    // visible_when_{param_id,value} by walking each input and resolving its
    // object-ref `.parent` / `.visible_when.watch` against the bundle's
    // now-complete specs[] + strings[].
    template <typename... Ts> constexpr StagedBundle stage(const Ts&... ps) {
        StagedBundle b {};
        // Pass 1: emit every input. Structural Sections emit their children
        // inline and stamp children's parent_param_id to the Section's index.
        (emit(b, ps), ...);

        // Pass 2: for each input, resolve object-ref .parent / .visible_when
        // against the complete bundle. SectionGroup inputs walk their children.
        std::size_t spec_cursor = 0;
        auto resolve_one        = [&](const auto& p) constexpr {
            resolve_parent_and_watcher(b, spec_cursor, p);
            spec_cursor += child_spec_count(p);
        };
        (resolve_one(ps), ...);

        return b;
    }

    // ---------------------------------------------------------------------------
    // Compile-time name -> slot-info lookup.
    // ---------------------------------------------------------------------------
    // Given a param name and a constexpr bundle reference, returns the value-slot
    // index (accounting for dual-slot Range/NoteRange kinds) and the slot_count
    // for that entry. Used by OP_MODE_PARAMS's emitted `param<>` template so
    // call sites read `param<Spec>()` instead of a magic integer.

    struct SlotInfo {
        uint8_t slot;        // first value slot
        uint8_t slot_count;  // 1 for scalar kinds, 2 for Range/NoteRange
    };

    // Keyed on the param's name, used by the `param<Spec>()` template where
    // Spec is a reference to a constexpr parameter object (e.g., Bpm). Spec.name
    // is constexpr at the instantiation site, so this runs at compile time.
    template <auto const& Bundle> consteval SlotInfo lookup_slot_by_name(std::string_view name) {
        uint8_t slot = 0;
        for (std::size_t i = 0; i < Bundle.param_count; ++i) {
            const auto name_off = Bundle.specs[i].name_offset;
            const auto slots    = Bundle.specs[i].value_slot_count;

            bool match = true;
            for (std::size_t c = 0; c < name.size(); ++c) {
                if (Bundle.strings[name_off + c] != name[c]) {
                    match = false;
                    break;
                }
            }
            if (match && Bundle.strings[name_off + name.size()] == '\0') {
                return SlotInfo {slot, slots};
            }
            slot = static_cast<uint8_t>(slot + slots);
        }
        return SlotInfo {0xFF, 0};
    }

}  // namespace detail

}  // namespace op::params

// ---------------------------------------------------------------------------
// OP_MODE_PARAMS(...): the one macro a declarative mode needs.
// ---------------------------------------------------------------------------
// Expansion outline:
//
//   1. A hidden `inline constexpr` staging bundle aggregates every ParamSpec
//      and string in order. Oversized at compile time, and costs zero flash.
//   2. `kParamCount` is exported as a single byte, the real param count.
//   3. `kParams` and `kParamStrings` are exported as `std::array`s sized
//      exactly to the used portion, copied out of the staging bundle by a
//      pair of constexpr immediately-invoked lambdas. pack_opm.py sees the
//      symbols at their natural packed byte sizes, byte-identical to the
//      hand-rolled `ParamSpec[N]` / `char[M]` layout it already understands.
//
//   Static asserts guard against exceeding the generous staging maxima.
//
// The bundle has a fixed hidden name, so exactly one OP_MODE_PARAMS or
// OP_MODE_NO_PARAMS is supported per translation unit, which is the natural
// granularity for a mode.

// Private core macro: emits op::api + the exactly-sized kParams/kParamCount/
// kParamStrings globals. Shared by OP_MODE_PARAMS (N >= 1) and
// OP_MODE_NO_PARAMS (N = 0). Callers should not invoke this directly.
//
// The bundle name is stable (no __LINE__ mangling) so the emitted
// `param<Spec>()` function template can reference it. As a result exactly
// one OP_MODE_PARAMS / OP_MODE_NO_PARAMS call per translation unit is
// supported, which is the natural granularity for a mode.

#define OP__MODE_API_DECL()                                                                                  \
    namespace op {                                                                                           \
        /* The operator API handle for this mode's translation unit. Bound                                   \
         * automatically by OP_MODE_REGISTER before init() runs and cleared                                  \
         * after destroy() returns, so no mode code ever assigns to it.                                      \
         * Read whenever you need the API pointer outside param<>(), e.g.                                    \
         *     op::api->send_midi(out, 0xF8, 0, 0);                                                          \
         *     const auto t = op::api->get_tick();                                                           \
         * `inline` gives external linkage, one definition per .opm binary. */                               \
        inline const OperatorApi* api = nullptr;                                                             \
    }                                                                                                        \
    static_assert(true, "") /* consume trailing ; */

/// @brief Declare a mode's full parameter table in one call.
///
/// Pass every parameter descriptor (Numeric, Bool, Enum, and so on, or a
/// make_section() group) and the SDK stages them, computes the string table,
/// and emits the three ABI globals the firmware reads (kParams, kParamCount,
/// kParamStrings). To be called exactly once per mode. It also makes the
/// param<>() accessor available for reading each parameter's value back.
///
///     OP_MODE_PARAMS(Bpm, Bypass, make_section("LFO", On, Rate));
///
/// Use OP_MODE_NO_PARAMS() instead for a mode with no declarative params.
#define OP_MODE_PARAMS(...)                                                                                  \
    OP__MODE_API_DECL();                                                                                     \
    namespace {                                                                                              \
        inline constexpr auto _op_mode_params_bundle = ::op::params::detail::stage(__VA_ARGS__);             \
        static_assert(_op_mode_params_bundle.param_count <= ::op::params::detail::kMaxParams,                \
                      "Too many params in OP_MODE_PARAMS.");                                                 \
        static_assert(_op_mode_params_bundle.strings_length <= ::op::params::detail::kMaxStringBytes,        \
                      "Param string blob too large.");                                                       \
        /* Extract the truncated arrays inside the anonymous namespace. The                                  \
         * lambdas must live here (not inside the OP_MODE_EXPORT definition)                                 \
         * because extern "C" linkage would otherwise propagate into the lambda                              \
         * body and forbid templates / local constexpr references. */                                        \
        inline constexpr auto _op_mode_params_specs_ = [] {                                                  \
            constexpr auto& s = _op_mode_params_bundle;                                                      \
            std::array<::op::modes::ParamSpec, s.param_count> out {};                                        \
            for (std::size_t i = 0; i < s.param_count; ++i) out[i] = s.specs[i];                             \
            return out;                                                                                      \
        }();                                                                                                 \
        inline constexpr auto _op_mode_params_strings_ = [] {                                                \
            constexpr auto& s = _op_mode_params_bundle;                                                      \
            std::array<char, s.strings_length> out {};                                                       \
            for (std::size_t i = 0; i < s.strings_length; ++i) out[i] = s.strings[i];                        \
            return out;                                                                                      \
        }();                                                                                                 \
        /* Exactly-sized Action callback table. A mode with no Action rows still                             \
         * emits a one-entry table so the export symbol resolves, and                                        \
         * kOnSelectCount carries the real length. */                                                        \
        inline constexpr auto _op_mode_on_select_ = [] {                                                     \
            constexpr auto& s          = _op_mode_params_bundle;                                             \
            constexpr std::size_t kLen = s.on_select_count == 0 ? 1 : s.on_select_count;                     \
            std::array<void (*)(), kLen> out {};                                                             \
            for (std::size_t i = 0; i < s.on_select_count; ++i) out[i] = s.on_select_callbacks[i];           \
            return out;                                                                                      \
        }();                                                                                                 \
        /* Compile-time spec -> value-slot resolution. The `param<Spec>()`                                   \
         * template below takes a reference to an `inline constexpr`                                         \
         * parameter object (e.g. `param<Bpm>()` where `Bpm` is declared                                     \
         * as `inline constexpr op::params::Numeric Bpm{.name="BPM",...}`) and                               \
         * looks up the value slot in the staged bundle by comparing names.                                  \
         * Runtime cost: one `api->get_param_value` call. The slot is resolved                               \
         * at compile time. */                                                                               \
        template <auto const& Spec> [[nodiscard]] constexpr auto param_slot_info() {                         \
            constexpr auto info = ::op::params::detail::lookup_slot_by_name<_op_mode_params_bundle>(         \
                Spec.name);                                                                                  \
            static_assert(info.slot != 0xFF,                                                                 \
                          "param<Spec>: the referenced spec is not listed in "                               \
                          "this TU's OP_MODE_PARAMS(...) invocation.");                                      \
            return info;                                                                                     \
        }                                                                                                    \
        /* Read a named parameter. For scalar kinds returns int32_t. For                                     \
         * dual-slot kinds (Range, NoteRange) returns std::pair<int32_t,int32_t>                             \
         * so call sites can structured-bind: `auto [lo, hi] = param<Notes>()`.                              \
         * For FilePicker kinds returns a typed                                                              \
         * FilePickerValue<Spec> whose .filename is populated from the firmware                              \
         * per-slot string ring via api->get_param_filename. `.empty()` is true                              \
         * when the user has not picked anything yet. */                                                     \
        template <auto const& Spec> [[nodiscard]] auto param() {                                             \
            constexpr auto info = param_slot_info<Spec>();                                                   \
            const auto* api     = ::op::api;                                                                 \
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(Spec)>, ::op::params::FilePicker>) {   \
                ::op::params::FilePickerValue<Spec> value {};                                                \
                if (api) {                                                                                   \
                    (void)api->get_param_filename(info.slot, value.filename, sizeof(value.filename));        \
                }                                                                                            \
                return value;                                                                                \
            } else if constexpr (info.slot_count == 1) {                                                     \
                return api ? api->get_param_value(info.slot) : int32_t {};                                   \
            } else {                                                                                         \
                return std::pair<int32_t, int32_t> {                                                         \
                    api ? api->get_param_value(info.slot) : 0,                                               \
                    api ? api->get_param_value(info.slot + 1) : 0,                                           \
                };                                                                                           \
            }                                                                                                \
        }                                                                                                    \
    }                                                                                                        \
    OP_MODE_EXPORT const uint8_t kParamCount    = static_cast<uint8_t>(_op_mode_params_bundle.param_count);  \
    OP_MODE_EXPORT constinit const auto kParams = _op_mode_params_specs_;                                    \
    OP_MODE_EXPORT constinit const auto kParamStrings = _op_mode_params_strings_;                            \
    OP_MODE_EXPORT const uint8_t kOnSelectCount       = static_cast<uint8_t>(                                \
        _op_mode_params_bundle.on_select_count);                                                             \
    OP_MODE_EXPORT constinit const auto kOnSelectCallbacks = _op_mode_on_select_

// ---------------------------------------------------------------------------
// OP_MODE_NO_PARAMS(): for modes with zero declarative parameters.
// ---------------------------------------------------------------------------
/// @brief Declare that a mode exposes no user-facing parameters.
///
/// Use this instead of #OP_MODE_PARAMS for a custom-UI mode that keeps all of
/// its state internally. It emits the ABI globals the firmware requires and
/// makes @ref op::api available. Call it exactly once per mode.
#define OP_MODE_NO_PARAMS()                                                                                  \
    OP__MODE_API_DECL();                                                                                     \
    OP_MODE_EXPORT const uint8_t kParamCount               = 0;                                              \
    OP_MODE_EXPORT const ::op::modes::ParamSpec kParams[1] = {};                                             \
    OP_MODE_EXPORT const char kParamStrings[1]             = ""

// ---------------------------------------------------------------------------
// OP_MODE_REGISTER(init, process, destroy): ABI registration.
// ---------------------------------------------------------------------------
// Write lifecycle bodies as ordinary C++ functions with any names you like,
// and register them through this single line. The SDK emits the `mode_init` /
// `mode_process` / `mode_destroy` exports the firmware looks up, binds
// op::api before calling the mode's init, clears op::api after the mode's
// destroy, and forwards process as-is.
//
// Mode's function signatures:
//
//     void init();                       // op::api is already bound
//     void process(OpMidiMessage* msgs,
//                  uint8_t    count,
//                  uint32_t   tick_us);
//     void destroy();                    // op::api is still valid
//
//     // Custom-UI modes additionally use:
//     void ui_render();
//     void ui_gesture(uint8_t encoder_id,
//                     uint8_t gesture,
//                     uint8_t gesture_type,
//                     int16_t value);
//
//     // A SysEx-capable mode passes a handler third, beside process:
//     void on_sysex(uint8_t port, const uint8_t* data,
//                   uint16_t len, uint8_t flags);
//
// Usage:
//
//     #include <operator_sdk.h>
//     #include <operator_sdk/params.h>
//
//     inline constexpr op::params::Numeric Bpm   {.name="BPM",    .min=2000,
//                                                  .max=30000, .default_=12000,
//                                                  .step=10, .decimal_places=2};
//     inline constexpr op::params::Bool    Bypass{.name="Bypass", .default_=false};
//
//     OP_MODE_PARAMS(Bpm, Bypass);
//
//     void init()    { /* op::api bound */ }
//     void process(OpMidiMessage*, uint8_t, uint32_t) {
//         if (param<Bypass>()) return;
//         const auto bpm_fixed = param<Bpm>();
//         // ...
//     }
//     void destroy() { /* teardown */ }
//
//     OP_MODE_REGISTER(init, process, destroy);
//     // For custom-UI modes:
//     // OP_MODE_REGISTER_UI(ui_render, ui_gesture);

// ---------------------------------------------------------------------------
// __opm_header emission hook (default: no-op).
//
// OP__MODE_EMIT_HEADER is consumed by OP_MODE_REGISTER. pack_opm.py synthesizes
// the 148-byte OpmHeader at post-link time and prepends it to the ELF.
//
// The `#ifndef` guard lets Operator's internal build tooling override the macro
// (via a force-included definition) to emit an exported ::OpmHeader constexpr
// used only by that internal tooling. That override is not part of the public
// SDK. When it defines the macro first, this default is skipped.
//
// Not a public API. Use OP_MODE_REGISTER instead.
// ---------------------------------------------------------------------------
#ifndef OP__MODE_EMIT_HEADER
#define OP__MODE_EMIT_HEADER(_name_, _type_, _flags_, _schema_)                                              \
    static_assert(true, "") /* ARM: pack_opm.py owns the header */
#endif

// Internal dispatch for OP_MODE_REGISTER's optional SysEx handler. OP__EXPAND
// forces a second scan so the argument-count pick resolves under MSVC's
// traditional preprocessor, and OP__REGISTER_PICK selects the three-arg or
// four-arg expansion by counting the arguments.
#define OP__EXPAND(x) x
#define OP__REGISTER_PICK(_1, _2, _3, _4, NAME, ...) NAME

// The lifecycle three plus the header hook, shared by both expansions.
#define OP__REGISTER_LIFECYCLE(init_fn, process_fn, destroy_fn)                                              \
    OP_MODE_EXPORT void mode_init(const OperatorApi* _op_api_in) {                                           \
        ::op::api = _op_api_in;                                                                              \
        (init_fn)();                                                                                         \
    }                                                                                                        \
    OP_MODE_EXPORT void mode_process(OpMidiMessage* _op_msgs, uint8_t _op_count, uint32_t _op_tick) {        \
        (process_fn)(_op_msgs, _op_count, _op_tick);                                                         \
    }                                                                                                        \
    OP_MODE_EXPORT void mode_destroy() {                                                                     \
        (destroy_fn)();                                                                                      \
        ::op::api = nullptr;                                                                                 \
    }                                                                                                        \
    OP__MODE_EMIT_HEADER(OP_MODE_NAME, OP_MODE_TYPE_VALUE, OP_MODE_FLAGS, OP_MODE_CONFIG_SCHEMA_VER)

// Three-argument form: the lifecycle three. The packed on_sysex_offset is 0.
#define OP__REGISTER3(init_fn, process_fn, destroy_fn)                                                       \
    OP__REGISTER_LIFECYCLE(init_fn, process_fn, destroy_fn);                                                 \
    static_assert(true, "")

// Four-argument form: also emit mode_on_sysex, forwarding to the handler.
#define OP__REGISTER4(init_fn, process_fn, on_sysex_fn, destroy_fn)                                          \
    OP__REGISTER_LIFECYCLE(init_fn, process_fn, destroy_fn);                                                 \
    OP_MODE_EXPORT bool mode_on_sysex(uint8_t _op_port, const uint8_t* _op_data, uint16_t _op_len,           \
                                      uint8_t _op_flags) {                                                   \
        return (on_sysex_fn)(_op_port, _op_data, _op_len, _op_flags);                                        \
    }                                                                                                        \
    static_assert(true, "")

/// @brief Export a mode's core lifecycle hooks to the firmware.
///
/// It takes the init, process, and destroy functions and wraps them as the
/// `extern "C"` `mode_init`, `mode_process`, and `mode_destroy` symbols the
/// device loads, assigning @ref op::api before init runs. The three take these
/// shapes:
///
///     void init();
///     void process(OpMidiMessage* msgs, uint8_t count, uint32_t tick);
///     void destroy();
///
/// A SysEx handler is optional and goes third, beside `process`. The mode also
/// carries `HANDLES_SYSEX` in its build declaration. Its shape is:
///
///     bool on_sysex(uint8_t port, const uint8_t* data, uint16_t len, uint8_t flags);
///
/// Returning true takes the message, so it is not sent on. Returning false lets
/// it continue to the routed outputs.
///
/// It is called exactly once per mode, alongside #OP_MODE_REGISTER_UI for a custom UI.
#define OP_MODE_REGISTER(...)                                                                                \
    OP__EXPAND(OP__REGISTER_PICK(__VA_ARGS__, OP__REGISTER4, OP__REGISTER3)(__VA_ARGS__))

/// @brief Export a custom-UI mode's render and gesture hooks.
///
/// Optional, and used in addition to #OP_MODE_REGISTER. Pass your render and
/// gesture functions:
///
///     void render();
///     void gesture(uint8_t control, op::Gesture gesture,
///                  op::GestureType type, int16_t value);
///
/// A mode that omits this is declarative-only: the firmware renders its
/// parameter screen for you.
#define OP_MODE_REGISTER_UI(render_fn, gesture_fn)                                                           \
    OP_MODE_EXPORT void mode_ui_render() {                                                                   \
        (render_fn)();                                                                                       \
    }                                                                                                        \
    OP_MODE_EXPORT void mode_ui_gesture(uint8_t _op_enc, uint8_t _op_gest, uint8_t _op_gtype,                \
                                        int16_t _op_val) {                                                   \
        /* ABI is raw uint8_t, hand the mode the typed enums so its handler                                  \
         * compares against op::Gesture / op::GestureType without casting. */                                \
        (gesture_fn)(_op_enc, static_cast<::op::Gesture>(_op_gest),                                          \
                     static_cast<::op::GestureType>(_op_gtype), _op_val);                                    \
    }                                                                                                        \
    static_assert(true, "")

