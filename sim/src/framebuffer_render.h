#pragma once
// Turning the framebuffer into something a person can read, so a test can show
// what a mode drew.
//
// Free functions over a raw framebuffer pointer, so they work on any framebuffer,
// not only MockDisplay's. The byte for a pixel is at (y / 8) * w + x, with bit
// y % 8 the row and bit 0 the top one.

#include <cstdint>
#include <string>

namespace op::sim {

// Default panel geometry, so the renderers below take dimensions as defaults and
// run on any framebuffer without pulling in the mock display.
inline constexpr std::uint16_t kRenderDefaultWidth  = 128;
inline constexpr std::uint16_t kRenderDefaultHeight = 64;

// Render the framebuffer as ASCII art: '#' for a lit pixel, '.' for an off
// pixel. Output is h lines of exactly w chars, each terminated by '\n'.
// '.' (not space) keeps golden snapshots free of trailing-whitespace churn.
std::string to_ascii(const std::uint8_t* fb,
                     std::uint16_t w = kRenderDefaultWidth,
                     std::uint16_t h = kRenderDefaultHeight);

// Render the framebuffer as an SVG document. Emits one <rect> per horizontal
// run of consecutive lit pixels, which keeps the output compact. viewBox is
// "0 0 w h". scale multiplies the width and height attributes so the result
// renders larger without changing the coordinate space.
std::string to_svg(const std::uint8_t* fb,
                   std::uint16_t w     = kRenderDefaultWidth,
                   std::uint16_t h     = kRenderDefaultHeight,
                   std::uint16_t scale = 1);

}  // namespace op::sim
