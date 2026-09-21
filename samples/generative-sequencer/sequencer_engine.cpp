// State machine for the Generative Sequencer sample mode.
//
// The engine keeps to 32-bit integer math throughout, so the step grid is
// derived from the pulse count by a plain divide and the tempo is rounded down
// to whole BPM before the interval is worked out.

#include "sequencer_engine.h"

#include <cstdint>

namespace op::samples::generative {

namespace {

    // The device clock runs at 24 pulses to the quarter note and the engine steps on
    // a 16th-note grid, so a step spans 6 pulses and the step index is just
    // pulse / 6.
    constexpr uint32_t kPulsesPerStep = 24u / 4u;  // 6

    // The internal clock is derived from the same numbers, so whichever clock is
    // driving, the steps land on the same grid.
    constexpr uint32_t kStepsPerBeat = 24u / kPulsesPerStep;  // 4

    constexpr uint32_t kMicrosPerMinute = 60'000'000;
    constexpr uint32_t kBpmFixedScale   = 100;

    constexpr uint8_t kNoteOn   = 0x90;
    constexpr uint8_t kNoteOff  = 0x80;
    constexpr uint8_t kVelocity = 80;  // a mezzo-forte
    constexpr uint8_t kChannel  = 0;   // channel 1

    // The tick has reached the deadline. Signed subtraction, so the answer holds
    // as the tick wraps.
    bool reached(uint32_t tick, uint32_t deadline) {
        return static_cast<int32_t>(tick - deadline) >= 0;
    }

    // A low and a high, in that order.
    struct Bounds {
        uint32_t low;
        uint32_t high;
    };

    // random_range reads its low first, so a range that arrives inverted is put
    // back in order here.
    Bounds ordered(uint32_t low, uint32_t high) {
        return high < low ? Bounds {high, low} : Bounds {low, high};
    }

    // Microseconds per step at a given tempo, which is the quarter note divided by
    // the steps in it: 60_000_000 / (BPM * kStepsPerBeat). The tempo arrives as fixed
    // point and is rounded down to whole BPM first, which keeps the divide 32-bit.
    // The UI moves BPM in steps of 0.10, so the rounding costs at most half a percent
    // and stays musically even.
    uint32_t step_interval_us(int32_t bpm_fixed) {
        if (bpm_fixed <= 0) return 0;
        const uint32_t bpm = bpm_fixed / kBpmFixedScale;
        if (bpm == 0) return 0;
        return kMicrosPerMinute / (bpm * kStepsPerBeat);
    }

    uint32_t xorshift32(uint32_t& rng) {
        uint32_t x = rng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        rng = x;
        return x;
    }

    // Uniform over low to high inclusive, which needs low at or below high.
    uint32_t random_range(uint32_t& rng, uint32_t low, uint32_t high) {
        const uint32_t span = high - low + 1u;
        return low + (xorshift32(rng) % span);
    }

    // Folding the top byte down spreads the 32-bit draw evenly over 0 to 99, which is
    // the range Probability is read against.
    bool roll_probability(uint32_t& rng, int32_t probability) {
        if (probability <= 0) return false;
        if (probability >= 100) return true;
        const int32_t roll = (xorshift32(rng) >> 24) * 100u / 256u;
        return roll < probability;
    }

    // The output after this one in the rotation.
    uint8_t next_in_rotation(uint8_t output) {
        return (output + 1u) % kNumOutputs;
    }

    ActiveNote* allocate_slot(State& state) {
        for (uint8_t i = 0; i < kMaxActiveNotes; ++i) {
            if (!state.active[i].alive) return &state.active[i];
        }
        return nullptr;
    }

    void send_note_off(const ActiveNote& slot, const OperatorApi* api) {
        api->send_midi(slot.output_idx, kNoteOff | slot.channel, slot.note, /*velocity=*/0);
    }

    // Release every note whose off-time has arrived and free its slot.
    void drain_due_note_offs(State& state, uint32_t tick, const OperatorApi* api) {
        for (uint8_t i = 0; i < kMaxActiveNotes; ++i) {
            auto& slot = state.active[i];
            if (!slot.alive) continue;
            if (reached(tick, slot.off_tick_us)) {
                send_note_off(slot, api);
                slot.alive = false;
            }
        }
    }

    // True when a step boundary has just been crossed, moving the accumulators along
    // with it. The caller decides what to do with the step.
    bool detect_step_edge(State& state, const TickInputs& inputs) {
        if (inputs.internal_clock) {
            const uint32_t interval = step_interval_us(inputs.internal_bpm);
            if (interval == 0) return false;

            // The first call primes the accumulator, so the first step lands one full
            // interval after the mode starts.
            if (state.next_step_tick_us == 0 && !state.steps.started()) {
                state.next_step_tick_us = inputs.tick_us + interval;
                state.steps.observe(0);
                return false;
            }
            if (reached(inputs.tick_us, state.next_step_tick_us)) {
                state.next_step_tick_us += interval;
                return true;
            }
            return false;
        }

        // The device clock holds its pulse still while it is stopped, so the step
        // index holds with it and the sequencer waits on the step it is on.
        const int32_t step_index = inputs.pulse / kPulsesPerStep;
        return state.steps.observe(step_index) == op::sdk::StepEdge::Crossed;
    }

    // Where this step's note goes. All rotates through the outputs and passes over
    // any that are inactive, which is usually the USB slot while USB MIDI is down.
    // The scan is bounded by the output count, so it settles on the last candidate
    // when none of them are active. The rotation only moves here, so leaving a
    // chosen output and coming back to All picks it up again.
    uint8_t pick_output(State& state, int32_t output_sel, const OperatorApi* api) {
        if (output_sel > 0) {
            const int32_t chosen = output_sel - 1;  // options 1 to 9 name outputs 0 to 8
            return chosen < kNumOutputs ? chosen : kNumOutputs - 1;
        }

        uint8_t output = state.next_output;
        for (uint8_t attempt = 0; attempt < kNumOutputs; ++attempt) {
            if (api->output_active(output)) break;
            output = next_in_rotation(output);
        }
        state.next_output = next_in_rotation(output);
        return output;
    }

}  // namespace

void advance(State& state, const TickInputs& inputs, const OperatorApi* api) {
    if (!api) return;

    // Note-offs go out first, so a note reaches its off-time even on a call that
    // carries no step.
    drain_due_note_offs(state, inputs.tick_us, api);

    if (!detect_step_edge(state, inputs)) return;
    if (!roll_probability(state.rng, inputs.probability)) return;

    Bounds notes = ordered(inputs.note_low, inputs.note_high);
    if (notes.high > 127u) notes.high = 127u;
    const Bounds lengths = ordered(inputs.length_low_ms, inputs.length_high_ms);

    const uint8_t note       = random_range(state.rng, notes.low, notes.high);
    const uint32_t length_ms = random_range(state.rng, lengths.low, lengths.high);
    const uint8_t output     = pick_output(state, inputs.output_sel, api);

    ActiveNote* slot = allocate_slot(state);
    if (!slot) return;  // every slot is sounding, so this step stays silent

    slot->output_idx  = output;
    slot->note        = note;
    slot->channel     = kChannel;
    slot->alive       = true;
    slot->off_tick_us = inputs.tick_us + length_ms * 1000u;

    api->send_midi(output, kNoteOn | kChannel, note, kVelocity);
}

void release_all(State& state, const OperatorApi* api) {
    if (!api) return;
    for (uint8_t i = 0; i < kMaxActiveNotes; ++i) {
        auto& slot = state.active[i];
        if (!slot.alive) continue;
        send_note_off(slot, api);
        slot.alive = false;
    }
}

void reset(State& state) {
    state.next_step_tick_us = 0;
    state.steps.reset();
    state.next_output       = 0;
    state.rng               = 0x9E3779B9u;
    for (uint8_t i = 0; i < kMaxActiveNotes; ++i) state.active[i] = ActiveNote {};
}

void seed_rng(State& state, uint32_t seed) {
    state.rng = seed != 0 ? seed : 0x9E3779B9u;
}

}  // namespace op::samples::generative
