// Phase-math unit tests for the cc-lfo sample.
//
// The phase helpers (phase_sync_off, phase_sync_on_interp, reanchor_offset,
// ratebeat_to_bpc24) live at file scope in main.cpp, outside
// the anonymous namespace, so this suite can forward-declare and call them
// directly with exact input/output pairs. That is the pattern for unit-testing
// a mode's pure integer math without driving the whole mode.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cstdint>
#include <set>  // distinct-phase counting

// Forward declarations of the file-scope helpers in main.cpp, linked via the
// cc_lfo_host static library.
uint16_t phase_sync_off(uint32_t tick_us, int32_t rate);
uint16_t phase_sync_on_interp(uint32_t pulse_now, float frac, int32_t bpc_x24);
uint16_t reanchor_offset(uint16_t base_old, uint16_t offset_old, uint16_t base_new);
int32_t ratebeat_to_bpc24(int32_t option_index);

// ---------------------------------------------------------------------------
// phase_sync_off, the free-running (Rate Hz) phase, 32-bit integer only
// ---------------------------------------------------------------------------

TEST_CASE("phase_sync_off is a linear ramp at 1 Hz") {
    // At rate=100 (1 Hz), phase = tick_us * 100 / 100'000 = tick_us / 1000.
    CHECK(phase_sync_off(500'000u, 100) == 500u);
}

TEST_CASE("phase_sync_off at 1 Hz and 1 second is 1000 mod 1024") {
    // 1'000'000 / 1000 = 1000, short of a full 1024 wrap.
    CHECK(phase_sync_off(1'000'000u, 100) == 1000u);
}

TEST_CASE("phase_sync_off wraps twice at 2 Hz and lands back on 0") {
    // At rate=200 (2 Hz), phase = tick_us / 500. At 1'024'000 us the raw phase is
    // 2048, which wraps (mod 1024) to 0.
    CHECK(phase_sync_off(1'024'000u, 200) == 0u);
}

// ---------------------------------------------------------------------------
// phase_sync_on_interp, the beat-synced phase with sub-pulse interpolation
// ---------------------------------------------------------------------------
// bpc_x24 == pulses-per-cycle, so at each pulse edge the phase is
// ((pulse mod bpc) * 1024) / bpc. A fraction in [0, 1) toward the next pulse
// blends the sub-pulse position so the phase sweeps continuously while staying
// clock-locked at every edge.

TEST_CASE("phase_sync_on_interp with frac=0 gives the exact integer phase at each edge") {
    // At a pulse edge the phase is ((pulse mod bpc) * 1024) / bpc. A cycle longer
    // than a measure (bpc_x24=192) does not reset at the bar line, so it climbs
    // past it and wraps only at the true cycle length.
    CHECK(phase_sync_on_interp(48u, 0.0f, 96) == 512u);    // one measure, half cycle
    CHECK(phase_sync_on_interp(96u, 0.0f, 96) == 0u);      // one measure, full wrap
    CHECK(phase_sync_on_interp(12u, 0.0f, 24) == 512u);    // one beat, half cycle
    CHECK(phase_sync_on_interp(96u, 0.0f, 192) == 512u);   // bar line, only the midpoint
    CHECK(phase_sync_on_interp(144u, 0.0f, 192) == 768u);  // climbing past the bar
    CHECK(phase_sync_on_interp(192u, 0.0f, 192) == 0u);    // true cycle end
    CHECK(phase_sync_on_interp(1536u, 0.0f, 1536) == 0u);  // 16-measure true wrap
}

TEST_CASE("phase_sync_on_interp lands a mid-pulse fraction between adjacent edges") {
    // At bpc_x24=24, each pulse is ~42.7 phase units. Frac 0.5 at pulse 0 sits
    // about half a step in, strictly between the edges at pulse 0 and pulse 1.
    const uint16_t phase_at_edge = phase_sync_on_interp(0u, 0.0f, 24);
    const uint16_t phase_at_half = phase_sync_on_interp(0u, 0.5f, 24);
    const uint16_t phase_at_next = phase_sync_on_interp(1u, 0.0f, 24);
    CHECK(phase_at_edge < phase_at_half);
    CHECK(phase_at_half < phase_at_next);
}

TEST_CASE("phase_sync_on_interp sub-pulse steps resolve far more phases than the integer path") {
    // A bpc_x24=24 cycle has at most 24 distinct integer phases. Sweeping frac
    // across [0, 1) at each pulse must resolve hundreds, fine enough for a
    // smooth 7-bit CC.
    std::set<uint16_t> phases;
    for (uint32_t pulse = 0; pulse < 24u; ++pulse) {
        for (int step = 0; step < 16; ++step) {
            const float frac = static_cast<float>(step) / 16.0f;
            phases.insert(phase_sync_on_interp(pulse, frac, 24));
        }
    }
    CHECK(phases.size() > 100);
}

TEST_CASE("phase_sync_on_interp never overshoots the next edge with its sub-pulse sweep") {
    // The frac contribution may round up to equal the next edge but must never
    // exceed it, so a rate error can only ever show as a local speed change,
    // never a jump into the next pulse.
    for (uint32_t pulse = 0; pulse < 23u; ++pulse) {  // bpc=24, stop before wrap
        const uint16_t next_edge = phase_sync_on_interp(pulse + 1u, 0.0f, 24);
        const uint16_t near_top  = phase_sync_on_interp(pulse, 0.99999f, 24);
        CHECK(near_top <= next_edge);
        CHECK(near_top >= phase_sync_on_interp(pulse, 0.0f, 24));
    }
}

// ---------------------------------------------------------------------------
// reanchor_offset, phase continuity across a rate change
// ---------------------------------------------------------------------------

TEST_CASE("reanchor_offset makes no shift when the base is unchanged") {
    CHECK(reanchor_offset(300u, 0u, 300u) == 0u);
}

TEST_CASE("reanchor_offset keeps the emitted phase unchanged at the change instant") {
    // After re-anchoring, (base_new + offset_new) mod 1024 must equal the old
    // emitted phase (base_old + offset_old) mod 1024 exactly, so the CC value
    // does not jump when the rate is edited live.
    struct Scenario {
        uint16_t base_old, offset_old, base_new;
    };
    const Scenario scenarios[] = {
        {200u,  0u,   900u }, // base jumps forward
        {900u,  0u,   100u }, // base jumps backward (wrap)
        {500u,  300u, 50u  }, // pre-existing offset carried through
        {0u,    0u,   1023u},
        {1023u, 1u,   0u   },
        {700u,  800u, 123u },
    };
    for (const auto& scenario : scenarios) {
        const uint16_t new_offset  = reanchor_offset(scenario.base_old, scenario.offset_old,
                                                     scenario.base_new);
        const uint16_t emitted_old = (scenario.base_old + scenario.offset_old) & 1023u;
        const uint16_t emitted_new = (scenario.base_new + new_offset) & 1023u;
        CHECK(emitted_new == emitted_old);
        CHECK(new_offset < 1024u);
    }
}

// ---------------------------------------------------------------------------
// ratebeat_to_bpc24, RateBeat option index -> pulses-per-cycle (x24 beats)
// ---------------------------------------------------------------------------
// The 20-option list runs slow to fast, from 0 ("16") through 12 ("1/4") to
// 19 ("1/32T"). Whole numbers are measures (4 beats), fractions are note values,
// "T" is a triplet, and one cycle spans bpc_x24 / 24 beats.

TEST_CASE("ratebeat_to_bpc24 maps the endpoints and the 1/4 reference correctly") {
    CHECK(ratebeat_to_bpc24(0) == 1536);  // "16", 64 beats/cycle, slowest
    CHECK(ratebeat_to_bpc24(12) == 24);   // "1/4", one beat/cycle
    CHECK(ratebeat_to_bpc24(19) == 2);    // "1/32T", fastest
}

TEST_CASE("ratebeat_to_bpc24 lists options strictly slow to fast") {
    int32_t previous = ratebeat_to_bpc24(0);
    for (int32_t i = 1; i < 20; ++i) {
        const int32_t bpc = ratebeat_to_bpc24(i);
        CHECK(bpc < previous);
        previous = bpc;
    }
}
