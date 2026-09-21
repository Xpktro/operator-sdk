// Note Probability sample, doctest behavior suite.
//
// The mode is a gate on note-ons, so the suite hands it a batch of messages and
// counts what came through. The sim starts the tick at zero, which leaves the
// engine on its default seed, so the runs below repeat exactly.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <operator_sdk.h>
#include <operator_sdk_sim.h>

#include <cstdint>

OP_MODE_UNDER_TEST();

namespace {

constexpr uint8_t kSlotProbability = 0;

void boot_at(int32_t probability) {
    op::sim::reset_state();
    op::sim::apply_param_defaults(kParams, kParamCount);
    mode_init(op::sim::get_api());
    op::sim::get_api()->set_param_value(kSlotProbability, probability);
}

// A dropped note comes back with its status cleared, which is what takes it out
// of the batch.
bool was_dropped(const OpMidiMessage& message) {
    return message.status == 0;
}

uint32_t count_played(const OpMidiMessage* messages, uint8_t count) {
    uint32_t played = 0;
    for (uint8_t i = 0; i < count; ++i) {
        if (!was_dropped(messages[i])) ++played;
    }
    return played;
}

// Identical note-ons, so the roll is the only thing that decides them.
void fill_note_ons(OpMidiMessage* messages, uint8_t count) {
    for (uint8_t i = 0; i < count; ++i) {
        messages[i] = OpMidiMessage {0x90, 60, 100, 0, 3};
    }
}

}  // namespace

// --- The gate ----------------------------------------------------------------

TEST_CASE("every note plays at probability 100") {
    boot_at(100);

    constexpr uint8_t kCount = 100;
    OpMidiMessage messages[kCount];
    fill_note_ons(messages, kCount);
    mode_process(messages, kCount, 0);

    CHECK(count_played(messages, kCount) == kCount);
    mode_destroy();
}

TEST_CASE("no note plays at probability 0") {
    boot_at(0);

    constexpr uint8_t kCount = 100;
    OpMidiMessage messages[kCount];
    fill_note_ons(messages, kCount);
    mode_process(messages, kCount, 0);

    CHECK(count_played(messages, kCount) == 0);
    mode_destroy();
}

// A thousand rolls at even odds land near five hundred, and the window here is
// wide enough that any reasonable run falls inside it.
TEST_CASE("about half the notes play at probability 50") {
    boot_at(50);

    constexpr uint8_t kBatch = 100;
    uint32_t played          = 0;
    for (uint32_t batch = 0; batch < 10; ++batch) {
        OpMidiMessage messages[kBatch];
        fill_note_ons(messages, kBatch);
        mode_process(messages, kBatch, batch);
        played += count_played(messages, kBatch);
    }

    CHECK(played >= 440);
    CHECK(played <= 560);
    mode_destroy();
}

// --- What the gate leaves alone ----------------------------------------------

// Only note-ons are gated. A release has to reach the synth whatever the odds
// are, or a note that did play would be left sounding.
TEST_CASE("everything other than a note-on passes whatever the probability") {
    boot_at(0);  // the setting that drops every note-on

    OpMidiMessage messages[] = {
        {0xB0, 7, 127, 0, 3},   // a control change
        {0xE0, 0, 64, 0, 3},    // a pitch bend
        {0xC0, 42, 0, 0, 2},    // a program change
        {0x80, 60, 64, 0, 3},   // a note-off
        {0x90, 60, 0, 0, 3},    // a note-on at velocity 0, which is a note-off
    };
    constexpr uint8_t kCount = sizeof(messages) / sizeof(messages[0]);

    mode_process(messages, kCount, 0);

    CHECK(count_played(messages, kCount) == kCount);
    mode_destroy();
}
