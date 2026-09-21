#pragma once
/// @file text.h
/// @brief Text-layout helpers for Operator modes (opt-in, header-only).
///
/// Include on demand. This header is deliberately not pulled in by the
/// umbrella ``operator_sdk.h``.
///
/// All functions are pure (no ABI calls, no global state) and take only what
/// is needed. Width math assumes the device's built-in fonts:
///   - Standard font (``draw_text``):       5x7 glyph, 1px spacing => 6px advance,
///                                          8px line height.
///   - Large font (``draw_text_large``):    8x12 cell (7px ink + 1px spacing) =>
///                                          8px advance, 13px line height.
/// These metrics match the device's built-in font renderer.

#include <cstddef>  // std::size_t (fs_strlen); <cstdint> does not guarantee it
#include <cstdint>

/// @namespace op::sdk::text
/// @brief Text measurement and layout helpers for mode UIs.
namespace op::sdk::text {

namespace detail {
    /// Freestanding inline strlen. The SDK ABI target is -nostdlib on
    /// arm-none-eabi, so `std::strlen` (from libc) is not available. Hand-rolled
    /// here so every TU that includes text.h stays libc-free.
    inline std::size_t fs_strlen(const char* s) noexcept {
        std::size_t n = 0;
        while (s && s[n] != '\0') ++n;
        return n;
    }
}  // namespace detail

/// @brief Horizontal advance (pixels) of one standard-font glyph.
inline constexpr uint8_t kFontStdAdvance = 6;

/// @brief Horizontal advance (pixels) of the standard-font space glyph.
/// Space renders narrower than other glyphs (4px advance), matching the
/// device's standard-font renderer.
inline constexpr uint8_t kFontStdSpaceAdvance = 4;

/// @brief Line height (pixels) of the standard font.
inline constexpr uint8_t kFontStdHeight = 8;

/// @brief Horizontal advance (pixels) of one large-font glyph.
inline constexpr uint8_t kFontLargeAdvance = 8;

/// @brief Horizontal advance (pixels) of the large-font space glyph.
/// Space renders narrower (6px advance), matching the device's
/// large-font renderer.
inline constexpr uint8_t kFontLargeSpaceAdvance = 6;

/// @brief Line height (pixels) of the large font.
inline constexpr uint8_t kFontLargeHeight = 13;

// ---------------------------------------------------------------------------
// Integer -> decimal formatting (freestanding: no libc, no double, no 64-bit
// division). These are the SDK-blessed replacement for the per-mode
// hand-rolled `buf[pos++] = '0' + v/10` digit loops. The conversion uses a
// uint32 `% 10` / `/= 10` reverse-digit loop (32-bit `UDIV` on Cortex-M33, so
// the compiler does not emit `__aeabi_uldivmod`). Negative int32 magnitude is
// taken via `-(int64)v` then narrowed to uint32 (negation only, not a 64-bit
// divide), so INT32_MIN is handled without underflow and the freestanding gate
// stays satisfied (the int64 temporary never reaches a division).
// ---------------------------------------------------------------------------

namespace detail {
    /// Shared core: write the decimal digits of an already-unsigned magnitude into
    /// `out[pos..]`, NUL-terminating. Returns nothing, bounded by `out_size`. A
    /// leading sign char (if any) must already be placed by the caller at `pos`.
    inline void fs_write_u32(uint32_t u, char* out, uint8_t out_size, uint8_t pos) {
        char tmp[10];  // 4294967295 == 10 digits, no NUL needed (reversed)
        uint8_t tlen = 0;
        do {
            tmp[tlen++] = static_cast<char>('0' + (u % 10u));
            u /= 10u;
        } while (u && tlen < sizeof(tmp));
        while (tlen > 0 && pos + 1 < out_size) out[pos++] = tmp[--tlen];
        out[pos] = '\0';
    }
}  // namespace detail

/// @brief Overwrite ``out`` with the decimal digits of ``v``, NUL-terminated.
/// @param v Value to format (0 .. 4294967295).
/// @param out Destination buffer. ``out[0]`` is overwritten, NUL-terminated on
///            return. Must be non-null with ``out_size >= 1``.
/// @param out_size Capacity of ``out`` in bytes. The result is truncated to fit
///                 (the least-significant digits are dropped if the buffer is too
///                 small), so size ``out`` for the widest value: 11 bytes holds
///                 any uint32 ("4294967295" + NUL).
///
/// Freestanding: no libc, no ``double``, 32-bit division only. ``v == 0`` yields
/// ``"0"``.
inline void format_uint(uint32_t v, char* out, uint8_t out_size) {
    if (!out || out_size == 0) return;
    detail::fs_write_u32(v, out, out_size, /*pos=*/0);
}

/// @brief Overwrite ``out`` with the signed decimal of ``v``, NUL-terminated.
/// @param v Value to format (INT32_MIN .. INT32_MAX). INT32_MIN is handled
///          correctly (magnitude taken via negation of an int64 temporary, then
///          narrowed, no 64-bit divide).
/// @param out Destination buffer, overwritten and NUL-terminated. Non-null,
///            ``out_size >= 1``.
/// @param out_size Capacity in bytes. Result truncates to fit. Size for the
///                 widest output: 12 bytes holds "-2147483648" + NUL (or
///                 "+2147483647" with ``force_plus``).
/// @param force_plus When true, non-negative values get a leading ``'+'``
///                   (``+5`` / ``-3``). When false, non-negative values are bare
///                   (``5`` / ``-3``). Negative values always get ``'-'``.
///
/// Freestanding: no libc, no ``double``, 32-bit division only.
inline void format_int(int32_t v, char* out, uint8_t out_size, bool force_plus = false) {
    if (!out || out_size == 0) return;
    const bool neg   = (v < 0);
    const uint32_t u = neg ? static_cast<uint32_t>(-static_cast<int64_t>(v)) : static_cast<uint32_t>(v);
    uint8_t pos      = 0;
    if (neg) {
        if (pos + 1 < out_size) out[pos++] = '-';
    } else if (force_plus) {
        if (pos + 1 < out_size) out[pos++] = '+';
    }
    detail::fs_write_u32(u, out, out_size, pos);
}

/// @brief Append the decimal digits of ``v`` at the existing NUL of ``buf``.
/// @param buf NUL-terminated buffer to extend in place (compose "Label: " +
///            number without a libc ``strcat``). Non-null.
/// @param buf_size Total capacity of ``buf`` in bytes. The appended digits
///                 truncate to fit the remaining space; ``buf`` stays
///                 NUL-terminated.
/// @param v Value to append (0 .. 4294967295).
///
/// Freestanding: no libc, no ``double``, 32-bit division only.
inline void append_uint(char* buf, uint8_t buf_size, uint32_t v) {
    if (!buf || buf_size == 0) return;
    const std::size_t len = detail::fs_strlen(buf);
    if (len + 1 >= buf_size) return;  // no room for even one digit
    detail::fs_write_u32(v, buf, buf_size, static_cast<uint8_t>(len));
}

/// @brief Append the signed decimal of ``v`` at the existing NUL of ``buf``.
/// @param buf NUL-terminated buffer to extend in place. Non-null.
/// @param buf_size Total capacity in bytes. Appended text truncates to fit;
///                 ``buf`` stays NUL-terminated.
/// @param v Value to append (INT32_MIN .. INT32_MAX); INT32_MIN-safe.
/// @param force_plus When true, non-negative ``v`` appends a leading ``'+'``.
///
/// Freestanding: no libc, no ``double``, 32-bit division only.
inline void append_int(char* buf, uint8_t buf_size, int32_t v, bool force_plus = false) {
    if (!buf || buf_size == 0) return;
    const std::size_t len = detail::fs_strlen(buf);
    if (len + 1 >= buf_size) return;
    const bool neg   = (v < 0);
    const uint32_t u = neg ? static_cast<uint32_t>(-static_cast<int64_t>(v)) : static_cast<uint32_t>(v);
    uint8_t pos      = static_cast<uint8_t>(len);
    if (neg) {
        if (pos + 1 < buf_size) buf[pos++] = '-';
    } else if (force_plus) {
        if (pos + 1 < buf_size) buf[pos++] = '+';
    }
    detail::fs_write_u32(u, buf, buf_size, pos);
}

// ---------------------------------------------------------------------------
// Runtime-string / enum-label composition (freestanding, no libc). These are
// the SDK-blessed replacement for the per-mode hand-rolled `for (i; src[i];)
// out[pos++] = src[i]` copy loops and unguarded `labels[idx]` lookups that get
// concatenated into a buffer. `format_str` is a bounded strlcpy (overwrite).
// `append_str` is a bounded strcat (compose at the existing NUL). `append_enum`
// is a bounds-checked label append (no out-of-bounds read on an out-of-range index). All
// silently truncate and always NUL-terminate, consistent with the number
// helpers above. A null source string is treated as empty.
// ---------------------------------------------------------------------------

/// @brief Overwrite ``out`` with a bounded copy of ``s`` (freestanding strlcpy).
/// @param s Source string (nullptr is treated as the empty string "").
/// @param out Destination buffer; ``out[0]`` is overwritten, NUL-terminated on
///            return. Non-null with ``out_size >= 1``.
/// @param out_size Capacity of ``out`` in bytes. The copy truncates to fit
///                 (``out_size - 1`` characters max) and ``out`` is always
///                 NUL-terminated.
///
/// Freestanding: no libc, no ``double``. Use for the runtime-string overwrite
/// primitive (filenames, resolved labels).
inline void format_str(const char* s, char* out, uint8_t out_size) {
    if (!out || out_size == 0) return;
    uint8_t pos = 0;
    if (s) {
        while (s[pos] != '\0' && pos + 1 < out_size) {
            out[pos] = s[pos];
            ++pos;
        }
    }
    out[pos] = '\0';
}

/// @brief Append a bounded copy of ``s`` at the existing NUL of ``buf``
///        (freestanding strcat).
/// @param buf NUL-terminated buffer to extend in place (compose "Label: " +
///            value without a libc ``strcat``). Non-null.
/// @param buf_size Total capacity of ``buf`` in bytes. The appended text
///                 truncates to fit the remaining space; ``buf`` stays
///                 NUL-terminated.
/// @param s Source string to append (nullptr is treated as the empty string).
///
/// Freestanding: no libc, no ``double``.
inline void append_str(char* buf, uint8_t buf_size, const char* s) {
    if (!buf || buf_size == 0) return;
    std::size_t pos = detail::fs_strlen(buf);
    if (!s) {
        return;
    }
    for (std::size_t i = 0; s[i] != '\0' && pos + 1 < buf_size; ++i) {
        buf[pos++] = s[i];
    }
    buf[pos] = '\0';
}

/// @brief Append ``labels[idx]`` at the existing NUL of ``buf``, bounds-checked.
/// @param buf NUL-terminated buffer to extend in place. Non-null.
/// @param buf_size Total capacity of ``buf`` in bytes. The appended label
///                 truncates to fit; ``buf`` stays NUL-terminated.
/// @param idx Index into ``labels``. If ``idx >= count`` the ``fallback`` string
///            is appended instead (no out-of-range read).
/// @param labels Caller-owned label table (``count`` entries). This helper only
///               indexes it safely. The mode owns the storage.
/// @param count Number of entries in ``labels``.
/// @param fallback Appended when ``idx`` is out of range (default ``"?"``).
///
/// Composes e.g. ``char b[24] = "Rate: "; append_enum(b, sizeof b, r,
/// kRateLabels, kRateCount);`` -> ``"Rate: 1/4"``. Implemented via
/// ``append_str``. Freestanding: no libc, no ``double``.
inline void append_enum(char* buf,
                        uint8_t buf_size,
                        uint32_t idx,
                        const char* const* labels,
                        uint32_t count,
                        const char* fallback = "?") {
    const char* s = (labels && idx < count) ? labels[idx] : fallback;
    append_str(buf, buf_size, s);
}

/// @brief Append ``value`` as a fixed-point decimal with ``decimals`` fractional
///        digits at the existing NUL of ``buf``.
/// @param buf NUL-terminated buffer to extend in place. Non-null.
/// @param buf_size Total capacity of ``buf`` in bytes. truncates to fit; ``buf``
///                 stays NUL-terminated.
/// @param value Integer carrying ``decimals`` fractional digits, the same
///              fixed-point convention as a Numeric param's ``decimal_places``.
/// @param decimals Number of fractional digits. ``0`` appends a plain integer.
///
/// Composes e.g. ``append_fixed(buf, size, 1250, 2)`` -> ``"12.50"`` and
/// ``append_fixed(buf, size, -5, 2)`` -> ``"-0.05"``. Freestanding: no libc, no
/// ``double``, 32-bit division only.
inline void append_fixed(char* buf, uint8_t buf_size, int32_t value, uint8_t decimals) {
    if (!buf || buf_size == 0) return;
    if (decimals == 0) {
        append_int(buf, buf_size, value);
        return;
    }

    uint32_t scale = 1;
    for (uint8_t i = 0; i < decimals; ++i) scale *= 10u;  // 10^decimals

    const bool neg     = (value < 0);
    const uint32_t mag = neg ? static_cast<uint32_t>(-static_cast<int64_t>(value))
                             : static_cast<uint32_t>(value);
    const uint32_t ip  = mag / scale;  // 32-bit UDIV
    const uint32_t fp  = mag % scale;

    if (neg) append_str(buf, buf_size, "-");
    append_uint(buf, buf_size, ip);
    append_str(buf, buf_size, ".");
    // Zero-padded fractional part, exactly `decimals` digits.
    for (uint32_t p = scale / 10u; p >= 1u; p /= 10u) {
        const char d[2] = {static_cast<char>('0' + (fp / p) % 10u), '\0'};
        append_str(buf, buf_size, d);
    }
}

/// @brief Count the characters in a NUL-terminated string.
/// @param s NUL-terminated string (may be nullptr, in which case 0 is returned).
/// @return Number of bytes preceding the terminator.
inline uint16_t strlen_chars(const char* s) {
    if (!s) return 0;
    return static_cast<uint16_t>(detail::fs_strlen(s));
}

/// @brief Pixel width of a string rendered in the standard 5x7 font.
/// @param s NUL-terminated string (nullptr is treated as empty).
/// @return Sum of per-glyph advances (space counts as kFontStdSpaceAdvance,
///         every other glyph as kFontStdAdvance), matching the firmware
///         renderer so callers (centering, underlines, right-anchoring) align
///         with the actual pixels. A flat strlen*advance over-measures any
///         string containing spaces.
inline uint16_t text_width(const char* s) {
    uint16_t w = 0;
    for (const char* p = s; p && *p; ++p)
        w = static_cast<uint16_t>(w + (*p == ' ' ? kFontStdSpaceAdvance : kFontStdAdvance));
    return w;
}

/// @brief Pixel width of a string rendered in the large 8x12 font.
/// @param s NUL-terminated string (nullptr is treated as empty).
/// @return Sum of per-glyph advances (space counts as kFontLargeSpaceAdvance,
///         every other glyph as kFontLargeAdvance), matching the firmware
///         renderer. A flat strlen*advance over-measures strings with spaces.
inline uint16_t large_text_width(const char* s) {
    uint16_t w = 0;
    for (const char* p = s; p && *p; ++p)
        w = static_cast<uint16_t>(w + (*p == ' ' ? kFontLargeSpaceAdvance : kFontLargeAdvance));
    return w;
}

/// @brief X-offset that horizontally centers a standard-font string inside a box.
/// @param s Text to measure.
/// @param total_w Width of the containing box in pixels.
/// @return Offset; 0 if the text is wider than the box.
inline uint16_t align_center_x(const char* s, uint16_t total_w) {
    const uint16_t tw = text_width(s);
    return tw >= total_w ? 0 : static_cast<uint16_t>((total_w - tw) / 2);
}

/// @brief X-offset that horizontally centers a large-font string inside a box.
/// @param s Text to measure.
/// @param total_w Width of the containing box in pixels.
/// @return Offset; 0 if the text is wider than the box.
inline uint16_t large_align_center_x(const char* s, uint16_t total_w) {
    const uint16_t tw = large_text_width(s);
    return tw >= total_w ? 0 : static_cast<uint16_t>((total_w - tw) / 2);
}

/// @brief Copy ``s`` into ``out``, appending ``"..."`` when it exceeds ``max_chars``.
/// @param s Source string (nullptr treated as empty).
/// @param max_chars Maximum number of visible characters (including the ellipsis).
///                  Values below 4 behave like a plain truncate (no ellipsis).
/// @param out Destination buffer; NUL-terminated on return.
/// @param out_size Size of ``out`` in bytes (must be >= 1).
inline void truncate_with_ellipsis(const char* s, uint16_t max_chars, char* out, uint16_t out_size) {
    if (!out || out_size == 0) return;
    const uint16_t len = strlen_chars(s);
    if (len <= max_chars) {
        // Full copy fits.
        uint16_t n = len < static_cast<uint16_t>(out_size - 1) ? len : static_cast<uint16_t>(out_size - 1);
        for (uint16_t i = 0; i < n; ++i) out[i] = s[i];
        out[n] = '\0';
        return;
    }
    // Need truncation.
    if (max_chars < 4) {
        uint16_t n = max_chars < static_cast<uint16_t>(out_size - 1) ? max_chars
                                                                     : static_cast<uint16_t>(out_size - 1);
        for (uint16_t i = 0; i < n; ++i) out[i] = s[i];
        out[n] = '\0';
        return;
    }
    const uint16_t keep = static_cast<uint16_t>(max_chars - 3);
    const uint16_t cap  = static_cast<uint16_t>(out_size - 1);
    const uint16_t n    = keep + 3 <= cap ? keep : (cap > 3 ? static_cast<uint16_t>(cap - 3) : 0);
    for (uint16_t i = 0; i < n; ++i) out[i] = s[i];
    uint16_t pos = n;
    if (pos + 3 <= cap) {
        out[pos++] = '.';
        out[pos++] = '.';
        out[pos++] = '.';
    }
    out[pos] = '\0';
}

/// @brief Greedy word-wrap a NUL-terminated string into lines no wider than
///        ``width_px`` pixels (standard font).
/// @param s Source string.
/// @param width_px Maximum pixel width of a single line.
/// @param line_starts Output array. Entry ``i`` points to the first character
///                    of line ``i`` inside the original buffer.
/// @param max_lines Capacity of ``line_starts``.
/// @return Number of lines populated (<= ``max_lines``).
///
/// Note: the input string is not modified, so callers treat ``line_starts[i]`` as
/// the start of a line whose length extends until the next line's start or
/// end-of-string (space trimmed on line breaks). For destructive wrapping use
/// your own routine. This implementation keeps the helper allocation-free.
inline uint8_t word_wrap(const char* s, uint16_t width_px, const char** line_starts, uint8_t max_lines) {
    if (!s || !line_starts || max_lines == 0 || width_px < kFontStdAdvance) {
        return 0;
    }
    uint8_t lines      = 0;
    const char* cursor = s;
    while (*cursor && lines < max_lines) {
        line_starts[lines++] = cursor;
        // Advance cursor up to width_px worth of characters, breaking at last
        // whitespace if possible.
        uint16_t budget        = width_px;
        const char* last_space = nullptr;
        const char* p          = cursor;
        while (*p && budget >= kFontStdAdvance) {
            if (*p == '\n') {
                ++p;
                break;
            }
            if (*p == ' ') last_space = p;
            budget = static_cast<uint16_t>(budget - kFontStdAdvance);
            ++p;
        }
        if (*p == '\0') {
            cursor = p;
        } else if (last_space && last_space > cursor) {
            cursor = last_space + 1;  // skip the wrapping space
        } else {
            cursor = p;  // hard break (no whitespace found in line)
        }
    }
    return lines;
}

}  // namespace op::sdk::text
