// Tests for the framebuffer renderers (op::sim::to_ascii / to_svg) and the
// MockDisplay convenience wrappers.
//
// The ASCII case is a golden-snapshot regression guard: a known pattern is
// drawn and the rendered art is compared against an inline expected literal.
// The SVG cases pin the run-length <rect> emission and the empty-framebuffer
// (zero <rect>) behavior.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "framebuffer_render.h"
#include "mock_display.h"

#include <cstdint>
#include <string>

TEST_CASE("render_ascii_golden") {
    // An 8x8 framebuffer is one page (8 rows). index = x, bit = y.
    // Pattern: a 1px outline rectangle covering the full 8x8 region with the
    // interior empty, which is a shape that reads at a glance.
    std::uint8_t fb[8] {};

    // Edge columns (x = 0 and x = 7): all 8 rows lit -> 0xFF.
    fb[0] = 0xFF;
    fb[7] = 0xFF;
    // Interior columns (x = 1..6): top row (y = 0, bit 0) and bottom row
    // (y = 7, bit 7) lit -> 0x81.
    for (int x = 1; x <= 6; ++x) fb[x] = 0x81;

    const std::string golden = "########\n"
                               "#......#\n"
                               "#......#\n"
                               "#......#\n"
                               "#......#\n"
                               "#......#\n"
                               "#......#\n"
                               "########\n";

    CHECK(op::sim::to_ascii(fb, 8, 8) == golden);
}

TEST_CASE("render_ascii_wrapper_matches_free_fn") {
    // The MockDisplay wrapper must produce the same art as the free function
    // applied to its framebuffer.
    op::sim::MockDisplay panel;
    panel.clear();
    panel.draw_rect(0, 0, 8, 8);

    const std::string via_wrapper = panel.to_ascii();
    const std::string via_free    = op::sim::to_ascii(panel.framebuffer(), op::sim::kMockDisplayWidth,
                                                      op::sim::kMockDisplayHeight);
    CHECK(via_wrapper == via_free);

    // Full-panel art is 64 lines of 128 chars + '\n' each.
    CHECK(via_wrapper.size() == static_cast<std::size_t>(64) * (128 + 1));
    // Top-left corner pixel is lit.
    CHECK(via_wrapper[0] == '#');
}

TEST_CASE("render_svg_empty_has_no_rect") {
    std::uint8_t fb[8] {};  // all off
    const std::string svg = op::sim::to_svg(fb, 8, 8);

    CHECK(svg.find("<svg") != std::string::npos);
    CHECK(svg.find("viewBox=\"0 0 8 8\"") != std::string::npos);
    CHECK(svg.find("</svg>") != std::string::npos);
    // Zero lit pixels -> zero <rect> elements.
    CHECK(svg.find("<rect") == std::string::npos);
}

TEST_CASE("render_svg_run_length_rect") {
    // Row 0: lit pixels at x = 2,3,4 -> a single run of width 3 at x = 2.
    std::uint8_t fb[8] {};
    fb[2] = 0x01;  // y = 0
    fb[3] = 0x01;
    fb[4] = 0x01;

    const std::string svg = op::sim::to_svg(fb, 8, 8);

    // Exactly one <rect>, with x = 2 and width = 3 at y = 0, height 1.
    const std::string expected = "<rect x=\"2\" y=\"0\" width=\"3\" height=\"1\"/>";
    CHECK(svg.find(expected) != std::string::npos);

    // Only one rect overall.
    const std::size_t first = svg.find("<rect");
    REQUIRE(first != std::string::npos);
    CHECK(svg.find("<rect", first + 1) == std::string::npos);
}

TEST_CASE("render_svg_scale_multiplies_dimensions") {
    std::uint8_t fb[8] {};
    const std::string svg = op::sim::to_svg(fb, 8, 8, 4);

    // The width and height attributes scale. viewBox stays in pixel coordinates.
    CHECK(svg.find("width=\"32\"") != std::string::npos);
    CHECK(svg.find("height=\"32\"") != std::string::npos);
    CHECK(svg.find("viewBox=\"0 0 8 8\"") != std::string::npos);
}
