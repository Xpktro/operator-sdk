#pragma once
/// @file operator_sdk.h
/// @brief Public ABI umbrella header for Operator SDK modes.
///
/// Include this one header to get the ABI your mode compiles against. It
/// pulls in the three ABI headers: the function table, the .opm file format,
/// and the declarative parameter schema.
///
/// Usage (mandatory):
///
///     #include <operator_sdk.h>
///
/// Optional helpers (opt-in, header-only, no transitive includes here):
///
///     #include <operator_sdk/text.h>    // text layout utilities
///     #include <operator_sdk/timing.h>  // musical subdivision math
///     #include <operator_sdk/midi.h>    // scales, chords, CC helpers

#include <operator_sdk/abi/mode_api.h>
#include <operator_sdk/abi/mode_format.h>
#include <operator_sdk/abi/mode_param.h>

/// @namespace op
/// @brief Root namespace for the Operator SDK.
namespace op {
/// @brief Mode's handle to the device.
///
/// Everything a mode can do goes through this pointer: sending MIDI, reading
/// timing and parameters, drawing to the display, and reading storage. @ref
/// OP_MODE_REGISTER binds it from the pointer the firmware hands ``mode_init``,
/// before your ``init`` runs, and it stays valid for the life of the mode. It is
/// null before the mode is loaded and again once ``mode_destroy`` has returned,
/// so helpers that might run outside that window check it first.
///
/// Never define ``op::api`` yourself. Its definition is emitted into each mode's
/// translation unit by @ref OP_MODE_PARAMS or @ref OP_MODE_NO_PARAMS. You just
/// call through it, for example ``op::api->send_midi(...)``.
extern const OperatorApi* api;
}  // namespace op

/// @namespace op::sdk
/// @brief Core SDK helpers for modes (timing, text, MIDI, and math).

// ---------------------------------------------------------------------------
// Mark a function or data definition as an ABI export.
//
// Apply it to each mode entry point the firmware looks up by name (mode_init,
// mode_process, mode_destroy, and the optional mode_ui_render and
// mode_ui_gesture) and to the three parameter-table globals (kParams,
// kParamCount, kParamStrings). Everything else in your mode stays ordinary
// C++ (classes, templates, namespaces, STL, and ported libraries are all
// fine).
//
//     OP_MODE_EXPORT void mode_init(const OperatorApi* api) { ... }
//     OP_MODE_EXPORT const ParamSpec kParams[3] = { ... };
//
// The macro gives the symbol a stable, unmangled name and keeps it visible so
// the loader can find it. You do not need to know how it does that.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#define OP_MODE_EXPORT extern "C" __declspec(dllexport)
#else
#define OP_MODE_EXPORT extern "C" __attribute__((used, visibility("default")))
#endif
