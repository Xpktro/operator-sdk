// Euclidean Sequencer sample, doctest behavior suite.
//
// Four independent sequences spread their Events onsets across Steps positions
// and emit a note-on at every onset boundary, driven by the clock's bar position.
// The mode carries its own two-screen UI, a four-row overview and a per-sequence
// editor, so the cases below cover the rhythm helper, the live emission through
// process(), and the UI through the rendered framebuffer.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <operator_sdk_sim.h>
#include <operator_sdk.h>

#include "../euclidean.h"

#include <algorithm>
#include <cstdint>
#include <vector>

OP_MODE_UNDER_TEST();

namespace {

// Parameter slots, in the (Seq 1 Steps, Seq 1 Events, Seq 1 Offset, Seq 2 Steps,
// ...) order that OP_MODE_PARAMS declares them.
constexpr uint8_t kParamSeq1Steps  = 0;
constexpr uint8_t kParamSeq1Events = 1;
constexpr uint8_t kParamSeq1Offset = 2;
constexpr uint8_t kParamSeq2Steps  = 3;
constexpr uint8_t kParamSeq2Events = 4;
constexpr uint8_t kParamSeq3Steps  = 6;
constexpr uint8_t kParamSeq3Events = 7;
constexpr uint8_t kParamSeq4Steps  = 9;
constexpr uint8_t kParamSeq4Events = 10;

// Base note per sequence, matching the mode's kick, snare, closed-hat, open-hat
// layout.
constexpr uint8_t kSeq1Note = 36;
constexpr uint8_t kSeq2Note = 38;
constexpr uint8_t kSeq3Note = 40;
constexpr uint8_t kSeq4Note = 42;

constexpr uint8_t kTestOutput = 0;
constexpr uint32_t kSliceUs   = 1000;

std::vector<OpMidiMessage> g_collected;

void drain_into_collected() {
    OpMidiMessage buffer[64] {};
    while (true) {
        auto drained = op::sim::drain_outgoing_midi(kTestOutput, buffer, 64);
        if (drained == 0) break;
        for (uint16_t i = 0; i < drained; ++i) g_collected.push_back(buffer[i]);
    }
}

// Drive the mode for `duration_us`, with the beat position tracking the tick at
// `beats_per_second`. Drains every 16 slices so the sim's 32-slot ring keeps up.
void run(uint32_t duration_us, float beats_per_second) {
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

std::vector<uint8_t> note_ons(const std::vector<OpMidiMessage>& messages) {
    std::vector<uint8_t> pitches;
    for (const auto& message : messages) {
        if ((message.status & 0xF0) == 0x90 && message.data2 != 0) pitches.push_back(message.data1);
    }
    return pitches;
}

uint32_t count_with_note(const std::vector<OpMidiMessage>& messages, uint8_t note) {
    uint32_t count = 0;
    for (const auto& message : messages) {
        if ((message.status & 0xF0) == 0x90 && message.data2 != 0 && message.data1 == note) ++count;
    }
    return count;
}

void setup_mode() {
    op::sim::reset_state();
    // The device seeds every slot from the declared defaults before the first
    // process(), so a case that leaves a param alone sees what the mode ships with.
    op::sim::apply_param_defaults(kParams, kParamCount);
    mode_init(op::sim::get_api());
    op::sim::set_beat_position(0.0f);
}

// Silence every sequence, so a case can enable only the one it is about.
void silence_all(const OperatorApi* api) {
    api->set_param_value(kParamSeq1Events, 0);
    api->set_param_value(kParamSeq2Events, 0);
    api->set_param_value(kParamSeq3Events, 0);
    api->set_param_value(kParamSeq4Events, 0);
}

uint32_t count_pixels_in_region(
    const uint8_t* framebuffer, uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    uint32_t count = 0;
    for (uint16_t row = y; row < y + height && row < 64; ++row) {
        const uint16_t page = row / 8;
        const uint8_t bit   = 1u << (row % 8);
        for (uint16_t column = x; column < x + width && column < 128; ++column) {
            if (framebuffer[page * 128 + column] & bit) ++count;
        }
    }
    return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// The Euclidean rhythm helper
// ---------------------------------------------------------------------------

TEST_CASE("three events over eight steps gives the triplet") {
    bool pattern[8] {};
    op::samples::euclidean::euclidean_bresenham(3, 8, 0, pattern);
    // The triplet puts its onsets at 0, 3 and 6.
    CHECK(pattern[0] == true);
    CHECK(pattern[1] == false);
    CHECK(pattern[2] == false);
    CHECK(pattern[3] == true);
    CHECK(pattern[4] == false);
    CHECK(pattern[5] == false);
    CHECK(pattern[6] == true);
    CHECK(pattern[7] == false);
}

TEST_CASE("zero events gives all rests") {
    bool pattern[16] {};
    for (uint8_t i = 0; i < 16; ++i) pattern[i] = true;  // seed the opposite value
    op::samples::euclidean::euclidean_bresenham(0, 16, 0, pattern);
    for (uint8_t i = 0; i < 16; ++i) CHECK(pattern[i] == false);
}

TEST_CASE("events at or above steps fills every step") {
    bool exact[8] {};
    op::samples::euclidean::euclidean_bresenham(8, 8, 0, exact);
    for (uint8_t i = 0; i < 8; ++i) CHECK(exact[i] == true);

    bool over[8] {};
    op::samples::euclidean::euclidean_bresenham(12, 8, 0, over);
    for (uint8_t i = 0; i < 8; ++i) CHECK(over[i] == true);
}

TEST_CASE("offset rotates the pattern") {
    bool unrotated[8] {};
    bool rotated[8] {};
    op::samples::euclidean::euclidean_bresenham(3, 8, 0, unrotated);
    op::samples::euclidean::euclidean_bresenham(3, 8, 2, rotated);
    // An offset of 2 shifts every onset two slots to the right, wrapping at 8.
    for (uint8_t i = 0; i < 8; ++i) {
        const uint8_t source = (i + 8 - 2) % 8;
        CHECK(rotated[i] == unrotated[source]);
    }
}

// ---------------------------------------------------------------------------
// Live emission through process()
// ---------------------------------------------------------------------------

TEST_CASE("four sequences run independently") {
    setup_mode();
    const auto* api = op::sim::get_api();

    // Four steps each, with 1, 2, 3 and 4 events, so each sequence lands a
    // different number of onsets per measure.
    api->set_param_value(kParamSeq1Steps, 4);
    api->set_param_value(kParamSeq1Events, 1);
    api->set_param_value(kParamSeq2Steps, 4);
    api->set_param_value(kParamSeq2Events, 2);
    api->set_param_value(kParamSeq3Steps, 4);
    api->set_param_value(kParamSeq3Events, 3);
    api->set_param_value(kParamSeq4Steps, 4);
    api->set_param_value(kParamSeq4Events, 4);

    // A measure is 4 beats, so at 2 beats per second one measure takes 2 s. Drive
    // 2.2 s for one full measure with margin.
    run(2'200'000, /*beats_per_second=*/2.0f);

    const auto messages = drain();

    // The counts are lower bounds, since landing exactly on the wrap-around step
    // can add one.
    CHECK(count_with_note(messages, kSeq1Note) >= 1);
    CHECK(count_with_note(messages, kSeq2Note) >= 2);
    CHECK(count_with_note(messages, kSeq3Note) >= 3);
    CHECK(count_with_note(messages, kSeq4Note) >= 4);

    mode_destroy();
}

TEST_CASE("steps advance with the measure position") {
    setup_mode();
    const auto* api = op::sim::get_api();
    silence_all(api);

    // Sequence 1 fills all four of its steps, and the rest stay silent.
    api->set_param_value(kParamSeq1Steps, 4);
    api->set_param_value(kParamSeq1Events, 4);

    // One measure at 2 beats per second is 2 s, giving onsets at each of the four
    // step positions.
    run(2'100'000, 2.0f);
    const auto pitches = note_ons(drain());
    CHECK(pitches.size() >= 4);
    for (auto pitch : pitches) CHECK(pitch == kSeq1Note);

    mode_destroy();
}

TEST_CASE("a sequence with zero events emits no notes") {
    setup_mode();
    const auto* api = op::sim::get_api();
    silence_all(api);

    api->set_param_value(kParamSeq1Steps, 8);
    api->set_param_value(kParamSeq1Events, 0);

    run(2'200'000, 2.0f);
    CHECK(note_ons(drain()).size() == 0);

    mode_destroy();
}

TEST_CASE("events above steps fills every step") {
    setup_mode();
    const auto* api = op::sim::get_api();
    silence_all(api);

    // Events beyond Steps resolves to every step being an onset, so a measure of
    // eight steps lands eight onsets.
    api->set_param_value(kParamSeq1Steps, 8);
    api->set_param_value(kParamSeq1Events, 12);

    run(2'100'000, 2.0f);
    CHECK(note_ons(drain()).size() >= 8);

    mode_destroy();
}

// Nothing advances the grid while the clock is idle, so there is no boundary to
// play on. This covers both spellings of idle: the source has no clock, and the
// source is a mode with no clock generator loaded.
TEST_CASE("a sequence loaded with no clock running stays silent") {
    const uint8_t idle_states[] = {kClockStateNoClock, kClockStateMode};
    for (uint8_t state : idle_states) {
        setup_mode();
        // A counter sitting wherever the last clock left it. The position it names
        // is one the sequence never moved through.
        op::sim::set_pulse_count(114353);
        op::sim::set_clock_state(state);

        run(500'000, /*beats_per_second=*/0.0f);
        CHECK(drain().empty());

        mode_destroy();
    }
}

// A sequence coming up partway through a step has not entered that step, so its
// onset already went by.
TEST_CASE("a sequence loaded inside an onset step waits for the next onset") {
    setup_mode();
    op::sim::set_clock_state(kClockStateActive);

    // Sixteen steps span the 96-pulse measure, so a step runs six pulses and the
    // default pattern puts onsets on steps 0, 4, 8 and 12. Pulse 3 sits halfway
    // through step 0.
    op::sim::set_pulse_count(3);
    run(kSliceUs, 0.0f);
    CHECK(drain().empty());

    // Step 4 carries the next onset, and crossing into it sounds all four sequences.
    op::sim::set_pulse_count(24);
    run(kSliceUs, 0.0f);
    CHECK(note_ons(drain()).size() == 4);

    mode_destroy();
}

// ---------------------------------------------------------------------------
// Main screen
// ---------------------------------------------------------------------------

TEST_CASE("the main screen renders one row per sequence") {
    setup_mode();
    const auto* api = op::sim::get_api();
    api->set_param_value(kParamSeq1Steps, 8);
    api->set_param_value(kParamSeq1Events, 4);
    api->set_param_value(kParamSeq2Steps, 8);
    api->set_param_value(kParamSeq2Events, 4);
    api->set_param_value(kParamSeq3Steps, 8);
    api->set_param_value(kParamSeq3Events, 4);
    api->set_param_value(kParamSeq4Steps, 8);
    api->set_param_value(kParamSeq4Events, 4);

    mode_ui_render();
    uint8_t framebuffer[op::sim::kFramebufferBytes];
    op::sim::capture_framebuffer(framebuffer);

    // Each row sits at one of y = 11, 22, 33, 44 and stands 11 px tall, and each
    // carries at least its "SEQ N:" label.
    CHECK(count_pixels_in_region(framebuffer, 0, 11, 128, 11) > 10);
    CHECK(count_pixels_in_region(framebuffer, 0, 22, 128, 11) > 10);
    CHECK(count_pixels_in_region(framebuffer, 0, 33, 128, 11) > 10);
    CHECK(count_pixels_in_region(framebuffer, 0, 44, 128, 11) > 10);

    mode_destroy();
}

TEST_CASE("Scroll moves the row selection on the main screen") {
    setup_mode();

    mode_ui_render();
    uint8_t before[op::sim::kFramebufferBytes];
    op::sim::capture_framebuffer(before);

    op::sim::ui_gesture(kEncoderLeft, op::Gesture::Rotate, op::GestureType::Scroll, +1);
    mode_ui_render();
    uint8_t after[op::sim::kFramebufferBytes];
    op::sim::capture_framebuffer(after);

    // The selection outline moves down a row, so the two frames differ.
    bool differs = false;
    for (std::size_t i = 0; i < op::sim::kFramebufferBytes; ++i) {
        if (before[i] != after[i]) {
            differs = true;
            break;
        }
    }
    CHECK(differs);

    mode_destroy();
}

// ---------------------------------------------------------------------------
// Detail screen
// ---------------------------------------------------------------------------

TEST_CASE("Enter on the main screen opens the detail editor") {
    setup_mode();

    op::sim::ui_gesture(kEncoderRight, op::Gesture::ShortPress, op::GestureType::Enter, 0);

    mode_ui_render();
    uint8_t framebuffer[op::sim::kFramebufferBytes];
    op::sim::capture_framebuffer(framebuffer);

    // The detail screen titles itself "SEQ N EDITOR" across y = 9..16.
    CHECK(count_pixels_in_region(framebuffer, 0, 9, 128, 8) > 20);

    mode_destroy();
}

TEST_CASE("Back on the detail screen returns to main") {
    setup_mode();

    op::sim::ui_gesture(kEncoderRight, op::Gesture::ShortPress, op::GestureType::Enter, 0);
    op::sim::ui_gesture(kEncoderLeft, op::Gesture::ShortPress, op::GestureType::Back, 0);

    mode_ui_render();
    uint8_t framebuffer[op::sim::kFramebufferBytes];
    op::sim::capture_framebuffer(framebuffer);

    // The four main rows are back.
    CHECK(count_pixels_in_region(framebuffer, 0, 11, 128, 11) > 10);
    CHECK(count_pixels_in_region(framebuffer, 0, 44, 128, 11) > 10);

    mode_destroy();
}

TEST_CASE("Change edits the selected detail param") {
    setup_mode();
    const auto* api = op::sim::get_api();

    op::sim::ui_gesture(kEncoderRight, op::Gesture::ShortPress, op::GestureType::Enter, 0);

    // The detail screen opens on Steps.
    const int32_t before = api->get_param_value(kParamSeq1Steps);
    op::sim::ui_gesture(kEncoderRight, op::Gesture::Rotate, op::GestureType::Change, +1);
    CHECK(api->get_param_value(kParamSeq1Steps) == before + 1);

    mode_destroy();
}

TEST_CASE("Scroll cycles the detail param selection") {
    setup_mode();
    const auto* api = op::sim::get_api();

    op::sim::ui_gesture(kEncoderRight, op::Gesture::ShortPress, op::GestureType::Enter, 0);

    // Move the selection from Steps to Events, so a right rotate edits Events.
    op::sim::ui_gesture(kEncoderLeft, op::Gesture::Rotate, op::GestureType::Scroll, +1);
    const int32_t steps_before  = api->get_param_value(kParamSeq1Steps);
    const int32_t events_before = api->get_param_value(kParamSeq1Events);
    op::sim::ui_gesture(kEncoderRight, op::Gesture::Rotate, op::GestureType::Change, +1);
    CHECK(api->get_param_value(kParamSeq1Steps) == steps_before);
    CHECK(api->get_param_value(kParamSeq1Events) == events_before + 1);

    mode_destroy();
}

TEST_CASE("Enter on the detail screen resets the sequence") {
    setup_mode();
    const auto* api = op::sim::get_api();

    // Move the params off their defaults so the reset is visible.
    api->set_param_value(kParamSeq1Steps, 3);
    api->set_param_value(kParamSeq1Events, 2);
    api->set_param_value(kParamSeq1Offset, 1);

    op::sim::ui_gesture(kEncoderRight, op::Gesture::ShortPress, op::GestureType::Enter, 0);
    op::sim::ui_gesture(kEncoderRight, op::Gesture::ShortPress, op::GestureType::Enter, 0);

    CHECK(api->get_param_value(kParamSeq1Steps) == 16);
    CHECK(api->get_param_value(kParamSeq1Events) == 4);
    CHECK(api->get_param_value(kParamSeq1Offset) == 0);

    mode_destroy();
}
