// Generative Sequencer sample mode.
//
// The state machine lives in sequencer_engine.h, so this file carries the params
// and the lifecycle only.
//
// Internal Clock picks what the sequencer follows. Off, it follows the device
// clock, reading the pulse position once a tick, and on, it runs its own
// accumulator at the Internal BPM.

#include <operator_sdk.h>
#include <operator_sdk/params.h>
#include <operator_sdk/timing.h>

#include "sequencer_engine.h"

#include <cstdint>

// --- Parameters --------------------------------------------------------------
//
// Note Range and Length Range each carry a low and a high, which a structured
// binding reads as a pair: auto [low, high] = param<NoteRange>().
//
// Internal BPM is fixed point at two decimal places, so 12000 is 120.00 BPM.

inline constexpr op::params::NoteRange NoteRange {
    .name         = "Note Range",
    .default_low  = 36,
    .default_high = 127,
};

inline constexpr op::params::Range LengthRange {
    .name         = "Length Range",
    .min          = 10,
    .max          = 2000,
    .default_low  = 100,
    .default_high = 2000,
};

inline constexpr op::params::Bool InternalClock {
    .name     = "Internal Clock",
    .default_ = false,
};

inline constexpr op::params::Numeric InternalBpm {
    .name           = "Internal BPM",
    .min            = 2000,
    .max            = 30000,
    .default_       = 12000,
    .step           = 10,
    .decimal_places = 2,
    // The tempo is the mode's own only while it runs its own clock, so the field
    // appears with Internal Clock.
    .visible_when = {.watch = InternalClock, .op = op::params::Op::Eq, .value = true},
};

inline constexpr op::params::Numeric Probability {
    .name     = "Probability",
    .min      = 0,
    .max      = 100,
    .default_ = 50,
};

// Option 0 is All, so a specific choice sits one ahead of the output it names:
// options 1 to 8 are physical outputs 0 to 7, and option 9 is USB.
inline constexpr op::params::Enum Output {
    .name     = "Output",
    .options  = {"All", "Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8", "USB"},
    .default_ = 0,
};

OP_MODE_PARAMS(NoteRange, LengthRange, InternalClock, InternalBpm, Probability, Output);

namespace {

op::samples::generative::State g_state {};
op::sdk::ClockInterpolator g_clock {};

}  // namespace

// --- Lifecycle ---------------------------------------------------------------

void init() {
    op::samples::generative::reset(g_state);

    // Seed from the current tick so successive power-ons diverge. xorshift needs
    // a non-zero seed, and reset() has already installed one.
    if (op::api) {
        const uint32_t tick = op::api->get_tick();
        if (tick != 0) op::samples::generative::seed_rng(g_state, tick);
    }
}

void process(OpMidiMessage* /*messages*/, uint8_t /*count*/, uint32_t tick_us) {
    if (!op::api) return;

    const auto [note_low, note_high]     = param<NoteRange>();
    const auto [length_low, length_high] = param<LengthRange>();

    // The clock position is read once per tick, which is the cadence now() is
    // written for. Its pulse holds still while the clock is stopped, so the
    // engine's step index holds with it.
    const op::sdk::ClockInterpolator::Position position = g_clock.now();

    op::samples::generative::TickInputs inputs {
        /*tick_us*/ tick_us,
        /*pulse*/ position.pulse,
        /*note_low*/ note_low,
        /*note_high*/ note_high,
        /*length_low_ms*/ length_low,
        /*length_high_ms*/ length_high,
        /*internal_clock*/ param<InternalClock>() != 0,
        /*internal_bpm*/ param<InternalBpm>(),
        /*probability*/ param<Probability>(),
        /*output_sel*/ param<Output>(),
    };
    op::samples::generative::advance(g_state, inputs, op::api);
}

void destroy() {
    // Unloading the mode leaves the synth quiet.
    op::samples::generative::release_all(g_state, op::api);
    op::samples::generative::reset(g_state);
}

OP_MODE_REGISTER(init, process, destroy);
