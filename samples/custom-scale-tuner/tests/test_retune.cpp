// Retune-math unit tests for the custom-scale-tuner sample.
//
// Covers the integer-cents pitch-bend math, the float hz_from_note fallback, and
// the MPE channel-allocation helpers.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../retune.h"
#include "../scala.h"

#include <cstdint>
#include <cstring>

using op::samples::scala::Scale;
using op::samples::scala::ScaleEntry;
using op::samples::tuner::cents_to_pitch_bend_14bit;
using op::samples::tuner::hz_from_note;
using op::samples::tuner::kMpeMemberChannelCount;
using op::samples::tuner::kMpeNoFreeChannel;
using op::samples::tuner::mpe_allocate_channel;
using op::samples::tuner::mpe_find_channel_for_note;
using op::samples::tuner::mpe_release_channel;
using op::samples::tuner::nearest_scale_note;
using op::samples::tuner::RetunedNote;

namespace {

// Build a 12-TET scale in memory with 12 entries at 100, 200, ..., 1200 cents.
Scale make_12tet() {
    Scale scale {};
    std::memset(&scale, 0, sizeof(scale));
    scale.count = 12;
    for (uint8_t i = 0; i < 12; ++i) {
        scale.entries[i] = ScaleEntry {
            /*is_cents*/ true,
            /*cents_x1000*/ static_cast<int32_t>((i + 1) * 100 * 1000),
            /*num*/ 0,
            /*den*/ 0,
        };
    }
    return scale;
}

// A "scale" with a 100.5-cent first degree over a 1200-cent (one octave) period,
// so it needs two entries, the 100.5 degree and the 1200 period. Indexing follows
// the scale's degree-per-key convention, where input_note = root_key + 1 picks
// entries[0] (the 100.5-cent degree) and input_note = root_key + 2 loops into
// entries[1], the 1200 period.
Scale make_single_offset_100_5() {
    Scale scale {};
    std::memset(&scale, 0, sizeof(scale));
    scale.count      = 2;
    scale.entries[0] = ScaleEntry {true, 100500, 0, 0};   // 100.500 cents
    scale.entries[1] = ScaleEntry {true, 1200000, 0, 0};  // 1200.000 cents period
    return scale;
}

Scale make_empty() {
    Scale scale {};
    std::memset(&scale, 0, sizeof(scale));
    scale.count = 0;
    return scale;
}

// Absolute-value helper, since the test avoids <cmath>.
float fabs_float(float value) {
    return value < 0.0f ? -value : value;
}

}  // namespace

// -----------------------------------------------------------------------
// nearest_scale_note, the integer-math retune
// -----------------------------------------------------------------------

TEST_CASE("nearest_scale_note 12-TET returns zero offset") {
    const Scale scale         = make_12tet();
    const RetunedNote retuned = nearest_scale_note(72, scale, 60);
    CHECK(retuned.midi_note == 72);
    CHECK(fabs_float(retuned.cents_offset) < 0.01f);
}

TEST_CASE("nearest_scale_note 100.5c offset is +0.5 cents") {
    const Scale scale = make_single_offset_100_5();
    // input_note = root_key + 1 picks entries[0] = 100.500c.
    const RetunedNote retuned = nearest_scale_note(61, scale, 60);
    // Total cents = 100.5 -> nearest whole semitone above root = +1 (MIDI 61),
    // fractional residual = +0.5c.
    CHECK(retuned.midi_note == 61);
    CHECK(fabs_float(retuned.cents_offset - 0.5f) < 0.01f);
}

// -----------------------------------------------------------------------
// cents_to_pitch_bend_14bit, an integer signature over integer math
// -----------------------------------------------------------------------

TEST_CASE("cents_to_pitch_bend_14bit integer 0 cents = 8192") {
    CHECK(cents_to_pitch_bend_14bit(0, 200) == 8192);
}

TEST_CASE("cents_to_pitch_bend_14bit integer +200k cents full up") {
    // +200 cents of bend at a +/-200-cent range -> clamps to 16383.
    CHECK(cents_to_pitch_bend_14bit(200000, 200) == 16383);
}

TEST_CASE("cents_to_pitch_bend_14bit integer -200k cents full down") {
    CHECK(cents_to_pitch_bend_14bit(-200000, 200) == 0);
}

TEST_CASE("cents_to_pitch_bend_14bit exact 50k/200k case = 10240") {
    // With cents_x1000 = 50000 (50 cents) and range = 200 cents, the result is
    //   8192 + 50000 * 8192 / (200 * 1000) = 8192 + 2048 = 10240.
    CHECK(cents_to_pitch_bend_14bit(50000, 200) == 10240);
}

TEST_CASE("cents_to_pitch_bend_14bit rounds half-away-from-zero") {
    // Construct a numerator that produces exactly .5 after division. Half-up at
    // positive gives q = (num + denom/2) / denom. With denom = 200 * 1000 =
    // 200000, pick a num that makes (x + 100000) / 200000 bump by exactly 1 at an
    // integer boundary.
    // Use cents_x1000 = 1, bend delta = (1 * 8192 + 100000) / 200000
    //                                = (8192 + 100000) / 200000
    //                                = 108192 / 200000 = 0. So q=0, bend=8192.
    CHECK(cents_to_pitch_bend_14bit(1, 200) == 8192);
    // A symmetric negative input keeps |q| the same magnitude as its positive
    // counterpart, which is what half-away-from-zero means.
    CHECK(cents_to_pitch_bend_14bit(-1, 200) == 8192);
    // Try a larger value where the rounding direction matters, cents_x1000 = 12.
    // (12 * 8192 + 100000) / 200000 = 198304 / 200000 = 0 -> q=0, bend=8192.
    CHECK(cents_to_pitch_bend_14bit(12, 200) == 8192);
    // At 13, (13 * 8192 + 100000) / 200000 = 206496 / 200000 = 1, so q=1 and
    // bend=8193.
    CHECK(cents_to_pitch_bend_14bit(13, 200) == 8193);
    // -13 -> q = -1 -> bend = 8191.
    CHECK(cents_to_pitch_bend_14bit(-13, 200) == 8191);
}

TEST_CASE("cents_to_pitch_bend_14bit clamps outside range") {
    // Absurdly large positive value -> clamped to 16383.
    CHECK(cents_to_pitch_bend_14bit(10'000'000, 200) == 16383);
    // Absurdly large negative value -> clamped to 0.
    CHECK(cents_to_pitch_bend_14bit(-10'000'000, 200) == 0);
}

TEST_CASE("cents_to_pitch_bend_14bit float overload matches the integer primary") {
    // The float overload defers to the integer primary and produces the same
    // result, give or take 1 for rounding variance.
    const uint16_t bend = cents_to_pitch_bend_14bit(50.0f, 2.0f);
    CHECK(bend >= 10239);
    CHECK(bend <= 10241);
}

// -----------------------------------------------------------------------
// hz_from_note, the float fallback
// -----------------------------------------------------------------------

TEST_CASE("hz_from_note 12-TET root returns exact root freq") {
    const Scale empty = make_empty();
    const float hz    = hz_from_note(69, 69, 440.0f, empty);
    CHECK(fabs_float(hz - 440.0f) < 0.001f);
}

TEST_CASE("hz_from_note 12-TET octave within 0.5 Hz") {
    const Scale empty = make_empty();
    const float hz    = hz_from_note(81, 69, 440.0f, empty);  // A5
    CHECK(fabs_float(hz - 880.0f) < 0.5f);
}

// -----------------------------------------------------------------------
// MPE helpers
// -----------------------------------------------------------------------

TEST_CASE("MPE allocate round-robin across member channels") {
    uint8_t active[kMpeMemberChannelCount] = {0};
    const uint8_t channel1                 = mpe_allocate_channel(60, active);
    const uint8_t channel2                 = mpe_allocate_channel(64, active);
    const uint8_t channel3                 = mpe_allocate_channel(67, active);
    // First-free round-robin, so the first free slot is index 0 and gives channel 2.
    CHECK(channel1 == 2);
    CHECK(channel2 == 3);
    CHECK(channel3 == 4);
}

TEST_CASE("MPE release frees channel") {
    uint8_t active[kMpeMemberChannelCount] = {0};
    const uint8_t channel                  = mpe_allocate_channel(60, active);
    REQUIRE(channel == 2);
    mpe_release_channel(channel, active);
    // Releasing clears the slot so the next allocate reuses channel 2.
    const uint8_t next_channel = mpe_allocate_channel(62, active);
    CHECK(next_channel == 2);
}

TEST_CASE("MPE find returns correct channel for allocated note") {
    uint8_t active[kMpeMemberChannelCount] = {0};
    mpe_allocate_channel(60, active);
    const uint8_t channel_for_62 = mpe_allocate_channel(62, active);
    const uint8_t found          = mpe_find_channel_for_note(62, active);
    CHECK(found == channel_for_62);
    // Unallocated note -> no channel.
    CHECK(mpe_find_channel_for_note(99, active) == kMpeNoFreeChannel);
}

TEST_CASE("MPE allocate returns sentinel when full") {
    uint8_t active[kMpeMemberChannelCount] = {0};
    // Fill every member slot.
    for (uint8_t i = 0; i < kMpeMemberChannelCount; ++i) {
        const uint8_t channel = mpe_allocate_channel(static_cast<uint8_t>(60 + i), active);
        REQUIRE(channel != kMpeNoFreeChannel);
    }
    // One more -> sentinel.
    CHECK(mpe_allocate_channel(99, active) == kMpeNoFreeChannel);
}
