// Custom Scale Tuner sample, doctest behavior suite.
//
// The mode retunes each incoming note and emits it on the port that note arrived
// on, so a retuned stream lands on one output and never fans out. These cases
// cover scale parsing, MPE channel spread, root-key shifts, and the contract that
// a message the mode consumes has its status cleared so it is never sent twice.
//
// MPE is per-instance, so each routed output toggles it on its own. A quantized
// Out 1 can run alongside an MPE Out 2 on the same device.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <operator_sdk_sim.h>
#include <operator_sdk.h>

#include <cstdint>
#include <cstring>
#include <vector>

OP_MODE_UNDER_TEST();

namespace {

constexpr uint8_t kParamScaleFile  = 0;
constexpr uint8_t kParamRootKey    = 1;
constexpr uint8_t kParamRootFreqHz = 2;
constexpr uint8_t kParamBendRange  = 3;
constexpr uint8_t kParamMpe        = 4;

// The mode emits on the port a note arrived on. Tests inject on port 0, so they
// drain output 0 and check the other outputs stay silent.
constexpr uint8_t kTargetOutput = 0;
constexpr uint8_t kOutputCount  = 9;  // OUT 1..8 at index 0..7, USB at index 8

uint32_t count_note_ons_on_output(uint8_t output_index) {
    OpMidiMessage buffer[64] {};
    uint32_t note_on_count = 0;
    while (true) {
        auto drained = op::sim::drain_outgoing_midi(output_index, buffer, 64);
        if (drained == 0) break;
        for (uint16_t i = 0; i < drained; ++i) {
            if ((buffer[i].status & 0xF0) == 0x90 && buffer[i].data2 != 0) ++note_on_count;
        }
    }
    return note_on_count;
}

// A note-off is either an explicit 0x80 status or a 0x90 status with velocity 0
// (the running-status release convention).
uint32_t count_note_offs_on_output(uint8_t output_index) {
    OpMidiMessage buffer[64] {};
    uint32_t note_off_count = 0;
    while (true) {
        auto drained = op::sim::drain_outgoing_midi(output_index, buffer, 64);
        if (drained == 0) break;
        for (uint16_t i = 0; i < drained; ++i) {
            const uint8_t status_hi = buffer[i].status & 0xF0;
            if (status_hi == 0x80 || (status_hi == 0x90 && buffer[i].data2 == 0)) ++note_off_count;
        }
    }
    return note_off_count;
}

uint32_t count_pitch_bends_on_output(uint8_t output_index) {
    OpMidiMessage buffer[64] {};
    uint32_t pitch_bend_count = 0;
    while (true) {
        auto drained = op::sim::drain_outgoing_midi(output_index, buffer, 64);
        if (drained == 0) break;
        for (uint16_t i = 0; i < drained; ++i) {
            if ((buffer[i].status & 0xF0) == 0xE0) ++pitch_bend_count;
        }
    }
    return pitch_bend_count;
}

uint32_t count_ccs_on_output(uint8_t output_index) {
    OpMidiMessage buffer[64] {};
    uint32_t control_change_count = 0;
    while (true) {
        auto drained = op::sim::drain_outgoing_midi(output_index, buffer, 64);
        if (drained == 0) break;
        for (uint16_t i = 0; i < drained; ++i) {
            if ((buffer[i].status & 0xF0) == 0xB0) ++control_change_count;
        }
    }
    return control_change_count;
}

// The note numbers a drained batch attacks and releases, in the order they were
// sent.
std::vector<uint8_t> note_on_pitches(const std::vector<OpMidiMessage>& messages) {
    std::vector<uint8_t> pitches;
    for (const auto& message : messages) {
        if ((message.status & 0xF0) == 0x90 && message.data2 != 0) pitches.push_back(message.data1);
    }
    return pitches;
}

std::vector<uint8_t> note_off_pitches(const std::vector<OpMidiMessage>& messages) {
    std::vector<uint8_t> pitches;
    for (const auto& message : messages) {
        const uint8_t status_hi = message.status & 0xF0;
        if (status_hi == 0x80 || (status_hi == 0x90 && message.data2 == 0)) pitches.push_back(message.data1);
    }
    return pitches;
}

std::vector<OpMidiMessage> drain_output(uint8_t output_index) {
    std::vector<OpMidiMessage> collected;
    OpMidiMessage buffer[64] {};
    while (true) {
        auto drained = op::sim::drain_outgoing_midi(output_index, buffer, 64);
        if (drained == 0) break;
        for (uint16_t i = 0; i < drained; ++i) collected.push_back(buffer[i]);
    }
    return collected;
}

const char kTwelveTet[] = "! 12-TET baseline\n"
                          "12-TET\n"
                          "12\n"
                          " 100.0\n"
                          " 200.0\n"
                          " 300.0\n"
                          " 400.0\n"
                          " 500.0\n"
                          " 600.0\n"
                          " 700.0\n"
                          " 800.0\n"
                          " 900.0\n"
                          "1000.0\n"
                          "1100.0\n"
                          "1200.0\n";

// A quarter-tone scale of 24 equal divisions of an octave (50 cents each). Every
// degree has a non-zero retune offset under the standard root_key=69 mapping, so
// the legato case can assert a bend value away from the 8192 center.
const char kQuarterTone[] = "! quarter-tone (24-EDO)\n"
                            "24-EDO\n"
                            "24\n"
                            " 50.0\n  100.0\n  150.0\n  200.0\n"
                            " 250.0\n  300.0\n  350.0\n  400.0\n"
                            " 450.0\n  500.0\n  550.0\n  600.0\n"
                            " 650.0\n  700.0\n  750.0\n  800.0\n"
                            " 850.0\n  900.0\n  950.0\n 1000.0\n"
                            "1050.0\n 1100.0\n 1150.0\n 1200.0\n";

const char kMalformed[] = "bad\n0\n";

void setup_mode_with_scale(const char* payload, uint32_t size) {
    op::sim::reset_state();
    op::sim::set_current_mode_name("custom_scale_tuner");
    op::sim::seed_extras_file("custom_scale_tuner", "test.scl", reinterpret_cast<const uint8_t*>(payload),
                              size);
    const auto* api = op::sim::get_api();
    mode_init(api);
    // param<ScaleFile>() returns the filename the user picked, not a list index,
    // so the test seeds a picked filename. Without it the mode sees an empty
    // filename, never loads a scale, and falls back to pass-through.
    op::sim::pick_filename(kParamScaleFile, "test.scl");
    api->set_param_value(kParamRootKey, 69);
    api->set_param_value(kParamRootFreqHz, 44000);
    api->set_param_value(kParamBendRange, 2);
    api->set_param_value(kParamMpe, 0);
}

void inject_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    OpMidiMessage message {
        /*status*/ static_cast<uint8_t>(0x90 | (channel & 0x0F)),
        /*data1*/ note,
        /*data2*/ velocity,
        /*port*/ 0,
        /*length*/ 3,
    };
    mode_process(&message, 1, op::sim::get_api()->get_tick());
}

void inject_note_off(uint8_t channel, uint8_t note) {
    OpMidiMessage message {
        /*status*/ static_cast<uint8_t>(0x80 | (channel & 0x0F)),
        /*data1*/ note,
        /*data2*/ 64,  // release velocity
        /*port*/ 0,
        /*length*/ 3,
    };
    mode_process(&message, 1, op::sim::get_api()->get_tick());
}

// These variants keep the message in the caller's scope so a test can read
// message.status and message.length after mode_process returns. A message the
// mode consumes comes back with its status cleared, which is how the mode signals
// that it owns the message and nothing else should send it.
OpMidiMessage inject_note_on_observe(uint8_t channel, uint8_t note, uint8_t velocity) {
    OpMidiMessage message {
        /*status*/ static_cast<uint8_t>(0x90 | (channel & 0x0F)),
        /*data1*/ note,
        /*data2*/ velocity,
        /*port*/ 0,
        /*length*/ 3,
    };
    mode_process(&message, 1, op::sim::get_api()->get_tick());
    return message;
}

OpMidiMessage inject_note_off_observe(uint8_t channel, uint8_t note) {
    OpMidiMessage message {
        /*status*/ static_cast<uint8_t>(0x80 | (channel & 0x0F)),
        /*data1*/ note,
        /*data2*/ 64,
        /*port*/ 0,
        /*length*/ 3,
    };
    mode_process(&message, 1, op::sim::get_api()->get_tick());
    return message;
}

}  // namespace

// ---------------------------------------------------------------------------
// Single-output emission
// ---------------------------------------------------------------------------

TEST_CASE("retunes single output only") {
    // The mode emits on exactly one output, the one the note came in on, and
    // never fans out to the others.
    setup_mode_with_scale(kTwelveTet, sizeof(kTwelveTet) - 1);

    inject_note_on(0, 60, 100);

    CHECK(count_note_ons_on_output(kTargetOutput) == 1);
    for (uint8_t output_index = 0; output_index < kOutputCount; ++output_index) {
        if (output_index == kTargetOutput) continue;
        CHECK(count_note_ons_on_output(output_index) == 0);
    }

    mode_destroy();
}

// ---------------------------------------------------------------------------
// Message ownership. A message the mode retunes comes back with its status
// cleared so it is never sent twice, and a message the mode leaves alone comes
// back intact so it passes through to the output.
// ---------------------------------------------------------------------------

TEST_CASE("non-MPE note-on is consumed and emits exactly one retuned note") {
    setup_mode_with_scale(kTwelveTet, sizeof(kTwelveTet) - 1);
    drain_output(kTargetOutput);  // discard startup noise

    const auto message = inject_note_on_observe(0, 60, 100);

    CHECK(message.status == 0);  // consumed by the mode
    CHECK(count_note_ons_on_output(kTargetOutput) == 1);

    mode_destroy();
}

TEST_CASE("non-MPE note-off is consumed and emits exactly one retuned note-off") {
    // Inject a note-on first so the channel is active and the note-off has
    // something to release.
    setup_mode_with_scale(kTwelveTet, sizeof(kTwelveTet) - 1);
    drain_output(kTargetOutput);

    inject_note_on(0, 60, 100);
    drain_output(kTargetOutput);  // discard the note-on sequence

    const auto message = inject_note_off_observe(0, 60);

    CHECK(message.status == 0);
    CHECK(count_note_offs_on_output(kTargetOutput) == 1);

    mode_destroy();
}

TEST_CASE("pitch wheel is composed with the retune and the raw wheel is consumed") {
    // The mode composes the wheel with each channel's retune offset and sends the
    // combined bend, then consumes the raw wheel message so the composed value is
    // what reaches the synth.
    //
    // 24-EDO keeps the retune offsets non-zero, so the composed bend is
    // distinguishable from the 8192 center.
    setup_mode_with_scale(kQuarterTone, sizeof(kQuarterTone) - 1);
    drain_output(kTargetOutput);

    inject_note_on(0, 60, 100);  // activate channel 0 so the bend re-emit fires
    drain_output(kTargetOutput);

    OpMidiMessage message {
        /*status*/ 0xE0,  // pitch wheel on channel 0
        /*data1*/ 0x40,   // LSB
        /*data2*/ 0x20,   // MSB (any non-center value)
        /*port*/ 0,
        /*length*/ 3,
    };
    mode_process(&message, 1, op::sim::get_api()->get_tick());

    CHECK(message.status == 0);                              // raw wheel consumed
    CHECK(count_pitch_bends_on_output(kTargetOutput) >= 1);  // composed bend emitted

    mode_destroy();
}

TEST_CASE("non-note messages are left intact for pass-through") {
    // The mode never re-emits CC, program change, aftertouch, clock, or SysEx. It
    // leaves them untouched, and on the device an untouched message reaches the
    // output on its own. The sim does not run that pass-through, so it observes
    // zero CCs on the output, which is the expected half of the contract here.
    setup_mode_with_scale(kTwelveTet, sizeof(kTwelveTet) - 1);
    drain_output(kTargetOutput);

    OpMidiMessage message {
        /*status*/ 0xB0,  // CC channel 0
        /*data1*/ 7,      // CC#7 (channel volume)
        /*data2*/ 100,
        /*port*/ 0,
        /*length*/ 3,
    };
    mode_process(&message, 1, op::sim::get_api()->get_tick());

    CHECK(message.status == 0xB0);  // left intact
    CHECK(message.length == 3);
    CHECK(count_ccs_on_output(kTargetOutput) == 0);  // the mode emitted nothing

    mode_destroy();
}

// ---------------------------------------------------------------------------
// Scale loading
// ---------------------------------------------------------------------------

TEST_CASE("rejects malformed .scl") {
    // A malformed file leaves the mode with no scale, so it emits nothing and
    // leaves the input intact to pass through. A warning is logged, which the sim
    // prints without capturing, so this asserts the behavior that matters end to
    // end.
    setup_mode_with_scale(kMalformed, sizeof(kMalformed) - 1);
    drain_output(kTargetOutput);

    const auto message = inject_note_on_observe(0, 60, 100);

    CHECK(message.status == 0x90);  // left intact
    CHECK(message.length == 3);
    CHECK(count_note_ons_on_output(kTargetOutput) == 0);  // the mode emitted nothing

    for (uint8_t output_index = 0; output_index < kOutputCount; ++output_index) {
        if (output_index == kTargetOutput) continue;
        CHECK(count_note_ons_on_output(output_index) == 0);
    }

    mode_destroy();
}

// ---------------------------------------------------------------------------
// MPE
// ---------------------------------------------------------------------------

TEST_CASE("MPE mode uses member channels on single output") {
    // Three overlapping notes must land on three distinct member channels, which
    // are 2..16 externally and nibbles 1..15 in the status byte.
    setup_mode_with_scale(kTwelveTet, sizeof(kTwelveTet) - 1);
    const auto* api = op::sim::get_api();
    api->set_param_value(kParamMpe, 1);

    inject_note_on(0, 60, 100);
    inject_note_on(0, 64, 100);
    inject_note_on(0, 67, 100);

    const auto messages = drain_output(kTargetOutput);
    std::vector<uint8_t> channels;
    for (const auto& message : messages) {
        if ((message.status & 0xF0) == 0x90 && message.data2 != 0) {
            const uint8_t channel_nibble = message.status & 0x0F;
            CHECK(channel_nibble >= 1);
            CHECK(channel_nibble <= 15);
            channels.push_back(channel_nibble);
        }
    }
    // Three input note-ons give exactly three member-channel allocations.
    REQUIRE(channels.size() == 3);
    CHECK(channels[0] != channels[1]);
    CHECK(channels[1] != channels[2]);
    CHECK(channels[0] != channels[2]);

    // Confirm no MPE fan-out to other outputs.
    for (uint8_t output_index = 0; output_index < kOutputCount; ++output_index) {
        if (output_index == kTargetOutput) continue;
        CHECK(count_note_ons_on_output(output_index) == 0);
    }

    mode_destroy();
}

// ---------------------------------------------------------------------------
// Non-MPE held notes. Channel bend is global per MIDI channel, so a channel
// holding several notes must restore the prior note's bend as each one releases.
// ---------------------------------------------------------------------------

TEST_CASE("a folded second key leaves the first note's bend standing") {
    // Sustain C4, add C#4 while C4 is held, then release C#4. Under 24-EDO with
    // root_key=69 the two land on one whole note, so C#4 joins the note C4 is
    // already sounding: its press emits no bend of its own and its release emits
    // nothing at all. The last 0xE0 on channel 0 is still C4's retune bend, and the
    // sounding C4 keeps its pitch through both.
    setup_mode_with_scale(kQuarterTone, sizeof(kQuarterTone) - 1);
    drain_output(kTargetOutput);  // discard any startup noise

    inject_note_on(0, 60, 100);  // C4 on, channel 0
    inject_note_on(0, 61, 100);  // C#4 on, same channel, folding onto C4's note
    inject_note_off(0, 61);      // C#4 off, and C4 is still down

    const auto messages = drain_output(kTargetOutput);

    const OpMidiMessage* last_bend = nullptr;
    for (const auto& message : messages) {
        if ((message.status & 0xF0) == 0xE0 && (message.status & 0x0F) == 0) {
            last_bend = &message;
        }
    }
    REQUIRE(last_bend != nullptr);
    const uint16_t bend_14bit = static_cast<uint16_t>((static_cast<uint16_t>(last_bend->data2 & 0x7F) << 7)
                                                      | static_cast<uint16_t>(last_bend->data1 & 0x7F));
    // The last bend on channel 0 is C4's retune bend, which C#4 never displaced.
    CHECK(bend_14bit != 8192);

    mode_destroy();
}

TEST_CASE("non-MPE final note-off clears channel and emits no trailing bend") {
    // With nothing else held on the channel there is no bend to restore, so the
    // mode skips the bend emit and the note-off stands alone.
    setup_mode_with_scale(kQuarterTone, sizeof(kQuarterTone) - 1);
    drain_output(kTargetOutput);

    inject_note_on(0, 60, 100);
    drain_output(kTargetOutput);  // discard the on sequence (bend + note-on)

    inject_note_off(0, 60);
    const auto messages = drain_output(kTargetOutput);

    uint32_t note_off_count = 0, bend_count = 0;
    for (const auto& message : messages) {
        const uint8_t status_hi = message.status & 0xF0;
        if (status_hi == 0x80) ++note_off_count;
        if (status_hi == 0xE0) ++bend_count;
    }
    CHECK(note_off_count == 1);
    CHECK(bend_count == 0);

    mode_destroy();
}

TEST_CASE("nine notes held on one channel all sound and all stop") {
    // The held-note record is pooled across ports and channels, so one channel can
    // hold as many notes as the player has fingers for. Under 24-EDO each pair of
    // adjacent keys retunes to one whole note, so nine keys sound five notes and
    // each of them stops as the second key of its pair lifts.
    setup_mode_with_scale(kQuarterTone, sizeof(kQuarterTone) - 1);
    drain_output(kTargetOutput);

    for (uint8_t note = 60; note <= 68; ++note) {
        inject_note_on(0, note, 100);
    }
    const auto sounded = note_on_pitches(drain_output(kTargetOutput));

    for (uint8_t note = 60; note <= 68; ++note) {
        inject_note_off(0, note);
    }
    const auto stopped = note_off_pitches(drain_output(kTargetOutput));

    REQUIRE(sounded.size() == 5);
    // Both runs go up the keyboard, so what sounded and what stopped line up.
    CHECK(sounded == stopped);

    mode_destroy();
}

TEST_CASE("two keys retuning to one note sound it once and stop it on the last release") {
    // Under 24-EDO C4 and C#4 land on the same whole note, so the second key joins
    // the note already ringing.
    setup_mode_with_scale(kQuarterTone, sizeof(kQuarterTone) - 1);
    drain_output(kTargetOutput);

    inject_note_on(0, 60, 100);
    CHECK(count_note_ons_on_output(kTargetOutput) == 1);

    const auto second_on = inject_note_on_observe(0, 61, 100);
    CHECK(second_on.status == 0);  // consumed by the mode
    CHECK(count_note_ons_on_output(kTargetOutput) == 0);

    const auto first_off = inject_note_off_observe(0, 60);
    CHECK(first_off.status == 0);
    CHECK(count_note_offs_on_output(kTargetOutput) == 0);

    inject_note_off(0, 61);
    CHECK(count_note_offs_on_output(kTargetOutput) == 1);

    mode_destroy();
}

TEST_CASE("a note-off restores the bend of a note still held on the channel") {
    // C4 and D#4 land on different whole notes under 24-EDO and carry different
    // retune offsets, so releasing the second hands the channel back to the first.
    setup_mode_with_scale(kQuarterTone, sizeof(kQuarterTone) - 1);
    drain_output(kTargetOutput);

    inject_note_on(0, 60, 100);
    inject_note_on(0, 63, 100);
    drain_output(kTargetOutput);

    inject_note_off(0, 63);
    const auto messages = drain_output(kTargetOutput);

    const OpMidiMessage* last_bend = nullptr;
    for (const auto& message : messages) {
        if ((message.status & 0xF0) == 0xE0 && (message.status & 0x0F) == 0) {
            last_bend = &message;
        }
    }
    REQUIRE(last_bend != nullptr);
    const uint16_t bend_14bit = static_cast<uint16_t>((static_cast<uint16_t>(last_bend->data2 & 0x7F) << 7)
                                                      | static_cast<uint16_t>(last_bend->data1 & 0x7F));
    // The channel is left on C4's retune bend, so the note still sounding keeps
    // its pitch.
    CHECK(bend_14bit != 8192);

    mode_destroy();
}

TEST_CASE("a note-off for a key that was never pressed still emits") {
    setup_mode_with_scale(kQuarterTone, sizeof(kQuarterTone) - 1);
    drain_output(kTargetOutput);

    const auto message = inject_note_off_observe(0, 60);

    CHECK(message.status == 0);  // consumed by the mode
    CHECK(count_note_offs_on_output(kTargetOutput) == 1);

    mode_destroy();
}

TEST_CASE("a Root Key change under a held note leaves the note-off on the pitch that sounded") {
    setup_mode_with_scale(kQuarterTone, sizeof(kQuarterTone) - 1);
    drain_output(kTargetOutput);

    inject_note_on(0, 60, 100);
    const auto sounded = note_on_pitches(drain_output(kTargetOutput));
    REQUIRE(sounded.size() == 1);

    op::sim::get_api()->set_param_value(kParamRootKey, 60);

    inject_note_off(0, 60);
    const auto stopped = note_off_pitches(drain_output(kTargetOutput));
    REQUIRE(stopped.size() == 1);
    CHECK(stopped[0] == sounded[0]);

    // The Root Key move did take, so the same key pressed afresh lands elsewhere.
    inject_note_on(0, 60, 100);
    const auto resounded = note_on_pitches(drain_output(kTargetOutput));
    REQUIRE(resounded.size() == 1);
    CHECK(resounded[0] != sounded[0]);

    mode_destroy();
}
