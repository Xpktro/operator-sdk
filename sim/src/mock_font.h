#pragma once
// SPDX-License-Identifier: CC0-1.0 AND CC-BY-SA-4.0
//
// The glyphs the mock draws text with.
//
// These are the device's own two fonts, byte for byte, so text in a test reads
// the way it does on the hardware.
//
// The fonts and where they come from:
//   * kMockFont5x7 is the Operator project's own 5x7 font, under CC0-1.0. Its
//     text is in licenses/CC0-1.0.txt.
//   * kMockFont8x12 is BigBlue Terminal 437 TT by VileR (int10h.org Oldschool PC
//     Font Pack), rasterized to an 8x12 cell, under CC-BY-SA-4.0. The attribution
//     is in THIRD-PARTY-NOTICES.md and the license text in
//     licenses/CC-BY-SA-4.0.txt.
//
// The cell sizes, which the pixel-region and right-anchor tests measure against:
//   * Small: a 5px cell, 6px to the next glyph, 4px for a space.
//   * Large: 8px to the next glyph, 6px for a space.
//
// The byte layout, which mock_display.cpp reads:
//   * 5x7: glyph[col] is a column byte, bit row is the pixel at (col, row), bit 0
//     at the top. 5 columns, 7 rows.
//   * 8x12: glyph[col] is the top page (rows 0 to 7), glyph[8 + col] the bottom
//     (rows 8 to 15). 8 columns, bit 0 at the top of each page.
//
// 95 glyphs cover printable ASCII, a space through '~', and a character maps to
// row (character - 32).

#include <cstdint>

namespace op::sim {

// 95 printable-ASCII glyphs, 5 column-bytes each (CC0-1.0).
extern const std::uint8_t kMockFont5x7[95][5];

// 95 printable-ASCII glyphs, 16 bytes each, 8 cols x 2 pages (CC-BY-SA-4.0).
extern const std::uint8_t kMockFont8x12[95][16];

}  // namespace op::sim
