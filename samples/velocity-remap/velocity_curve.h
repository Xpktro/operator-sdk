#pragma once
// Builder for the velocity-remap lookup table.
//
// The table maps an input note-on velocity onto a remapped output velocity. Every
// entry lands between the two ends of the output window, and both ends are playable
// velocities, so a remapped note sounds. Building the table costs two
// transcendentals per entry, which is why the mode caches it and rebuilds it only
// when a param moves.
//
// The playable range 1..127 normalizes onto x in 0..1. Compand shapes x, Tension
// bends what comes out of that, and the window scales the result back onto
// velocities. Each line below uses only the lines above it.
//
//   x         = (input - 1) / 126               the input, as 0..1
//
//   offset    = |2x - 1|                        how far x sits from the middle
//   log_depth = ln(1 + depth)                   depth is fixed at 200
//   curved    = ln(1 + depth * offset) / log_depth        for compand > 0
//             = (exp(offset * log_depth) - 1) / depth     for compand < 0
//   full      = 0.5 + 0.5 * sign(2x - 1) * curved         x, fully shaped
//   blend     = |compand| / 100
//   companded = x + blend * (full - x)          x, carried toward full
//
//   bias      = 0.0099 * tension                |bias| stays under 1
//   anchor    = (1 + bias) / 2                  so anchor stays above 0
//   schlick   = 1 / anchor - 2
//   biased    = companded / (schlick * (1 - companded) + 1)
//
//   output    = round(minimum + (maximum - minimum) * biased)
//
// Normalizing on (input - 1) / 126 is what makes the default window an exact
// identity, since 1 + 126 * ((input - 1) / 126) comes back as input.
//
// depth is fixed at 200, which is the fully shaped curve. Compand sets how far x
// travels toward it, and it travels in step with the knob, so half of Compand gives
// half the shaping.
//
// Single precision throughout, and expf and logf come from the SDK.

#include <cstdint>

extern "C" float expf(float);
extern "C" float logf(float);

// Fill out[input] with the remapped velocity for each input velocity. minimum and
// maximum are the output window, each a playable velocity, and a minimum above the
// maximum inverts the curve so soft playing comes out loud. tension and compand are
// -100..+100 with 0 neutral.
inline void build_lut(uint8_t out[128], int32_t minimum, int32_t maximum, int32_t tension, int32_t compand) {
    const float bias      = 0.0099f * static_cast<float>(tension);
    const float anchor    = 0.5f * (bias + 1.0f);
    const float schlick   = (1.0f / anchor) - 2.0f;
    const int magnitude   = compand < 0 ? -compand : compand;
    const float blend     = static_cast<float>(magnitude) * 0.01f;
    const float depth     = 200.0f;
    const float log_depth = logf(1.0f + depth);

    for (int input = 0; input < 128; ++input) {
        float x = static_cast<float>(input - 1) / 126.0f;
        if (x < 0.0f) x = 0.0f;

        float companded = x;
        if (compand != 0) {
            float offset     = 2.0f * x - 1.0f;
            const float sign = offset < 0.0f ? -1.0f : 1.0f;
            if (offset < 0.0f) offset = -offset;
            const float curved = compand > 0 ? logf(1.0f + depth * offset) / log_depth
                                             : (expf(offset * log_depth) - 1.0f) / depth;
            const float full   = 0.5f + 0.5f * sign * curved;
            companded          = x + blend * (full - x);
        }

        // biased runs 0 to 1, so scaled is a blend of the two window ends and lands
        // between them.
        const float biased = (tension == 0) ? companded : companded / (schlick * (1.0f - companded) + 1.0f);
        const float scaled = static_cast<float>(minimum) + static_cast<float>(maximum - minimum) * biased;

        out[input] = static_cast<uint8_t>(scaled + 0.5f);  // round to nearest
    }
}
