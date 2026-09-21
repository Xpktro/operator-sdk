#pragma once
/// @file timing.h
/// @brief Musical subdivision helpers + clock interpolation (opt-in, header-only).
///
/// Three groups of facilities live here:
///   * ``op::sdk::timing::*``, the pure integer/float subdivision math
///     (beats to microseconds, grid quantization, swing). These do not call
///     the ABI themselves. They translate between microseconds, beats, and
///     musical grid subdivisions.
///   * ``op::sdk::ClockInterpolator``, which converts the device's integer
///     24 PPQ ``api->get_pulse_count()`` counter into a smooth fractional
///     position by interpolating between pulse edges using
///     ``api->get_tick()`` microseconds. (The ABI exposes no fractional-beat
///     call, so the interpolator is the mode-side way to get a continuous
///     position.)
///   * ``op::sdk::StepTracker``, which tells a mode running on a musical grid
///     when the clock has carried it into a new step.
///
/// Include on demand. This header is deliberately not pulled in by
/// ``operator_sdk.h``.

#include <cstdint>

#include <operator_sdk/abi/mode_api.h>  // OperatorApi, OpClockState

namespace op {
// Declaration of the per-mode API handle. The definition
// (``inline const OperatorApi* api = nullptr;``) is emitted into each mode's
// translation unit by OP_MODE_PARAMS / OP_MODE_NO_PARAMS (see params.h).
// This ``extern`` declaration only lets ``ClockInterpolator::now()`` below
// name ``op::api`` at parse time. It does not create a second definition.
extern const OperatorApi* api;
}  // namespace op

/// @namespace op::sdk::timing
/// @brief Musical subdivision constants and clock/timing helpers.
namespace op::sdk::timing {

/// @brief Whole note subdivision (4 beats in 4/4).
inline constexpr uint8_t kSubdiv1 = 0;
/// @brief Half note subdivision.
inline constexpr uint8_t kSubdiv_2 = 1;
/// @brief Quarter note subdivision (one beat).
inline constexpr uint8_t kSubdiv_4 = 2;
/// @brief Eighth note subdivision.
inline constexpr uint8_t kSubdiv_8 = 3;
/// @brief Sixteenth note subdivision.
inline constexpr uint8_t kSubdiv_16 = 4;
/// @brief Thirty-second note subdivision.
inline constexpr uint8_t kSubdiv_32 = 5;
/// @brief Quarter-note triplet (three notes per half-note).
inline constexpr uint8_t kSubdivTripletQuarter = 6;
/// @brief Eighth-note triplet (three notes per quarter-note).
inline constexpr uint8_t kSubdivTripletEighth = 7;
/// @brief Sixteenth-note triplet (three notes per eighth-note).
inline constexpr uint8_t kSubdivTripletSixteenth = 8;

/// @brief Convert musical beats at a given tempo into microseconds.
/// @param beats Fractional beat count.
/// @param bpm Tempo in beats per minute (must be non-zero).
/// @return Duration in microseconds, or 0 if ``bpm == 0``.
inline uint32_t beats_to_ticks(float beats, uint32_t bpm) {
    if (bpm == 0) return 0;
    return static_cast<uint32_t>(beats * 60'000'000.0f / static_cast<float>(bpm));
}

/// @brief Microseconds per single subdivision at the given tempo.
/// @param subdiv One of the kSubdiv* constants.
/// @param bpm Tempo in beats per minute (must be non-zero).
/// @return Duration of one step of that subdivision, or 0 for unknown input.
///
/// Reference (120 BPM): quarter = 500'000us, eighth = 250'000us,
/// triplet eighth = 166'666us.
///
/// Safe for bpm in [1, ...]. The result fits uint32_t for all bpm >= 1
/// (the worst case, kSubdiv1 at bpm == 1, is 240'000'000us, well within
/// the uint32_t range).
inline uint32_t subdivision_us(uint8_t subdiv, uint32_t bpm) {
    if (bpm == 0) return 0;
    const uint64_t quarter_us = 60'000'000ull / bpm;  // microseconds per beat
    switch (subdiv) {
        case kSubdiv1: return static_cast<uint32_t>(quarter_us * 4ull);
        case kSubdiv_2: return static_cast<uint32_t>(quarter_us * 2ull);
        case kSubdiv_4: return static_cast<uint32_t>(quarter_us);
        case kSubdiv_8: return static_cast<uint32_t>(quarter_us / 2ull);
        case kSubdiv_16: return static_cast<uint32_t>(quarter_us / 4ull);
        case kSubdiv_32: return static_cast<uint32_t>(quarter_us / 8ull);
        case kSubdivTripletQuarter: return static_cast<uint32_t>((quarter_us * 2ull) / 3ull);
        case kSubdivTripletEighth: return static_cast<uint32_t>(quarter_us / 3ull);
        case kSubdivTripletSixteenth: return static_cast<uint32_t>(quarter_us / 6ull);
        default: return 0;
    }
}

/// @brief Round a tick value down to the nearest grid boundary.
/// @param tick_us Microsecond timestamp.
/// @param subdiv_us Grid step in microseconds (typically from
///                  ``subdivision_us``).
/// @return Largest multiple of ``subdiv_us`` that is <= ``tick_us``.
/// @return ``tick_us`` unchanged when ``subdiv_us == 0``.
inline uint32_t quantize_to_grid(uint32_t tick_us, uint32_t subdiv_us) {
    if (subdiv_us == 0) return tick_us;
    return (tick_us / subdiv_us) * subdiv_us;
}

/// @brief Microsecond offset to apply to "every other" subdivision to create
///        swing feel.
/// @param tick_us Target tick (used to decide whether we are on an even or
///                odd subdivision).
/// @param swing_percent 0..100. 50 is "no swing" (straight feel). Higher values
///                     delay the off-beat subdivision proportionally.
/// @param subdiv_us Duration of one grid step (see ``subdivision_us``).
/// @return Signed microsecond offset to add to the off-beat's nominal time,
///         0 on the on-beat or when ``subdiv_us == 0``.
///
/// Pass a grid-quantized ``tick_us`` (see ``quantize_to_grid``) so the
/// even/odd subdivision classification lands on true grid-step boundaries.
/// An unquantized ``tick_us`` near a boundary can flip parity under
/// integer truncation.
inline int32_t swing_offset(uint32_t tick_us, uint8_t swing_percent, uint32_t subdiv_us) {
    if (subdiv_us == 0) return 0;
    // swing_percent clamped to [0, 100]
    uint32_t sw = swing_percent > 100 ? 100 : swing_percent;
    // Straight timing = 50, anything else moves the off-beat.
    const int32_t delta = static_cast<int32_t>(sw) - 50;  // -50..+50
    // Even subdivisions are on-beat (no offset), odd are off-beat.
    const uint32_t step_index = tick_us / subdiv_us;
    if ((step_index & 1u) == 0u) return 0;
    // Offset is delta% of one subdivision.
    return static_cast<int32_t>((static_cast<int64_t>(subdiv_us) * delta) / 100);
}

}  // namespace op::sdk::timing

// ===========================================================================
// ClockInterpolator: smooth fractional position between 24 PPQ pulse edges.
// ===========================================================================
//
// The firmware exposes only an integer 24 PPQ counter (``get_pulse_count()``),
// which at 120 BPM advances ~48 times per second (one step every ~21 ms).
// Modes that play time-stamped material (SMF playback, LFOs, smooth visuals)
// look choppy if they snap to those edges. ``ClockInterpolator`` measures the
// recent pulse interval and reports a continuous fractional position between
// edges, derived purely from ``get_pulse_count()`` + ``get_tick()``.
//
// Freestanding-safe by construction: no ``double``,
// no libc, no 64-bit divide. The only float ops are the final fraction divide
// and clamp comparisons, and all interval/period math is ``uint32_t``.

namespace op::sdk::detail {

// Interpolation state for a ClockInterpolator. All members are
// default-initialized, so embedding one as a member emits no static
// constructor and keeps each mode instance isolated.
struct InterpState {
    uint32_t last_pulse    = 0;          // Last observed raw pulse count.
    uint32_t last_pulse_us = 0;          // `get_tick()` at the last pulse edge.
    uint32_t intervals[3]  = {0, 0, 0};  // 3-slot ring of inter-pulse us intervals.
    uint8_t interval_count = 0;          // 0..3 valid intervals, < 3 means no period yet.
    bool seen_first        = false;      // Has the first-ever pulse been observed?
    float last_frac        = 0.0f;       // Last reported fraction (monotonic guard).
    uint8_t last_status    = 0;          // Last OpClockState seen (stale-resume detect).
    uint32_t last_period   = 0;          // Last median inter-pulse interval (us), 0 until ready.
};

// Median-of-3, pure integer. Three comparisons, no division/float. Returns
// the middle value of a, b, c.
inline uint32_t median3(uint32_t a, uint32_t b, uint32_t c) {
    if (a > b) {
        uint32_t t = a;
        a          = b;
        b          = t;
    }
    if (b > c) {
        uint32_t t = b;
        b          = c;
        c          = t;
    }
    if (a > b) {
        uint32_t t = a;
        a          = b;
        b          = t;
    }
    return b;
}

}  // namespace op::sdk::detail

namespace op::sdk {

/// @brief Converts the firmware's integer 24 PPQ pulse counter into a smooth
///        fractional position by interpolating between pulse edges.
///
/// Header-only, per-instance. Hold one ``ClockInterpolator`` as a plain member
/// of your mode struct. Call ``now()`` exactly once per ``process()`` call.
class ClockInterpolator {
public:
    /// @brief Health of the interpolated position returned by ``now()``.
    enum class Status : uint8_t {
        Live,         ///< Clock active, fraction sweeping normally within a pulse.
        HeldOverdue,  ///< Clock active but the next pulse is late, fraction clamped
                      ///< just below unity and held until the pulse arrives.
        HeldStale,    ///< Clock active state seen but no pulse for ~4x the period,
                      ///< position frozen at last-known (likely a stalled clock).
        NoClock,      ///< Clock idle (NoClock) or delegated (Mode), position frozen.
    };

    /// @brief Interpolated clock position.
    struct Position {
        uint32_t pulse;  ///< Exact raw ``get_pulse_count()`` value (24 PPQ pulses).
        float frac;      ///< Fractional progress in [0, 1) toward the next pulse.
        Status status;   ///< Health of this sample (see ``Status``).
    };

    /// @brief Sample the current interpolated clock position.
    ///
    /// Consume-only: reads ``op::api`` (``get_pulse_count`` / ``get_tick`` /
    /// ``get_clock_state``), updates internal interpolation state, and returns
    /// the position. Call once per process(). Units are 24 PPQ pulses, so
    /// divide by your own modulus for beats. Never resets to zero on clock
    /// loss: a stalled or absent clock freezes the last-known position.
    inline Position now();

    /// @brief Smoothed microseconds between consecutive 24 PPQ pulse edges.
    ///
    /// Returns the median of the last three inter-pulse intervals, or 0 until
    /// three have been observed at cold start. After that it holds the last
    /// period across a clock stop, the same freeze ``now()`` applies to the
    /// position. One quarter note spans 24 pulses, so multiply by 24 for
    /// microseconds-per-quarter-note and divide 60'000'000 by that for BPM.
    /// Reflects whatever ``now()`` last sampled, so call ``now()`` once per
    /// process() tick to keep it current.
    inline uint32_t pulse_period_us() const {
        return state_.last_period;
    }

private:
    detail::InterpState state_ {};
};

/// @brief What a step observation means.
enum class StepEdge : uint8_t {
    Unchanged,  ///< The mode is on the step it reported last time.
    Adopted,    ///< The first look. This step is where the mode came in.
    Crossed,    ///< The clock has carried the mode into a new step.
};

/// @brief Tells a mode when the clock has carried it into a new step.
///
/// A sequencer or an arpeggiator acts at one moment, when the music reaches the
/// next step of its grid. Work out which step the clock is on, pass that number
/// in once per ``process()``, and this reports whether it just changed.
///
/// Where the number comes from is the mode's own business. A fraction of a bar,
/// a fixed count of pulses per step, and a subdivision that follows a rate
/// parameter all arrive here as a plain step number, so the grid stays with the
/// mode that defines it.
///
/// The first call is the one worth understanding, and it reports
/// ``StepEdge::Adopted``. A mode comes up at an arbitrary moment, rarely the
/// instant a step begins, and starting inside a step is not the same as reaching
/// one. That step is where the mode came in, so it plays nothing and waits for
/// the next.
///
/// Give every grid its own tracker. Four sequences at four step counts run four
/// trackers, each fed its own number.
///
/// Default-initialized throughout, so one held at file scope needs no constructor.
class StepTracker {
public:
    /// @brief Hand in the step the clock has reached this tick.
    /// @param step The current step index, from the mode's own division of the clock.
    /// @return Whether the mode reached this step, came in on it, or is on it already.
    StepEdge observe(int32_t step) {
        if (!started_) {
            started_ = true;
            step_    = step;
            return StepEdge::Adopted;
        }
        if (step == step_) return StepEdge::Unchanged;
        step_ = step;
        return StepEdge::Crossed;
    }

    /// @brief Treat the next ``observe()`` as a fresh start.
    ///
    /// A sequence that restarts comes in on whatever step it resumes at, the same
    /// as a mode that has just loaded, so it plays from the next one.
    void reset() {
        started_ = false;
        step_    = 0;
    }

    /// @brief The step last handed in, 0 before the first call.
    int32_t current() const {
        return step_;
    }

    /// @brief Whether any step has been handed in since construction or ``reset()``.
    bool started() const {
        return started_;
    }

private:
    int32_t step_ = 0;
    bool started_ = false;
};

}  // namespace op::sdk

namespace op::sdk::detail {

// Pure interpolation step, the testable core of ClockInterpolator. It uses no
// globals and makes no ABI calls, so host tests can drive it directly without
// mocking op::api. Given the raw counter `pulse`, the current us tick
// `tick_us`, and the current clock state `clock_state` (an OpClockState
// value), it advances `st` and returns the interpolated position.
inline ClockInterpolator::Position interpolate_step(InterpState& st,
                                                    uint32_t pulse,
                                                    uint32_t tick_us,
                                                    uint8_t clock_state) {
    using Status = ClockInterpolator::Status;

    // --- A. No-clock / Mode: freeze at last-known, never reset to zero.
    if (clock_state == kClockStateNoClock || clock_state == kClockStateMode) {
        st.last_status = clock_state;
        return ClockInterpolator::Position {st.last_pulse, st.last_frac, Status::NoClock};
    }

    // --- B. Stale-resume: a stop gap poisons the interval ring. When the
    // previous call was a pulse-idle state (NoClock, or the delegated Mode) and
    // this call is Active again, drop the interval history and restart cold
    // (keep last_pulse so the integer part of the position stays monotonic).
    if (st.last_status == kClockStateNoClock || st.last_status == kClockStateMode) {
        st.interval_count = 0;
        st.seen_first     = false;
        st.intervals[0] = st.intervals[1] = st.intervals[2] = 0;
    }

    // --- C. Pulse-edge detection.
    if (!st.seen_first) {
        // First-ever Active call: anchor, report integer position.
        st.seen_first    = true;
        st.last_pulse    = pulse;
        st.last_pulse_us = tick_us;
        st.last_frac     = 0.0f;
        st.last_status   = clock_state;
        return ClockInterpolator::Position {pulse, 0.0f, Status::Live};
    }

    if (pulse < st.last_pulse) {
        // Counter wrap (or backward jump): re-sync, push no bogus interval.
        st.last_pulse    = pulse;
        st.last_pulse_us = tick_us;
        st.last_frac     = 0.0f;
        st.last_status   = clock_state;
        return ClockInterpolator::Position {pulse, 0.0f, Status::Live};
    }

    if (pulse > st.last_pulse) {
        // A real pulse edge arrived. uint32 modular subtraction is correct for
        // any true interval < 2^32 us.
        const uint32_t interval = tick_us - st.last_pulse_us;
        // Only push the sample for a normal single-pulse advance. A multi-pulse
        // jump (missed process() call) re-syncs but does not pollute the ring.
        if (pulse - st.last_pulse == 1u) {
            st.intervals[0] = st.intervals[1];
            st.intervals[1] = st.intervals[2];
            st.intervals[2] = interval;
            if (st.interval_count < 3) ++st.interval_count;
        }
        st.last_pulse    = pulse;
        st.last_pulse_us = tick_us;
        st.last_frac     = 0.0f;
    }

    // --- D. Fraction sweep between pulses.
    if (st.interval_count < 3) {
        // Cold start: no valid period yet, graceful integer fallback.
        st.last_status = clock_state;
        return ClockInterpolator::Position {st.last_pulse, 0.0f, Status::Live};
    }

    const uint32_t period = median3(st.intervals[0], st.intervals[1], st.intervals[2]);
    st.last_period        = period;  // published via ClockInterpolator::pulse_period_us()
    if (period == 0u) {
        // Defensive: degenerate period, fall back to integer position.
        st.last_status = clock_state;
        return ClockInterpolator::Position {st.last_pulse, 0.0f, Status::Live};
    }

    const uint32_t elapsed = tick_us - st.last_pulse_us;

    // Staleness: no pulse for ~4x the expected period. Compute the
    // product in uint64_t to avoid 32-bit overflow at very low BPM. This is a
    // multiply + compare, not a 64-bit divide, so it is freestanding-safe.
    constexpr uint32_t kStaleMultiple = 4u;
    const uint64_t stale_threshold    = static_cast<uint64_t>(period) * kStaleMultiple;
    if (static_cast<uint64_t>(elapsed) > stale_threshold) {
        st.last_status = clock_state;
        return ClockInterpolator::Position {st.last_pulse, st.last_frac, Status::HeldStale};
    }

    // Normal / overdue fraction. Single-precision float divide -> vdiv.f32.
    float frac = static_cast<float>(elapsed) / static_cast<float>(period);

    // Forward clamp: keep frac strictly below unity and monotonic
    // non-decreasing within the same pulse.
    constexpr float kFracCap = 0.99999f;
    const bool overdue       = frac > kFracCap;
    if (overdue) frac = kFracCap;
    if (frac < st.last_frac) frac = st.last_frac;

    st.last_frac   = frac;
    st.last_status = clock_state;
    return ClockInterpolator::Position {st.last_pulse, frac, overdue ? Status::HeldOverdue : Status::Live};
}

}  // namespace op::sdk::detail

namespace op::sdk {

// Out-of-line definition of ClockInterpolator::now(), placed after
// detail::interpolate_step so the call below resolves.
inline ClockInterpolator::Position ClockInterpolator::now() {
    return detail::interpolate_step(state_, ::op::api->get_pulse_count(), ::op::api->get_tick(),
                                    ::op::api->get_clock_state());
}

}  // namespace op::sdk
