#pragma once
/// @file draw.h
/// @brief Convenience wrappers over the display draw API (opt-in, header-only).
///
/// Include on demand. This header is deliberately not pulled in by the
/// umbrella ``operator_sdk.h``.
///
/// Why this exists
/// ---------------
/// The raw display entries on ``OperatorApi`` (``draw_rect``, ``fill_rect``,
/// ``draw_text``, ``draw_text_large``, ``draw_bitmap``, ``set_pixel``) take
/// ``uint16_t`` coordinate and dimension parameters. In mode UI code a
/// coordinate is almost always an *expression* (``y + 8``, ``x - 2``,
/// ``origin + span_w / 2``), and C++ integer promotion makes every such
/// expression a 32-bit ``int``. Passing an ``int`` to a ``uint16_t`` parameter
/// is a narrowing conversion, so you would end up writing
/// ``static_cast<uint16_t>(y + 8)`` at every single call-site adding unnecessary
/// noise.
///
/// These wrappers take ``int`` for every coordinate/dimension parameter and
/// forward to the matching raw ``op::api->...`` entry, performing the
/// narrowing once, in one audited place (``detail::u16``). The result is
/// behaviorally identical to the per-call-site cast. On the Cortex-M33 the
/// arithmetic runs in 32-bit registers regardless, and the only narrowing is
/// the store into the 16-bit ABI parameter, which happens either way.
///
/// Coordinates
/// -----------
/// The screen is 128x64, so valid coordinates are small: ``x`` in ``[0, 127]``
/// and ``y`` in ``[0, 63]``. Each argument is truncated to the ``uint16_t``
/// the draw ABI uses, exactly as ``static_cast<uint16_t>(expr)`` would, so
/// every in-range draw is lossless. The firmware also bounds-checks and
/// no-ops off-screen pixels.
///
/// The ABI itself is unchanged: ``mode_api.h`` still declares ``uint16_t``
/// parameters. The raw ``op::api->...`` pointers remain fully usable, and
/// these wrappers are pure supplements.
///
/// Usage
/// -----
/// @code{.cpp}
/// #include <operator_sdk/draw.h>
/// ...
/// op::draw::draw_text(kLabelX, y + 2, label);          // no cast
/// op::draw::fill_rect(x - 2, y + 8, 5, 1, /*on=*/true); // no cast
/// @endcode

#include <cstdint>

#include <operator_sdk/abi/mode_api.h>  // OperatorApi

namespace op {
// Implementation note: the definition (``inline const OperatorApi* api =
// nullptr;``) is emitted into each mode's translation unit by
// OP_MODE_PARAMS / OP_MODE_NO_PARAMS (see params.h). This ``extern`` only lets
// the wrappers below name ``op::api`` at parse time. It does not create a
// second definition.
extern const OperatorApi* api;
}  // namespace op

/// @namespace op::draw
/// @brief Screen drawing primitives for mode UIs: rectangles, text, and bitmaps.
namespace op::draw {

namespace detail {
    // Centralized narrowing: truncate a coordinate/dimension expression to the
    // ``uint16_t`` width the draw ABI uses. Modulo-65536, so values outside
    // ``[0, 65535]`` wrap (see the file-level Coordinates note). This is the
    // single audited narrow that all wrappers below funnel through.
    inline constexpr uint16_t u16(int v) noexcept {
        return static_cast<uint16_t>(v);
    }
}  // namespace detail

/// Draw a 1px rectangle outline. Forwards to ``op::api->draw_rect``.
/// @param x Left edge in pixels.
/// @param y Top edge in pixels.
/// @param w Width in pixels.
/// @param h Height in pixels.
inline void draw_rect(int x, int y, int w, int h) {
    ::op::api->draw_rect(detail::u16(x), detail::u16(y), detail::u16(w), detail::u16(h));
}

/// Fill or clear a rectangle. Forwards to ``op::api->fill_rect``.
/// @param x Left edge in pixels.
/// @param y Top edge in pixels.
/// @param w Width in pixels.
/// @param h Height in pixels.
/// @param on True sets the pixels, false clears them.
inline void fill_rect(int x, int y, int w, int h, bool on) {
    ::op::api->fill_rect(detail::u16(x), detail::u16(y), detail::u16(w), detail::u16(h), on);
}

/// Draw a NUL-terminated string in the standard 5x7 font. Forwards to
/// ``op::api->draw_text``.
/// @param x Left edge of the first glyph in pixels.
/// @param y Top edge in pixels.
/// @param text NUL-terminated string to draw.
inline void draw_text(int x, int y, const char* text) {
    ::op::api->draw_text(detail::u16(x), detail::u16(y), text);
}

/// Draw a NUL-terminated string in the large 8x12 font. Forwards to
/// ``op::api->draw_text_large``.
/// @param x Left edge of the first glyph in pixels.
/// @param y Top edge in pixels.
/// @param text NUL-terminated string to draw.
inline void draw_text_large(int x, int y, const char* text) {
    ::op::api->draw_text_large(detail::u16(x), detail::u16(y), text);
}

/// Blit a 1bpp bitmap. Forwards to ``op::api->draw_bitmap``.
/// @param x Left edge in pixels.
/// @param y Top edge in pixels.
/// @param data Page-addressed 1-bit pixel data.
/// @param w Bitmap width in pixels.
/// @param h Bitmap height in pixels.
inline void draw_bitmap(int x, int y, const uint8_t* data, int w, int h) {
    ::op::api->draw_bitmap(detail::u16(x), detail::u16(y), data, detail::u16(w), detail::u16(h));
}

/// Plot or clear a single pixel. Forwards to ``op::api->set_pixel`` (the
/// firmware bounds-checks and no-ops off-screen plots).
/// @param x Column in pixels.
/// @param y Row in pixels.
/// @param on True sets the pixel, false clears it.
inline void set_pixel(int x, int y, bool on) {
    ::op::api->set_pixel(detail::u16(x), detail::u16(y), on);
}

}  // namespace op::draw
