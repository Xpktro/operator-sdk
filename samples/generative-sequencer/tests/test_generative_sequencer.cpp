// Generative Sequencer sample, doctest behavior suite.
//
// The mode is stochastic, so the assertions are on the properties every run has
// to hold: notes land inside the range, the step count follows the clock that is
// driving it, every note that starts is released, and the notes go where Output
// says they go. The sim starts the tick at zero, which leaves the engine on its
// default seed, so the runs below repeat exactly.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <operator_sdk.h>
#include <operator_sdk_sim.h>

#include <cstdint>
#include <vector>

OP_MODE_UNDER_TEST();

namespace {

// A dual-slot param takes two consecutive value slots, so Note Range and Length
// Range each carry a low and a high. The rest are scalars.
constexpr uint8_t kSlotNoteLow       = 0;
constexpr uint8_t kSlotNoteHigh      = 1;
constexpr uint8_t kSlotLengthLow     = 2;
constexpr uint8_t kSlotLengthHigh    = 3;
constexpr uint8_t kSlotInternalClock = 4;
constexpr uint8_t kSlotInternalBpm   = 5;
constexpr uint8_t kSlotProbability   = 6;
constexpr uint8_t kSlotOutput        = 7;

constexpr uint8_t kUsbOutput  = 8;   // the 8 physical outputs are 0..7
constexpr uint8_t kNumOutputs = 12;  // drain past the mode's 9 to catch a stray

void set_param(uint8_t slot, int32_t value) {
    op::sim::get_api()->set_param_value(slot, value);
}

void reset_mode() {
    op::sim::reset_state();
    op::sim::apply_param_defaults(kParams, kParamCount);
    mode_init(op::sim::get_api());
}

// Run the mode in 1 ms slices. A non-zero beats_per_second drives the device
// clock alongside the tick, which is what the mode follows unless Internal Clock
// is on.
void run(uint32_t duration_us, float beats_per_second = 0.0f) {
    constexpr uint32_t kSlice = 1000;
    for (uint32_t i = 0; i < duration_us / kSlice; ++i) {
        op::sim::advance_tick(kSlice);
        const uint32_t tick = op::sim::get_api()->get_tick();
        if (beats_per_second > 0.0f) {
            op::sim::set_beat_position(static_cast<float>(tick) * beats_per_second / 1'000'000.0f);
        }
        mode_process(nullptr, 0, tick);
    }
}

bool is_note_on(const OpMidiMessage& message) {
    return (message.status & 0xF0) == 0x90 && message.data2 != 0;
}
bool is_note_off(const OpMidiMessage& message) {
    return (message.status & 0xF0) == 0x80 || ((message.status & 0xF0) == 0x90 && message.data2 == 0);
}

// Everything the mode sent, tagged with the output it went to.
struct Sent {
    uint8_t output;
    OpMidiMessage message;
};

std::vector<Sent> collect() {
    std::vector<Sent> sent;
    OpMidiMessage buffer[32] {};
    for (uint8_t output = 0; output < kNumOutputs; ++output) {
        while (true) {
            const auto count = op::sim::drain_outgoing_midi(output, buffer, 32);
            if (count == 0) break;
            for (uint16_t i = 0; i < count; ++i) sent.push_back({output, buffer[i]});
        }
    }
    return sent;
}

uint32_t count_note_ons(const std::vector<Sent>& sent) {
    uint32_t total = 0;
    for (const auto& entry : sent) {
        if (is_note_on(entry.message)) ++total;
    }
    return total;
}

uint32_t count_note_offs(const std::vector<Sent>& sent) {
    uint32_t total = 0;
    for (const auto& entry : sent) {
        if (is_note_off(entry.message)) ++total;
    }
    return total;
}

uint32_t note_ons_on(const std::vector<Sent>& sent, uint8_t output) {
    uint32_t total = 0;
    for (const auto& entry : sent) {
        if (entry.output == output && is_note_on(entry.message)) ++total;
    }
    return total;
}

}  // namespace

// --- Notes -------------------------------------------------------------------

TEST_CASE("every note lands inside the note range") {
    reset_mode();
    set_param(kSlotNoteLow, 60);
    set_param(kSlotNoteHigh, 72);
    set_param(kSlotInternalClock, 1);
    set_param(kSlotProbability, 100);

    run(5'000'000);

    const auto sent = collect();
    REQUIRE(count_note_ons(sent) > 0);
    for (const auto& entry : sent) {
        if (!is_note_on(entry.message)) continue;
        CHECK(entry.message.data1 >= 60);
        CHECK(entry.message.data1 <= 72);
    }
    mode_destroy();
}

TEST_CASE("probability 0 plays nothing") {
    reset_mode();
    set_param(kSlotInternalClock, 1);
    set_param(kSlotProbability, 0);

    run(5'000'000);

    CHECK(count_note_ons(collect()) == 0);
    mode_destroy();
}

// At 120 BPM a 16th note is 125 ms, so 2 seconds covers 16 steps. The count is
// held to a window because the first and last step edges fall on the boundary of
// the run.
TEST_CASE("probability 100 plays a note on every step") {
    reset_mode();
    set_param(kSlotInternalClock, 1);
    set_param(kSlotInternalBpm, 12000);
    set_param(kSlotProbability, 100);

    run(2'000'000);

    const auto ons = count_note_ons(collect());
    CHECK(ons >= 14);
    CHECK(ons <= 18);
    mode_destroy();
}

// A note is released at the off-time it was given when it started, so a run that
// outlasts the longest note leaves at most the final note still sounding.
TEST_CASE("every note that starts is released") {
    reset_mode();
    set_param(kSlotLengthLow, 50);
    set_param(kSlotLengthHigh, 150);
    set_param(kSlotInternalClock, 1);
    set_param(kSlotProbability, 100);

    run(3'000'000);

    const auto sent = collect();
    const auto ons  = count_note_ons(sent);
    const auto offs = count_note_offs(sent);
    REQUIRE(ons > 0);
    CHECK(offs >= ons - 1);
    CHECK(offs <= ons);
    mode_destroy();
}

TEST_CASE("unloading releases the notes still sounding") {
    reset_mode();
    set_param(kSlotLengthLow, 2000);  // long enough to outlast the run
    set_param(kSlotLengthHigh, 2000);
    set_param(kSlotInternalClock, 1);
    set_param(kSlotProbability, 100);

    run(500'000);
    const auto during = collect();
    REQUIRE(count_note_ons(during) > 0);
    REQUIRE(count_note_offs(during) == 0);  // nothing has reached its off-time

    mode_destroy();

    CHECK(count_note_offs(collect()) == count_note_ons(during));
}

// --- Clock -------------------------------------------------------------------

// 2 beats per second over 2 seconds is 4 beats, and the engine steps on a
// 16th-note grid, so the run crosses about 16 step edges.
TEST_CASE("the sequencer follows the device clock") {
    reset_mode();
    set_param(kSlotInternalClock, 0);
    set_param(kSlotProbability, 100);

    run(2'000'000, /*beats_per_second=*/2.0f);

    const auto ons = count_note_ons(collect());
    CHECK(ons >= 14);
    CHECK(ons <= 18);
    mode_destroy();
}

// With the clock held still the step index stops moving, so the sequencer holds on
// the step it came up on and plays nothing. Were it running its own accumulator,
// 2 seconds at 120 BPM would have played about 16 notes.
TEST_CASE("a stopped clock leaves the sequencer on its step") {
    reset_mode();
    set_param(kSlotInternalClock, 0);
    set_param(kSlotInternalBpm, 12000);
    set_param(kSlotProbability, 100);

    run(2'000'000);  // the clock never moves

    CHECK(count_note_ons(collect()) == 0);
    mode_destroy();
}

// --- Output ------------------------------------------------------------------

TEST_CASE("All spreads the notes across the outputs in turn") {
    reset_mode();
    set_param(kSlotInternalClock, 1);
    set_param(kSlotProbability, 100);
    set_param(kSlotOutput, 0);  // All

    run(3'000'000);

    const auto sent      = collect();
    uint8_t outputs_used = 0;
    for (uint8_t output = 0; output < kNumOutputs; ++output) {
        if (note_ons_on(sent, output) > 0) ++outputs_used;
    }
    CHECK(count_note_ons(sent) > 0);
    CHECK(outputs_used > 1);
    mode_destroy();
}

TEST_CASE("All skips an output that cannot play") {
    reset_mode();
    set_param(kSlotInternalClock, 1);
    set_param(kSlotProbability, 100);
    set_param(kSlotOutput, 0);  // All
    op::sim::set_output_active(kUsbOutput, false);

    run(3'000'000);

    const auto sent = collect();
    CHECK(count_note_ons(sent) > 0);
    CHECK(note_ons_on(sent, kUsbOutput) == 0);
    mode_destroy();
}

TEST_CASE("a chosen output takes every note") {
    reset_mode();
    set_param(kSlotInternalClock, 1);
    set_param(kSlotProbability, 100);
    set_param(kSlotOutput, 3);  // Out 3 is physical output 2

    run(3'000'000);

    auto sent = collect();
    CHECK(note_ons_on(sent, 2) > 0);
    CHECK(count_note_ons(sent) == note_ons_on(sent, 2));
    mode_destroy();

    // USB sits past the eight physical outputs, at the end of the list.
    reset_mode();
    set_param(kSlotInternalClock, 1);
    set_param(kSlotProbability, 100);
    set_param(kSlotOutput, 9);  // USB

    run(3'000'000);

    sent = collect();
    CHECK(note_ons_on(sent, kUsbOutput) > 0);
    CHECK(count_note_ons(sent) == note_ons_on(sent, kUsbOutput));
    mode_destroy();
}

// A chosen output is where the notes go, so the sequencer stays on it and falls
// silent when it cannot play, which is what makes the choice hold.
TEST_CASE("a chosen output that cannot play stays silent") {
    reset_mode();
    set_param(kSlotInternalClock, 1);
    set_param(kSlotProbability, 100);
    set_param(kSlotOutput, 3);  // Out 3 is physical output 2
    op::sim::set_output_active(2, false);

    run(3'000'000);

    CHECK(count_note_ons(collect()) == 0);
    mode_destroy();
}
