// Arpeggiator sample mode.
//
// Output declarative generator. It tracks the notes held on its bound output
// and, on each step boundary, emits from that set following the selected Style.
// Step timing runs off the integer 24-PPQ pulse count (see
// compute_step_pulses).

#include <operator_sdk.h>
#include <operator_sdk/params.h>
#include <operator_sdk/timing.h>  // op::sdk::ClockInterpolator, op::sdk::StepTracker
                                  // (opt-in, not pulled in by operator_sdk.h).

#include <cstdint>

// --- Parameters ------------------------------------------------------------

inline constexpr op::params::Enum Style {
    .name     = "Style",
    .options  = {"As Played", "Up", "Down", "Up-Down", "Down-Up", "Converge", "Diverge", "Random"},
    .default_ = 1, // "Up", the musical default
};

inline constexpr op::params::Enum Rate {
    .name     = "Rate",
    .options  = {"1/4", "1/8", "1/16", "1/32", "1/64"},
    .default_ = 2, // "1/16", a 16th-note feel at 4/4
};

inline constexpr op::params::Bool Triplet {
    .name     = "Triplet",
    .default_ = false,
};

inline constexpr op::params::Numeric Octave {
    .name     = "Octave",
    .min      = -3,
    .max      = +3,
    .default_ = 0,
};

inline constexpr op::params::Numeric Fill {
    .name     = "Fill",
    .min      = 1,
    .max      = 100,
    .default_ = 50,
};

inline constexpr op::params::Bool Chord {
    .name     = "Chord",
    .default_ = false,
};

OP_MODE_PARAMS(Style, Rate, Triplet, Octave, Fill, Chord);

// --- Engine internals ------------------------------------------------------

namespace {

// --- Tunables -------------------------------------------------------------
//
// A 10-finger performer cannot realistically hold more than ten notes, and 16
// gives headroom for sustain-pedal-heavy playing styles without spending RAM on
// a dynamic container.
constexpr uint8_t kMaxHeldNotes = 16;

// Maximum number of chord-mode transpositions we emit per step. Equals the
// held-note count, so kMaxHeldNotes suffices.
constexpr uint8_t kMaxChordVoices = kMaxHeldNotes;

// Rate + Triplet to pulses-per-step lookup, on a 24-PPQ baseline. The triplet
// column encodes the 2/3 timing relationship as smaller integer step_pulses.
//
//   Rate  triplet=false  triplet=true
//   1/4   24             16
//   1/8   12             8
//   1/16  6              4
//   1/32  3              2
//   1/64  1              1   (degrades to 1, since triplet 1/64 is musically
//                             uncommon at 24 PPQ)
//
// Both arrays carry uint32 entries so pulse_now / step_pulses stays a plain
// 32-bit divide.
constexpr uint32_t kStepPulsesNoTriplet[5] = {24u, 12u, 6u, 3u, 1u};
constexpr uint32_t kStepPulsesTriplet[5]   = {16u, 8u, 4u, 2u, 1u};

// Style enum values (mirrored from the options list order above).
constexpr int32_t kStyleAsPlayed = 0;
constexpr int32_t kStyleUp       = 1;
constexpr int32_t kStyleDown     = 2;
constexpr int32_t kStyleUpDown   = 3;
constexpr int32_t kStyleDownUp   = 4;
constexpr int32_t kStyleConverge = 5;
constexpr int32_t kStyleDiverge  = 6;
constexpr int32_t kStyleRandom   = 7;

// Unused output argument, since an output mode reaches the output it was loaded
// on.
constexpr uint8_t kAnyOutput = 0;

// --- Held-note table -----------------------------------------------------

struct ArpState {
    // Three parallel arrays over the held notes, one in arrival order, one
    // ascending (recomputed on every mutation), and one of velocities keyed by
    // arrival order.
    uint8_t played[kMaxHeldNotes]     = {};
    uint8_t sorted[kMaxHeldNotes]     = {};
    uint8_t velocities[kMaxHeldNotes] = {};
    uint8_t count                     = 0;

    // Which step the pattern is on, and whether the grid has moved it there.
    op::sdk::StepTracker steps;
    uint32_t last_step_tick_us = 0;
    uint32_t step_interval_us  = 0;

    // Smoothed per-pulse period (wall-clock us between consecutive 24-PPQ
    // pulse edges). It is rate-independent (it reflects only tempo). The
    // current-rate step length is then step_pulses(current_rate) *
    // pulse_period_us, the bound we use to clamp note-off deadlines so a voice
    // is never scheduled more than one current-rate step into the future.
    op::sdk::ClockInterpolator clock;
    // Cached at the emitting step edge so emit_voice can clamp its off-delay
    // without re-deriving rate/triplet.
    uint32_t current_step_len_us_at_emit = 0;

    uint8_t pattern_cursor = 0;

    // Active emitted notes pending note-off. Each slot tracks the exact
    // MIDI byte we sent so we can release it without recomputing the
    // arpeggio formula.
    struct ActiveNote {
        uint8_t note;
        uint8_t velocity;
        bool alive;
        uint32_t off_tick_us;
        // Tick at note-on. drain_due_offs re-evaluates every pending deadline
        // against on_tick_us + one current-rate step each tick, so a deadline
        // poisoned by a pre-rate-change interval cannot strand a live voice
        // past one step at the current rate.
        uint32_t on_tick_us;
    };
    static constexpr uint8_t kMaxActive = kMaxChordVoices * 4;
    ActiveNote active[kMaxActive]       = {};

    // xorshift32 RNG (Random style). Seeded non-zero so the initial sequence
    // is deterministic even before init() re-seeds from get_tick().
    uint32_t rng = 0x9E3779B9u;
};

ArpState g_state;

// --- Held-note mutations -------------------------------------------------

// A cheap insertion sort. At N <= 16 it costs little, and it keeps sorted[]
// valid for the styles that read notes by pitch.
void rebuild_sorted(ArpState& state) {
    for (uint8_t i = 0; i < state.count; ++i) state.sorted[i] = state.played[i];
    for (uint8_t i = 1; i < state.count; ++i) {
        const uint8_t value = state.sorted[i];
        uint8_t j           = i;
        while (j > 0 && state.sorted[j - 1] > value) {
            state.sorted[j] = state.sorted[j - 1];
            --j;
        }
        state.sorted[j] = value;
    }
}

// Register an incoming note-on. Re-holding a pitch already in the table just
// refreshes its velocity, so a soft-then-hard press escalates the arp's emitted
// velocity without growing the table.
void add_held_note(ArpState& state, uint8_t note, uint8_t velocity) {
    for (uint8_t i = 0; i < state.count; ++i) {
        if (state.played[i] == note) {
            state.velocities[i] = velocity;
            return;
        }
    }
    if (state.count >= kMaxHeldNotes) return;  // table full, drop silently
    state.played[state.count]     = note;
    state.velocities[state.count] = velocity;
    ++state.count;
    rebuild_sorted(state);
}

// Remove a held note (by pitch). If the released note was the last in the
// table the arpeggio stops on the next step boundary. pattern_cursor is not
// reset, so re-holding a chord picks the pattern up where it left off.
void remove_held_note(ArpState& state, uint8_t note) {
    for (uint8_t i = 0; i < state.count; ++i) {
        if (state.played[i] == note) {
            for (uint8_t j = i; j + 1 < state.count; ++j) {
                state.played[j]     = state.played[j + 1];
                state.velocities[j] = state.velocities[j + 1];
            }
            --state.count;
            rebuild_sorted(state);
            return;
        }
    }
}

// --- Step-index math -----------------------------------------------------
//
// The arpeggiator fires on integer step edges. compute_arp_step_index returns
// pulse_count / step_pulses, and the step tracker reports when that index moves.
// There is no fractional interpolation, so the arp snaps exactly to pulse
// boundaries.

// Returns 0 for an out-of-range rate, which callers treat as "no step this
// tick".
uint32_t compute_step_pulses(int32_t rate, bool triplet) {
    if (rate < 0 || rate >= 5) return 0u;
    return triplet ? kStepPulsesTriplet[rate] : kStepPulsesNoTriplet[rate];
}

int32_t compute_arp_step_index(uint32_t pulse_count, int32_t rate, bool triplet) {
    const uint32_t step_pulses = compute_step_pulses(rate, triplet);
    if (step_pulses == 0u) return 0;
    return static_cast<int32_t>(pulse_count / step_pulses);
}

// --- Style dispatch ------------------------------------------------------
//
// Each style returns the index (into sorted) of the next note to emit and
// advances the pattern cursor. state.count is guaranteed > 0 by the caller.

// PRNG for the Random style (state in ArpState::rng).
uint32_t xorshift32(ArpState& state) {
    uint32_t x = state.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state.rng = x;
    return x;
}

inline void advance_mod(uint8_t& cursor, uint8_t modulus) {
    cursor = (cursor + 1) % modulus;
}

// Pick the next index for the current style. All styles index into sorted
// except As Played, which indexes into played.
uint8_t advance_cursor(ArpState& state, int32_t style) {
    const uint8_t note_count = state.count;  // > 0 by caller

    switch (style) {
        case kStyleAsPlayed: {
            const uint8_t index = state.pattern_cursor % note_count;
            advance_mod(state.pattern_cursor, note_count);
            return index;
        }
        case kStyleUp: {
            const uint8_t index = state.pattern_cursor % note_count;
            advance_mod(state.pattern_cursor, note_count);
            return index;
        }
        case kStyleDown: {
            const uint8_t index = (note_count - 1) - (state.pattern_cursor % note_count);
            advance_mod(state.pattern_cursor, note_count);
            return index;
        }
        case kStyleUpDown: {
            // Bounce without repeating the peak/trough. note_count <= 2
            // degenerates to Up, note_count >= 3 has cycle length
            // 2*note_count-2.
            if (note_count <= 2) {
                const uint8_t index = state.pattern_cursor % note_count;
                advance_mod(state.pattern_cursor, note_count);
                return index;
            }
            const uint8_t cycle_len  = 2 * note_count - 2;
            const uint8_t cursor_pos = state.pattern_cursor % cycle_len;
            advance_mod(state.pattern_cursor, cycle_len);
            return (cursor_pos < note_count) ? cursor_pos : (cycle_len - cursor_pos);
        }
        case kStyleDownUp: {
            if (note_count <= 2) {
                const uint8_t index = (note_count - 1) - (state.pattern_cursor % note_count);
                advance_mod(state.pattern_cursor, note_count);
                return index;
            }
            const uint8_t cycle_len  = 2 * note_count - 2;
            const uint8_t cursor_pos = state.pattern_cursor % cycle_len;
            advance_mod(state.pattern_cursor, cycle_len);
            if (cursor_pos < note_count) return (note_count - 1) - cursor_pos;
            return cursor_pos - (note_count - 1);
        }
        case kStyleConverge: {
            // Outer to inner, so 0, note_count-1, 1, note_count-2, 2, and so on.
            const uint8_t cursor_pos = state.pattern_cursor % note_count;
            advance_mod(state.pattern_cursor, note_count);
            return (cursor_pos & 1u) == 0u ? (cursor_pos / 2) : (note_count - 1 - (cursor_pos / 2));
        }
        case kStyleDiverge: {
            // Inner to outer, working from the middle. note_count=3 gives
            // 1, 0, 2 and note_count=4 gives 1, 2, 0, 3. Start at
            // floor((note_count-1)/2), alternating +offset then -offset.
            const uint8_t cursor_pos = state.pattern_cursor % note_count;
            advance_mod(state.pattern_cursor, note_count);
            const int32_t midpoint    = (note_count - 1) / 2;
            const int32_t half_offset = cursor_pos / 2;
            const int32_t raw_index   = (cursor_pos & 1u) == 0u ? midpoint - half_offset
                                                                : midpoint + 1 + half_offset;
            // Clamp to [0, note_count-1], only needed for the note_count=1
            // case.
            const int32_t clamped = raw_index < 0 ? 0
                                                  : (raw_index > note_count - 1 ? note_count - 1 : raw_index);
            return clamped;
        }
        case kStyleRandom: {
            // Uniform over [0, note_count) by modulo. pattern_cursor is unused
            // for Random, but the cursor advances anyway for consistency.
            ++state.pattern_cursor;
            const uint32_t roll = xorshift32(state);
            return roll % note_count;
        }
        default: return 0;
    }
}

// --- Active-note table ---------------------------------------------------

ArpState::ActiveNote* allocate_active(ArpState& state) {
    for (uint8_t i = 0; i < ArpState::kMaxActive; ++i) {
        if (!state.active[i].alive) return &state.active[i];
    }
    return nullptr;  // full, the caller drops the emission
}

// Length of one step at the current rate, in microseconds, or 0 if not yet
// estimable (per-pulse period times the current step_pulses). This is the
// upper bound on a note-off delay, since a voice may live at most one
// current-rate step. step_pulses <= 24 keeps the product well within uint32.
uint32_t current_step_len_us(const ArpState& state, int32_t rate, bool triplet) {
    const uint32_t period = state.clock.pulse_period_us();
    if (period == 0u) return 0u;
    const uint32_t step_pulses = compute_step_pulses(rate, triplet);
    if (step_pulses == 0u) return 0u;
    return step_pulses * period;
}

// The tick has reached the deadline. Signed subtraction, so the answer holds as
// the tick wraps, which it does about every 71 minutes.
bool reached(uint32_t tick, uint32_t deadline) {
    return static_cast<int32_t>(tick - deadline) >= 0;
}

// Drain any active notes whose off_tick_us has arrived and send their
// note-offs. step_len_us (0 = unknown) re-clamps each pending deadline to one
// current-rate step, so a deadline left stale by a Rate change cannot hold a
// voice open too long.
void drain_due_offs(ArpState& state, uint32_t tick_us, uint32_t step_len_us) {
    for (uint8_t i = 0; i < ArpState::kMaxActive; ++i) {
        auto& slot = state.active[i];
        if (!slot.alive) continue;
        uint32_t deadline = slot.off_tick_us;
        if (step_len_us != 0u) {
            // Pick the earlier of the scheduled deadline and the current-rate
            // one-step bound.
            const uint32_t clamped = slot.on_tick_us + step_len_us;
            if (!reached(clamped, deadline)) {
                deadline = clamped;
            }
        }
        if (reached(tick_us, deadline)) {
            op::api->send_midi(kAnyOutput, 0x80, slot.note, /*velocity=*/0);
            slot.alive = false;
        }
    }
}

// Immediately close every active voice.
void flush_all_active(ArpState& state) {
    for (uint8_t i = 0; i < ArpState::kMaxActive; ++i) {
        auto& slot = state.active[i];
        if (!slot.alive) continue;
        op::api->send_midi(kAnyOutput, 0x80, slot.note, /*velocity=*/0);
        slot.alive = false;
    }
}

// --- Emission ------------------------------------------------------------
//
// Emit a single arpeggiator voice, a note-on now and a note-off at
// step_tick + interval * fill / 100.
void emit_voice(ArpState& state,
                uint8_t raw_note,
                uint8_t velocity,
                int32_t octave_shift,
                int32_t fill,
                uint32_t step_tick_us) {
    const int32_t shifted = raw_note + octave_shift * 12;
    if (shifted < 0 || shifted > 127) return;
    const uint8_t note = shifted;  // guard above keeps shifted within uint8_t

    // Never emit a note-on we cannot later close. Reserve the active-table
    // slot first, and only send the 0x90 once a slot is secured. If the table
    // is full we drop this voice, since a dropped voice is strictly preferable
    // to a leaked 0x90 with no scheduled note-off (a hung note).
    auto* slot = allocate_active(state);
    if (!slot) return;

    op::api->send_midi(kAnyOutput, 0x90, note, velocity);

    slot->alive      = true;
    slot->note       = note;
    slot->velocity   = velocity;
    slot->on_tick_us = step_tick_us;

    // With no prior interval estimate, leave the note-off to the next step's
    // pre-emission drain (or drain_due_offs' one-step clamp once a period is
    // known).
    if (state.step_interval_us == 0) {
        slot->off_tick_us = step_tick_us;
        return;
    }

    const uint32_t clamped_fill = fill < 1 ? 1u : (fill > 100 ? 100u : static_cast<uint32_t>(fill));
    // Bound the off-delay to at most one current-rate step. step_interval_us is
    // a lagging cross-edge delta that a Rate slow-down balloons, so clamp it to
    // current_step_len_us_at_emit when that is known.
    uint32_t interval       = state.step_interval_us;
    const uint32_t step_len = state.current_step_len_us_at_emit;
    if (step_len != 0u && interval > step_len) interval = step_len;
    const uint32_t off_delay = (interval * clamped_fill) / 100u;

    slot->off_tick_us = step_tick_us + off_delay;
}

// Emit one step's payload. In single-note mode one voice fires. In chord
// mode the entire held-note set transposes so the style-chosen note
// becomes the new root, preserving every pairwise semitone interval.
void emit_step(ArpState& state,
               int32_t style,
               int32_t octave_shift,
               int32_t fill,
               bool chord_mode,
               uint32_t step_tick_us) {
    if (state.count == 0) return;

    const uint8_t cursor_index = advance_cursor(state, style);

    uint8_t base_note;
    uint8_t base_velocity;
    if (style == kStyleAsPlayed) {
        base_note     = state.played[cursor_index];
        base_velocity = state.velocities[cursor_index];
    } else {
        base_note = state.sorted[cursor_index];
        // sorted[] carries no velocity, so recover it from the played table.
        base_velocity = 100;
        for (uint8_t i = 0; i < state.count; ++i) {
            if (state.played[i] == base_note) {
                base_velocity = state.velocities[i];
                break;
            }
        }
    }

    if (!chord_mode) {
        emit_voice(state, base_note, base_velocity, octave_shift, fill, step_tick_us);
        return;
    }

    // In chord mode, transpose every held note by (base - lowest_held) so the
    // chord's lowest voice moves up to `base`. The sorted table's element
    // 0 is always the lowest held note.
    const int32_t lowest = state.sorted[0];
    const int32_t delta  = base_note - lowest;
    for (uint8_t i = 0; i < state.count; ++i) {
        const int32_t shifted = state.sorted[i] + delta;
        if (shifted < 0 || shifted > 127) continue;
        uint8_t velocity = 100;
        for (uint8_t j = 0; j < state.count; ++j) {
            if (state.played[j] == state.sorted[i]) {
                velocity = state.velocities[j];
                break;
            }
        }
        emit_voice(state, static_cast<uint8_t>(shifted), velocity, octave_shift, fill, step_tick_us);
    }
}

// --- Incoming-batch observation ------------------------------------------

void observe_incoming(ArpState& state, OpMidiMessage* messages, uint8_t count) {
    if (count == 0 || messages == nullptr) return;

    for (uint8_t i = 0; i < count; ++i) {
        OpMidiMessage& message  = messages[i];
        const uint8_t status_hi = message.status & 0xF0;

        if (status_hi == 0x90 && message.data2 != 0) {
            // Reset the pattern cursor when the chord goes from idle to held.
            // Adding or removing notes mid-playback does not restart it.
            const bool was_idle = (state.count == 0);
            add_held_note(state, message.data1, message.data2);
            if (was_idle && state.count > 0) {
                // Re-grip, so flush before re-arming first-observation
                // suppression below and no voice from the previous chord leaks
                // into the new grip.
                flush_all_active(state);
                state.pattern_cursor = 0;
                state.steps.reset();
            }
        } else if (status_hi == 0x80 || (status_hi == 0x90 && message.data2 == 0)) {
            // Note-off (explicit 0x80 or note-on vel=0). On the last release,
            // flush now, since no later step edge is guaranteed to close the
            // voices.
            remove_held_note(state, message.data1);
            if (state.count == 0) {
                flush_all_active(state);
            }
        }

        // Clearing the status consumes the held notes, so the output carries the
        // arpeggio alone. Notes are the only kind suppressed, so every other
        // message (aftertouch, CC, pitch-bend, and so on) passes through and a
        // downstream synth keeps full expression.
        if (status_hi == 0x90 || status_hi == 0x80) {
            message.status = 0;
        }
    }
}

}  // namespace

// --- Lifecycle -------------------------------------------------------------

void init() {
    g_state = ArpState {};
    if (op::api) {
        const uint32_t tick = op::api->get_tick();
        if (tick != 0) g_state.rng = tick ^ 0xC0FFEE55u;
    }
}

void process(OpMidiMessage* messages, uint8_t count, uint32_t tick_us) {
    if (!op::api) return;

    observe_incoming(g_state, messages, count);

    const int32_t style   = param<Style>();
    const int32_t rate    = param<Rate>();
    const bool triplet    = param<Triplet>() != 0;
    const int32_t octave  = param<Octave>();
    const int32_t fill    = param<Fill>();
    const bool chord_mode = param<Chord>() != 0;

    // Drain before emitting so a closed voice frees its slot for this step.
    drain_due_offs(g_state, tick_us, current_step_len_us(g_state, rate, triplet));

    // ClockInterpolator.now() must be called exactly once per tick.
    g_state.clock.now();

    // The step is tracked even with no held notes so a re-grip resumes at the
    // correct phase.
    const uint32_t pulse_now = op::api->get_pulse_count();
    const int32_t step_index = compute_arp_step_index(pulse_now, rate, triplet);
    const op::sdk::StepEdge edge = g_state.steps.observe(step_index);

    if (edge == op::sdk::StepEdge::Crossed) {
        // Update the running step-interval estimate (signed math is
        // wrap-safe across the ~71 min uptime rollover).
        const uint32_t delta = tick_us - g_state.last_step_tick_us;
        if (delta != 0) g_state.step_interval_us = delta;

        g_state.current_step_len_us_at_emit = current_step_len_us(g_state, rate, triplet);

        // Close any voice left alive from the previous step before emitting
        // the new one (at fill == 100 a voice is scheduled right at now).
        flush_all_active(g_state);

        if (g_state.count > 0) {
            emit_step(g_state, style, octave, fill, chord_mode, tick_us);
        }
    }

    if (edge != op::sdk::StepEdge::Unchanged) {
        // The baseline step stamps the time as a crossing does, so the interval
        // the next crossing measures spans one step.
        g_state.last_step_tick_us = tick_us;
    }
}

void destroy() {
    if (op::api) flush_all_active(g_state);
    g_state = ArpState {};
}

OP_MODE_REGISTER(init, process, destroy);
