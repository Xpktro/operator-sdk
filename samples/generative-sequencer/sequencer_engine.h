#pragma once
// State machine for the Generative Sequencer sample mode.
//
// A step is the only moment a new note is decided. Between steps the engine just
// releases the notes whose length has run out.
//
// Internal Clock picks what the steps follow. On, the engine runs its own
// microsecond accumulator at the configured BPM, and off, it follows the device
// clock and its 24 PPQ pulse count. Either way the steps land on the same
// 16th-note grid, four to the quarter note.

#include <operator_sdk.h>
#include <operator_sdk/timing.h>  // op::sdk::StepTracker (opt-in)

#include <cstdint>

namespace op::samples::generative {

// How many notes the engine can hold sounding at once. The slowest tempo (20 BPM)
// gives a 3 second quarter note and the longest note is 2 seconds, so even a
// dense pattern stays well inside this.
constexpr uint8_t kMaxActiveNotes = 16;

// The outputs the engine rotates between, the 8 physical outputs plus USB.
constexpr uint8_t kNumOutputs = 9;

// A note the engine is currently holding. Once the clock passes off_tick_us the
// matching note-off goes out and the slot is free again.
struct ActiveNote {
    uint8_t output_idx;
    uint8_t note;
    uint8_t channel;
    bool alive;
    uint32_t off_tick_us;  // compared by signed subtraction, so it survives the tick wrapping
};

// Everything the engine remembers between calls.
struct State {
    // The tick the next internal-clock step falls on.
    uint32_t next_step_tick_us = 0;
    // The 16th-note step the device clock has the engine on, and whether the grid
    // has moved it there.
    op::sdk::StepTracker steps;
    // Where the next note goes while Output is All.
    uint8_t next_output = 0;
    // xorshift32 state, seeded from the tick at init.
    uint32_t rng = 0x9E3779B9u;
    ActiveNote active[kMaxActiveNotes] {};
};

// A snapshot of the params and the clock for one call, so the engine reads its
// inputs from one place and the param decoding stays in main.cpp.
struct TickInputs {
    uint32_t tick_us;
    uint32_t pulse;  // 24 PPQ count, which the engine divides down to a step index
    int32_t note_low;
    int32_t note_high;
    int32_t length_low_ms;
    int32_t length_high_ms;
    bool internal_clock;   // false follows the device clock
    int32_t internal_bpm;  // fixed point at two decimal places
    int32_t probability;   // 0 to 100, the chance a step plays
    int32_t output_sel;    // 0 is All, 1 to 8 are physical outputs 0 to 7, 9 is USB
};

// Advance the sequencer by one call, sending any note-ons and note-offs that
// fall due through the api.
void advance(State& state, const TickInputs& inputs, const OperatorApi* api);

// Release every note still sounding, which is what unloading the mode needs so
// the notes it left behind stop.
void release_all(State& state, const OperatorApi* api);

// Return the state to its just-initialized defaults, which init and destroy both
// want.
void reset(State& state);

// Seed the RNG. init calls this with the current tick so successive power-ons
// diverge.
void seed_rng(State& state, uint32_t seed);

}  // namespace op::samples::generative
