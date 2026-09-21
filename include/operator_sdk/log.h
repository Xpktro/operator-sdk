#pragma once
/// @file log.h
/// @brief Ergonomic logging: ``op::log(...)`` wrappers over the raw ABI log entry.
///
/// Opt-in, header-only. Include on demand:
///
///     #include <operator_sdk/log.h>
///     op::log(op::LogLevel::Info, "count", 42);      // "count: 42"
///     op::log(op::LogLevel::Info, "rate", 1250, 2);  // "rate: 12.50"
///
/// ``op::log`` wraps ``OperatorApi::log`` with number formatting (via
/// ``op::sdk::text``) and a null-safety guard, so a mode never has to build a
/// buffer or reach for ``printf`` to log a value.

#include <cstdint>

#include <operator_sdk/abi/mode_api.h>  // OperatorApi, op::LogLevel
#include <operator_sdk/text.h>          // op::sdk::text formatters

namespace op {

extern const OperatorApi* api;

/// @brief Log a message to the device Log Monitor.
///
/// No-ops safely before a mode is fully loaded (while @ref op::api is null).
inline void log(LogLevel level, const char* message) {
    if (api) api->log(level, message);
}

/// @brief Log a labeled integer.
///
/// ``log(LogLevel::Info, "count", 42)`` shows ``count: 42`` on the Log Monitor.
inline void log(LogLevel level, const char* label, int32_t value) {
    constexpr uint8_t N = 64;
    char buf[N];
    ::op::sdk::text::format_str(label, buf, N);
    ::op::sdk::text::append_str(buf, N, ": ");
    ::op::sdk::text::append_int(buf, N, value);
    if (api) api->log(level, buf);
}

/// @brief Log a labeled fixed-point number.
///
/// ``value`` carries ``decimals`` fractional digits, the same fixed-point
/// convention as a Numeric param's ``decimal_places``, so
/// ``log(LogLevel::Info, "rate", 1250, 2)`` shows ``rate: 12.50``.
inline void log(LogLevel level, const char* label, int32_t value, uint8_t decimals) {
    constexpr uint8_t N = 64;
    char buf[N];
    ::op::sdk::text::format_str(label, buf, N);
    ::op::sdk::text::append_str(buf, N, ": ");
    ::op::sdk::text::append_fixed(buf, N, value, decimals);
    if (api) api->log(level, buf);
}

}  // namespace op
