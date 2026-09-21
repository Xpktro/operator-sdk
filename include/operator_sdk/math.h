#pragma once
/// @file math.h
/// @brief Value-mapping helpers (opt-in, header-only).
///
/// Header-only, opt-in, so usage is on demand. These map a value from one
/// numeric range into another. ``map_linear`` is integer-only. ``map_exp`` and
/// ``map_log`` return ``float`` and bend the curve, so they suit UI or other
/// non-hot-path code.

#include <cstdint>

// expf and logf come from the SDK's freestanding math shim, which links into every
// mode.
extern "C" float expf(float x);
extern "C" float logf(float x);

/// @namespace op::sdk::math
/// @brief Value-range mapping helpers.
namespace op::sdk::math {

namespace detail {

// The magnitude of a signed value, so a 64-bit divide can stay unsigned. The shim
// provides __aeabi_uldivmod for the unsigned divide. The signed __aeabi_ldivmod is
// not linked into a mode.
inline uint64_t magnitude(int64_t value) {
    return value < 0 ? 0u - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
}

}  // namespace detail

/// @brief Linearly map ``v`` from ``[in_lo, in_hi]`` to ``[out_lo, out_hi]``.
/// @param v Input value.
/// @param in_lo Input range low.
/// @param in_hi Input range high (must differ from ``in_lo``).
/// @param out_lo Output range low.
/// @param out_hi Output range high.
/// @return Mapped value, or ``out_lo`` when the input range collapses to a point.
inline int32_t map_linear(int32_t v, int32_t in_lo, int32_t in_hi, int32_t out_lo, int32_t out_hi) {
    if (in_hi == in_lo) return out_lo;
    const int64_t num = static_cast<int64_t>(v - in_lo) * (out_hi - out_lo);
    const int64_t den = in_hi - in_lo;
    // Dividing the magnitudes and restoring the sign truncates toward zero.
    const bool negative  = (num < 0) != (den < 0);
    const int64_t scaled = static_cast<int64_t>(detail::magnitude(num) / detail::magnitude(den));
    return out_lo + static_cast<int32_t>(negative ? -scaled : scaled);
}

/// @brief Exponential-curve map (input perceived linearly, output bent toward
///        ``out_lo``). Useful for frequency, time, or volume controls, where it
///        gives fine resolution near ``out_lo`` and coarse steps near ``out_hi``.
/// @param v Input value.
/// @param in_lo Input range low.
/// @param in_hi Input range high.
/// @param out_lo Output range low.
/// @param out_hi Output range high.
/// @return Mapped value, or ``out_lo`` when the input range collapses to a point.
inline float map_exp(float v, float in_lo, float in_hi, float out_lo, float out_hi) {
    if (in_hi == in_lo) return out_lo;
    const float t     = (v - in_lo) / (in_hi - in_lo);
    const float curve = (expf(t) - 1.0f) / (2.71828182845904523536f - 1.0f);
    return out_lo + curve * (out_hi - out_lo);
}

/// @brief Logarithmic-curve map (input perceived linearly, output bent toward
///        ``out_hi``). Rises quickly and then flattens, so it gives coarse steps
///        near ``out_lo`` and fine resolution near ``out_hi``.
/// @param v Input value.
/// @param in_lo Input range low.
/// @param in_hi Input range high.
/// @param out_lo Output range low.
/// @param out_hi Output range high.
/// @return Mapped value, or ``out_lo`` when the input range collapses to a point.
inline float map_log(float v, float in_lo, float in_hi, float out_lo, float out_hi) {
    if (in_hi == in_lo) return out_lo;
    const float t = (v - in_lo) / (in_hi - in_lo);
    // log(1 + t) / log(2), so t in [0, 1] maps the curve to [0, 1].
    const float curve = logf(1.0f + t) / 0.6931471805599453f;
    return out_lo + curve * (out_hi - out_lo);
}

}  // namespace op::sdk::math
