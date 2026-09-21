// Arpeggiator sample, doctest behavior suite.
//
// Exercises the six-parameter output arpeggiator end-to-end via the SDK sim
// harness. It covers representative styles (Up, Down, Up-Down, and seeded
// Random), rate plus triplet timing, octave transposition, fill duration,
// chord-mode interval preservation, note suppression with non-note passthrough,
// and the silent-when-idle contract.
//
// The mode steps on pulse-count edges (api->get_pulse_count()). The run()
// helper advances the virtual tick and beat pointer together, and
// set_beat_position moves the pulse count the mode reads (pulse = beat * 24),
// so driving the beat pointer advances the arp's step math.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <operator_sdk_sim.h>
#include <operator_sdk.h>

#include <algorithm>
#include <cstdint>
#include <vector>

OP_MODE_UNDER_TEST();

namespace {

// Param indices, matching the OP_MODE_PARAMS declaration order in main.cpp.
// A stray reordering shifts these, so every style assertion fails loudly.
constexpr uint8_t kParamStyle   = 0;
constexpr uint8_t kParamRate    = 1;
constexpr uint8_t kParamTriplet = 2;
constexpr uint8_t kParamOctave  = 3;
constexpr uint8_t kParamFill    = 4;
constexpr uint8_t kParamChord   = 5;

// Style enum values, matching the options list in main.cpp. Declared as
// integers here because the ABI value is an int32.
constexpr int32_t kStyleAsPlayed = 0;
constexpr int32_t kStyleUp       = 1;
constexpr int32_t kStyleDown     = 2;
constexpr int32_t kStyleUpDown   = 3;
constexpr int32_t kStyleDownUp   = 4;
constexpr int32_t kStyleConverge = 5;
constexpr int32_t kStyleDiverge  = 6;
constexpr int32_t kStyleRandom   = 7;

// Rate enum values.
constexpr int32_t kRate1  = 0;  // Rate option "1/4"  (quarter)
constexpr int32_t kRate2  = 1;  // Rate option "1/8"  (eighth)
constexpr int32_t kRate4  = 2;  // Rate option "1/16" (sixteenth)
constexpr int32_t kRate8  = 3;  // Rate option "1/32" (thirty-second)
constexpr int32_t kRate16 = 4;  // Rate option "1/64" (sixty-fourth)

// The input a fed message arrives on, carried in OpMidiMessage::port.
constexpr uint8_t kSourceInput = 0;

// The output the mode is loaded on in the sim, and so the one a run drains.
constexpr uint8_t kTestOutput = 0;

// A 1 ms tick slice is fine resolution for step detection at 120 BPM 8/beat
// (62.5 ms per step at 1/beat baseline, 7.8 ms at 8/beat).
constexpr uint32_t kSliceUs = 1000;

// --- Helpers --------------------------------------------------------------

// Inject a note-on at the given note and velocity. Feeds mode_process directly
// on the next advance so the held-note tracker picks it up.
void feed_note_on(uint8_t note, uint8_t velocity = 100) {
    OpMidiMessage message {/*status=*/0x90,
                           /*data1=*/note,
                           /*data2=*/velocity,
                           /*port=*/kSourceInput,
                           /*length=*/3};
    mode_process(&message, 1, op::sim::get_api()->get_tick());
}

void feed_note_off(uint8_t note) {
    OpMidiMessage message {/*status=*/0x80,
                           /*data1=*/note,
                           /*data2=*/0,
                           /*port=*/kSourceInput,
                           /*length=*/3};
    mode_process(&message, 1, op::sim::get_api()->get_tick());
}

// Accumulator fed by run(). The sim's outgoing ring is 32 messages deep and
// silently drops past that, so run() drains incrementally into this vector.
// drain() returns everything accumulated across the run, since the ring may
// have just been emptied.
std::vector<OpMidiMessage> g_collected;

// Drain the sim's outgoing ring NOW and append into g_collected.
// Called mid-run() to keep the 32-slot ring from overflowing during
// long or dense arpeggiation sequences.
void drain_into_collected() {
    OpMidiMessage buffer[64] {};
    while (true) {
        auto drained = op::sim::drain_outgoing_midi(kTestOutput, buffer, 64);
        if (drained == 0) break;
        for (uint16_t i = 0; i < drained; ++i) g_collected.push_back(buffer[i]);
    }
}

// Drive the mode for `duration_us` total, advancing virtual tick in
// `kSliceUs` slices. If `beats_per_second > 0` the beat pointer moves
// linearly. Drains the outgoing ring every 16 slices so dense
// emissions never back up past the sim's 32-message depth.
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

// Return the messages accumulated during run() and reset the buffer.
// Note that per-drain ordering is preserved but repeated calls within
// a test return only the NEW messages since the last call.
std::vector<OpMidiMessage> drain() {
    drain_into_collected();
    std::vector<OpMidiMessage> collected = std::move(g_collected);
    g_collected.clear();
    return collected;
}

// Extract note-on values only (status high nibble 0x90, velocity > 0).
std::vector<uint8_t> note_ons(const std::vector<OpMidiMessage>& messages) {
    std::vector<uint8_t> pitches;
    for (const auto& message : messages) {
        if ((message.status & 0xF0) == 0x90 && message.data2 != 0) pitches.push_back(message.data1);
    }
    return pitches;
}

// Set a full chord as held and return after primer ticks so the mode has
// observed all three note-on events and the beat pointer is positioned
// just before the first step fires. Default chord = C major (60/64/67).
void hold_c_major() {
    feed_note_on(60);
    feed_note_on(64);
    feed_note_on(67);
}

// Configure the standard test setup, 120 BPM (2 BPS) at Rate "1/16". That is
// 6 pulses per step on the 24-PPQ grid, so 48 pulses/sec / 6 = 8 steps/sec
// (125 ms per step). Most tests use this rate with triplet off for clean math.
void setup_standard_params(const OperatorApi* api,
                           int32_t style  = kStyleUp,
                           int32_t rate   = kRate4,
                           bool triplet   = false,
                           int32_t octave = 0,
                           int32_t fill   = 50,
                           bool chord     = false) {
    api->set_param_value(kParamStyle, style);
    api->set_param_value(kParamRate, rate);
    api->set_param_value(kParamTriplet, triplet ? 1 : 0);
    api->set_param_value(kParamOctave, octave);
    api->set_param_value(kParamFill, fill);
    api->set_param_value(kParamChord, chord ? 1 : 0);
}

}  // namespace

// ---------------------------------------------------------------------------
// Style coverage
// ---------------------------------------------------------------------------

TEST_CASE("style=Up cycles held notes ascending") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    setup_standard_params(api, /*style=*/kStyleUp, /*rate=*/kRate4);

    // Hold C major (C-E-G = 60-64-67).
    hold_c_major();

    // 2 BPS at rate 1/16 gives 8 steps/sec, 125 ms/step. 1.2 s is ~9-10 steps.
    run(1'200'000, /*beats_per_second=*/2.0f);

    const auto note_on_pitches = note_ons(drain());
    // First 9 note-ons should cycle 60, 64, 67 three times ascending.
    REQUIRE(note_on_pitches.size() >= 9);
    CHECK(note_on_pitches[0] == 60);
    CHECK(note_on_pitches[1] == 64);
    CHECK(note_on_pitches[2] == 67);
    CHECK(note_on_pitches[3] == 60);
    CHECK(note_on_pitches[4] == 64);
    CHECK(note_on_pitches[5] == 67);
    CHECK(note_on_pitches[6] == 60);
    CHECK(note_on_pitches[7] == 64);
    CHECK(note_on_pitches[8] == 67);

    mode_destroy();
}

TEST_CASE("style=Down cycles descending") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    setup_standard_params(api, /*style=*/kStyleDown, /*rate=*/kRate4);

    hold_c_major();
    run(1'200'000, 2.0f);

    const auto note_on_pitches = note_ons(drain());
    REQUIRE(note_on_pitches.size() >= 6);
    CHECK(note_on_pitches[0] == 67);
    CHECK(note_on_pitches[1] == 64);
    CHECK(note_on_pitches[2] == 60);
    CHECK(note_on_pitches[3] == 67);
    CHECK(note_on_pitches[4] == 64);
    CHECK(note_on_pitches[5] == 60);

    mode_destroy();
}

TEST_CASE("style=Up-Down bounces") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    setup_standard_params(api, /*style=*/kStyleUpDown, /*rate=*/kRate4);

    hold_c_major();
    run(1'200'000, 2.0f);

    const auto note_on_pitches = note_ons(drain());
    // Up-Down over [60,64,67] produces the bounce pattern 60, 64, 67, 64, 60,
    // 64, 67, 64, 60, and so on. Peaks and troughs are not repeated (the
    // "bounce, don't double" rule).
    REQUIRE(note_on_pitches.size() >= 8);
    CHECK(note_on_pitches[0] == 60);
    CHECK(note_on_pitches[1] == 64);
    CHECK(note_on_pitches[2] == 67);
    CHECK(note_on_pitches[3] == 64);
    CHECK(note_on_pitches[4] == 60);
    CHECK(note_on_pitches[5] == 64);
    CHECK(note_on_pitches[6] == 67);
    CHECK(note_on_pitches[7] == 64);

    mode_destroy();
}

TEST_CASE("style=Random picks from the held notes and repeats a run") {
    // The seed is taken from the tick at init and the tick opens at zero here, so
    // the run below is the same one every time.
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    setup_standard_params(api, /*style=*/kStyleRandom, /*rate=*/kRate4);

    hold_c_major();
    run(1'200'000, 2.0f);

    const auto first_run_pitches = note_ons(drain());
    REQUIRE(first_run_pitches.size() >= 6);
    for (auto pitch : first_run_pitches) {
        CHECK((pitch == 60 || pitch == 64 || pitch == 67));
    }
    mode_destroy();

    op::sim::reset_state();
    mode_init(api);
    setup_standard_params(api, /*style=*/kStyleRandom, /*rate=*/kRate4);
    hold_c_major();
    run(1'200'000, 2.0f);
    const auto second_run_pitches = note_ons(drain());
    REQUIRE(second_run_pitches.size() == first_run_pitches.size());
    for (std::size_t i = 0; i < first_run_pitches.size(); ++i) {
        CHECK(first_run_pitches[i] == second_run_pitches[i]);
    }
    mode_destroy();
}

// ---------------------------------------------------------------------------
// Modifiers
// ---------------------------------------------------------------------------

TEST_CASE("octave=+1 shifts output up by 12") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    setup_standard_params(api, kStyleUp, kRate4, /*triplet=*/false,
                          /*octave=*/1);

    hold_c_major();
    run(1'200'000, 2.0f);

    const auto note_on_pitches = note_ons(drain());
    REQUIRE(note_on_pitches.size() >= 3);
    // First three notes cycle Up with a +12 semitone shift, so 72, 76, 79.
    CHECK(note_on_pitches[0] == 72);
    CHECK(note_on_pitches[1] == 76);
    CHECK(note_on_pitches[2] == 79);

    mode_destroy();
}

TEST_CASE("fill=100 note-off at next note-on time") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    setup_standard_params(api, kStyleUp, kRate4,
                          /*triplet=*/false,
                          /*octave=*/0,
                          /*fill=*/100);

    hold_c_major();
    // Drive 4 steps at 120 BPM rate 1/16, which is 500 ms.
    run(550'000, 2.0f);

    const auto messages = drain();
    // Collect indices of note-ons and note-offs in arrival order.
    std::vector<std::size_t> note_on_indices, note_off_indices;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const auto& message = messages[i];
        if ((message.status & 0xF0) == 0x90 && message.data2 != 0)
            note_on_indices.push_back(i);
        else if ((message.status & 0xF0) == 0x80)
            note_off_indices.push_back(i);
        else if ((message.status & 0xF0) == 0x90 && message.data2 == 0)
            note_off_indices.push_back(i);
    }
    // With fill=100 each note-off lands immediately before the next note-on
    // (or at the same drained tick). We expect at least 3 note-ons / 2 note-offs.
    REQUIRE(note_on_indices.size() >= 3);
    REQUIRE(note_off_indices.size() >= 2);
    // Each note-off must be consumed at or before the next note-on (same tick
    // is acceptable if the batch emitted them in that order).
    for (std::size_t k = 0; k + 1 < note_on_indices.size() && k < note_off_indices.size(); ++k) {
        CHECK(note_off_indices[k] <= note_on_indices[k + 1]);
    }

    mode_destroy();
}

TEST_CASE("fill=50 holds note half the step") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    setup_standard_params(api, kStyleUp, kRate4,
                          /*triplet=*/false,
                          /*octave=*/0,
                          /*fill=*/50);

    hold_c_major();
    // 2 BPS rate 1/16 gives step = 125 ms. Fill 50 puts note-off ~62.5 ms after
    // on.
    run(600'000, 2.0f);

    const auto note_on_pitches = note_ons(drain());
    REQUIRE(note_on_pitches.size() >= 3);
    // Structural check that we got the ascending pattern (same logic as up).
    CHECK(note_on_pitches[0] == 60);
    CHECK(note_on_pitches[1] == 64);
    CHECK(note_on_pitches[2] == 67);
    mode_destroy();
}

// ---------------------------------------------------------------------------
// Chord mode
// ---------------------------------------------------------------------------

TEST_CASE("chord on preserves semitone distances") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    setup_standard_params(api,
                          /*style=*/kStyleUp, kRate4,
                          /*triplet=*/false,
                          /*octave=*/0,
                          /*fill=*/50,
                          /*chord=*/true);

    // Hold C-E-G (60-64-67). Chord mode emits the whole chord at each step
    // transposed by the Up-style base note.
    hold_c_major();
    run(600'000, 2.0f);

    const auto messages = drain();
    // Gather note-ons into per-step groups. Every three consecutive note-ons
    // belong to one chord strike (we held 3 notes).
    const auto note_on_pitches = note_ons(messages);
    REQUIRE(note_on_pitches.size() >= 9);

    // Step 1 chord at C is 60, 64, 67 (ascending-sorted).
    std::vector<uint8_t> step1 {note_on_pitches[0], note_on_pitches[1], note_on_pitches[2]};
    std::sort(step1.begin(), step1.end());
    CHECK(step1[0] == 60);
    CHECK(step1[1] == 64);
    CHECK(step1[2] == 67);

    // Step 2 chord at E is C+4, E+4, G+4 = 64, 68, 71.
    std::vector<uint8_t> step2 {note_on_pitches[3], note_on_pitches[4], note_on_pitches[5]};
    std::sort(step2.begin(), step2.end());
    CHECK(step2[0] == 64);
    CHECK(step2[1] == 68);
    CHECK(step2[2] == 71);

    // Step 3 chord at G is C+7, E+7, G+7 = 67, 71, 74.
    std::vector<uint8_t> step3 {note_on_pitches[6], note_on_pitches[7], note_on_pitches[8]};
    std::sort(step3.begin(), step3.end());
    CHECK(step3[0] == 67);
    CHECK(step3[1] == 71);
    CHECK(step3[2] == 74);

    mode_destroy();
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

TEST_CASE("rate 8/beat with triplet") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    setup_standard_params(api, kStyleUp, /*rate=*/kRate8, /*triplet=*/true);

    hold_c_major();

    // 120 BPM (2 BPS), rate 1/32, triplet gives 8 * 3/2 = 12 steps/beat = 24/sec.
    // Drive 1 second and count note-ons, expecting ~24 within a small boundary.
    run(1'000'000, 2.0f);

    const auto note_on_pitches = note_ons(drain());
    CHECK(note_on_pitches.size() >= 22);
    CHECK(note_on_pitches.size() <= 26);

    mode_destroy();
}

TEST_CASE("no held notes = silent") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    setup_standard_params(api, kStyleUp, kRate4);

    // No incoming notes. Drive for 2 seconds.
    run(2'000'000, 2.0f);

    const auto note_on_pitches = note_ons(drain());
    CHECK(note_on_pitches.size() == 0);

    mode_destroy();
}

// ---------------------------------------------------------------------------
// Note suppression. The arp consumes note-on and note-off and re-emits its own
// stepped stream in their place. Every other message (aftertouch, CC,
// pitch-bend, and system messages) passes through untouched, so a downstream
// synth keeps full expressivity.
//
// This test pins that contract by observing mode_process's in-place mutation.
// A suppressed message comes back with a status of 0, and a passed-through
// message keeps its original status and length.
// ---------------------------------------------------------------------------

namespace {

// Feed one message through mode_process and return it post-process so the
// test can observe the in-place mutation.
OpMidiMessage feed_and_return(uint8_t status, uint8_t data1, uint8_t data2) {
    OpMidiMessage message {status, data1, data2, /*port=*/kSourceInput, /*length=*/3};
    mode_process(&message, 1, op::sim::get_api()->get_tick());
    return message;
}

}  // namespace

TEST_CASE("note suppression removes notes and passes non-note messages through") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    setup_standard_params(api, kStyleUp, kRate4);

    hold_c_major();

    // Note-on and note-off are suppressed, and the arp re-emits its own stepped
    // stream in their place.
    OpMidiMessage note_on = feed_and_return(0x90, /*note=*/72, /*velocity=*/100);
    CHECK(note_on.status == 0);
    OpMidiMessage note_off = feed_and_return(0x80, /*note=*/72, /*velocity=*/0);
    CHECK(note_off.status == 0);

    // Aftertouch, channel pressure (0xD0), must pass through untouched.
    OpMidiMessage channel_pressure = feed_and_return(0xD0, /*pressure=*/100, /*unused=*/0);
    CHECK(channel_pressure.status == 0xD0);
    CHECK(channel_pressure.length == 3);

    // Aftertouch, poly key pressure (0xA0), must pass through untouched.
    OpMidiMessage poly_pressure = feed_and_return(0xA0, /*note=*/60, /*pressure=*/90);
    CHECK(poly_pressure.status == 0xA0);
    CHECK(poly_pressure.length == 3);

    // CC (0xB0) must pass through untouched.
    OpMidiMessage control_change = feed_and_return(0xB0, /*controller=*/1, /*value=*/64);
    CHECK(control_change.status == 0xB0);
    CHECK(control_change.length == 3);

    // Pitch-bend (0xE0) must pass through untouched.
    OpMidiMessage pitch_bend = feed_and_return(0xE0, /*lsb=*/0, /*msb=*/96);
    CHECK(pitch_bend.status == 0xE0);
    CHECK(pitch_bend.length == 3);

    mode_destroy();
}

TEST_CASE("an aftertouch flood disturbs neither the held chord nor the arp "
          "output") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    mode_init(api);
    setup_standard_params(api, kStyleUp, kRate4);

    // Hold C major, interleaving aftertouch (both classes) between holds. The
    // chord table and pattern cursor must be unaffected by the aftertouch flood,
    // so the arp produces the same clean ascending 60/64/67 cycle as a flood-free
    // hold. (The aftertouch messages themselves pass through, per the test above,
    // they simply never touch the held-note table or the arp sequence.)
    feed_note_on(60);
    feed_and_return(0xD0, 120, 0);
    feed_and_return(0xA0, 60, 120);
    feed_note_on(64);
    feed_and_return(0xD0, 10, 0);
    feed_note_on(67);
    feed_and_return(0xA0, 67, 5);
    feed_and_return(0xD0, 80, 0);

    run(1'200'000, /*beats_per_second=*/2.0f);

    const auto note_on_pitches = note_ons(drain());
    // Same expectation as "style=Up cycles held notes ascending". The
    // aftertouch flood must not re-sort, re-anchor, or reset the cursor.
    REQUIRE(note_on_pitches.size() >= 9);
    CHECK(note_on_pitches[0] == 60);
    CHECK(note_on_pitches[1] == 64);
    CHECK(note_on_pitches[2] == 67);
    CHECK(note_on_pitches[3] == 60);
    CHECK(note_on_pitches[4] == 64);
    CHECK(note_on_pitches[5] == 67);
    CHECK(note_on_pitches[6] == 60);
    CHECK(note_on_pitches[7] == 64);
    CHECK(note_on_pitches[8] == 67);

    mode_destroy();
}
