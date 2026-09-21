#pragma once
// The screen a mode draws on, mocked.
//
// The 128 x 64 panel is 8 pages, each 8 pixels tall. The byte for a pixel is at
// page * 128 + x, where page is y / 8, and inside that byte bit y % 8 is the row
// with bit 0 the top one.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "framebuffer_render.h"

namespace op::sim {

// 128 columns across 8 pages, 1024 bytes total.
inline constexpr std::uint16_t kMockDisplayWidth    = 128;
inline constexpr std::uint16_t kMockDisplayHeight   = 64;
inline constexpr std::uint16_t kMockPageCount       = kMockDisplayHeight / 8;             // 8
inline constexpr std::uint16_t kMockFramebufferSize = kMockDisplayWidth * kMockPageCount;  // 1024

class MockDisplay {
public:
    MockDisplay() = default;

    // Zero the framebuffer (all pixels off).
    void clear();

    // Plot or clear one pixel. An off-screen coordinate does nothing, as on the
    // device.
    void set_pixel(std::uint16_t x, std::uint16_t y, bool on);

    // 1px rectangle outline.
    void draw_rect(std::uint16_t x, std::uint16_t y, std::uint16_t w, std::uint16_t h, bool on = true);

    // Filled rectangle (or cleared region when on == false).
    void fill_rect(std::uint16_t x, std::uint16_t y, std::uint16_t w, std::uint16_t h, bool on = true);

    // Blit a 1bpp page-addressed bitmap. Source layout matches the framebuffer:
    // index = page * w + col, page = row / 8, bit = row % 8 (LSB top).
    void draw_bitmap(std::uint16_t x,
                     std::uint16_t y,
                     std::span<const std::uint8_t> bitmap,
                     std::uint16_t w,
                     std::uint16_t h);

    // Render a NUL-terminated string in the 5x7 font.
    // Advance: 6px per glyph (5px ink + 1px gap), 4px for space.
    void draw_text(std::uint16_t x, std::uint16_t y, const char* text);

    // Render a NUL-terminated string in the 8x12 font.
    // Advance: 8px per glyph, 6px for space.
    void draw_text_large(std::uint16_t x, std::uint16_t y, const char* text);

    std::uint8_t* framebuffer() {
        return fb_;
    }
    const std::uint8_t* framebuffer() const {
        return fb_;
    }

    // The framebuffer as ASCII art, through op::sim::to_ascii.
    std::string to_ascii() const {
        return op::sim::to_ascii(fb_, kMockDisplayWidth, kMockDisplayHeight);
    }

    // The framebuffer as an SVG, through op::sim::to_svg.
    std::string to_svg(std::uint16_t scale = 1) const {
        return op::sim::to_svg(fb_, kMockDisplayWidth, kMockDisplayHeight, scale);
    }

private:
    std::uint8_t fb_[kMockFramebufferSize] {};
};

}  // namespace op::sim
