// Quantizer sample, doctest behavior suite.
//
// The mode holds a late note back and lets it out on the next grid point, so the
// suite drives the beat by hand and watches when the note actually leaves. A note
// that passes straight through stays in the batch it arrived in, and a note that
// was held is sent afresh, which is what tells the two apart.
//
// The beat is driven directly, so at 2 beats a second a quarter-note grid point
// falls every 500 ms.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <operator_sdk.h>
#include <operator_sdk_sim.h>

#include <cstdint>
#include <vector>

OP_MODE_UNDER_TEST();

namespace {

constexpr uint8_t kSlotDivision = 0;
constexpr uint8_t kSlotTriplet  = 1;

// The Division options, in the order they are declared.
constexpr int32_t kWhole     = 0;
constexpr int32_t kHalf      = 1;
constexpr int32_t kQuarter   = 2;
constexpr int32_t kEighth    = 3;
constexpr int32_t kSixteenth = 4;
constexpr int32_t kDivisionCount = 7;

constexpr uint8_t kOutput = 0;

// The gesture type is what the mode branches on, so the encoder and the press
// that produced it are left to the active input map.
constexpr uint8_t kAnyEncoder = 0;

std::vector<OpMidiMessage> g_collected;

void collect() {
    OpMidiMessage buffer[64] {};
    while (true) {
        const auto count = op::sim::drain_outgoing_midi(kOutput, buffer, 64);
        if (count == 0) break;
        for (uint16_t i = 0; i < count; ++i) g_collected.push_back(buffer[i]);
    }
}

// Everything the mode has sent since the last call. A note that was held comes
// out here, and a note that passed straight through does not.
std::vector<OpMidiMessage> sent() {
    collect();
    std::vector<OpMidiMessage> out = std::move(g_collected);
    g_collected.clear();
    return out;
}

void boot(int32_t division = kQuarter, bool triplet = false) {
    op::sim::reset_state();
    op::sim::apply_param_defaults(kParams, kParamCount);
    g_collected.clear();
    mode_init(op::sim::get_api());
    op::sim::get_api()->set_param_value(kSlotDivision, division);
    op::sim::get_api()->set_param_value(kSlotTriplet, triplet ? 1 : 0);
    op::sim::set_beat_position(0.0f);
}

void set_beat(float beat) {
    op::sim::set_beat_position(beat);
}

// Run the mode forward, carrying the beat along with the tick.
void run(uint32_t duration_us, float beats_per_second) {
    constexpr uint32_t kSlice = 1000;
    for (uint32_t i = 0; i < duration_us / kSlice; ++i) {
        op::sim::advance_tick(kSlice);
        const uint32_t tick = op::sim::get_api()->get_tick();
        set_beat(static_cast<float>(tick) * beats_per_second / 1'000'000.0f);
        mode_process(nullptr, 0, tick);
        if ((i & 0xF) == 0xF) collect();
    }
    collect();
}

// Hand one message to the mode and give it back, so the caller can see whether it
// survived the batch or was taken out of it.
OpMidiMessage feed(uint8_t status, uint8_t data1, uint8_t data2) {
    OpMidiMessage message {status, data1, data2, kOutput, 3};
    mode_process(&message, 1, op::sim::get_api()->get_tick());
    return message;
}

OpMidiMessage feed_note_on(uint8_t pitch, uint8_t velocity = 100) {
    return feed(0x90, pitch, velocity);
}

bool passed_through(const OpMidiMessage& message) {
    return message.status != 0;
}

uint32_t count_note_ons(const std::vector<OpMidiMessage>& messages, uint8_t pitch) {
    uint32_t total = 0;
    for (const auto& message : messages) {
        if ((message.status & 0xF0) == 0x90 && message.data2 != 0 && message.data1 == pitch) ++total;
    }
    return total;
}

void scroll(int16_t steps) {
    op::sim::ui_gesture(kAnyEncoder, op::Gesture::Rotate, op::GestureType::Scroll, steps);
}
void change() {
    op::sim::ui_gesture(kAnyEncoder, op::Gesture::Rotate, op::GestureType::Change, 1);
}

}  // namespace

// --- Snapping to the grid ----------------------------------------------------

// A note just past a grid point is already where it wants to be, so holding it
// back would only make it late. A note in the second half of the window is on its
// way to the next grid point, so it waits there.
TEST_CASE("a note early in the window plays at once and a late one waits") {
    boot(kQuarter);

    set_beat(0.3f);
    const OpMidiMessage early = feed_note_on(60);
    CHECK(passed_through(early));
    CHECK(sent().empty());

    set_beat(0.6f);
    const OpMidiMessage late = feed_note_on(64);
    CHECK_FALSE(passed_through(late));  // taken out of the batch to be held
    CHECK(sent().empty());

    run(700'000, /*beats_per_second=*/2.0f);  // over the beat-1 grid point

    CHECK(count_note_ons(sent(), 64) == 1);
    mode_destroy();
}

// An eighth-note grid puts a grid point every half beat, so the window turns over
// at a quarter beat where a quarter-note grid turns over at a half.
TEST_CASE("Division sets how far apart the grid points are") {
    boot(kEighth);

    set_beat(0.2f);  // first half of the eighth-note window
    CHECK(passed_through(feed_note_on(60)));

    set_beat(0.3f);  // second half of it
    CHECK_FALSE(passed_through(feed_note_on(64)));

    mode_destroy();
}

TEST_CASE("Triplet moves the grid points closer together") {
    boot(kQuarter, /*triplet=*/true);

    // Triplets put three grid points where two would go, so the quarter-note
    // window ends two thirds of a beat in and 0.4 is past the halfway mark.
    set_beat(0.4f);
    CHECK_FALSE(passed_through(feed_note_on(60)));

    mode_destroy();
}

TEST_CASE("a held note comes out even when the bar turns over") {
    boot(kQuarter);

    set_beat(3.6f);  // the last window of the bar
    CHECK_FALSE(passed_through(feed_note_on(60)));

    run(2'200'000, 2.0f);  // past beat 4, which is the next bar

    CHECK(count_note_ons(sent(), 60) == 1);
    mode_destroy();
}

TEST_CASE("two notes held in the same window both come out") {
    boot(kQuarter);

    set_beat(0.6f);
    CHECK_FALSE(passed_through(feed_note_on(60)));
    CHECK_FALSE(passed_through(feed_note_on(64)));

    run(700'000, 2.0f);

    const auto messages = sent();
    CHECK(count_note_ons(messages, 60) == 1);
    CHECK(count_note_ons(messages, 64) == 1);
    mode_destroy();
}

// Eight notes can wait at once, and a ninth in the same window is dropped, which
// costs a note where letting it through would put it off the grid.
TEST_CASE("a ninth note waiting in one window is dropped") {
    boot(kQuarter);

    set_beat(0.6f);
    for (uint8_t pitch = 60; pitch < 68; ++pitch) {
        CHECK_FALSE(passed_through(feed_note_on(pitch)));
    }
    const OpMidiMessage ninth = feed_note_on(68);
    CHECK_FALSE(passed_through(ninth));  // dropped, not passed on

    run(700'000, 2.0f);

    const auto messages = sent();
    CHECK(count_note_ons(messages, 60) == 1);
    CHECK(count_note_ons(messages, 68) == 0);
    mode_destroy();
}

// --- What the grid leaves alone ----------------------------------------------

// A release is never held, so a note keeps the length it was played with and is
// never left sounding.
TEST_CASE("a note-off passes straight through") {
    boot(kQuarter);

    set_beat(0.6f);  // the half of the window that holds a note-on back
    CHECK(passed_through(feed(0x80, 60, 64)));

    CHECK(sent().empty());
    mode_destroy();
}

TEST_CASE("anything that is not a note passes straight through") {
    boot(kQuarter);
    set_beat(0.6f);

    CHECK(passed_through(feed(0xB0, 7, 127)));  // a control change
    CHECK(passed_through(feed(0xE0, 0, 64)));   // a pitch bend
    CHECK(passed_through(feed(0xF8, 0, 0)));    // a clock pulse

    mode_destroy();
}

// A note tapped and released inside one window never reached the grid point it
// was waiting for, so neither it nor its release is sent.
TEST_CASE("a note released before its grid point never sounds") {
    boot(kQuarter);

    set_beat(0.6f);
    CHECK_FALSE(passed_through(feed_note_on(60)));

    const OpMidiMessage release = feed(0x80, 60, 64);
    CHECK_FALSE(passed_through(release));  // the release goes with it

    run(700'000, 2.0f);

    CHECK(count_note_ons(sent(), 60) == 0);
    mode_destroy();
}

// --- The transport -----------------------------------------------------------

// The grid is measured from wherever the transport last started, so the notes line
// up with bar one of whatever plays next. Both Start and Stop re-anchor it, since
// MIDI has no pause and the gear that sends these rewinds on either.
TEST_CASE("Start re-anchors the grid and drops what was waiting") {
    boot(kQuarter);

    set_beat(0.6f);
    CHECK_FALSE(passed_through(feed_note_on(60)));

    feed(0xFA, 0, 0);  // Start
    run(700'000, 2.0f);

    CHECK(count_note_ons(sent(), 60) == 0);  // the held note went with the anchor
    mode_destroy();
}

TEST_CASE("Stop re-anchors the grid as Start does") {
    boot(kQuarter);

    set_beat(0.6f);
    CHECK_FALSE(passed_through(feed_note_on(60)));

    feed(0xFC, 0, 0);  // Stop
    run(700'000, 2.0f);

    CHECK(count_note_ons(sent(), 60) == 0);
    mode_destroy();
}

// --- The screen --------------------------------------------------------------

// Changing the grid under a waiting note would put it somewhere it was never
// played, so the ones waiting are let go.
TEST_CASE("changing Division drops the notes that were waiting") {
    boot(kQuarter);

    set_beat(0.6f);
    CHECK_FALSE(passed_through(feed_note_on(60)));

    op::sim::get_api()->set_param_value(kSlotDivision, kEighth);
    run(700'000, 2.0f);

    CHECK(count_note_ons(sent(), 60) == 0);
    mode_destroy();
}

TEST_CASE("Scroll cycles the Division and comes back round") {
    boot(kWhole);
    const auto* api = op::sim::get_api();

    scroll(+1);
    CHECK(api->get_param_value(kSlotDivision) == kHalf);

    scroll(+1);
    CHECK(api->get_param_value(kSlotDivision) == kQuarter);

    // Back past the first option, which wraps round to the last.
    scroll(-1);
    scroll(-1);
    scroll(-1);
    CHECK(api->get_param_value(kSlotDivision) == kDivisionCount - 1);

    mode_destroy();
}

TEST_CASE("Change toggles Triplet") {
    boot(kQuarter);
    const auto* api = op::sim::get_api();
    REQUIRE(api->get_param_value(kSlotTriplet) == 0);

    change();
    CHECK(api->get_param_value(kSlotTriplet) == 1);

    change();
    CHECK(api->get_param_value(kSlotTriplet) == 0);

    mode_destroy();
}
