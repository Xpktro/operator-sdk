// The framebuffer renderers. See framebuffer_render.h.
//
// The byte for a pixel is at (y / 8) * w + x, with bit y % 8 the row and bit 0 the
// top one.

#include "framebuffer_render.h"

namespace op::sim {

namespace {

    // True when the pixel at (x, y) is lit in the page-addressed framebuffer.
    inline bool pixel_on(const std::uint8_t* fb, std::uint16_t w, std::uint16_t x, std::uint16_t y) {
        const std::uint16_t page = static_cast<std::uint16_t>(y / 8);
        const std::uint8_t bit   = static_cast<std::uint8_t>(y % 8);  // LSB = top
        const std::size_t idx    = static_cast<std::size_t>(page) * w + x;
        return ((fb[idx] >> bit) & 1u) != 0;
    }

    // Append the base-10 text of a value to out.
    void append_uint(std::string& out, std::uint32_t value) {
        if (value == 0) {
            out.push_back('0');
            return;
        }
        // A uint32 is at most 10 digits, so this holds the widest value.
        char digits[10];
        int count = 0;
        while (value > 0) {
            digits[count++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
        while (count > 0) out.push_back(digits[--count]);
    }

}  // namespace

std::string to_ascii(const std::uint8_t* fb, std::uint16_t w, std::uint16_t h) {
    std::string out;
    if (fb == nullptr || w == 0 || h == 0) return out;
    // h rows of w chars + one '\n' per row.
    out.reserve(static_cast<std::size_t>(h) * (static_cast<std::size_t>(w) + 1));
    for (std::uint16_t y = 0; y < h; ++y) {
        for (std::uint16_t x = 0; x < w; ++x) {
            out.push_back(pixel_on(fb, w, x, y) ? '#' : '.');
        }
        out.push_back('\n');
    }
    return out;
}

std::string to_svg(const std::uint8_t* fb, std::uint16_t w, std::uint16_t h, std::uint16_t scale) {
    std::string out;
    if (fb == nullptr || w == 0 || h == 0) return out;
    if (scale == 0) scale = 1;

    const std::uint32_t scaled_w = static_cast<std::uint32_t>(w) * scale;
    const std::uint32_t scaled_h = static_cast<std::uint32_t>(h) * scale;

    out += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"";
    append_uint(out, scaled_w);
    out += "\" height=\"";
    append_uint(out, scaled_h);
    out += "\" viewBox=\"0 0 ";
    append_uint(out, w);
    out.push_back(' ');
    append_uint(out, h);
    out += "\">\n";

    for (std::uint16_t y = 0; y < h; ++y) {
        std::uint16_t x = 0;
        while (x < w) {
            if (!pixel_on(fb, w, x, y)) {
                ++x;
                continue;
            }
            const std::uint16_t run_start = x;
            while (x < w && pixel_on(fb, w, x, y)) ++x;
            const std::uint16_t run_len = static_cast<std::uint16_t>(x - run_start);
            out += "<rect x=\"";
            append_uint(out, run_start);
            out += "\" y=\"";
            append_uint(out, y);
            out += "\" width=\"";
            append_uint(out, run_len);
            out += "\" height=\"1\"/>\n";
        }
    }

    out += "</svg>\n";
    return out;
}

}  // namespace op::sim
