// CC LFO sample, doctest behavior suite.
//
// Exercises the 4-slot LFO output mode. It covers 6 waveforms via the
// quarter-sine LUT (sine, triangle, saw, inv-saw, square, S&H), sync-on
// (subdivision) and sync-off (Hz) rate modes, low/high clamp, CC# routing, and
// independent slot operation.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <operator_sdk_sim.h>
#include <operator_sdk.h>

#include "waveforms.h"

#include <cstdint>
#include <cstdlib>  // std::abs
#include <set>
#include <vector>

OP_MODE_UNDER_TEST();

namespace {

// --- Param layout -----------------------------------------------------------
//
// OP_MODE_PARAMS declares 28 value slots in slot order
// [LFO1][LFO2][LFO3][LFO4], each slot contributing seven scalar params
// (Source, CC, Shape, Low, High, RateHz, RateBeat). Keeping these
// slot/param indices explicit in the test file pins the test to the declaration
// order in main.cpp.
constexpr uint8_t kParamsPerSlot = 7;
constexpr uint8_t kSlotCount     = 4;

constexpr uint8_t kOffSource   = 0;
constexpr uint8_t kOffCc       = 1;
constexpr uint8_t kOffShape    = 2;
constexpr uint8_t kOffLow      = 3;
constexpr uint8_t kOffHigh     = 4;
constexpr uint8_t kOffRateHz   = 5;
constexpr uint8_t kOffRateBeat = 6;

// Source enum option indices (mirror the options list order in main.cpp).
constexpr int32_t kSourceOff  = 0;
constexpr int32_t kSourceFree = 1;
constexpr int32_t kSourceSync = 2;

// RateBeat option indices mirror the 20-option slow-to-fast list in main.cpp.
// Index 0 is "16", 1 "16T", 2 "8", 3 "8T", 4 "4", 5 "4T", 6 "2", 7 "2T",
// 8 "1", 9 "1T", 10 "1/2", 11 "1/2T", 12 "1/4", 13 "1/4T", 14 "1/8",
// 15 "1/8T", 16 "1/16", 17 "1/16T", 18 "1/32", and 19 "1/32T".
constexpr int32_t kRateBeatSlow16  = 0;   // "16" is 64 beats/cycle
constexpr int32_t kRateBeatQuarter = 12;  // "1/4" is 1 beat/cycle

// Per-slot fair-share emission throttle (mirrors kTotalEmitBudgetHz /
// kEmitPeriodPerLfoUs in main.cpp, a local mirror since there is no production
// accessor). The instance's combined emission is paced to a polite
// ~kTotalEmitBudgetHz total, split evenly across the active slots, so each
// active slot's minimum emit interval is N_active x kEmitPeriodPerLfoUs.
constexpr uint32_t kTotalEmitBudgetHz  = 500;
constexpr uint32_t kEmitPeriodPerLfoUs = 1'000'000u / kTotalEmitBudgetHz;

constexpr uint8_t param_id(uint8_t slot, uint8_t offset) {
    return slot * kParamsPerSlot + offset;
}

// Shape enum values (mirrored from the options list order in main.cpp).
constexpr int32_t kShapeSine       = 0;
constexpr int32_t kShapeTriangle   = 1;
constexpr int32_t kShapeSaw        = 2;
constexpr int32_t kShapeInvSaw     = 3;
constexpr int32_t kShapeSquare     = 4;
constexpr int32_t kShapeSampleHold = 5;

constexpr uint8_t kTestOutput = 0;
constexpr uint32_t kSliceUs   = 1000;

// --- Helpers ---------------------------------------------------------------

// The sim's outgoing ring is 32 messages deep and silently drops past
// that, and a dense LFO can generate hundreds of CC messages per second, so
// run() drains incrementally into g_collected and the ring never overflows.
// drain() returns the accumulated buffer and clears it.
std::vector<OpMidiMessage> g_collected;

void drain_into_collected() {
    OpMidiMessage buffer[128] {};
    while (true) {
        auto drained = op::sim::drain_outgoing_midi(kTestOutput, buffer, 128);
        if (drained == 0) break;
        for (uint16_t i = 0; i < drained; ++i) g_collected.push_back(buffer[i]);
    }
}

void run(uint32_t duration_us, float beats_per_second = 0.0f) {
    const auto* api       = op::sim::get_api();
    const uint32_t slices = duration_us / kSliceUs;
    for (uint32_t i = 0; i < slices; ++i) {
        op::sim::advance_tick(kSliceUs);
        if (beats_per_second > 0.0f) {
            const float beats = static_cast<float>(api->get_tick()) * beats_per_second / 1'000'000.0f;
            op::sim::set_beat_position(beats);
        }
        mode_process(nullptr, 0, api->get_tick());
        if ((i & 0xF) == 0xF) drain_into_collected();
    }
    drain_into_collected();
}

std::vector<OpMidiMessage> drain() {
    drain_into_collected();
    std::vector<OpMidiMessage> collected = std::move(g_collected);
    g_collected.clear();
    return collected;
}

// Disable every slot as a default setup. Tests that want traffic re-enable the
// slots they care about by setting Source to Free or Sync.
void disable_all(const OperatorApi* api) {
    for (uint8_t slot = 0; slot < kSlotCount; ++slot) {
        api->set_param_value(param_id(slot, kOffSource), kSourceOff);
    }
}

// Only CC messages (status high nibble 0xB0).
std::vector<OpMidiMessage> filter_cc(const std::vector<OpMidiMessage>& messages) {
    std::vector<OpMidiMessage> collected;
    for (const auto& message : messages) {
        if ((message.status & 0xF0) == 0xB0) collected.push_back(message);
    }
    return collected;
}

}  // namespace

// ---------------------------------------------------------------------------
// Waveform LUT unit checks, direct calls into waveforms.h with no mode drive.
// ---------------------------------------------------------------------------
// These pin the phase contract. phase_1024 is a 10-bit full-cycle index and the
// sine quadrant-mirror produces the documented shape (center=127, peak=254,
// trough=0).

TEST_CASE("sine LUT phase=0 -> 127") {
    CHECK(op::samples::cc_lfo::sine_u7(0) == 127);
}

TEST_CASE("sine LUT phase=256 -> 254") {
    CHECK(op::samples::cc_lfo::sine_u7(256) == 254);
}

// ---------------------------------------------------------------------------
// Mode-level behavior
// ---------------------------------------------------------------------------

TEST_CASE("slot disabled emits no CC") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    run(1'000'000);
    const auto cc_messages = filter_cc(drain());
    CHECK(cc_messages.size() == 0);

    mode_destroy();
}

TEST_CASE("saw waveform ramps 0..127 linearly") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    // Slot 0 is a saw on CC#42, low 0, high 127, rate 100 (1 Hz, Source=Free).
    const uint8_t slot = 0;
    api->set_param_value(param_id(slot, kOffCc), 42);
    api->set_param_value(param_id(slot, kOffShape), kShapeSaw);
    api->set_param_value(param_id(slot, kOffLow), 0);
    api->set_param_value(param_id(slot, kOffHigh), 127);
    api->set_param_value(param_id(slot, kOffRateHz), 100);  // 1 Hz
    api->set_param_value(param_id(slot, kOffSource), kSourceFree);

    // Drive just under one cycle so we don't observe the wrap.
    run(950'000);

    const auto cc_messages = filter_cc(drain());
    REQUIRE(cc_messages.size() >= 8);

    // Monotonic non-decreasing across a single cycle.
    uint8_t previous = 0;
    for (const auto& message : cc_messages) {
        CHECK(message.data1 == 42);
        CHECK(message.data2 >= previous);
        previous = message.data2;
    }
    // Covered a meaningful fraction of the 0..127 range.
    CHECK(cc_messages.back().data2 >= 100);

    mode_destroy();
}

TEST_CASE("sync on maps rate to subdivision") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    // Slot 0 is a saw on CC#7, full range, RateBeat "1/4" (one cycle per
    // beat), Source=Sync. At 120 BPM (2 beats/sec) the cycle is 500 ms.
    const uint8_t slot = 0;
    api->set_param_value(param_id(slot, kOffCc), 7);
    api->set_param_value(param_id(slot, kOffShape), kShapeSaw);
    api->set_param_value(param_id(slot, kOffLow), 0);
    api->set_param_value(param_id(slot, kOffHigh), 127);
    api->set_param_value(param_id(slot, kOffRateBeat), kRateBeatQuarter);
    api->set_param_value(param_id(slot, kOffSource), kSourceSync);

    // Drive 500 ms at 2 beats/sec, one full cycle.
    run(500'000, /*beats_per_second=*/2.0f);

    const auto cc_messages = filter_cc(drain());
    REQUIRE(cc_messages.size() >= 8);
    // Reached the top of the saw (close to 127) and started from 0.
    CHECK(cc_messages.front().data2 <= 16);
    // Peak observed near the end of the window.
    uint8_t peak = 0;
    for (const auto& message : cc_messages)
        if (message.data2 > peak) peak = message.data2;
    CHECK(peak >= 110);

    mode_destroy();
}

TEST_CASE("sync off uses Hz via get_tick") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    // Slot 0 is a saw on CC#7, rate=100 (1.00 Hz), Source=Free. One cycle per second
    // regardless of beat position (which we leave frozen at 0 to prove it).
    const uint8_t slot = 0;
    api->set_param_value(param_id(slot, kOffCc), 7);
    api->set_param_value(param_id(slot, kOffShape), kShapeSaw);
    api->set_param_value(param_id(slot, kOffLow), 0);
    api->set_param_value(param_id(slot, kOffHigh), 127);
    api->set_param_value(param_id(slot, kOffRateHz), 100);
    api->set_param_value(param_id(slot, kOffSource), kSourceFree);

    run(950'000, /*beats_per_second=*/0.0f);

    const auto cc_messages = filter_cc(drain());
    REQUIRE(cc_messages.size() >= 8);
    uint8_t peak = 0;
    for (const auto& message : cc_messages)
        if (message.data2 > peak) peak = message.data2;
    CHECK(peak >= 110);

    mode_destroy();
}

TEST_CASE("low/high clamp CC range") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    const uint8_t slot = 0;
    api->set_param_value(param_id(slot, kOffCc), 11);
    api->set_param_value(param_id(slot, kOffShape), kShapeSaw);
    api->set_param_value(param_id(slot, kOffLow), 30);
    api->set_param_value(param_id(slot, kOffHigh), 90);
    api->set_param_value(param_id(slot, kOffRateHz), 200);  // 2 Hz
    api->set_param_value(param_id(slot, kOffSource), kSourceFree);

    run(1'500'000);

    const auto cc_messages = filter_cc(drain());
    REQUIRE(cc_messages.size() >= 16);
    for (const auto& message : cc_messages) {
        CHECK(message.data1 == 11);
        CHECK(message.data2 >= 30);
        CHECK(message.data2 <= 90);
    }
    mode_destroy();
}

TEST_CASE("CC# routes correctly") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    const uint8_t slot = 0;
    api->set_param_value(param_id(slot, kOffCc), 74);
    api->set_param_value(param_id(slot, kOffShape), kShapeTriangle);
    api->set_param_value(param_id(slot, kOffLow), 0);
    api->set_param_value(param_id(slot, kOffHigh), 127);
    api->set_param_value(param_id(slot, kOffRateHz), 200);
    api->set_param_value(param_id(slot, kOffSource), kSourceFree);

    run(1'000'000);
    const auto cc_messages = filter_cc(drain());
    REQUIRE(cc_messages.size() >= 8);
    for (const auto& message : cc_messages) CHECK(message.data1 == 74);
    mode_destroy();
}

TEST_CASE("4 slots run independently") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    const uint8_t cc_numbers[kSlotCount] = {20, 21, 22, 23};
    const int32_t shapes[kSlotCount]     = {kShapeSine, kShapeTriangle, kShapeSaw, kShapeSquare};
    for (uint8_t slot = 0; slot < kSlotCount; ++slot) {
        api->set_param_value(param_id(slot, kOffCc), cc_numbers[slot]);
        api->set_param_value(param_id(slot, kOffShape), shapes[slot]);
        api->set_param_value(param_id(slot, kOffLow), 0);
        api->set_param_value(param_id(slot, kOffHigh), 127);
        api->set_param_value(param_id(slot, kOffRateHz), 100 + 50 * slot);
        api->set_param_value(param_id(slot, kOffSource), kSourceFree);
    }

    run(1'500'000);
    const auto cc_messages = filter_cc(drain());
    REQUIRE(cc_messages.size() >= 16);
    std::set<uint8_t> seen;
    for (const auto& message : cc_messages) seen.insert(message.data1);
    CHECK(seen.count(20) == 1);
    CHECK(seen.count(21) == 1);
    CHECK(seen.count(22) == 1);
    CHECK(seen.count(23) == 1);

    mode_destroy();
}

TEST_CASE("sample-and-hold changes value per step only") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    // S&H picks a new random value per cycle. Over many cycles we should
    // see distinct values (a constant emission means the mode isn't
    // actually re-rolling).
    const uint8_t slot = 0;
    api->set_param_value(param_id(slot, kOffCc), 60);
    api->set_param_value(param_id(slot, kOffShape), kShapeSampleHold);
    api->set_param_value(param_id(slot, kOffLow), 0);
    api->set_param_value(param_id(slot, kOffHigh), 127);
    api->set_param_value(param_id(slot, kOffRateHz), 400);  // 4 Hz -> 0.25 s/step
    api->set_param_value(param_id(slot, kOffSource), kSourceFree);

    run(3'000'000);

    const auto cc_messages = filter_cc(drain());
    REQUIRE(cc_messages.size() >= 4);
    std::set<uint8_t> values;
    for (const auto& message : cc_messages) values.insert(message.data2);
    // Over 12 cycles we expect more than one distinct sample to land.
    CHECK(values.size() >= 3);

    mode_destroy();
}

TEST_CASE("sync slow option 16 sweeps far slower than 1/4") {
    // Option 0 "16" is 64 beats/cycle (bpc_x24=1536) and option 12 "1/4" is
    // 1 beat/cycle. Over the same short window the "16" saw must cover a much
    // smaller value span than "1/4" (which completes a full cycle).

    // Quarter note, one full cycle over the 1-beat window, so it reaches near peak.
    op::sim::reset_state();
    {
        const auto* api = op::sim::get_api();
        mode_init(api);
        disable_all(api);
        const uint8_t slot = 0;
        api->set_param_value(param_id(slot, kOffCc), 7);
        api->set_param_value(param_id(slot, kOffShape), kShapeSaw);
        api->set_param_value(param_id(slot, kOffLow), 0);
        api->set_param_value(param_id(slot, kOffHigh), 127);
        api->set_param_value(param_id(slot, kOffRateBeat), kRateBeatQuarter);
        api->set_param_value(param_id(slot, kOffSource), kSourceSync);
        run(500'000, /*beats_per_second=*/2.0f);
        const auto cc_messages = filter_cc(drain());
        uint8_t peak_quarter   = 0;
        for (const auto& message : cc_messages)
            if (message.data2 > peak_quarter) peak_quarter = message.data2;
        mode_destroy();
        CHECK(peak_quarter >= 110);  // "1/4" completes its cycle
    }

    // Slow "16" covers 1/64 of a cycle over the same window, a tiny span that
    // never peaks.
    op::sim::reset_state();
    {
        const auto* api = op::sim::get_api();
        mode_init(api);
        disable_all(api);
        const uint8_t slot = 0;
        api->set_param_value(param_id(slot, kOffCc), 7);
        api->set_param_value(param_id(slot, kOffShape), kShapeSaw);
        api->set_param_value(param_id(slot, kOffLow), 0);
        api->set_param_value(param_id(slot, kOffHigh), 127);
        api->set_param_value(param_id(slot, kOffRateBeat), kRateBeatSlow16);
        api->set_param_value(param_id(slot, kOffSource), kSourceSync);
        run(500'000, /*beats_per_second=*/2.0f);
        const auto cc_messages = filter_cc(drain());
        uint8_t peak_slow      = 0;
        for (const auto& message : cc_messages)
            if (message.data2 > peak_slow) peak_slow = message.data2;
        mode_destroy();
        // 1 beat of a 64-beat cycle is ~1.5% of the ramp -> stays low.
        CHECK(peak_slow < 30);
    }
}

TEST_CASE("fair-share throttle splits the budget evenly across 2 LFOs at the same rate") {
    // Two equally fast LFOs must each get an equal share of the polite total.
    // CC18 and CC19 should emit roughly the same count over the window, and the
    // combined total must stay under the polite ceiling.
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    // Slots 0 and 1 are both 20 Hz saws (CC18, CC19). Equal load, so the fair-share
    // split should hand each about half the budget.
    api->set_param_value(param_id(0, kOffCc), 18);
    api->set_param_value(param_id(0, kOffShape), kShapeSaw);
    api->set_param_value(param_id(0, kOffLow), 0);
    api->set_param_value(param_id(0, kOffHigh), 127);
    api->set_param_value(param_id(0, kOffRateHz), 2000);  // 20 Hz
    api->set_param_value(param_id(0, kOffSource), kSourceFree);

    api->set_param_value(param_id(1, kOffCc), 19);
    api->set_param_value(param_id(1, kOffShape), kShapeSaw);
    api->set_param_value(param_id(1, kOffLow), 0);
    api->set_param_value(param_id(1, kOffHigh), 127);
    api->set_param_value(param_id(1, kOffRateHz), 2000);  // 20 Hz
    api->set_param_value(param_id(1, kOffSource), kSourceFree);

    const uint32_t window_us = 1'000'000;
    run(window_us);
    const auto cc_messages = filter_cc(drain());

    std::size_t cc18_count = 0, cc19_count = 0;
    for (const auto& message : cc_messages) {
        if (message.data1 == 18) ++cc18_count;
        if (message.data1 == 19) ++cc19_count;
    }
    // Both LFOs emit a substantial share, neither starved.
    CHECK(cc18_count > 0);
    CHECK(cc19_count > 0);
    // Even split. With 2 active slots each gets ~half the total, so the counts
    // stay within a factor of ~1.5 of each other (rounding and phase-alignment
    // slack).
    const std::size_t lower  = cc18_count < cc19_count ? cc18_count : cc19_count;
    const std::size_t higher = cc18_count < cc19_count ? cc19_count : cc18_count;
    CHECK(higher <= lower * 3 / 2 + 4);
    // Combined total stays under the polite ceiling (+ slack).
    const std::size_t bound = window_us / kEmitPeriodPerLfoUs + 16;
    CHECK(cc_messages.size() <= bound);

    mode_destroy();
}

TEST_CASE("fair-share throttle keeps a fast LFO from starving a slow sibling") {
    // One fast LFO alongside a slow one. The slow LFO (1 Hz) wants only a few
    // emits (dedup skips unchanged values) and the fast LFO (20 Hz) wants many,
    // but the fast one is capped at its fair half so it cannot consume the
    // whole budget, leaving headroom the slow LFO can use.
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    api->set_param_value(param_id(0, kOffCc), 50);
    api->set_param_value(param_id(0, kOffShape), kShapeSaw);
    api->set_param_value(param_id(0, kOffLow), 0);
    api->set_param_value(param_id(0, kOffHigh), 127);
    api->set_param_value(param_id(0, kOffRateHz), 2000);  // 20 Hz (fast)
    api->set_param_value(param_id(0, kOffSource), kSourceFree);

    api->set_param_value(param_id(1, kOffCc), 51);
    api->set_param_value(param_id(1, kOffShape), kShapeSaw);
    api->set_param_value(param_id(1, kOffLow), 0);
    api->set_param_value(param_id(1, kOffHigh), 127);
    api->set_param_value(param_id(1, kOffRateHz), 100);  // 1 Hz (slow)
    api->set_param_value(param_id(1, kOffSource), kSourceFree);

    const uint32_t window_us = 1'000'000;
    run(window_us);
    const auto cc_messages = filter_cc(drain());

    std::size_t fast_count = 0, slow_count = 0;
    for (const auto& message : cc_messages) {
        if (message.data1 == 50) ++fast_count;
        if (message.data1 == 51) ++slow_count;
    }
    // With 2 active slots, each slot's interval is 2 x period -> ~250/s per
    // slot.
    const std::size_t per_slot_cap = window_us / (2u * kEmitPeriodPerLfoUs);
    // The fast LFO is capped at ~its fair share.
    CHECK(fast_count <= per_slot_cap + 16);
    // The slow LFO emits its ~1 cycle/s of distinct saw values.
    CHECK(slow_count > 0);

    mode_destroy();
}

TEST_CASE("Source=Off emits no CC and Source=Free emits") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    // Slot 0 is configured but Source is left Off, so it must stay silent.
    const uint8_t slot = 0;
    api->set_param_value(param_id(slot, kOffCc), 33);
    api->set_param_value(param_id(slot, kOffShape), kShapeSaw);
    api->set_param_value(param_id(slot, kOffLow), 0);
    api->set_param_value(param_id(slot, kOffHigh), 127);
    api->set_param_value(param_id(slot, kOffRateHz), 200);
    api->set_param_value(param_id(slot, kOffSource), kSourceOff);

    run(1'000'000);
    CHECK(filter_cc(drain()).size() == 0);

    // Flip Source to Free, and it must emit.
    api->set_param_value(param_id(slot, kOffSource), kSourceFree);
    run(1'000'000);
    CHECK(filter_cc(drain()).size() > 0);

    mode_destroy();
}

// ---------------------------------------------------------------------------
// Pulse-derived behavior (the real process() path)
// ---------------------------------------------------------------------------
// These drive process() with a pulse feed (set_pulse_count), so the whole
// synced-phase path runs end to end. CC values are observed through the public
// drain.

namespace {

// Drive the mode over `pulse_steps` 24-PPQN pulses, advancing the us tick by
// `us_per_step` each pulse (>= the per-instance throttle interval so emissions
// are not all deferred). Returns every CC emitted, in order.
std::vector<OpMidiMessage> drive_pulses(const OperatorApi* api,
                                        uint32_t start_pulse,
                                        uint32_t pulse_steps,
                                        uint32_t us_per_step) {
    std::vector<OpMidiMessage> collected;
    OpMidiMessage buffer[64] {};
    for (uint32_t pulse = 0; pulse <= pulse_steps; ++pulse) {
        op::sim::advance_tick(us_per_step);
        op::sim::set_pulse_count(start_pulse + pulse);
        mode_process(nullptr, 0, api->get_tick());
        while (true) {
            auto drained = op::sim::drain_outgoing_midi(kTestOutput, buffer, 64);
            if (drained == 0) break;
            for (uint16_t i = 0; i < drained; ++i) collected.push_back(buffer[i]);
        }
    }
    return collected;
}

// Same pulse-feed as drive_pulses, but on the pulse step whose
// index equals `inject_at_step` it passes a one-message batch carrying the
// realtime transport `transport_status` byte (0xFA/0xFB/0xFC) into the
// production mode_process(), exactly how the device delivers a transport
// byte. The byte is a single-byte realtime message (length 1, no data). All
// other steps pass nullptr/0 like drive_pulses. Returns every CC emitted.
std::vector<OpMidiMessage> drive_pulses_with_transport(const OperatorApi* api,
                                                       uint32_t start_pulse,
                                                       uint32_t pulse_steps,
                                                       uint32_t us_per_step,
                                                       uint32_t inject_at_step,
                                                       uint8_t transport_status) {
    std::vector<OpMidiMessage> collected;
    OpMidiMessage buffer[64] {};
    for (uint32_t pulse = 0; pulse <= pulse_steps; ++pulse) {
        op::sim::advance_tick(us_per_step);
        op::sim::set_pulse_count(start_pulse + pulse);
        if (pulse == inject_at_step) {
            OpMidiMessage transport_message {};
            transport_message.status = transport_status;
            transport_message.length = 1;
            mode_process(&transport_message, 1, api->get_tick());
        } else {
            mode_process(nullptr, 0, api->get_tick());
        }
        while (true) {
            auto drained = op::sim::drain_outgoing_midi(kTestOutput, buffer, 64);
            if (drained == 0) break;
            for (uint16_t i = 0; i < drained; ++i) collected.push_back(buffer[i]);
        }
    }
    return collected;
}

// Drive the mode with sub-pulse time resolution. Each 24-PPQN
// pulse spans `pulse_period_us` of wall-clock, and we step the us tick in
// `substeps` equal slices across that period, calling process() at each slice
// (the pulse count only advances at the slice that crosses the pulse edge).
// This mimics the device calling process() many times between clock pulses,
// where an integer-only phase would look flat (stair-stepped) between pulses.
// Returns every CC emitted, in order.
//
// The clock interpolator needs 3 clean intervals before it reports a non-zero
// fraction, so the caller should warm it up (a few whole pulses) before
// asserting on sub-pulse motion.
std::vector<OpMidiMessage> drive_subpulse(const OperatorApi* api,
                                          uint32_t start_pulse,
                                          uint32_t pulse_steps,
                                          uint32_t pulse_period_us,
                                          uint32_t substeps) {
    std::vector<OpMidiMessage> collected;
    OpMidiMessage buffer[64] {};
    if (substeps == 0) substeps = 1;
    const uint32_t slice_us = pulse_period_us / substeps;
    for (uint32_t pulse = 0; pulse <= pulse_steps; ++pulse) {
        // Set the pulse count for this whole pulse interval, then sweep the
        // tick across it in `substeps` slices, processing at each.
        op::sim::set_pulse_count(start_pulse + pulse);
        for (uint32_t sub = 0; sub < substeps; ++sub) {
            op::sim::advance_tick(slice_us);
            mode_process(nullptr, 0, api->get_tick());
            while (true) {
                auto drained = op::sim::drain_outgoing_midi(kTestOutput, buffer, 64);
                if (drained == 0) break;
                for (uint16_t i = 0; i < drained; ++i) collected.push_back(buffer[i]);
            }
        }
    }
    return collected;
}

}  // namespace

TEST_CASE("sync interpolation moves phase between pulses (smooth not "
          "stair-stepped)") {
    // Drive a synced saw at "1/4" (bpc_x24=24, one cycle per
    // beat) with many process() calls between each 24-PPQN pulse. An integer-only
    // phase is flat between pulses, at most 24 distinct values per cycle.
    // With sub-pulse interpolation the phase sweeps continuously, so within a
    // single pulse interval we now see multiple distinct CC values, and across
    // a whole cycle far more than the 24-step integer ceiling.
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    const uint8_t slot = 0;
    api->set_param_value(param_id(slot, kOffCc), 7);
    api->set_param_value(param_id(slot, kOffShape), kShapeSaw);
    api->set_param_value(param_id(slot, kOffLow), 0);
    api->set_param_value(param_id(slot, kOffHigh), 127);
    api->set_param_value(param_id(slot, kOffRateBeat),
                         kRateBeatQuarter);  // "1/4" -> bpc=24
    api->set_param_value(param_id(slot, kOffSource), kSourceSync);

    // At 140 BPM one beat is ~428'571 us and one pulse (1/24 beat) is ~17'857 us.
    // Use 20 sub-steps per pulse (~893 us/slice, just under the ~1ms throttle so
    // most slices can emit). Warm up the interpolator (needs 3 clean intervals)
    // over a few pulses, then measure.
    const uint32_t pulse_us = 17'857;
    const uint32_t substeps = 20;

    // Warm up over 4 whole pulses so the median-of-3 period is established.
    drive_subpulse(api, 0, 4, pulse_us, substeps);

    // Measure one full beat (24 pulses) of sub-pulse motion.
    const auto cc_messages = filter_cc(drive_subpulse(api, 5, 24, pulse_us, substeps));
    REQUIRE(cc_messages.size() >= 24);

    // (a) Far more than the 24-step integer ceiling across the cycle.
    std::set<uint8_t> distinct;
    for (const auto& message : cc_messages) {
        CHECK(message.data1 == 7);
        distinct.insert(message.data2);
    }
    // A 7-bit saw over a full cycle resolves to ~128 levels, and with smoothing we
    // should comfortably exceed the 24-value integer ceiling.
    CHECK(distinct.size() > 40);

    // (b) Monotonic-ish climb of the saw over the cycle (continuous, not flat-
    // then-jump), so the run reaches both near the floor and near the peak.
    uint8_t lowest = 127, highest = 0;
    for (const auto& message : cc_messages) {
        if (message.data2 < lowest) lowest = message.data2;
        if (message.data2 > highest) highest = message.data2;
    }
    CHECK(lowest <= 16);    // started near the saw floor
    CHECK(highest >= 110);  // climbed to near the peak

    mode_destroy();
}

TEST_CASE("sync interpolation freezes phase when the clock stops with no "
          "overshoot") {
    // When the clock stops, the interpolator freezes frac at last-known (never
    // resets, never runs forward into the next pulse). Drive a synced saw, warm
    // up, then hold the pulse count steady while time keeps advancing far past
    // the stale threshold. The emitted value must not keep climbing unbounded
    // (no overshoot past the held pulse's cycle position).
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    const uint8_t slot = 0;
    api->set_param_value(param_id(slot, kOffCc), 7);
    api->set_param_value(param_id(slot, kOffShape), kShapeSaw);
    api->set_param_value(param_id(slot, kOffLow), 0);
    api->set_param_value(param_id(slot, kOffHigh), 127);
    api->set_param_value(param_id(slot, kOffRateBeat),
                         kRateBeatQuarter);  // "1/4" -> bpc=24
    api->set_param_value(param_id(slot, kOffSource), kSourceSync);

    const uint32_t pulse_us = 17'857;
    const uint32_t substeps = 8;

    // Warm up + advance to a known mid-cycle pulse.
    drive_subpulse(api, 0, 6, pulse_us, substeps);

    // Now freeze the pulse count (clock stopped) but keep time marching for
    // many pulse-periods (well past the ~4x stale threshold). pulse stays at 7.
    op::sim::set_pulse_count(7u);
    uint8_t last = 0;
    OpMidiMessage buffer[64] {};
    for (int i = 0; i < 200; ++i) {
        op::sim::advance_tick(pulse_us);  // ~1 pulse-period per step, 200 total
        mode_process(nullptr, 0, api->get_tick());
        while (true) {
            auto drained = op::sim::drain_outgoing_midi(kTestOutput, buffer, 64);
            if (drained == 0) break;
            for (uint16_t k = 0; k < drained; ++k) last = buffer[k].data2;
        }
    }
    // The frozen-clock value must stay within the same pulse's cycle slice. A
    // "1/4" pulse is 1/24 of the cycle ~= 5 CC of saw. Even with the overdue
    // forward-clamp (frac -> just under 1), the value cannot exceed the next
    // pulse's floor. Held at pulse 7 of 24 -> phase ~7/24 cycle -> saw ~37,
    // plus at most ~one pulse-step of clamp -> well under, say, 60. It must not
    // have run away toward 127.
    CHECK(last < 60);

    mode_destroy();
}

TEST_CASE("sync multi-measure (2=192) sweeps the full sine across two bars") {
    // bpc_x24=192 spans 8 beats (2 measures). Because the phase comes from the
    // continuous pulse count (not a measure-relative position that would reset
    // every bar), one full cycle sweeps the whole sine, reaching both near its
    // trough (low) and near its peak (high), a complete shape and not a half.
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    const uint8_t slot = 0;
    api->set_param_value(param_id(slot, kOffCc), 7);
    api->set_param_value(param_id(slot, kOffShape), kShapeSine);
    api->set_param_value(param_id(slot, kOffLow), 0);
    api->set_param_value(param_id(slot, kOffHigh), 127);
    api->set_param_value(param_id(slot, kOffRateBeat), 6);  // "2" -> bpc_x24=192
    api->set_param_value(param_id(slot, kOffSource), kSourceSync);

    // One full 192-pulse cycle, ~2ms per pulse so the throttle lets values
    // pass.
    const auto cc_messages = filter_cc(drive_pulses(api, 0, 192, 2000));
    REQUIRE(cc_messages.size() >= 8);

    uint8_t lowest = 127, highest = 0;
    for (const auto& message : cc_messages) {
        CHECK(message.data1 == 7);
        if (message.data2 < lowest) lowest = message.data2;
        if (message.data2 > highest) highest = message.data2;
    }
    // Full sine over the cycle, dipping low and climbing high (not a clipped half).
    CHECK(lowest <= 20);
    CHECK(highest >= 107);

    mode_destroy();
}

TEST_CASE("sync rate change stays continuous (no value jump)") {
    // Change bpc_x24 live and confirm the emitted CC does not
    // jump. We sweep a saw on "2" (192) for a while, switch to "1" (96) at a
    // fixed pulse, and check the value delta straddling the change is small.
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    const uint8_t slot = 0;
    api->set_param_value(param_id(slot, kOffCc), 7);
    api->set_param_value(param_id(slot, kOffShape), kShapeSaw);
    api->set_param_value(param_id(slot, kOffLow), 0);
    api->set_param_value(param_id(slot, kOffHigh), 127);
    api->set_param_value(param_id(slot, kOffRateBeat), 6);  // "2" -> 192
    api->set_param_value(param_id(slot, kOffSource), kSourceSync);

    // Run up to pulse 70 on "2", capture the last value before the change.
    auto before = filter_cc(drive_pulses(api, 0, 70, 2000));
    REQUIRE(!before.empty());
    const uint8_t last_before = before.back().data2;

    // Change rate to "1" (bpc_x24=96) at the same instant, and re-anchor should
    // keep the next emitted value close to last_before, then continue smoothly.
    api->set_param_value(param_id(slot, kOffRateBeat), 8);  // "1" -> 96
    auto after = filter_cc(drive_pulses(api, 71, 6, 2000));
    REQUIRE(!after.empty());
    const uint8_t first_after = after.front().data2;

    // Continuity check. The step across the rate change is small, rounding only.
    const int delta = std::abs(first_after - last_before);
    CHECK(delta <= 12);

    mode_destroy();
}

TEST_CASE("free-run rate change stays continuous (no value jump)") {
    // Same continuity guarantee on the Free (Hz) path. Changing Rate Hz live
    // re-anchors so the emitted value does not jump.
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    const uint8_t slot = 0;
    api->set_param_value(param_id(slot, kOffCc), 7);
    api->set_param_value(param_id(slot, kOffShape), kShapeSaw);
    api->set_param_value(param_id(slot, kOffLow), 0);
    api->set_param_value(param_id(slot, kOffHigh), 127);
    api->set_param_value(param_id(slot, kOffRateHz), 100);  // 1 Hz
    api->set_param_value(param_id(slot, kOffSource), kSourceFree);

    // Free path ignores pulses, but drive_pulses advances the us tick anyway,
    // which is what the Hz phase uses. Run ~140ms at 1 Hz.
    auto before = filter_cc(drive_pulses(api, 0, 70, 2000));
    REQUIRE(!before.empty());
    const uint8_t last_before = before.back().data2;

    // Jump to 20 Hz at the same instant.
    api->set_param_value(param_id(slot, kOffRateHz), 2000);  // 20 Hz
    auto after = filter_cc(drive_pulses(api, 71, 2, 2000));
    REQUIRE(!after.empty());
    const uint8_t first_after = after.front().data2;

    const int delta = std::abs(first_after - last_before);
    CHECK(delta <= 12);

    mode_destroy();
}

// ---------------------------------------------------------------------------
// Transport re-anchoring
// ---------------------------------------------------------------------------
// The synced phase is measured relative to a per-instance pulse origin that
// MIDI Start (0xFA) and Stop (0xFC) re-snapshot, so a multi-measure cycle
// re-anchors to the song's downbeat. Continue (0xFB) is ignored, and free-run
// (Hz) is wall-clock so it is unaffected. These drive the production
// mode_process() with injected realtime transport bytes.

TEST_CASE("sync Start (0xFA) re-anchors phase to 0 at the downbeat") {
    // Saw on "2" (bpc_x24=192). Climb to mid-cycle, inject Start, and confirm
    // the very next emitted value drops back near 0 (the shape's start), then
    // climbs again from there, and the cycle re-anchors to "now".
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    const uint8_t slot = 0;
    api->set_param_value(param_id(slot, kOffCc), 7);
    api->set_param_value(param_id(slot, kOffShape), kShapeSaw);
    api->set_param_value(param_id(slot, kOffLow), 0);
    api->set_param_value(param_id(slot, kOffHigh), 127);
    api->set_param_value(param_id(slot, kOffRateBeat), 6);  // "2" -> bpc_x24=192
    api->set_param_value(param_id(slot, kOffSource), kSourceSync);

    // Climb to ~mid-cycle (pulses 0..70 of a 192-pulse cycle -> phase ~0.36
    // cyc, saw value ~46). Capture the last value before the reset.
    auto before = filter_cc(drive_pulses(api, 0, 70, 2000));
    REQUIRE(!before.empty());
    const uint8_t last_before = before.back().data2;
    CHECK(last_before >= 20);  // genuinely mid-climb, not near 0

    // Inject Start (0xFA) on the first step of this segment (pulse 71), then
    // keep feeding pulses. After the re-anchor the synced phase restarts at 0,
    // so the first emitted value must be near the saw's bottom, not continuing
    // the climb.
    auto after = filter_cc(drive_pulses_with_transport(api,
                                                       /*start_pulse=*/71,
                                                       /*pulse_steps=*/30,
                                                       /*us_per_step=*/2000,
                                                       /*inject_at_step=*/0,
                                                       /*transport_status=*/0xFA));
    REQUIRE(!after.empty());
    CHECK(after.front().data2 <= 16);  // re-anchored to the downbeat (~0)
    // And it climbs again from there (the saw rises past the floor).
    CHECK(after.back().data2 > after.front().data2);

    mode_destroy();
}

TEST_CASE("sync Stop (0xFC) also re-anchors phase to 0") {
    // Stop re-anchors the grid identically to Start (the MIDI standard has no
    // true Pause). Same shape as the Start case above.
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    const uint8_t slot = 0;
    api->set_param_value(param_id(slot, kOffCc), 7);
    api->set_param_value(param_id(slot, kOffShape), kShapeSaw);
    api->set_param_value(param_id(slot, kOffLow), 0);
    api->set_param_value(param_id(slot, kOffHigh), 127);
    api->set_param_value(param_id(slot, kOffRateBeat), 6);  // "2" -> bpc_x24=192
    api->set_param_value(param_id(slot, kOffSource), kSourceSync);

    auto before = filter_cc(drive_pulses(api, 0, 70, 2000));
    REQUIRE(!before.empty());
    CHECK(before.back().data2 >= 20);

    auto after = filter_cc(drive_pulses_with_transport(api,
                                                       /*start_pulse=*/71,
                                                       /*pulse_steps=*/30,
                                                       /*us_per_step=*/2000,
                                                       /*inject_at_step=*/0,
                                                       /*transport_status=*/0xFC));
    REQUIRE(!after.empty());
    CHECK(after.front().data2 <= 16);
    CHECK(after.back().data2 > after.front().data2);

    mode_destroy();
}

TEST_CASE("sync Continue (0xFB) does not reset phase") {
    // Continue is deliberately ignored, so the saw keeps climbing across the
    // injected 0xFB and does not drop back to 0.
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    const uint8_t slot = 0;
    api->set_param_value(param_id(slot, kOffCc), 7);
    api->set_param_value(param_id(slot, kOffShape), kShapeSaw);
    api->set_param_value(param_id(slot, kOffLow), 0);
    api->set_param_value(param_id(slot, kOffHigh), 127);
    api->set_param_value(param_id(slot, kOffRateBeat), 6);  // "2" -> bpc_x24=192
    api->set_param_value(param_id(slot, kOffSource), kSourceSync);

    auto before = filter_cc(drive_pulses(api, 0, 70, 2000));
    REQUIRE(!before.empty());
    const uint8_t last_before = before.back().data2;
    CHECK(last_before >= 20);

    // Inject Continue (0xFB) and keep feeding pulses. Phase is unaffected, so
    // the first value after must continue the climb (>= where it was), never
    // drop to 0.
    auto after = filter_cc(drive_pulses_with_transport(api,
                                                       /*start_pulse=*/71,
                                                       /*pulse_steps=*/20,
                                                       /*us_per_step=*/2000,
                                                       /*inject_at_step=*/0,
                                                       /*transport_status=*/0xFB));
    REQUIRE(!after.empty());
    // Not re-anchored, so the value did not collapse to the saw floor.
    CHECK(after.front().data2 >= last_before - 6);

    mode_destroy();
}

TEST_CASE("free-run (Hz) phase is not affected by transport") {
    // Source=Free uses the wall-clock tick, not the pulse grid, so an injected
    // Start must not perturb its value, so the saw keeps ramping continuously.
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    disable_all(api);

    const uint8_t slot = 0;
    api->set_param_value(param_id(slot, kOffCc), 7);
    api->set_param_value(param_id(slot, kOffShape), kShapeSaw);
    api->set_param_value(param_id(slot, kOffLow), 0);
    api->set_param_value(param_id(slot, kOffHigh), 127);
    api->set_param_value(param_id(slot, kOffRateHz), 100);  // 1 Hz
    api->set_param_value(param_id(slot, kOffSource), kSourceFree);

    // ~140ms at 1 Hz climbs the saw partway. Capture the last value.
    auto before = filter_cc(drive_pulses(api, 0, 70, 2000));
    REQUIRE(!before.empty());
    const uint8_t last_before = before.back().data2;

    // Inject Start, and the Free path ignores it. The value must continue smoothly
    // (no collapse to 0, no jump), and the step across the injected byte is small.
    auto after = filter_cc(drive_pulses_with_transport(api,
                                                       /*start_pulse=*/71,
                                                       /*pulse_steps=*/4,
                                                       /*us_per_step=*/2000,
                                                       /*inject_at_step=*/0,
                                                       /*transport_status=*/0xFA));
    REQUIRE(!after.empty());
    const int delta = std::abs(after.front().data2 - last_before);
    CHECK(delta <= 12);

    mode_destroy();
}
