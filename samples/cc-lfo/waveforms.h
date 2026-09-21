#pragma once
// Waveform evaluators for the CC LFO sample mode.
//
// Private header, included only from cc-lfo/main.cpp and the sample's host
// doctest suite. It exposes the six shape evaluators the mode dispatches on
// (sine, triangle, saw, inv-saw, square, and sample-and-hold).
//
// Sine is served from a 256-entry quarter-wave LUT mirrored to cover the full
// cycle, because the mode cannot call sinf(). The lookup
// plus quadrant arithmetic runs in a couple of cycles, and the table costs
// 256 x 1 byte = 256 B per mode, sitting in flash.
//
// Phase convention.
//   phase_1024 is a 10-bit unsigned full-cycle index (0..1023)
//   0    = start of cycle (center for sine, 0 for saw, low for square)
//   256  = pi/2    (peak for sine, high for the square transition)
//   512  = pi      (center again)
//   768  = 3*pi/2  (trough for sine)
//   1024 = 2*pi (wraps to 0)
//
// All evaluators return a uint8_t in [0, 254] where 127 is the natural center.
// The mode then maps this into [low, high] via low + value * (high - low) / 254.

#include <cstdint>

namespace op::samples::cc_lfo {

// ---------------------------------------------------------------------------
// Quarter-sine LUT.
// ---------------------------------------------------------------------------
// Precomputed at build time by the standard formula
//     kQuarterSine[i] = round(sin(i * pi / 512) * 127)  for i in 0..255
// so kQuarterSine[0] = 0 (center of cycle) and kQuarterSine[255] ~= 127
// (just shy of pi/2, which the mirror logic treats as the peak).
inline constexpr uint8_t kQuarterSine[256] = {
    0,   1,   2,   2,   3,   4,   5,   5,   6,   7,   8,   9,   9,   10,  11,  12,  12,  13,  14,  15,
    16,  16,  17,  18,  19,  19,  20,  21,  22,  22,  23,  24,  25,  26,  26,  27,  28,  29,  29,  30,
    31,  32,  32,  33,  34,  35,  35,  36,  37,  38,  38,  39,  40,  41,  41,  42,  43,  44,  44,  45,
    46,  46,  47,  48,  49,  49,  50,  51,  51,  52,  53,  54,  54,  55,  56,  56,  57,  58,  58,  59,
    60,  61,  61,  62,  63,  63,  64,  65,  65,  66,  67,  67,  68,  69,  69,  70,  71,  71,  72,  72,
    73,  74,  74,  75,  76,  76,  77,  78,  78,  79,  79,  80,  81,  81,  82,  82,  83,  84,  84,  85,
    85,  86,  86,  87,  88,  88,  89,  89,  90,  90,  91,  91,  92,  93,  93,  94,  94,  95,  95,  96,
    96,  97,  97,  98,  98,  99,  99,  100, 100, 101, 101, 102, 102, 102, 103, 103, 104, 104, 105, 105,
    106, 106, 106, 107, 107, 108, 108, 109, 109, 109, 110, 110, 111, 111, 111, 112, 112, 112, 113, 113,
    113, 114, 114, 114, 115, 115, 115, 116, 116, 116, 117, 117, 117, 118, 118, 118, 118, 119, 119, 119,
    120, 120, 120, 120, 121, 121, 121, 121, 122, 122, 122, 122, 122, 123, 123, 123, 123, 123, 124, 124,
    124, 124, 124, 124, 125, 125, 125, 125, 125, 125, 125, 126, 126, 126, 126, 126, 126, 126, 126, 126,
    126, 126, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
};

// ---------------------------------------------------------------------------
// Sine (LUT-based, quadrant-mirrored).
// ---------------------------------------------------------------------------
// Returns 0..254 centered at 127.
//
//   phase 0     -> 127 (center rising)
//   phase 256   -> 254 (peak)
//   phase 512   -> 127 (center falling)
//   phase 768   ->   0 (trough)
//   phase 1023  -> ~127 (approaching center from below)
//
// The quadrant selector (top two bits of phase_1024) picks whether we
// read the LUT forwards (quads 0, 3) or reversed (quads 1, 2), and
// whether we add the LUT value to 127 (quads 0, 1) or subtract it from
// 127 (quads 2, 3).
inline uint8_t sine_u7(uint16_t phase_1024) {
    const uint8_t quadrant = (phase_1024 >> 8) & 0x03u;
    const uint8_t index    = phase_1024 & 0xFFu;
    switch (quadrant) {
        case 0: return 127 + kQuarterSine[index];
        case 1: return 127 + kQuarterSine[255 - index];
        case 2: return 127 - kQuarterSine[index];
        case 3: return 127 - kQuarterSine[255 - index];
        default: return 127;  // unreachable, silences warnings
    }
}

// ---------------------------------------------------------------------------
// Saw (linear 0..254 ramp over the full phase).
// ---------------------------------------------------------------------------
// phase_1024 / 4 gives 0..255, capped at 254 so the centered-at-127 mapping in
// the mode's scaling stage stays symmetric with the other shapes. Monotonic
// within a single cycle.
inline uint8_t saw_u7(uint16_t phase_1024) {
    const uint16_t value = phase_1024 >> 2;  // 0..255
    return value > 254 ? 254 : value;
}

// ---------------------------------------------------------------------------
// Inverted saw (linear 254..0 fall).
// ---------------------------------------------------------------------------
inline uint8_t inv_saw_u7(uint16_t phase_1024) {
    return 254 - saw_u7(phase_1024);
}

// ---------------------------------------------------------------------------
// Triangle (rise 0..254 over the first half, fall 254..0 over the second).
// ---------------------------------------------------------------------------
// Half-phase is 512, and each half maps linearly to [0, 254].
inline uint8_t triangle_u7(uint16_t phase_1024) {
    if (phase_1024 < 512) {
        // Rising half, 0..254 over phase 0..511.
        const uint16_t value = phase_1024 >> 1;  // 0..255
        return value > 254 ? 254 : value;
    }
    // Falling half.
    const uint16_t value = (1023 - phase_1024) >> 1;  // 0..255 as phase goes 1023->512
    return value > 254 ? 254 : value;
}

// ---------------------------------------------------------------------------
// Square (0 for the first half, 254 for the second).
// ---------------------------------------------------------------------------
// 50% duty, and the mode's low/high scaling turns the 0/254 pair into the
// user-configured extremes without further branching.
inline uint8_t square_u7(uint16_t phase_1024) {
    return phase_1024 < 512 ? 0 : 254;
}

// ---------------------------------------------------------------------------
// Sample-and-hold.
// ---------------------------------------------------------------------------
// xorshift32 fold. The caller passes a seed and the step index (which increments
// once per cycle). The combination deterministically picks a new value per step
// while staying the same within a step. Returns 0..254 to match the other shape
// evaluators.
inline uint8_t sample_hold_u7(uint32_t seed, uint32_t step_index) {
    uint32_t x = seed ^ (step_index * 0x9E3779B9u);
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    // Top-byte fold -> 0..255, then clamp to 254 to match saw/tri/square.
    const uint8_t value = (x >> 24) & 0xFFu;
    return value > 254 ? 254 : value;
}

}  // namespace op::samples::cc_lfo
