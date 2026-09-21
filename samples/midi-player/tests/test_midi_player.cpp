// MIDI Player sample, doctest behavior suite.
//
// Everything the player does is observable in the MIDI it sends, so the suite
// watches that rather than reaching into the transport. Playing means notes come
// out, pausing and stopping mean the ringing notes are silenced, and stopping
// rewinds so the next play starts the song again from the top.
//
// The fixtures are hand-written SMF bytes, small enough to read and check
// against the format by eye.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <operator_sdk.h>
#include <operator_sdk_sim.h>

#include <cstdint>
#include <vector>

OP_MODE_UNDER_TEST();

namespace {

constexpr uint8_t kSlotClockSource = 0;
constexpr uint8_t kSlotMidiFile    = 1;

constexpr uint8_t kClockFromFile     = 0;
constexpr uint8_t kClockFromExternal = 1;

constexpr uint8_t kOutput = 0;

// Files live under the mode's own name in the extras area.
constexpr const char* kModeName = "midi_player";

// The gesture type is what the mode branches on, so the encoder and the press
// that produced it are left to the active input map.
constexpr uint8_t kAnyEncoder = 0;

// Division 96 ticks per quarter at 500000 us per quarter, so a quarter note is
// 96 ticks and 500 ms. C4 sounds for one quarter, then E4 for the next.
constexpr uint8_t kSongMid[] = {
    'M',  'T',  'h',  'd',  0x00, 0x00, 0x00, 0x06, 0x00, 0x00,  // format 0
    0x00, 0x01,                                                  // 1 track
    0x00, 0x60,                                                  // 96 ticks per quarter
    'M',  'T',  'r',  'k',  0x00, 0x00, 0x00, 0x1B,              // 27 bytes of track
    0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,                    // tempo 500000 us per quarter
    0x00, 0x90, 0x3C, 0x64,                                      // note-on C4 at tick 0
    0x60, 0x80, 0x3C, 0x40,                                      // note-off C4 at tick 96
    0x00, 0x90, 0x40, 0x64,                                      // note-on E4 at tick 96
    0x60, 0x80, 0x40, 0x40,                                      // note-off E4 at tick 192
    0x00, 0xFF, 0x2F, 0x00,                                      // end of track
};
static_assert(sizeof(kSongMid) == 14 + 8 + 27);

// Division 480 puts 20 ticks between the 24 PPQ pulses, and the note-off lands
// at tick 110, half a pulse past pulse 5. Playing it proves the clock position
// moves between pulse edges.
constexpr uint8_t kSubPulseMid[] = {
    'M',  'T',  'h',  'd',  0x00, 0x00, 0x00, 0x06, 0x00, 0x00,  // format 0
    0x00, 0x01,                                                  // 1 track
    0x01, 0xE0,                                                  // 480 ticks per quarter
    'M',  'T',  'r',  'k',  0x00, 0x00, 0x00, 0x0C,              // 12 bytes of track
    0x00, 0x90, 0x3C, 0x64,                                      // note-on C4 at tick 0
    0x6E, 0x80, 0x3C, 0x40,                                      // note-off C4 at tick 110
    0x00, 0xFF, 0x2F, 0x00,                                      // end of track
};
static_assert(sizeof(kSubPulseMid) == 14 + 8 + 12);

// The magic bytes are wrong, so the header never parses.
constexpr uint8_t kBadMid[] = {
    'X', 'X', 'X', 'X', 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x60,
};

// A 16-track file large enough that every track outruns its 1 KB window several
// times over, so playing it end to end exercises the refills.
std::vector<uint8_t> build_large_file(uint32_t target_bytes) {
    constexpr uint16_t kTracks       = 16;
    constexpr int kNotePairsPerTrack = 250;

    std::vector<std::vector<uint8_t>> tracks(kTracks);
    for (auto& track : tracks) {
        for (int pair = 0; pair < kNotePairsPerTrack; ++pair) {
            const uint8_t note_pair[] = {0x60, 0x90, 0x40, 0x40, 0x60, 0x80, 0x40, 0x40};
            track.insert(track.end(), std::begin(note_pair), std::end(note_pair));
        }
    }

    auto total_size = [&tracks]() -> uint32_t {
        uint32_t size = 14;
        for (const auto& track : tracks) size += 8 + static_cast<uint32_t>(track.size()) + 4;
        return size;
    };
    // Pad with skippable meta events until the file crosses the target.
    while (total_size() < target_bytes) {
        for (auto& track : tracks) {
            const uint8_t padding[] = {0x00, 0xFF, 0x7F, 0x00};
            track.insert(track.end(), std::begin(padding), std::end(padding));
            if (total_size() >= target_bytes) break;
        }
    }

    std::vector<uint8_t> out {
        'M',
        'T',
        'h',
        'd',
        0x00,
        0x00,
        0x00,
        0x06,
        0x00,
        0x01,  // format 1
        static_cast<uint8_t>(kTracks >> 8),
        static_cast<uint8_t>(kTracks & 0xFF),
        0x01,
        0xE0,  // 480 ticks per quarter
    };
    for (const auto& track : tracks) {
        const uint32_t length        = static_cast<uint32_t>(track.size()) + 4;  // with end of track
        const uint8_t chunk[]        = {'M',
                                        'T',
                                        'r',
                                        'k',
                                        static_cast<uint8_t>(length >> 24),
                                        static_cast<uint8_t>(length >> 16),
                                        static_cast<uint8_t>(length >> 8),
                                        static_cast<uint8_t>(length)};
        const uint8_t end_of_track[] = {0x00, 0xFF, 0x2F, 0x00};
        out.insert(out.end(), std::begin(chunk), std::end(chunk));
        out.insert(out.end(), track.begin(), track.end());
        out.insert(out.end(), std::begin(end_of_track), std::end(end_of_track));
    }
    return out;
}

// --- Harness -----------------------------------------------------------------

std::vector<OpMidiMessage> g_collected;

void collect() {
    OpMidiMessage buffer[64] {};
    while (true) {
        const auto count = op::sim::drain_outgoing_midi(kOutput, buffer, 64);
        if (count == 0) break;
        for (uint16_t i = 0; i < count; ++i) g_collected.push_back(buffer[i]);
    }
}

// Everything the mode has sent since the last call. The sim's ring is small, so
// playback drains as it runs and this hands back the accumulated total.
std::vector<OpMidiMessage> sent() {
    collect();
    std::vector<OpMidiMessage> out = std::move(g_collected);
    g_collected.clear();
    return out;
}

// Load a file and settle the mode on it, leaving the transport ready to play.
void load(const char* filename, const uint8_t* bytes, std::size_t size, uint8_t clock_source) {
    op::sim::reset_state();
    op::sim::set_current_mode_name(kModeName);
    g_collected.clear();
    mode_init(op::sim::get_api());

    op::sim::seed_extras_file(kModeName, filename, bytes, static_cast<uint32_t>(size));
    op::sim::pick_filename(kSlotMidiFile, filename);
    op::sim::get_api()->set_param_value(kSlotClockSource, clock_source);
    mode_process(nullptr, 0, 0);
    sent();
}

void enter() {
    op::sim::ui_gesture(kAnyEncoder, op::Gesture::ShortPress, op::GestureType::Enter, 0);
}
void scroll(int16_t steps) {
    op::sim::ui_gesture(kAnyEncoder, op::Gesture::Rotate, op::GestureType::Scroll, steps);
}
void change() {
    op::sim::ui_gesture(kAnyEncoder, op::Gesture::Rotate, op::GestureType::Change, 1);
}

// Button 0 is play and pause, button 1 is stop, and the selection starts on 0.
void press_play_pause() {
    enter();
}
void press_stop() {
    scroll(+1);
    enter();
    scroll(-1);  // leave the selection where it started
}

// Run the mode on its own tick, which is what Clock Source File follows.
void run(uint32_t duration_us) {
    constexpr uint32_t kSlice = 1000;
    for (uint32_t i = 0; i < duration_us / kSlice; ++i) {
        op::sim::advance_tick(kSlice);
        mode_process(nullptr, 0, op::sim::get_api()->get_tick());
        if ((i & 0xF) == 0xF) collect();
    }
    collect();
}

// Drive the device clock alongside the tick, which is what Clock Source Ext.
// follows. Holding the pulses still moves only the tick, which is how the
// between-edges position shows up.
struct Clock {
    uint32_t since_pulse_us = 0;
    uint32_t pulse          = 0;
    uint32_t pulse_us       = 20833;  // 120 BPM at 24 PPQ
};

void run_clock(Clock& clock, uint32_t steps, uint32_t slice_us, bool advance_pulses) {
    for (uint32_t i = 0; i < steps; ++i) {
        op::sim::advance_tick(slice_us);
        if (advance_pulses) {
            clock.since_pulse_us += slice_us;
            while (clock.since_pulse_us >= clock.pulse_us) {
                clock.since_pulse_us -= clock.pulse_us;
                ++clock.pulse;
            }
        }
        op::sim::set_pulse_count(clock.pulse);
        mode_process(nullptr, 0, op::sim::get_api()->get_tick());
        if ((i & 0xF) == 0xF) collect();
    }
    collect();
}

bool is_note_on(const OpMidiMessage& message, uint8_t pitch) {
    return (message.status & 0xF0) == 0x90 && message.data2 > 0 && message.data1 == pitch;
}
bool is_note_off(const OpMidiMessage& message, uint8_t pitch) {
    const uint8_t kind = message.status & 0xF0;
    return message.data1 == pitch && (kind == 0x80 || (kind == 0x90 && message.data2 == 0));
}

uint32_t count_note_ons(const std::vector<OpMidiMessage>& messages) {
    uint32_t total = 0;
    for (const auto& message : messages) {
        if ((message.status & 0xF0) == 0x90 && message.data2 > 0) ++total;
    }
    return total;
}

// The player silences a channel with All Notes Off followed by All Sound Off.
constexpr uint8_t kCcAllSoundOff = 0x78;
constexpr uint8_t kCcAllNotesOff = 0x7B;

bool has_silencing(const std::vector<OpMidiMessage>& messages) {
    bool notes_off = false;
    bool sound_off = false;
    for (const auto& message : messages) {
        if ((message.status & 0xF0) != 0xB0) continue;
        if (message.data1 == kCcAllNotesOff) notes_off = true;
        if (message.data1 == kCcAllSoundOff) sound_off = true;
    }
    return notes_off && sound_off;
}

bool contains(const std::vector<OpMidiMessage>& messages,
              bool (*match)(const OpMidiMessage&, uint8_t),
              uint8_t pitch) {
    for (const auto& message : messages) {
        if (match(message, pitch)) return true;
    }
    return false;
}

constexpr uint8_t kC4 = 0x3C;
constexpr uint8_t kE4 = 0x40;

}  // namespace

// --- Playing -----------------------------------------------------------------

TEST_CASE("a loaded file stays silent until the transport starts") {
    load("song.mid", kSongMid, sizeof(kSongMid), kClockFromFile);

    run(1'000'000);

    CHECK(sent().empty());
    mode_destroy();
}

// The file puts C4 at the downbeat and releases it a quarter note later, where
// E4 takes over, so a second of playing at 120 BPM covers both.
TEST_CASE("playing sends the file's notes in time with its own tempo") {
    load("song.mid", kSongMid, sizeof(kSongMid), kClockFromFile);

    press_play_pause();
    run(300'000);
    const auto early = sent();
    CHECK(contains(early, is_note_on, kC4));
    CHECK_FALSE(contains(early, is_note_off, kC4));  // the quarter has not passed

    run(700'000);
    const auto later = sent();
    CHECK(contains(later, is_note_off, kC4));
    CHECK(contains(later, is_note_on, kE4));

    mode_destroy();
}

TEST_CASE("playing follows the device clock when Clock Source is Ext.") {
    load("song.mid", kSongMid, sizeof(kSongMid), kClockFromExternal);

    press_play_pause();
    Clock clock;
    run_clock(clock, /*steps=*/1200, /*slice_us=*/1000, /*advance_pulses=*/true);

    const auto messages = sent();
    CHECK(contains(messages, is_note_on, kC4));
    CHECK(contains(messages, is_note_off, kC4));
    CHECK(contains(messages, is_note_on, kE4));

    mode_destroy();
}

// The note-off sits half a pulse past pulse 5. Holding the pulse counter still
// and moving only the tick has to carry the position across it, which it can
// only do by reading between the edges.
TEST_CASE("Ext. playback moves between the clock's pulse edges") {
    load("fine.mid", kSubPulseMid, sizeof(kSubPulseMid), kClockFromExternal);

    // Pulse 5 falls at 104.2 ms and the note-off at 114.6 ms, so stopping at
    // 105 ms leaves the transport just short of it, a hair past the pulse edge.
    press_play_pause();
    Clock clock;
    run_clock(clock, /*steps=*/105, /*slice_us=*/1000, /*advance_pulses=*/true);
    const auto warm = sent();
    REQUIRE(contains(warm, is_note_on, kC4));
    REQUIRE_FALSE(contains(warm, is_note_off, kC4));

    // Hold the pulse counter still and advance the tick alone, over the mark.
    run_clock(clock, /*steps=*/15, /*slice_us=*/1000, /*advance_pulses=*/false);

    CHECK(contains(sent(), is_note_off, kC4));
    mode_destroy();
}

TEST_CASE("a large multi-track file plays through its buffer refills") {
    const auto payload = build_large_file(500'000u);
    REQUIRE(payload.size() >= 500'000u);
    load("large.mid", payload.data(), payload.size(), kClockFromFile);

    press_play_pause();
    run(5'000'000);

    // Every one of the 16 tracks is sounding notes well past its first refill.
    CHECK(count_note_ons(sent()) > 100u);
    mode_destroy();
}

// --- Pausing and stopping ----------------------------------------------------

TEST_CASE("pausing silences the notes still ringing") {
    load("song.mid", kSongMid, sizeof(kSongMid), kClockFromFile);

    press_play_pause();
    run(200'000);  // C4 is sounding and its note-off is still to come
    REQUIRE(contains(sent(), is_note_on, kC4));

    press_play_pause();  // pause

    CHECK(has_silencing(sent()));
    mode_destroy();
}

TEST_CASE("pausing with nothing ringing sends no silencing") {
    load("song.mid", kSongMid, sizeof(kSongMid), kClockFromFile);

    press_play_pause();  // play, but never run, so no note has sounded
    press_play_pause();  // pause

    CHECK_FALSE(has_silencing(sent()));
    mode_destroy();
}

TEST_CASE("a paused transport sends nothing") {
    load("song.mid", kSongMid, sizeof(kSongMid), kClockFromFile);

    press_play_pause();
    run(200'000);
    press_play_pause();  // pause
    sent();

    run(1'000'000);

    CHECK(sent().empty());
    mode_destroy();
}

// Resuming carries the position with it, so the note-off that was pending
// arrives on time and the song does not fall silent while a restarted clock
// climbs back to where it left off.
TEST_CASE("resuming picks the song up where it paused") {
    load("song.mid", kSongMid, sizeof(kSongMid), kClockFromFile);

    press_play_pause();
    run(200'000);
    press_play_pause();  // pause
    sent();

    press_play_pause();  // resume
    run(400'000);        // the quarter note falls due 300 ms after the pause

    const auto messages = sent();
    CHECK(contains(messages, is_note_off, kC4));
    CHECK(contains(messages, is_note_on, kE4));
    CHECK_FALSE(has_silencing(messages));  // resuming silences nothing

    mode_destroy();
}

TEST_CASE("stopping silences the notes and rewinds to the top") {
    load("song.mid", kSongMid, sizeof(kSongMid), kClockFromFile);

    press_play_pause();
    run(200'000);
    REQUIRE(contains(sent(), is_note_on, kC4));

    press_stop();
    CHECK(has_silencing(sent()));

    // Playing again starts the song from its first note.
    press_play_pause();
    run(100'000);
    CHECK(contains(sent(), is_note_on, kC4));

    mode_destroy();
}

TEST_CASE("reaching the end holds until play starts the song again") {
    load("song.mid", kSongMid, sizeof(kSongMid), kClockFromFile);

    press_play_pause();
    run(3'000'000);  // well past the song's end
    sent();

    run(1'000'000);
    CHECK(sent().empty());  // the transport is at rest

    press_play_pause();
    run(100'000);
    CHECK(contains(sent(), is_note_on, kC4));  // back at the top

    mode_destroy();
}

// --- The screen --------------------------------------------------------------

TEST_CASE("Scroll moves the selection onto stop") {
    load("song.mid", kSongMid, sizeof(kSongMid), kClockFromFile);

    // With the selection moved on, Enter reaches stop and no playing begins.
    scroll(+1);
    enter();
    run(500'000);

    CHECK(sent().empty());
    mode_destroy();
}

TEST_CASE("Change cycles the Clock Source") {
    load("song.mid", kSongMid, sizeof(kSongMid), kClockFromFile);
    const auto* api = op::sim::get_api();
    REQUIRE(api->get_param_value(kSlotClockSource) == kClockFromFile);

    change();
    CHECK(api->get_param_value(kSlotClockSource) == kClockFromExternal);

    change();
    CHECK(api->get_param_value(kSlotClockSource) == kClockFromFile);

    mode_destroy();
}

// The two clock sources measure the position from different origins. Switching
// mid-song re-anchors both, so the song carries on from where it had reached.
TEST_CASE("switching the Clock Source mid-song keeps the position") {
    load("song.mid", kSongMid, sizeof(kSongMid), kClockFromExternal);

    press_play_pause();
    Clock clock;
    run_clock(clock, /*steps=*/200, /*slice_us=*/1000, /*advance_pulses=*/true);
    REQUIRE(contains(sent(), is_note_on, kC4));

    change();  // over to the file's own tempo, mid-song
    run(600'000);

    // The song continues from the note it was on, without silence or a jump.
    const auto messages = sent();
    CHECK(contains(messages, is_note_off, kC4));
    CHECK(contains(messages, is_note_on, kE4));

    mode_destroy();
}

// --- Nothing to play ---------------------------------------------------------

TEST_CASE("a malformed file plays nothing") {
    load("bad.mid", kBadMid, sizeof(kBadMid), kClockFromFile);

    press_play_pause();
    run(1'000'000);

    CHECK(sent().empty());
    mode_destroy();
}

TEST_CASE("the transport does nothing with no file loaded") {
    op::sim::reset_state();
    op::sim::set_current_mode_name(kModeName);
    g_collected.clear();
    mode_init(op::sim::get_api());
    mode_process(nullptr, 0, 0);

    press_play_pause();
    run(1'000'000);

    CHECK(sent().empty());
    mode_destroy();
}

