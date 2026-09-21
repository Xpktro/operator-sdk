// The clock interpolation math, doctest behavior suite.
//
// Each case drives op::sdk::detail::interpolate_step over synthetic pulse and tick
// streams. The function is pure, so a case needs only a default InterpState and the
// three scalars it reads, with no mock and no call to ClockInterpolator::now().
//
// This is a host test, so it is free of the freestanding rule and doctest's use of
// double is fine.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <operator_sdk/timing.h>
#include <operator_sdk/abi/mode_api.h>  // kClockStateActive / NoClock / Mode

#include <cstdint>

using op::sdk::ClockInterpolator;
using Status   = ClockInterpolator::Status;
using Position = ClockInterpolator::Position;
using op::sdk::detail::interpolate_step;
using op::sdk::detail::InterpState;

namespace {

// The 24 PPQ pulse counter wraps at this value, the largest multiple of 7200
// below 2^32. It is not an exported constant, so the test names it.
constexpr uint32_t kPulseWrap = 4'294'965'600u;

// ~120 BPM at 24 PPQ: one pulse every 60e6 / (120 * 24) = 20833 us. A round
// 20000 us runs throughout for arithmetically clean intermediate fractions.
constexpr uint32_t kInterval = 20000u;

// Feed `n` pulses into `st` at a fixed interval, starting at `start_pulse` /
// `start_us`. The pulse k lands at tick_us = start_us + k*interval_us. Mutates
// `st` in place and returns the last Position produced.
Position feed_steady(InterpState& st, uint32_t start_pulse, uint32_t start_us, uint32_t interval_us, int n) {
    Position latest {};
    for (int i = 0; i < n; ++i) {
        latest = interpolate_step(st, start_pulse + static_cast<uint32_t>(i),
                                  start_us + static_cast<uint32_t>(i) * interval_us, kClockStateActive);
    }
    return latest;
}

}  // namespace

// ---------------------------------------------------------------------------
TEST_CASE("steady tempo: frac sweeps monotonically between pulse edges") {
    InterpState st {};
    // 4 pulses at a fixed 20000 us interval -> interval_count saturates at 3,
    // last edge is pulse 103 at tick 60000 us.
    feed_steady(st, /*start_pulse=*/100, /*start_us=*/0, kInterval, /*n=*/4);

    const uint32_t edge_pulse = 103;
    const uint32_t edge_us    = 60000;

    // Sample three intermediate ticks past the last edge: +5000, +10000,
    // +15000 us -> fractions ~0.25, ~0.50, ~0.75.
    const Position q1 = interpolate_step(st, edge_pulse, edge_us + 5000, kClockStateActive);
    const Position q2 = interpolate_step(st, edge_pulse, edge_us + 10000, kClockStateActive);
    const Position q3 = interpolate_step(st, edge_pulse, edge_us + 15000, kClockStateActive);

    CHECK(q1.status == Status::Live);
    CHECK(q2.status == Status::Live);
    CHECK(q3.status == Status::Live);

    // pulse stays pinned to the last real edge between pulses.
    CHECK(q1.pulse == edge_pulse);
    CHECK(q2.pulse == edge_pulse);
    CHECK(q3.pulse == edge_pulse);

    // frac tracks elapsed/period.
    CHECK(q1.frac == doctest::Approx(0.25f).epsilon(0.05f));
    CHECK(q2.frac == doctest::Approx(0.50f).epsilon(0.05f));
    CHECK(q3.frac == doctest::Approx(0.75f).epsilon(0.05f));

    // Monotonic non-decreasing within the same pulse.
    CHECK(q2.frac >= q1.frac);
    CHECK(q3.frac >= q2.frac);
}

// ---------------------------------------------------------------------------
TEST_CASE("pulse period: last_period publishes the median inter-pulse interval") {
    InterpState st {};

    // Cold start, no period until three single-pulse intervals are seen.
    interpolate_step(st, 100, 0, kClockStateActive);
    interpolate_step(st, 101, kInterval, kClockStateActive);
    interpolate_step(st, 102, 2 * kInterval, kClockStateActive);
    CHECK(st.last_period == 0u);

    // Third interval completes the 3-slot ring -> median = 20000 us. This is
    // what ClockInterpolator::pulse_period_us() returns, and x24 gives the
    // microseconds per quarter note.
    interpolate_step(st, 103, 3 * kInterval, kClockStateActive);
    CHECK(st.last_period == kInterval);

    // Halve the tempo: three fresh 10000 us intervals -> the median tracks
    // down to the new period.
    uint32_t t = 3 * kInterval;
    for (uint32_t i = 0; i < 3; ++i) {
        t += 10000u;
        interpolate_step(st, 104 + i, t, kClockStateActive);
    }
    CHECK(st.last_period == 10000u);
}

// ---------------------------------------------------------------------------
TEST_CASE("cold start: first pulses return frac 0 with no divide-by-zero") {
    InterpState st {};

    // Pulse 1 (first-ever): anchor, frac == 0 exactly.
    const Position p0 = interpolate_step(st, 50, 0, kClockStateActive);
    CHECK(p0.pulse == 50);
    CHECK(p0.frac == 0.0f);
    CHECK(p0.status == Status::Live);

    // Pulses 2 and 3: interval_count is below 3 -> graceful integer fallback,
    // frac stays exactly 0, no NaN, no divide-by-zero.
    const Position p1 = interpolate_step(st, 51, kInterval, kClockStateActive);
    CHECK(p1.frac == 0.0f);
    CHECK(p1.status == Status::Live);

    const Position p2 = interpolate_step(st, 52, 2 * kInterval, kClockStateActive);
    CHECK(p2.frac == 0.0f);
    CHECK(p2.status == Status::Live);

    // A mid-pulse sample before a period exists also returns frac 0 (no NaN).
    const Position mid = interpolate_step(st, 52, 2 * kInterval + 5000, kClockStateActive);
    CHECK(mid.frac == 0.0f);
    CHECK(mid.frac == mid.frac);  // not NaN
    CHECK(mid.status == Status::Live);
}

// ---------------------------------------------------------------------------
TEST_CASE("tempo change: median-of-3 period adapts within two pulses") {
    InterpState st {};
    // Establish a steady 20000 us period (4 pulses -> intervals {20k,20k,20k}).
    feed_steady(st, 100, 0, kInterval, 4);

    // Instant tempo doubling: switch to a 10000 us interval. The last steady
    // edge is pulse 103 @ 60000 us.
    uint32_t pulse = 103;
    uint32_t t     = 60000;

    // First fast pulse -> intervals ring becomes {20k, 20k, 10k}, median 20k.
    pulse += 1;
    t += 10000;  // pulse 104 @ 70000
    interpolate_step(st, pulse, t, kClockStateActive);

    // Second fast pulse -> intervals {20k, 10k, 10k}, median 10000, fully
    // adapted to the new tempo.
    pulse += 1;
    t += 10000;  // pulse 105 @ 80000
    interpolate_step(st, pulse, t, kClockStateActive);

    // A +5000 us mid-pulse sample reads ~0.5 against the adapted 10000 us period.
    const Position q = interpolate_step(st, pulse, t + 5000, kClockStateActive);
    CHECK(q.status == Status::Live);
    CHECK(q.frac == doctest::Approx(0.5f).epsilon(0.05f));
}

// ---------------------------------------------------------------------------
TEST_CASE("clock stop/restart: HeldStale freeze then fresh period re-derivation") {
    InterpState st {};
    // Steady 20000 us stream, last edge pulse 103 @ 60000 us.
    feed_steady(st, 100, 0, kInterval, 4);

    // Stop feeding pulses but keep advancing tick_us. The period is 20000 us, so
    // staleness triggers past 4x period = 80000 us elapsed.
    const Position stale1 = interpolate_step(st, 103, 60000 + 90000, kClockStateActive);  // 90000 > 80000
    CHECK(stale1.status == Status::HeldStale);

    // A further stale call returns the identical frozen Position.
    const Position stale2 = interpolate_step(st, 103, 60000 + 120000, kClockStateActive);
    CHECK(stale2.status == Status::HeldStale);
    CHECK(stale2.pulse == stale1.pulse);
    CHECK(stale2.frac == stale1.frac);

    // Resume with a fresh fast stream. The first post-resume interval is the
    // multi-second gap and must not be admitted as a period sample. A brand-new
    // steady 10000 us stream follows, and after 3 fresh intervals the derived
    // period is 10000 us.
    uint32_t pulse = 104;
    uint32_t t     = 60000 + 120000 + 4'000'000;  // multi-second gap before resume
    for (int i = 0; i < 4; ++i) {
        interpolate_step(st, pulse + static_cast<uint32_t>(i), t + static_cast<uint32_t>(i) * 10000u,
                         kClockStateActive);
    }
    const uint32_t last_edge = pulse + 3;  // pulse 107
    const uint32_t last_us   = t + 3 * 10000u;

    // A +5000 us mid-pulse sample reads ~0.5 against the new 10000 us period.
    const Position q = interpolate_step(st, last_edge, last_us + 5000, kClockStateActive);
    CHECK(q.status == Status::Live);
    CHECK(q.frac == doctest::Approx(0.5f).epsilon(0.05f));
}

// ---------------------------------------------------------------------------
TEST_CASE("idle resume: NoClock and Mode both drop the stale ring on resume") {
    // Both pulse-idle states reset the interval ring on the return to Active, so
    // the first resume pulse reads cold-start frac 0. Sample there, because the
    // median-of-3 absorbs the lone stale interval within two more edges and the
    // reset stops being observable.
    for (const uint8_t idle_state : {kClockStateNoClock, kClockStateMode}) {
        InterpState st {};
        // Steady 20000 us stream, interval ring saturates to {20k,20k,20k}.
        feed_steady(st, 100, 0, kInterval, 4);

        // Idle for several seconds. The sample sets last_status = idle_state and
        // freezes the position.
        const Position idle = interpolate_step(st, 103, 60000 + 5'000'000, idle_state);
        CHECK(idle.status == Status::NoClock);

        const uint32_t t = 60000 + 8'000'000;

        // First resume edge, then a mid-pulse sample. The reset leaves the
        // interpolator in cold start, so the fraction reads 0.
        interpolate_step(st, 104, t, kClockStateActive);
        const Position after_one = interpolate_step(st, 104, t + 5000, kClockStateActive);
        CHECK(after_one.frac == doctest::Approx(0.0f));

        // Three fresh 10000 us intervals re-derive the new period, so a +5000 us
        // mid-pulse sample then reads ~0.5.
        for (uint32_t i = 1; i < 4; ++i) {
            interpolate_step(st, 104 + i, t + i * 10000u, kClockStateActive);
        }
        const Position q = interpolate_step(st, 107, t + 3 * 10000u + 5000, kClockStateActive);
        CHECK(q.status == Status::Live);
        CHECK(q.frac == doctest::Approx(0.5f).epsilon(0.05f));
    }
}

// ---------------------------------------------------------------------------
TEST_CASE("no-clock freeze: NoClock and Mode states hold the last-known position") {
    InterpState st {};
    // Establish a position, then sample a mid-pulse frac so last_frac is non-zero.
    feed_steady(st, 200, 0, kInterval, 4);
    const Position live = interpolate_step(st, 203, 60000 + 8000, kClockStateActive);
    REQUIRE(live.status == Status::Live);
    REQUIRE(live.frac > 0.0f);

    // clock_state == NoClock -> freeze at last-known {pulse, frac}, not {0,0}.
    const Position no_clock = interpolate_step(st, 999, 123456, kClockStateNoClock);
    CHECK(no_clock.status == Status::NoClock);
    CHECK(no_clock.pulse == live.pulse);
    CHECK(no_clock.frac == live.frac);
    CHECK(no_clock.pulse != 0u);  // explicitly not reset to zero

    // clock_state == Mode -> same freeze behavior, status NoClock.
    const Position mode = interpolate_step(st, 888, 222222, kClockStateMode);
    CHECK(mode.status == Status::NoClock);
    CHECK(mode.pulse == live.pulse);
    CHECK(mode.frac == live.frac);
}

// ---------------------------------------------------------------------------
TEST_CASE("forward clamp: frac stays strictly below 1.0 and never snaps back") {
    InterpState st {};
    // Steady 20000 us stream, last edge pulse 103 @ 60000 us.
    feed_steady(st, 100, 0, kInterval, 4);

    // Sample at 3x period elapsed (60000 us), still below the 4x stale
    // threshold (80000 us). frac is clamped into the kFracCap region.
    const Position overdue = interpolate_step(st, 103, 60000 + 60000, kClockStateActive);
    CHECK(overdue.status == Status::HeldOverdue);
    CHECK(overdue.frac < 1.0f);
    CHECK(overdue.frac > 0.99f);

    // A subsequent earlier-elapsed sample within the same pulse must not make
    // frac decrease (monotonic non-decreasing guard).
    const Position back = interpolate_step(st, 103, 60000 + 10000, kClockStateActive);
    CHECK(back.frac >= overdue.frac);

    // When the real pulse finally arrives, pulse increments and frac resets
    // toward 0, no snap-back to an in-between value.
    const Position next_edge = interpolate_step(st, 104, 60000 + 80000, kClockStateActive);
    CHECK(next_edge.pulse == 104u);
    CHECK(next_edge.frac == doctest::Approx(0.0f).epsilon(0.05f));
    CHECK(next_edge.frac < overdue.frac);
}

// ---------------------------------------------------------------------------
TEST_CASE("counter wrap: a pulse value below last_pulse re-syncs without a glitch") {
    InterpState st {};
    // Seed last_pulse near the wrap boundary by feeding pulses there.
    const uint32_t near_wrap = kPulseWrap - 2u;
    feed_steady(st, near_wrap, 0, kInterval, 3);  // pulses wrap-2, wrap-1, wrap

    // Next pulse wraps: the post-wrap counter value is small (0-based again).
    // interpolate_step sees pulse < last_pulse -> treats it as a wrap re-sync.
    const uint32_t post_wrap_pulse = 4u;  // small value, well below last_pulse
    const Position wrapped = interpolate_step(st, post_wrap_pulse, 3 * kInterval + 1234, kClockStateActive);

    CHECK(wrapped.status == Status::Live);
    CHECK(wrapped.pulse == post_wrap_pulse);  // re-synced to the new value
    CHECK(wrapped.frac == 0.0f);              // no bogus huge-delta fraction
}

// ---------------------------------------------------------------------------
TEST_CASE("per-instance independence: two InterpState structs never interfere") {
    InterpState slow {};
    InterpState fast {};

    // Drive slow with a 20000 us stream and fast with a 10000 us stream over a
    // different pulse range.
    feed_steady(slow, 100, 0, kInterval, 4);
    feed_steady(fast, 700, 0, 10000u, 4);

    // Sample each mid-pulse. slow at +10000 us of a 20000 us period reads ~0.5,
    // fast at +5000 us of a 10000 us period reads ~0.5, and the pulse fields
    // stay in their own ranges.
    const Position pos_slow = interpolate_step(slow, 103, 60000 + 10000, kClockStateActive);
    const Position pos_fast = interpolate_step(fast, 703, 30000 + 5000, kClockStateActive);

    CHECK(pos_slow.pulse == 103u);
    CHECK(pos_fast.pulse == 703u);
    CHECK(pos_slow.frac == doctest::Approx(0.5f).epsilon(0.05f));
    CHECK(pos_fast.frac == doctest::Approx(0.5f).epsilon(0.05f));

    // Mutating fast further must not perturb slow's last-known state.
    const Position slow_before = interpolate_step(slow, 103, 60000 + 12000, kClockStateActive);
    feed_steady(fast, 800, 1'000'000, 5000u, 6);  // hammer fast
    const Position slow_after = interpolate_step(slow, 103, 60000 + 12000, kClockStateActive);
    CHECK(slow_after.pulse == slow_before.pulse);
    CHECK(slow_after.frac == slow_before.frac);
}
