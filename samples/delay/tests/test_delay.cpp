// Delay sample, doctest behavior suite.
//
// Pins the release-triggered, note-relative echo engine in ../main.cpp. The suite
// drives the mode through the SDK sim harness and observes it through
// drain_outgoing_midi and the message buffer the mode writes back.
//
// Engine model recap (from main.cpp).
//   * The mode owns each note. It consumes the incoming note-on and note-off and
//     re-emits the dry note itself, so drain captures the dry note and its echoes
//     as one strictly-alternating same-pitch stream.
//   * Echoes are release-triggered. While a note is held no echo is emitted, and
//     on the note-off the train fires Repeats decayed repeats, each gated to
//     min(source_gate, delta) and spaced one delta apart, the first at
//     max(on + delta, release). Two same-pitch notes never overlap.
//   * Scheduling is note-relative on the us tick, so an echo lands exactly `delta`
//     after the dry note's true arrival. Sync=on derives delta_us from tempo via
//     op::sdk::ClockInterpolator (delta_pulses x pulse_period_us) and Sync=off
//     uses Time x 1000, so Sync tests prime the interpolator with a steady pulse
//     and tick advance before delta resolves (see sync_reset()).
//   * Velocity decays by floor(velocity * feedback / 100), and a velocity below 1
//     stops the trail.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <operator_sdk_sim.h>
#include <operator_sdk.h>

#include <cstdint>
#include <vector>

OP_MODE_UNDER_TEST();

namespace {

// --- Param indices (mirror OP_MODE_PARAMS order in main.cpp) -----------------
// OP_MODE_PARAMS(Sync, Rate, Rhythm, Time, Repeats, Feedback, Transpose,
// Channel, Bypass).
constexpr uint8_t kParamSync      = 0;
constexpr uint8_t kParamRate      = 1;
constexpr uint8_t kParamRhythm    = 2;
constexpr uint8_t kParamTime      = 3;
constexpr uint8_t kParamRepeats   = 4;
constexpr uint8_t kParamFeedback  = 5;
constexpr uint8_t kParamTranspose = 6;
constexpr uint8_t kParamChannel   = 7;
constexpr uint8_t kParamBypass    = 8;

// Rate enum values (options {"2","1","1/2","1/4","1/8","1/16","1/32"}).
constexpr int32_t kRate_2    = 0;
constexpr int32_t kRate_1    = 1;
constexpr int32_t kRate_1_2  = 2;
constexpr int32_t kRate_1_4  = 3;
constexpr int32_t kRate_1_8  = 4;
constexpr int32_t kRate_1_16 = 5;
constexpr int32_t kRate_1_32 = 6;

// Rhythm enum values (options {"Straight","Dotted","Triplet"}).
constexpr int32_t kRhythm_Straight = 0;
constexpr int32_t kRhythm_Dotted   = 1;
constexpr int32_t kRhythm_Triplet  = 2;

// The output the mode is bound to, which is where every dry note and echo lands
// and so the one the tests drain.
constexpr uint8_t kTestOutput = 0;

// Test transport is a steady 1000 us per 24-PPQ pulse. After the clock is primed,
// pulse_period_us() == kPulseUs, so a synced delta of D pulses == D x 1000 us.
constexpr uint32_t kPulseUs     = 1000;
constexpr uint32_t kPrimePulses = 6;  // > 3 clean intervals -> interpolator
                                      // ready

// --- MIDI status helpers -----------------------------------------------------
constexpr uint8_t kNoteOn  = 0x90;
constexpr uint8_t kNoteOff = 0x80;

bool is_note_on(const OpMidiMessage& message) {
    return (message.status & 0xF0u) == 0x90u && message.data2 != 0u;
}
bool is_note_off(const OpMidiMessage& message) {
    return (message.status & 0xF0u) == 0x80u || ((message.status & 0xF0u) == 0x90u && message.data2 == 0u);
}

// --- Mode lifecycle reset ----------------------------------------------------
void reset_mode() {
    op::sim::reset_state();
    op::sim::apply_param_defaults(kParams, kParamCount);
    mode_init(op::sim::get_api());
}

void feed_note_on(uint8_t pitch, uint8_t velocity = 100, uint8_t channel = 0) {
    OpMidiMessage message {static_cast<uint8_t>(kNoteOn | (channel & 0x0Fu)), pitch, velocity, kTestOutput,
                           3};
    mode_process(&message, 1, op::sim::get_api()->get_tick());
}

void feed_note_off(uint8_t pitch, uint8_t channel = 0) {
    OpMidiMessage message {static_cast<uint8_t>(kNoteOff | (channel & 0x0Fu)), pitch, 0, kTestOutput, 3};
    mode_process(&message, 1, op::sim::get_api()->get_tick());
}

void tick_engine() {
    mode_process(nullptr, 0, op::sim::get_api()->get_tick());
}

// Feed a note-on or note-off and return the message status after process(). The
// mode zeroes the status of a note it owns and leaves a note it does not own
// intact, which is how a test tells ownership from passthrough.
uint8_t feed_note_on_status(uint8_t pitch, uint8_t velocity = 100, uint8_t channel = 0) {
    OpMidiMessage message {static_cast<uint8_t>(kNoteOn | (channel & 0x0Fu)), pitch, velocity, kTestOutput,
                           3};
    mode_process(&message, 1, op::sim::get_api()->get_tick());
    return message.status;
}

uint8_t feed_note_off_status(uint8_t pitch, uint8_t channel = 0) {
    OpMidiMessage message {static_cast<uint8_t>(kNoteOff | (channel & 0x0Fu)), pitch, 0, kTestOutput, 3};
    mode_process(&message, 1, op::sim::get_api()->get_tick());
    return message.status;
}

std::vector<OpMidiMessage> drain(uint8_t output = kTestOutput) {
    std::vector<OpMidiMessage> collected;
    OpMidiMessage buffer[64] {};
    while (true) {
        auto drained = op::sim::drain_outgoing_midi(output, buffer, 64);
        if (drained == 0) break;
        for (uint16_t i = 0; i < drained; ++i) collected.push_back(buffer[i]);
    }
    return collected;
}

std::vector<OpMidiMessage> note_ons(const std::vector<OpMidiMessage>& messages) {
    std::vector<OpMidiMessage> filtered;
    for (const auto& message : messages)
        if (is_note_on(message)) filtered.push_back(message);
    return filtered;
}
std::vector<OpMidiMessage> note_offs(const std::vector<OpMidiMessage>& messages) {
    std::vector<OpMidiMessage> filtered;
    for (const auto& message : messages)
        if (is_note_off(message)) filtered.push_back(message);
    return filtered;
}

void set_params(int32_t sync,
                int32_t rate,
                int32_t feel,
                int32_t time_ms,
                int32_t repeats,
                int32_t feedback,
                int32_t transpose) {
    const auto* api = op::sim::get_api();
    api->set_param_value(kParamSync, sync);
    api->set_param_value(kParamRate, rate);
    api->set_param_value(kParamRhythm, feel);
    api->set_param_value(kParamTime, time_ms);
    api->set_param_value(kParamRepeats, repeats);
    api->set_param_value(kParamFeedback, feedback);
    api->set_param_value(kParamTranspose, transpose);
}

// Virtual transport cursor (pulses). Advanced one pulse at a time at kPulseUs
// so the mode's ClockInterpolator observes a steady, rate-independent per-pulse
// period.
uint32_t g_pulse = 0;

// Advance pulse + tick in lockstep to absolute pulse `target` (kPulseUs/pulse),
// running the engine at each pulse edge, returns drained messages. Running the
// engine each edge is what primes pulse_period_us() for Sync delta resolution.
std::vector<OpMidiMessage> advance_to_pulse(uint32_t target) {
    std::vector<OpMidiMessage> collected;
    const auto* api = op::sim::get_api();
    while (g_pulse < target) {
        ++g_pulse;
        op::sim::advance_tick(kPulseUs);
        op::sim::set_pulse_count(g_pulse);
        mode_process(nullptr, 0, api->get_tick());
        for (auto& message : drain()) collected.push_back(message);
    }
    return collected;
}

// Fresh mode + Sync params, transport at pulse 0, then prime the interpolator
// so pulse_period_us() == kPulseUs before any note is played. Returns the
// baseline pulse (kPrimePulses) the first note will be played on.
uint32_t sync_reset(int32_t rate, int32_t feel, int32_t repeats, int32_t feedback, int32_t transpose) {
    reset_mode();
    set_params(
        /*sync=*/1, rate, feel, /*time=*/250, repeats, feedback, transpose);
    g_pulse = 0;
    advance_to_pulse(kPrimePulses);  // establish the per-pulse period
    drain();
    return kPrimePulses;
}

// Filter a stream to a single pitch (on + off only).
std::vector<OpMidiMessage> pitch_stream(const std::vector<OpMidiMessage>& messages, uint8_t pitch) {
    std::vector<OpMidiMessage> filtered;
    for (const auto& message : messages)
        if (message.data1 == pitch && (is_note_on(message) || is_note_off(message)))
            filtered.push_back(message);
    return filtered;
}

// Max simultaneously-open notes at `pitch` across the stream. It must never
// exceed 1, since MIDI 1.0 cannot represent two same-pitch notes at once.
int max_concurrency(const std::vector<OpMidiMessage>& messages, uint8_t pitch) {
    int open_count = 0, max_open = 0;
    for (const auto& message : messages) {
        if (message.data1 != pitch) continue;
        if (is_note_on(message)) {
            ++open_count;
            if (open_count > max_open) max_open = open_count;
        } else if (is_note_off(message)) {
            if (open_count > 0) --open_count;
        }
    }
    return max_open;
}

// True if `messages` strictly alternates on, off, on, off ... starting with on.
bool strictly_alternating(const std::vector<OpMidiMessage>& messages) {
    bool expect_on = true;
    for (const auto& message : messages) {
        if (is_note_on(message) != expect_on) return false;
        expect_on = !expect_on;
    }
    return true;
}

// In Sync mode, play a note held `gate` pulses starting at the current (primed)
// pulse, then collect every emission out to `gate + span` pulses. The dry note-on
// lands at the start pulse, the dry note-off at +gate, and echoes follow the
// release rule.
std::vector<OpMidiMessage> play_held(uint8_t pitch, uint8_t velocity, uint32_t gate, uint32_t span) {
    std::vector<OpMidiMessage> collected;
    const uint32_t base = g_pulse;
    feed_note_on(pitch, velocity);
    for (const auto& message : drain()) collected.push_back(message);
    for (uint32_t pulse_offset = 1; pulse_offset <= gate + span; ++pulse_offset) {
        for (const auto& message : advance_to_pulse(base + pulse_offset)) collected.push_back(message);
        if (pulse_offset == gate) {
            feed_note_off(pitch);
            for (const auto& message : drain()) collected.push_back(message);
        }
    }
    return collected;
}

}  // namespace

// Rate -> delta. A short note (gate < delta) echoes at on+delta,
// note-relative on the us tick (Sync delta = delta_pulses x pulse_period_us).
TEST_CASE("sync division maps to pulses") {
    struct Cell {
        int32_t rate;
        uint32_t pulses;
    };
    const Cell cells[] = {
        {kRate_2,    192},
        {kRate_1,    96 },
        {kRate_1_2,  48 },
        {kRate_1_4,  24 },
        {kRate_1_8,  12 },
        {kRate_1_16, 6  },
        {kRate_1_32, 3  },
    };
    for (const auto& cell : cells) {
        const uint32_t base = sync_reset(cell.rate, kRhythm_Straight,
                                         /*repeats=*/8,
                                         /*feedback=*/100,
                                         /*transpose=*/0);
        feed_note_on(60, 100);
        advance_to_pulse(base + 1);  // gate 1 pulse < delta
        feed_note_off(60);
        drain();

        // Echo 1 lands delta after the note-on (pulse base + cell.pulses).
        CHECK(note_ons(advance_to_pulse(base + cell.pulses - 1)).empty());
        auto on_messages = note_ons(advance_to_pulse(base + cell.pulses));
        REQUIRE(on_messages.size() >= 1);
        CHECK(on_messages[0].data1 == 60);

        mode_destroy();
    }
}

// Rhythm enum modifies the division (Dotted x3/2, Triplet x2/3).
TEST_CASE("sync dotted and triplet pulses") {
    struct Cell {
        int32_t rate;
        int32_t feel;
        uint32_t pulses;
    };
    const Cell cells[] = {
        {kRate_2,    kRhythm_Dotted,  288},
        {kRate_1,    kRhythm_Dotted,  144},
        {kRate_1_2,  kRhythm_Dotted,  72 },
        {kRate_1_4,  kRhythm_Dotted,  36 },
        {kRate_1_8,  kRhythm_Dotted,  18 },
        {kRate_1_16, kRhythm_Dotted,  9  },
        {kRate_1_32, kRhythm_Dotted,  5  },
        {kRate_2,    kRhythm_Triplet, 128},
        {kRate_1,    kRhythm_Triplet, 64 },
        {kRate_1_2,  kRhythm_Triplet, 32 },
        {kRate_1_4,  kRhythm_Triplet, 16 },
        {kRate_1_8,  kRhythm_Triplet, 8  },
        {kRate_1_16, kRhythm_Triplet, 4  },
        {kRate_1_32, kRhythm_Triplet, 2  },
    };
    for (const auto& cell : cells) {
        CHECK(cell.pulses >= 1u);
        const uint32_t base = sync_reset(cell.rate, cell.feel,
                                         /*repeats=*/8,
                                         /*feedback=*/100,
                                         /*transpose=*/0);
        feed_note_on(64, 100);
        advance_to_pulse(base + 1);
        feed_note_off(64);
        drain();

        CHECK(note_ons(advance_to_pulse(base + cell.pulses - 1)).empty());
        auto on_messages = note_ons(advance_to_pulse(base + cell.pulses));
        REQUIRE(on_messages.size() >= 1);
        CHECK(on_messages[0].data1 == 64);

        mode_destroy();
    }
}

// Sync=off free-run. Echo 1 fires at on_tick + Timex1000 us.
TEST_CASE("free-run ms delay fires at tick") {
    reset_mode();
    set_params(
        /*sync=*/0, kRate_1_8, kRhythm_Straight,
        /*time=*/250,
        /*repeats=*/3,
        /*feedback=*/100,
        /*transpose=*/0);

    feed_note_on(72, 100);  // dry note at tick 0
    op::sim::advance_tick(1000);
    feed_note_off(72);  // gate 1000 us < delta 250000
    drain();

    op::sim::advance_tick(248999);  // total 249999
    tick_engine();
    CHECK(note_ons(drain()).empty());

    op::sim::advance_tick(1);  // total 250000
    tick_engine();
    auto on_messages = note_ons(drain());
    REQUIRE(on_messages.size() >= 1);
    CHECK(on_messages[0].data1 == 72);

    mode_destroy();
}

// A note played off the pulse grid echoes exactly delta after its true arrival,
// never snapped to a pulse edge.
TEST_CASE("sync echo is note-relative not grid-quantized") {
    const uint32_t base     = sync_reset(kRate_1_16, kRhythm_Straight,
                                         /*repeats=*/1,
                                         /*feedback=*/100,
                                         /*transpose=*/0);
    const uint32_t delta_us = 6u * kPulseUs;  // 1/16 = 6 pulses

    (void)base;
    // Play the note off-grid, 300 us past a pulse edge and not a pulse multiple.
    // The note-off locks the echo's us schedule (echo1 = on_tick + delta_us), so
    // the us tick can be driven directly from here. The primed period is already
    // captured.
    op::sim::advance_tick(300);
    const uint32_t on_tick = op::sim::get_api()->get_tick();  // base*kPulseUs + 300
    feed_note_on(60, 100);
    op::sim::advance_tick(50);  // short gate
    feed_note_off(60);
    drain();

    const uint32_t target = on_tick + delta_us;  // 300 us past a pulse edge

    // Just before the target, and already past the pulse edge at target - 300.
    // Nothing has fired yet.
    op::sim::advance_tick(target - 1 - op::sim::get_api()->get_tick());
    tick_engine();
    CHECK(note_ons(drain()).empty());

    // Exactly at the off-grid target the echo fires, tracking the note's
    // micro-timing to the us.
    op::sim::advance_tick(1);
    tick_engine();
    auto on_messages = note_ons(drain());
    REQUIRE(on_messages.size() >= 1);
    CHECK(on_messages[0].data1 == 60);

    mode_destroy();
}

// gate == delta gives back-to-back repeats whose same-pitch stream stays
// strictly alternating.
TEST_CASE("gate equals delta same-pitch echo stream is strictly alternating") {
    const uint32_t base = sync_reset(kRate_1_8, kRhythm_Straight,
                                     /*repeats=*/3,
                                     /*feedback=*/70,
                                     /*transpose=*/0);
    (void)base;
    constexpr uint8_t kPitch  = 67;
    constexpr uint32_t kDelta = 12;  // Rate 1/8 -> 12 pulses

    auto stream = pitch_stream(play_held(kPitch, 100, /*gate=*/kDelta, /*span=*/kDelta * 4), kPitch);

    REQUIRE(stream.size() >= 2);
    CHECK(note_ons(stream).size() == note_offs(stream).size());
    CHECK(is_note_on(stream.front()));
    CHECK(strictly_alternating(stream));
    CHECK(note_ons(stream).size() == 4);  // dry + Repeats echoes

    mode_destroy();
}

// echoes are release-triggered. While held, no echo, the train
// fires on the note-off.
TEST_CASE("held note defers echoes until release") {
    const uint32_t base  = sync_reset(kRate_1_16, kRhythm_Straight,
                                      /*repeats=*/3,
                                      /*feedback=*/100,
                                      /*transpose=*/0);
    const uint32_t delta = 6;

    feed_note_on(60, 100);
    drain();

    for (uint32_t k = 1; k <= 4; ++k)
        CHECK(note_ons(advance_to_pulse(base + k * delta)).empty());  // held, so no echo

    feed_note_off(60);
    auto rel = drain();
    CHECK(note_offs(rel).size() >= 1);  // dry note-off
    auto on_messages = note_ons(rel);
    REQUIRE(on_messages.size() >= 1);  // echo 1 fires at release
    CHECK(on_messages[0].data1 == 60);

    mode_destroy();
}

// exponential velocity decay (vel x fb/100) across the echoes.
TEST_CASE("feedback decays velocity") {
    const uint32_t base  = sync_reset(kRate_1_16, kRhythm_Straight,
                                      /*repeats=*/3,
                                      /*feedback=*/70,
                                      /*transpose=*/0);
    const uint32_t delta = 6;

    feed_note_on(60, 100);
    advance_to_pulse(base + 1);
    feed_note_off(60);
    drain();

    const uint8_t expected[] = {70, 49, 34};
    for (uint32_t k = 1; k <= 3; ++k) {
        auto on_messages = note_ons(advance_to_pulse(base + k * delta));
        REQUIRE(on_messages.size() >= 1);
        CHECK(on_messages[0].data1 == 60);
        CHECK(on_messages[0].data2 == expected[k - 1]);
    }
    mode_destroy();
}

// velocity < 1 stops the trail, never emit a vel-0 note-on.
TEST_CASE("velocity below 1 stops trail") {
    const uint32_t base  = sync_reset(kRate_1_16, kRhythm_Straight,
                                      /*repeats=*/8,
                                      /*feedback=*/20,
                                      /*transpose=*/0);
    const uint32_t delta = 6;

    feed_note_on(60, 4);  // echo1 = floor(4*20/100)=0 -> no echoes
    advance_to_pulse(base + 1);
    feed_note_off(60);
    drain();

    for (uint32_t k = 1; k <= 8; ++k) {
        auto collected = advance_to_pulse(base + k * delta);
        for (const auto& message : collected) {
            const bool vel0_note_on = ((message.status & 0xF0u) == 0x90u) && (message.data2 == 0u);
            CHECK_FALSE(vel0_note_on);
        }
        CHECK(note_ons(collected).empty());
    }
    mode_destroy();
}

// Repeats caps echo count (Repeats=N => exactly N echoes).
TEST_CASE("repeats caps echo count") {
    auto count_echoes = [](int32_t repeats) -> int {
        const uint32_t base  = sync_reset(kRate_1_16, kRhythm_Straight, repeats,
                                          /*feedback=*/100,
                                          /*transpose=*/0);
        const uint32_t delta = 6;
        feed_note_on(60, 100);
        advance_to_pulse(base + 1);
        feed_note_off(60);
        drain();

        int echoes = 0;
        for (uint32_t k = 1; k <= static_cast<uint32_t>(repeats) + 3; ++k)
            echoes += static_cast<int>(note_ons(advance_to_pulse(base + k * delta)).size());
        mode_destroy();
        return echoes;
    };
    CHECK(count_echoes(1) == 1);
    CHECK(count_echoes(3) == 3);
    CHECK(count_echoes(5) == 5);
}

// Transpose shifts each echo by echo_index x transpose, and an echo that leaves
// the MIDI range is dropped.
TEST_CASE("transpose shifts echoes and drops out of range") {
    uint32_t base        = sync_reset(kRate_1_16, kRhythm_Straight,
                                      /*repeats=*/3,
                                      /*feedback=*/100,
                                      /*transpose=*/12);
    const uint32_t delta = 6;

    feed_note_on(60, 100);
    advance_to_pulse(base + 1);
    feed_note_off(60);
    drain();

    const uint8_t expected_pitch[] = {72, 84, 96};  // 60 + k*12
    for (uint32_t k = 1; k <= 3; ++k) {
        auto on_messages = note_ons(advance_to_pulse(base + k * delta));
        REQUIRE(on_messages.size() >= 1);
        CHECK(on_messages[0].data1 == expected_pitch[k - 1]);
    }
    mode_destroy();

    // Out of range, 120 + 1*24 = 144 > 127, so the first echo is dropped and the
    // trail ends.
    base = sync_reset(kRate_1_16, kRhythm_Straight,
                      /*repeats=*/3,
                      /*feedback=*/100,
                      /*transpose=*/24);
    feed_note_on(120, 100);
    advance_to_pulse(base + 1);
    feed_note_off(120);
    drain();
    for (uint32_t k = 1; k <= 4; ++k) CHECK(note_ons(advance_to_pulse(base + k * delta)).empty());

    mode_destroy();
}

// A same-pitch re-press while held releases the old note (its note-off) before
// the fresh note-on.
TEST_CASE("same-pitch retrigger releases old note") {
    const uint32_t base = sync_reset(kRate_1_8, kRhythm_Straight,
                                     /*repeats=*/3,
                                     /*feedback=*/100,
                                     /*transpose=*/0);
    feed_note_on(60, 100);
    drain();  // held

    advance_to_pulse(base + 3);
    OpMidiMessage second_press {kNoteOn, 60, 90, kTestOutput, 3};
    mode_process(&second_press, 1, op::sim::get_api()->get_tick());
    auto collected = drain();

    REQUIRE(note_offs(collected).size() >= 1);
    REQUIRE(note_ons(collected).size() >= 1);
    CHECK(note_offs(collected)[0].data1 == 60);
    CHECK(note_ons(collected)[0].data1 == 60);
    CHECK(note_ons(collected)[0].data2 == 90);  // fresh velocity, not the old voice

    mode_destroy();
}

// a full pool steals the oldest voice, emitting its note-off
// before reuse (no hang).
TEST_CASE("pool full steals oldest with note-off") {
    sync_reset(kRate_1_16, kRhythm_Straight,
               /*repeats=*/8,
               /*feedback=*/100,
               /*transpose=*/0);

    for (uint8_t i = 0; i < 32; ++i) feed_note_on(static_cast<uint8_t>(60 + i), 100);
    drain();  // 32 held dry notes (pool full, matches kPoolSize)

    feed_note_on(92, 100);  // 33rd -> steals oldest (pitch 60)
    auto off_messages = note_offs(drain());
    bool off60        = false;
    for (const auto& message : off_messages)
        if (message.data1 == 60) off60 = true;
    CHECK(off60);

    mode_destroy();
}

// transport Stop closes the held dry note and any open echo.
TEST_CASE("transport stop flushes pending") {
    const uint32_t delta = 6;

    // (a) Stop while a note is held -> the dry note is closed.
    sync_reset(kRate_1_16, kRhythm_Straight,
               /*repeats=*/3,
               /*feedback=*/100,
               /*transpose=*/0);
    feed_note_on(60, 100);
    drain();
    OpMidiMessage stop {0xFC, 0, 0, kTestOutput, 3};
    mode_process(&stop, 1, op::sim::get_api()->get_tick());
    {
        auto off_messages = note_offs(drain());
        bool off60        = false;
        for (const auto& message : off_messages)
            if (message.data1 == 60) off60 = true;
        CHECK(off60);
    }

    // (b) Stop while echoing -> the one open echo is closed.
    uint32_t base = sync_reset(kRate_1_16, kRhythm_Straight,
                               /*repeats=*/3,
                               /*feedback=*/100,
                               /*transpose=*/0);
    feed_note_on(62, 100);
    advance_to_pulse(base + 1);
    feed_note_off(62);  // gate 1 < delta
    drain();
    auto echo1 = note_ons(advance_to_pulse(base + delta));  // echo 1 now open
    REQUIRE(echo1.size() >= 1);
    OpMidiMessage stop2 {0xFC, 0, 0, kTestOutput, 3};
    mode_process(&stop2, 1, op::sim::get_api()->get_tick());
    {
        auto off_messages = note_offs(drain());
        bool off62        = false;
        for (const auto& message : off_messages)
            if (message.data1 == 62) off62 = true;
        CHECK(off62);
    }
    CHECK(note_ons(advance_to_pulse(base + delta * 8)).empty());  // no more echoes

    mode_destroy();
}

// non-note messages pass through dry.
TEST_CASE("non-note passes through dry") {
    reset_mode();
    set_params(
        /*sync=*/1, kRate_1_16, kRhythm_Straight,
        /*time=*/250,
        /*repeats=*/3,
        /*feedback=*/100,
        /*transpose=*/0);

    OpMidiMessage control_change {0xB0, 7, 100, kTestOutput, 3};
    mode_process(&control_change, 1, op::sim::get_api()->get_tick());
    CHECK(control_change.status == 0xB0);  // left intact, so it passes through dry
    CHECK(drain().empty());                // mode emits nothing for a non-note

    mode_destroy();
}

// destroy() releases every alive voice (no stuck notes on reload).
TEST_CASE("destroy releases all voices") {
    uint32_t base = sync_reset(kRate_1_16, kRhythm_Straight,
                               /*repeats=*/3,
                               /*feedback=*/100,
                               /*transpose=*/0);
    feed_note_on(60, 100);
    feed_note_on(67, 100);
    drain();  // two held dry notes

    mode_destroy();
    auto off_messages = note_offs(drain());
    bool off60 = false, off67 = false;
    for (const auto& message : off_messages) {
        if (message.data1 == 60) off60 = true;
        if (message.data1 == 67) off67 = true;
    }
    CHECK(off60);
    CHECK(off67);

    // Fresh init works, and a new short note echoes cleanly.
    base                 = sync_reset(kRate_1_16, kRhythm_Straight,
                                      /*repeats=*/3,
                                      /*feedback=*/100,
                                      /*transpose=*/0);
    const uint32_t delta = 6;
    feed_note_on(72, 100);
    advance_to_pulse(base + 1);
    feed_note_off(72);
    drain();
    auto fresh = note_ons(advance_to_pulse(base + delta));
    REQUIRE(fresh.size() >= 1);
    CHECK(fresh[0].data1 == 72);

    mode_destroy();
}

// a held note (gate > delta) yields back-to-back delta-length
// repeats trailing the release, strictly alternating (no same-pitch overlap).
TEST_CASE("held note echoes are back-to-back delta-length after release") {
    sync_reset(kRate_1_16, kRhythm_Straight,
               /*repeats=*/3,
               /*feedback=*/100,
               /*transpose=*/0);
    const uint32_t delta = 6;

    auto stream = pitch_stream(play_held(60, 100, /*gate=*/2 * delta, /*span=*/delta * 4), 60);

    REQUIRE(stream.size() >= 2);
    CHECK(is_note_on(stream.front()));
    CHECK(strictly_alternating(stream));
    CHECK(note_ons(stream).size() == note_offs(stream).size());
    CHECK(note_ons(stream).size() == 4);  // dry + Repeats echoes

    auto on_messages = note_ons(stream);
    REQUIRE(on_messages.size() == 4);
    CHECK(on_messages[0].data2 == 100);  // dry at full velocity

    mode_destroy();
}

// With Transpose, two voices' echoes can land on the same output pitch. Under
// Transpose -12 an octave-down jump sends source-72's 2nd echo and source-60's
// 1st echo both to 48. The global one-note-per-pitch serializer keeps every
// output pitch at concurrency <= 1.
TEST_CASE("cross-voice transposed echoes never overlap on one pitch") {
    const uint32_t base  = sync_reset(kRate_1_16, kRhythm_Straight,
                                      /*repeats=*/2,
                                      /*feedback=*/100,
                                      /*transpose=*/-12);
    const uint32_t delta = 6;

    std::vector<OpMidiMessage> all;
    auto take = [&](const std::vector<OpMidiMessage>& messages) {
        for (const auto& message : messages) all.push_back(message);
    };

    // Octave-jump melody of short notes (gate < delta). source 72 echoes to 60,
    // 48, source 60 (one delta later) is a dry note at 60 (collides 72's echo1)
    // and echoes to 48 (collides 72's echo2), 36.
    feed_note_on(72, 100);
    take(drain());
    take(advance_to_pulse(base + 1));
    feed_note_off(72);
    take(drain());
    take(advance_to_pulse(base + delta));
    feed_note_on(60, 100);
    take(drain());
    take(advance_to_pulse(base + delta + 1));
    feed_note_off(60);
    take(drain());
    take(advance_to_pulse(base + delta * 8));  // let every echo play out

    // No output pitch is ever open twice at once, and every pitch balances, so
    // on count == off count and nothing hangs.
    for (uint8_t pitch = 20; pitch < 100; ++pitch) {
        CHECK(max_concurrency(all, pitch) <= 1);
        int on_messages = 0, off_messages = 0;
        for (const auto& message : all)
            if (message.data1 == pitch) {
                if (is_note_on(message))
                    ++on_messages;
                else if (is_note_off(message))
                    ++off_messages;
            }
        CHECK(on_messages == off_messages);
    }

    // Dry-note priority means the played note 60 sounds.
    bool dry60 = false;
    for (const auto& message : all)
        if (is_note_on(message) && message.data1 == 60 && message.data2 == 100) dry60 = true;
    CHECK(dry60);

    mode_destroy();
}

// Bypass flushes voices once on engage, then passes input through dry until it
// is cleared, and clearing resumes normal operation. An unowned note-off, such as
// one held across the toggle, is passed through so it never strands. Free-run
// timing keeps this deterministic.
TEST_CASE("bypass flushes voices and passes input through") {
    reset_mode();
    set_params(
        /*sync=*/0, kRate_1_8, kRhythm_Straight,
        /*time=*/250,
        /*repeats=*/3,
        /*feedback=*/100,
        /*transpose=*/0);
    const auto* api = op::sim::get_api();

    // --- (a) Not bypassed. The mode owns the note and emits the dry note plus
    //         the release-triggered echo train. ---
    CHECK(feed_note_on_status(60, 100) == 0);  // owned -> suppressed
    op::sim::advance_tick(1000);               // short gate < delta (250000)
    CHECK(feed_note_off_status(60) == 0);      // owned -> suppressed
    {
        auto rel = drain();
        CHECK(note_offs(rel).size() >= 1);  // dry note-off emitted
    }
    op::sim::advance_tick(250000);  // reach echo 1
    tick_engine();
    CHECK(note_ons(drain()).size() >= 1);  // echo train is alive

    // --- (b) Engage Bypass with a note sounding. The transition flushes it with
    //         a note-off and cuts the engine. ---
    feed_note_on(62, 100);  // held dry note now sounding
    drain();
    api->set_param_value(kParamBypass, 1);
    tick_engine();
    {
        auto flushed = note_offs(drain());
        CHECK(flushed.size() >= 1);  // the sounding voice was closed by the flush
    }

    // --- (c) While bypassed, a note-on is passed through. The mode emits nothing,
    //         leaves the message intact, and ticks produce no echoes. ---
    CHECK(feed_note_on_status(64, 100) == (kNoteOn | 0));  // passthrough, not owned
    CHECK(drain().empty());                                // mode emitted nothing
    op::sim::advance_tick(250000);
    tick_engine();
    CHECK(drain().empty());  // no echoes while bypassed

    // --- (d) A note-off for a pitch the mode does not own, whose note-on passed
    //         through during bypass, is passed through too, so nothing strands. ---
    CHECK(feed_note_off_status(64) == (kNoteOff | 0));  // passthrough -> closes externally
    CHECK(drain().empty());

    // --- (e) Disengage Bypass and ownership resumes. A fresh note-on is owned
    //         and the mode emits the dry note again. ---
    api->set_param_value(kParamBypass, 0);
    CHECK(feed_note_on_status(67, 100) == 0);  // owned again -> suppressed
    {
        auto resumed = note_ons(drain());
        REQUIRE(resumed.size() >= 1);
        CHECK(resumed[0].data1 == 67);  // dry note re-emitted by the mode
    }

    mode_destroy();
}

// The Channel filter gates ownership by MIDI channel. With Channel set to option
// 3 (MIDI channel 2), a note on channel 2 is owned and the mode emits the dry note
// plus echoes, while a note on any other channel passes through and the mode emits
// nothing. Channel = Omni (0) owns every channel. Free-run timing keeps this
// deterministic.
TEST_CASE("channel filter delays only the selected channel") {
    const auto* api = op::sim::get_api();

    // --- (a) Channel = option 3 owns only MIDI channel 2. ---
    reset_mode();
    set_params(
        /*sync=*/0, kRate_1_8, kRhythm_Straight,
        /*time=*/250,
        /*repeats=*/1,  // one echo, so the owned voice finishes before the
                        // off-channel checks below
        /*feedback=*/100,
        /*transpose=*/0);
    api->set_param_value(kParamChannel, 3);  // MIDI channel 2

    // A note on the selected channel (2) is owned, so the dry note is emitted.
    CHECK(feed_note_on_status(60, 100, /*channel=*/2) == 0);  // owned -> suppressed
    {
        auto on_messages = note_ons(drain());
        REQUIRE(on_messages.size() >= 1);
        CHECK(on_messages[0].data1 == 60);  // dry note emitted by the mode
    }
    op::sim::advance_tick(1000);                          // short gate < delta (250000)
    CHECK(feed_note_off_status(60, /*channel=*/2) == 0);  // owned -> suppressed
    drain();
    op::sim::advance_tick(250000);  // reach echo 1
    tick_engine();
    CHECK(note_ons(drain()).size() >= 1);  // echo train alive for channel 2

    // A note on a different channel (5) is not owned, so it passes through with
    // no emission.
    CHECK(feed_note_on_status(64, 100, /*channel=*/5)
          == static_cast<uint8_t>(kNoteOn | 5));  // passthrough, not owned
    CHECK(drain().empty());                       // mode emitted nothing
    op::sim::advance_tick(250000);
    tick_engine();
    CHECK(drain().empty());  // no echoes for the off-channel note
    // Its note-off also passes through (mode owns no voice for it).
    CHECK(feed_note_off_status(64, /*channel=*/5) == static_cast<uint8_t>(kNoteOff | 5));
    CHECK(drain().empty());

    mode_destroy();

    // --- (b) Channel = Omni (0) owns every channel. ---
    reset_mode();
    set_params(
        /*sync=*/0, kRate_1_8, kRhythm_Straight,
        /*time=*/250,
        /*repeats=*/3,
        /*feedback=*/100,
        /*transpose=*/0);
    api->set_param_value(kParamChannel, 0);  // Omni

    CHECK(feed_note_on_status(60, 100, /*channel=*/2) == 0);  // owned
    {
        auto on_messages = note_ons(drain());
        REQUIRE(on_messages.size() >= 1);
        CHECK(on_messages[0].data1 == 60);
    }
    CHECK(feed_note_on_status(72, 100, /*channel=*/9) == 0);  // owned on another channel too
    {
        auto on_messages = note_ons(drain());
        REQUIRE(on_messages.size() >= 1);
        CHECK(on_messages[0].data1 == 72);
    }

    mode_destroy();
}
