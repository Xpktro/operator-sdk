#pragma once
// Header-only retune + MPE channel-allocation helpers for the Custom Scale
// Tuner sample mode.
//
// Paired with `scala.h`, where the parser turns a .scl byte payload into a
// `Scale` and these helpers turn an incoming MIDI note plus a parameter snapshot
// into a retuned note plus (optionally) an MPE member-channel assignment.
//
// Two separable concerns live in this file.
//
//   1. Scale mapping (`nearest_scale_note`, `hz_from_note`,
//      `cents_to_pitch_bend_14bit`).
//      Given a 12-TET input MIDI note, find the closest degree in the
//      user-supplied Scale and return both the nearest whole MIDI note
//      and the fractional cents offset from that note. The caller emits
//      a pitch-bend MIDI message (0xE0 | channel) with the cents-to-14bit
//      conversion, then the note-on on that channel, and together they
//      reproduce the scale degree's exact pitch.
//
//   2. MPE allocation (`mpe_allocate_channel`, `mpe_release_channel`).
//      MPE (MIDI Polyphonic Expression, MMA 1.0) designates MIDI channel 1
//      as the master channel and channels 2..16 as member channels, one
//      note per channel.
//
// The math is integer throughout (cents x 1000 in int32, int64 only for the few
// products that need the range), doing its own rounding, power, and log work.
// The one float path is `hz_from_note`, for on-screen frequency display via a
// 2^(k/32) table (`fs_pow2f`). `cents_to_pitch_bend_14bit` is integer, with a
// float overload for float call sites.

#include "scala.h"

#include <cstdint>

namespace op::samples::tuner {

// ---------------------------------------------------------------------------
// MPE constants.
// ---------------------------------------------------------------------------
// The MPE 1.0 spec (MIDI Manufacturers Association) defines a zone with a
// master channel and a contiguous block of member channels. The default
// Lower Zone uses channel 1 as master and 2..16 as 15 member channels.
constexpr uint8_t kMpeMasterChannel      = 1;
constexpr uint8_t kMpeMemberChannelStart = 2;
constexpr uint8_t kMpeMemberChannelCount = 15;  // channels 2..16 inclusive

// Sentinel returned by mpe_allocate_channel when every member channel is
// currently busy. Callers should drop the note.
constexpr uint8_t kMpeNoFreeChannel = 0;

// ---------------------------------------------------------------------------
// Scale mapping.
// ---------------------------------------------------------------------------

// Produces the nearest whole MIDI note plus the fractional cents offset from
// that note. Callers emit a pitch-bend message carrying `cents_offset` so the
// final sounding pitch matches the microtonal scale degree exactly.
//
// `cents_offset` is in [-50.0, +50.0] by construction, since picking the nearest
// whole MIDI note leaves a residual of at most half a semitone in either
// direction. It is a `float`, computed from an int32 cents x 1000 divided by
// 1000.0f at the last step.
struct RetunedNote {
    uint8_t midi_note;   // nearest whole MIDI note (0..127)
    float cents_offset;  // signed cents from `midi_note`, +/-50.0
};

// Retune an incoming MIDI note to the corresponding degree of the
// user-supplied scale, using the scale's own period. Returns the nearest whole
// MIDI note and the fractional cents offset the caller should apply via pitch
// bend to reach the true microtonal pitch.
//
// Mapping model, one MIDI key per scale degree (MTS / Scala convention).
//
//   * MIDI key `root_key`          -> degree 0 (the implicit 1/1 base)
//   * MIDI key `root_key + 1`      -> degree 1 (entries[0])
//   * ...
//   * MIDI key `root_key + (N-1)`  -> degree N-1 (entries[N-2])
//   * MIDI key `root_key + N`      -> degree 0 of the next period
//                                    (entries[N-1] is the period boundary)
//
// Algorithm (all integer, cents x 1000).
//
//   1. key_distance = input_note - root_key  (signed distance from root).
//   2. Floor-divide key_distance by N into (periods, degree) in [0, N-1].
//   3. total_cents_x1000 = periods * period_cents_x1000 + degree_cents_x1000
//                          (int64 to hold the product).
//   4. nearest_semi = round_half_away_from_zero(total_cents_x1000 / 100000).
//   5. retuned_note = clamp(root_key + nearest_semi, 0, 127).
//   6. cents_offset = (total_cents_x1000 - nearest_semi * 100000) / 1000.0f
//                     (float conversion only at the final step).
//
// Empty scale -> pass through unchanged, no bend.
inline RetunedNote nearest_scale_note(uint8_t input_note,
                                      const scala::Scale& scale,
                                      uint8_t root_key) noexcept {
    if (scale.count == 0) return RetunedNote {input_note, 0.0f};

    const int32_t N_degrees          = static_cast<int32_t>(scale.count);
    const int32_t period_cents_x1000 = scala::entry_to_cents_x1000(scale.entries[scale.count - 1]);

    const int32_t key_distance = input_note - root_key;

    // Floor-divide + positive remainder so negative distances wrap cleanly.
    int32_t periods = key_distance / N_degrees;
    int32_t degree  = key_distance % N_degrees;
    if (degree < 0) {
        degree += N_degrees;
        --periods;
    }

    int32_t degree_cents_x1000 = 0;
    if (degree > 0) {
        degree_cents_x1000 = scala::entry_to_cents_x1000(scale.entries[degree - 1]);
    }

    // Total cents (x1000) = periods * period_cents_x1000 + degree_cents_x1000.
    // The product `periods * period_cents_x1000` can exceed int32 (e.g. 127
    // octaves * 1'200'000 cents_x1000 ~= 1.5e8, still fits, but extreme periods
    // with large scales may climb higher). Decompose so both sub-divides stay
    // int32/int32.
    //
    //   period_cents_x1000 = qa * 100000 + ra   (qa, ra are int32, ra in
    //   (-100000, +100000)) periods * period_cents = periods*qa*100000 +
    //   periods*ra total / 100000 ~= periods*qa + (periods*ra +
    //   degree_cents_x1000) / 100000
    const int32_t qa         = period_cents_x1000 / 100000;
    const int32_t ra         = period_cents_x1000 - qa * 100000;   // in (-100000, +100000)
    const int32_t big_part   = periods * qa;                       // integer semitones from full periods
    const int32_t small_part = periods * ra + degree_cents_x1000;  // residual cents_x1000

    // Round half-away-from-zero divide of small_part by 100000, all int32.
    // |small_part| <= |periods|*99999 + |degree_cents_x1000|, which stays
    // well under int32 for realistic musical inputs (|periods| <= 128, degree
    // cents bounded by the last period entry).
    int32_t q2;
    if (small_part >= 0) {
        q2 = (small_part + 50000) / 100000;
    } else {
        q2 = -((-small_part + 50000) / 100000);
    }
    const int32_t nearest_semi = big_part + q2;
    // frac = total_cents - nearest_semi * 100000
    //      = big_part * 100000 + small_part - (big_part + q2) * 100000
    //      = small_part - q2 * 100000
    const int32_t frac_cents_x1000 = small_part - q2 * 100000;

    int32_t retuned = root_key + nearest_semi;
    if (retuned < 0) retuned = 0;
    if (retuned > 127) retuned = 127;

    return RetunedNote {
        static_cast<uint8_t>(retuned),
        static_cast<float>(frac_cents_x1000) / 1000.0f,
    };
}

// ---------------------------------------------------------------------------
// Pitch-bend encoding.
// ---------------------------------------------------------------------------

// Default pitch-bend range in semitones. MIDI's out-of-the-box default
// is +/-2 semitones (no RPN setup required).
constexpr float kDefaultBendRangeSemis = 2.0f;

// MIDI pitch-bend "no bend" center value.
constexpr uint16_t kPitchBendCenter = 8192;

// Convert an integer signed cents x 1000 offset to a 14-bit MIDI
// pitch-bend value with an integer bend-range expressed in whole cents.
//
//   bend = 8192 + round(cents_x1000 * 8192 / (bend_range_cents * 1000))
//
// Clamped to [0, 16383]. `bend_range_cents` defaults to 200 cents (+/-2
// semitones).
//
// The gcd(8192, 1000) = 8 reduction rewrites the quotient as
// `cents_x1000 * 1024 / (bend_range_cents * 125)`, and pre-clamping cents_x1000
// to the saturation bound keeps that numerator inside int32 for every musically
// meaningful bend range (outputs beyond the 14-bit window saturate anyway).
inline uint16_t cents_to_pitch_bend_14bit(int32_t cents_x1000, int32_t bend_range_cents = 200) noexcept {
    if (bend_range_cents <= 0) return kPitchBendCenter;

    // At saturation, |cents_x1000| >= bend_range_cents * 1000 maps to |q| >= 8192
    // which clamps the 14-bit bend anyway, so pre-clamp via an int64 compare with
    // no divide.
    const int64_t sat_limit_64 = static_cast<int64_t>(bend_range_cents) * 1000;
    if (static_cast<int64_t>(cents_x1000) >= sat_limit_64) return 16383;
    if (static_cast<int64_t>(cents_x1000) <= -sat_limit_64) return 0;

    // Clamp cents_x1000 so the reduced numerator c * 1024 stays inside int32.
    // Real bend ranges (MMA tops out near 96 semitones = 9600 cents) sit far
    // below this bound.
    constexpr int32_t kMulSafeBound = 2'000'000;  // |clamped_cents| * 1024 < 2^31
    int32_t clamped_cents           = cents_x1000;
    if (clamped_cents > kMulSafeBound) clamped_cents = kMulSafeBound;
    if (clamped_cents < -kMulSafeBound) clamped_cents = -kMulSafeBound;

    const int32_t num32   = clamped_cents * 1024;    // fits int32 by clamp
    const int32_t denom32 = bend_range_cents * 125;  // fits int32 trivially
    int32_t bend_delta;
    if (num32 >= 0) {
        bend_delta = (num32 + denom32 / 2) / denom32;
    } else {
        bend_delta = -((-num32 + denom32 / 2) / denom32);
    }
    int32_t bend = 8192 + bend_delta;
    if (bend < 0) bend = 0;
    if (bend > 16383) bend = 16383;
    return bend;
}

// Scale a float and round it to the nearest integer, halves away from zero.
inline int32_t quantize(float value, float scale) noexcept {
    return static_cast<int32_t>(value >= 0.0f ? value * scale + 0.5f : value * scale - 0.5f);
}

// Float overload over the integer primary for float call sites. Quantizes the
// float inputs into integers then delegates.
inline uint16_t cents_to_pitch_bend_14bit(float cents_offset,
                                          float range_semis = kDefaultBendRangeSemis) noexcept {
    return cents_to_pitch_bend_14bit(quantize(cents_offset, 1000.0f), quantize(range_semis, 100.0f));
}

// ---------------------------------------------------------------------------
// Float 2^x for hz_from_note.
// ---------------------------------------------------------------------------
//
// 33-entry precomputed 2^(k/32) table + linear interpolation for the fractional
// part of x, and a bounded multiply loop for the integer part. Used for
// on-screen display of note frequencies, not on the MIDI-emission hot path.

inline float fs_pow2f(float x) noexcept {
    // Floor of x (integer part), allowing negative values.
    const int xi                          = static_cast<int>(x >= 0.0f ? x : x - 1.0f);
    const float xf                        = x - static_cast<float>(xi);  // in [0, 1)
    static constexpr float kPow2Table[33] = {
        1.0000000f, 1.0218971f, 1.0442737f, 1.0671405f, 1.0905077f, 1.1143867f, 1.1387886f,
        1.1637249f, 1.1892071f, 1.2152473f, 1.2418578f, 1.2690510f, 1.2968396f, 1.3252367f,
        1.3542555f, 1.3839096f, 1.4142135f, 1.4451809f, 1.4768262f, 1.5091640f, 1.5422108f,
        1.5759809f, 1.6104903f, 1.6457555f, 1.6817928f, 1.7186193f, 1.7562521f, 1.7947091f,
        1.8340080f, 1.8741676f, 1.9152065f, 1.9571441f, 2.0000000f,
    };
    const float index_f  = xf * 32.0f;
    const int index_i    = static_cast<int>(index_f);
    const float frac     = index_f - static_cast<float>(index_i);
    const int index      = index_i < 0 ? 0 : (index_i > 31 ? 31 : index_i);
    const float a        = kPow2Table[index];
    const float b        = kPow2Table[index + 1];
    const float mantissa = a + (b - a) * frac;
    float result         = mantissa;
    if (xi > 0) {
        for (int i = 0; i < xi; ++i) result *= 2.0f;
    } else if (xi < 0) {
        for (int i = 0; i < -xi; ++i) result *= 0.5f;
    }
    return result;
}

// Compute the tuned frequency (Hz) for a MIDI note under the given scale.
// Mirrors `nearest_scale_note`'s degree-per-key semantics but returns a real
// frequency for on-screen pitch display, not used by the mode's MIDI emission
// path.
//
// Empty scale -> 12-TET fallback from root.
inline float hz_from_note(uint8_t note,
                          uint8_t root_key,
                          float root_freq_hz,
                          const scala::Scale& scale) noexcept {
    const int32_t key_distance = note - root_key;

    if (scale.count == 0) {
        // 12-TET fallback, where one MIDI key is one semitone.
        return root_freq_hz * fs_pow2f(static_cast<float>(key_distance) / 12.0f);
    }

    const int32_t N_degrees          = static_cast<int32_t>(scale.count);
    const int32_t period_cents_x1000 = scala::entry_to_cents_x1000(scale.entries[scale.count - 1]);

    int32_t periods = key_distance / N_degrees;
    int32_t degree  = key_distance % N_degrees;
    if (degree < 0) {
        degree += N_degrees;
        --periods;
    }

    int32_t degree_cents_x1000 = 0;
    if (degree > 0) {
        degree_cents_x1000 = scala::entry_to_cents_x1000(scale.entries[degree - 1]);
    }

    const int64_t total_cents_x1000 = static_cast<int64_t>(periods) * static_cast<int64_t>(period_cents_x1000)
        + static_cast<int64_t>(degree_cents_x1000);

    // Octaves = total_cents_x1000 / (1200 * 1000), converted to float only at
    // the last step (the integer accumulation is exact).
    const float octaves = static_cast<float>(total_cents_x1000) / 1200000.0f;
    return root_freq_hz * fs_pow2f(octaves);
}

// ---------------------------------------------------------------------------
// MPE channel allocation.
// ---------------------------------------------------------------------------
// The allocation table `active_channels` is a 15-slot byte array indexed by
// member-channel position (0 -> channel 2, 14 -> channel 16). Each slot is
// either 0 (free) or the MIDI note number currently held on that channel
// (1..127). Mode state owns the array, these helpers are pure functions.

// Allocate a free member channel for a new note. Round-robin from channel
// 2 upward, returns the assigned channel (2..16) on success or
// `kMpeNoFreeChannel` (0) if every slot is busy.
inline uint8_t mpe_allocate_channel(uint8_t input_note, uint8_t* active_channels) {
    if (!active_channels) return kMpeNoFreeChannel;
    for (uint8_t i = 0; i < kMpeMemberChannelCount; ++i) {
        if (active_channels[i] == 0) {
            active_channels[i] = input_note;
            return kMpeMemberChannelStart + i;
        }
    }
    return kMpeNoFreeChannel;
}

// Release a previously-allocated member channel by channel number (2..16).
inline void mpe_release_channel(uint8_t channel, uint8_t* active_channels) {
    if (!active_channels) return;
    if (channel < kMpeMemberChannelStart) return;
    const uint8_t slot_index = channel - kMpeMemberChannelStart;
    if (slot_index >= kMpeMemberChannelCount) return;
    active_channels[slot_index] = 0;
}

// Find the member channel currently holding `note`, or `kMpeNoFreeChannel`
// if no slot is holding it. Used when a note-off arrives and the mode
// needs to route the off to the same channel the on went out on.
inline uint8_t mpe_find_channel_for_note(uint8_t note, const uint8_t* active_channels) {
    if (!active_channels) return kMpeNoFreeChannel;
    for (uint8_t i = 0; i < kMpeMemberChannelCount; ++i) {
        if (active_channels[i] == note) {
            return kMpeMemberChannelStart + i;
        }
    }
    return kMpeNoFreeChannel;
}

}  // namespace op::samples::tuner
