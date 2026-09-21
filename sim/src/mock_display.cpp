// MockDisplay. See mock_display.h.
//
// The geometry is the page addressing mock_display.h lays out, and the text routines
// read the glyphs in mock_font.h.

#include "mock_display.h"

#include "mock_font.h"

namespace op::sim {

void MockDisplay::clear() {
    for (std::uint16_t i = 0; i < kMockFramebufferSize; ++i) fb_[i] = 0;
}

void MockDisplay::set_pixel(std::uint16_t x, std::uint16_t y, bool on) {
    if (x >= kMockDisplayWidth || y >= kMockDisplayHeight) return;
    const std::uint16_t page = static_cast<std::uint16_t>(y / 8);
    const std::uint8_t bit   = static_cast<std::uint8_t>(y % 8);  // LSB = top
    const std::size_t idx    = static_cast<std::size_t>(page) * kMockDisplayWidth + x;
    if (on)
        fb_[idx] = static_cast<std::uint8_t>(fb_[idx] | (1u << bit));
    else
        fb_[idx] = static_cast<std::uint8_t>(fb_[idx] & ~(1u << bit));
}

void MockDisplay::draw_rect(std::uint16_t x, std::uint16_t y, std::uint16_t w, std::uint16_t h, bool on) {
    if (w == 0 || h == 0) return;
    // Top + bottom edges.
    for (std::uint16_t i = 0; i < w; ++i) {
        set_pixel(static_cast<std::uint16_t>(x + i), y, on);
        set_pixel(static_cast<std::uint16_t>(x + i), static_cast<std::uint16_t>(y + h - 1), on);
    }
    // Left + right edges.
    for (std::uint16_t j = 0; j < h; ++j) {
        set_pixel(x, static_cast<std::uint16_t>(y + j), on);
        set_pixel(static_cast<std::uint16_t>(x + w - 1), static_cast<std::uint16_t>(y + j), on);
    }
}

void MockDisplay::fill_rect(std::uint16_t x, std::uint16_t y, std::uint16_t w, std::uint16_t h, bool on) {
    if (w == 0 || h == 0) return;
    for (std::uint16_t j = 0; j < h; ++j) {
        for (std::uint16_t i = 0; i < w; ++i) {
            set_pixel(static_cast<std::uint16_t>(x + i), static_cast<std::uint16_t>(y + j), on);
        }
    }
}

void MockDisplay::draw_bitmap(std::uint16_t x,
                              std::uint16_t y,
                              std::span<const std::uint8_t> bitmap,
                              std::uint16_t w,
                              std::uint16_t h) {
    // Page-addressed source: byte = page * w + col, bit = row % 8 (LSB top).
    for (std::uint16_t row = 0; row < h; ++row) {
        const std::uint16_t src_page = static_cast<std::uint16_t>(row / 8);
        const std::uint8_t src_bit   = static_cast<std::uint8_t>(row % 8);
        for (std::uint16_t col = 0; col < w; ++col) {
            const std::size_t src_idx = static_cast<std::size_t>(src_page) * w + col;
            if (src_idx >= bitmap.size()) break;
            const bool on = ((bitmap[src_idx] >> src_bit) & 1u) != 0;
            if (on) {
                set_pixel(static_cast<std::uint16_t>(x + col), static_cast<std::uint16_t>(y + row), true);
            }
        }
    }
}

void MockDisplay::draw_text(std::uint16_t x, std::uint16_t y, const char* text) {
    if (!text) return;
    std::uint16_t cx = x;
    while (*text) {
        char ch = *text++;
        if (ch < 32 || ch > 126) ch = '?';
        if (ch == ' ') {
            cx = static_cast<std::uint16_t>(cx + 4);  // narrow space advance
            if (cx >= kMockDisplayWidth) break;
            continue;
        }
        const std::uint8_t* glyph = kMockFont5x7[ch - 32];
        for (std::uint8_t col = 0; col < 5; ++col) {
            const std::uint8_t column_bits = glyph[col];
            for (std::uint8_t row = 0; row < 7; ++row) {
                if (column_bits & (1u << row)) {
                    set_pixel(static_cast<std::uint16_t>(cx + col), static_cast<std::uint16_t>(y + row),
                              true);
                }
            }
        }
        cx = static_cast<std::uint16_t>(cx + 6);  // 5px ink + 1px gap
        if (cx >= kMockDisplayWidth) break;
    }
}

void MockDisplay::draw_text_large(std::uint16_t x, std::uint16_t y, const char* text) {
    if (!text) return;
    std::uint16_t cx = x;
    while (*text) {
        char ch = *text++;
        if (ch < 32 || ch > 126) ch = '?';
        if (ch == ' ') {
            cx = static_cast<std::uint16_t>(cx + 6);  // large space advance
            if (cx >= kMockDisplayWidth) break;
            continue;
        }
        const std::uint8_t* glyph = kMockFont8x12[ch - 32];
        // Glyph is 16 bytes: 8 cols x 2 pages (page 0 = rows 0-7, page 1 = 8-15).
        for (std::uint8_t col = 0; col < 8; ++col) {
            const std::uint8_t p0 = glyph[col];
            for (std::uint8_t row = 0; row < 8; ++row) {
                if (p0 & (1u << row)) {
                    set_pixel(static_cast<std::uint16_t>(cx + col), static_cast<std::uint16_t>(y + row),
                              true);
                }
            }
            const std::uint8_t p1 = glyph[8 + col];
            for (std::uint8_t row = 0; row < 8; ++row) {
                if (p1 & (1u << row)) {
                    set_pixel(static_cast<std::uint16_t>(cx + col), static_cast<std::uint16_t>(y + 8 + row),
                              true);
                }
            }
        }
        cx = static_cast<std::uint16_t>(cx + 8);  // 8px advance, the glyph carries its own spacing
        if (cx >= kMockDisplayWidth) break;
    }
}

}  // namespace op::sim
