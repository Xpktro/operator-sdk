#pragma once
// Bresenham-style Euclidean rhythm distributor for the Euclidean Sequencer
// sample.
//
// Bresenham is the simpler equivalent of Bjorklund's algorithm, and both produce
// the same onset set for the same (events, steps) pair. Demaine et al., "Distance
// Geometry of Music", carries the proof and the full discussion.
//
// Two edges are handled up front. With events == 0 every entry is false and the
// sequence is silent, and with events >= steps every entry is true and every step
// is an onset. Both cases sit outside the distribution math, which needs
// 0 < events < steps to say anything meaningful.

#include <cstdint>

namespace op::samples::euclidean {

// Fill `out[0 .. steps-1]` with the Euclidean rhythm, true at an onset and false
// elsewhere.
//
// `events` is the number of onsets to distribute and is clamped to [0, steps].
// `steps` is the pattern length, and the caller ensures it is greater than 0.
// `offset` rotates the finished pattern, shifting every onset right by that many
// slots and wrapping at `steps`. `out` is a caller-owned buffer of at least
// `steps` entries.
//
// Position `i` carries an onset when `(i * events) mod steps < events`, which puts
// the first onset at position 0. That is the alignment every popular
// Euclidean-rhythm UI shows, so (3,8) reads as the triplet `[1,0,0,1,0,0,1,0]`.
//
// The cost is one multiply and one mod per step, and the pattern is rebuilt only
// when (steps, events, offset) change, so a tick that changes nothing costs
// nothing.
inline void euclidean_bresenham(uint8_t events, uint8_t steps, uint8_t offset, bool* out) {
    if (events == 0) {
        for (uint8_t i = 0; i < steps; ++i) out[i] = false;
        return;
    }
    if (events >= steps) {
        for (uint8_t i = 0; i < steps; ++i) out[i] = true;
        return;
    }

    // Build the unrotated pattern first, then rotate it into `out`. Two passes keep
    // the rotation arithmetic clear when offset runs past events.
    bool unrotated[256] = {};
    for (uint8_t i = 0; i < steps; ++i) {
        const uint32_t position = (i * events) % steps;
        unrotated[i]            = (position < events);
    }
    const uint8_t rotation = offset % steps;
    for (uint8_t i = 0; i < steps; ++i) {
        const uint8_t source = (i + steps - rotation) % steps;
        out[i]               = unrotated[source];
    }
}

}  // namespace op::samples::euclidean
