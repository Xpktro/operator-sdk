// MIDI Player sample mode.
//
// Plays a Standard MIDI File from the mode's own storage area, sending its
// events out at the tick each one is scheduled for. The parser lives in
// smf_parser.h and the transport screen at the bottom of this file.
//
// The file is streamed as it plays. Each track reads through a 1 KB window that
// refills from storage as the music moves past it, so a long file costs the same
// memory as a short one.
//
// Clock Source picks what the transport follows. On File it is the elapsed
// microseconds and the tempo the file declares, and on Ext. it is the device
// clock, so a tempo change upstream carries the playback with it.

#include <operator_sdk.h>
#include <operator_sdk/draw.h>
#include <operator_sdk/log.h>
#include <operator_sdk/params.h>
#include <operator_sdk/text.h>
#include <operator_sdk/timing.h>

#include "smf_parser.h"

#include <cstdint>

// --- Parameters ------------------------------------------------------------
//
// Clock Source picks what drives playback. File plays at the tempo the file
// declares, and Ext. follows the device clock. The mode reads the enum as
// follow = value != 0, so Ext. is the following one.

inline constexpr op::params::Enum ClockSource {
    .name     = "Clock Source",
    .options  = {"File", "Ext."},
    .default_ = 0,
};

inline constexpr op::params::FilePicker MidiFile {
    .name      = "MIDI File",
    .extension = ".mid",
};

OP_MODE_PARAMS(ClockSource, MidiFile);

// The reader reaches op::api, which OP_MODE_PARAMS declares, so it comes in here.
#include "storage_reader.h"

// --- Mode state ------------------------------------------------------------

namespace {

enum State : uint8_t {
    kStateIdleNoFile     = 0,
    kStateIdleFileLoaded = 1,
    kStatePlaying        = 2,
    kStatePaused         = 3,
    kStateError          = 4,
    // The song has run out. The time row holds the full duration and the next
    // play starts again from the top, which is what tells this apart from
    // IdleFileLoaded and its 00:00.
    kStatePlaybackFinished = 5,
};

// Unused output argument, since an output mode reaches the output it was loaded
// on.
constexpr uint8_t kAnyOutput = 0;

// Button 0 is the play and pause toggle, whose action follows the transport,
// and button 1 is stop.
constexpr uint8_t kButtonCount = 2;

// A track's window into the file. window_offset is where bytes[0] sits in the
// file, and window_size is how much of the window holds data.
struct TrackBuffer {
    uint8_t bytes[1024];
    uint32_t window_offset;  // UINT32_MAX = invalid (no data cached)
    uint32_t window_size;    // valid bytes in `bytes` (<=1024)
};
constexpr uint32_t kInvalidWindow = 0xFFFFFFFFu;

// --- File loading state ---------------------------------------------------

// The open file. It is held across load, play and pause, so resuming costs
// nothing, and closed when the pick changes, the parse fails, or the mode goes.
int32_t g_file_handle = -1;
uint32_t g_file_size  = 0;

// Per-track rolling buffers (16 x 1024 B = 16 KB).
TrackBuffer g_track_buffers[op::samples::smf::kMaxTracks] {};

// The file the mode has loaded, compared against the pick each tick so a change
// of file is noticed. Empty means nothing is loaded.
char g_loaded_filename[64] = {};

// Parsed SMF state. Lives across process() calls, mutated as tempo /
// end-of-track meta events are consumed.
op::samples::smf::Header g_header {};
op::samples::smf::TrackCursor g_tracks[op::samples::smf::kMaxTracks];
uint8_t g_track_count = 0;

// The next event waiting on each track, decoded ahead of time so playback can
// always see which track is due next.
op::samples::smf::Event g_pending[op::samples::smf::kMaxTracks] {};
bool g_pending_valid[op::samples::smf::kMaxTracks] = {};

// --- Transport state ------------------------------------------------------

State g_state = kStateIdleNoFile;

// Which notes are sounding, a bit per pitch per channel. Leaving playback
// silences them. Each pitch is tracked on its own, so a pitch struck twice with no
// release in between is still silenced by the one that follows.
//
// A channel is silenced with All Notes Off followed by All Sound Off. The first
// releases the note and lets the tail ring, the second cuts it, and synths differ
// on which they honor, so both go out.
constexpr uint8_t kCcAllSoundOff = 0x78;
constexpr uint8_t kCcAllNotesOff = 0x7B;
uint8_t g_active_notes[16][16]   = {};  // [channel][pitch / 8], bit = pitch % 8

inline void mark_note_on(uint8_t channel, uint8_t pitch) noexcept {
    if (channel >= 16 || pitch >= 128) return;
    g_active_notes[channel][pitch >> 3] |= 1u << (pitch & 0x7);
}

inline void mark_note_off(uint8_t channel, uint8_t pitch) noexcept {
    if (channel >= 16 || pitch >= 128) return;
    g_active_notes[channel][pitch >> 3] &= ~(1u << (pitch & 0x7));
}

// Follow what has actually gone out, so leaving playback silences exactly the
// notes the synth is holding. A note-on at velocity 0 is a note-off, which is
// how many files write their releases.
inline void track_emitted_event(uint8_t status, uint8_t pitch, uint8_t velocity) noexcept {
    const uint8_t kind    = status & 0xF0;
    const uint8_t channel = status & 0x0F;
    if (kind == 0x90 && velocity > 0)
        mark_note_on(channel, pitch);
    else if (kind == 0x80 || kind == 0x90)
        mark_note_off(channel, pitch);
}

// Silence every channel holding a note, which is what leaving playback needs.
void silence_active_channels() noexcept {
    if (!op::api) return;
    for (uint8_t channel = 0; channel < 16; ++channel) {
        bool sounding = false;
        for (uint8_t group = 0; group < 16; ++group) {
            if (g_active_notes[channel][group]) {
                sounding = true;
                break;
            }
        }
        if (!sounding) continue;
        const uint8_t status = 0xB0u | channel;
        op::api->send_midi(kAnyOutput, status, kCcAllNotesOff, 0);
        op::api->send_midi(kAnyOutput, status, kCcAllSoundOff, 0);
        for (uint8_t group = 0; group < 16; ++group) g_active_notes[channel][group] = 0;
    }
}

// Forget the sounding notes without sending anything, which is what starting
// fresh wants.
inline void clear_active_notes() noexcept {
    for (uint8_t channel = 0; channel < 16; ++channel) {
        for (uint8_t group = 0; group < 16; ++group) g_active_notes[channel][group] = 0;
    }
}

// Storage can report that it is busy, meaning the read simply has to be tried
// again. The parser answers in bools, which cannot tell that apart from a real
// end or a real error, so a busy read raises this instead and the callers above
// leave their state alone and wait for the next tick. Cleared each process().
bool g_storage_busy = false;

// Where play began, in each of the two scales the clock sources measure from.
uint32_t g_play_start_us = 0;
float g_play_start_beat  = 0.0f;

// The device clock ticks 24 times a quarter note, which at 120 BPM is only about
// 48 times a second. Events written at the file's finer resolution would land in
// clumps on those edges, so the position is read between them.
op::sdk::ClockInterpolator g_clock {};

// The position the pause froze, which resuming picks back up from.
uint32_t g_paused_tick = 0;

// --- UI state -------------------------------------------------------------

uint8_t g_selected_button = 0;

// Reads a track through its 1 KB window. A read the window cannot cover moves it
// to start at what was asked for and fills it again from the file. The parser
// takes its bytes a few at a time, so the window carries it a long way between
// refills.
class CachedTrackReader {
public:
    CachedTrackReader(const op::samples::midi_player::StorageFileReader& upstream,
                      TrackBuffer& window) noexcept
        : upstream_(upstream)
        , window_(window) { }

    int32_t read_at(uint32_t offset, uint8_t* out, uint32_t count) noexcept {
        if (count == 0) return 0;

        const bool filled   = (window_.window_offset != kInvalidWindow);
        uint32_t window_end = filled ? window_.window_offset + window_.window_size : 0;

        if (!filled || offset < window_.window_offset || offset + count > window_end) {
            const int32_t read_count = upstream_.read_at(offset, window_.bytes, sizeof(window_.bytes));
            // A busy read is one to try again, so the window keeps what it has and
            // the callers above hold their state for the next tick.
            if (read_count == kStorageBusy) {
                g_storage_busy = true;
                return kStorageBusy;
            }
            if (read_count <= 0) {
                window_.window_offset = kInvalidWindow;
                window_.window_size   = 0;
                return read_count;
            }
            window_.window_offset = offset;
            window_.window_size   = read_count;
            window_end            = window_.window_offset + window_.window_size;
        }

        // Near the end of the file the window can hold less than was asked for.
        const uint32_t available    = window_end - offset;
        const uint32_t to_copy      = (count < available) ? count : available;
        const uint32_t window_start = offset - window_.window_offset;
        for (uint32_t i = 0; i < to_copy; ++i) out[i] = window_.bytes[window_start + i];
        return static_cast<int32_t>(to_copy);
    }

    uint32_t size() const noexcept {
        return upstream_.size();
    }

private:
    const op::samples::midi_player::StorageFileReader& upstream_;
    TrackBuffer& window_;
};

static_assert(op::samples::smf::SmfReader<CachedTrackReader>,
              "CachedTrackReader must satisfy the SmfReader concept");

}  // namespace

// ---------------------------------------------------------------------------
// Button table (state -> logical buttons)
// ---------------------------------------------------------------------------

namespace {

// The transport buttons answer to gestures once a file is loaded. With no file,
// or with one that would not parse, the row is still drawn so the screen keeps
// its shape, and the file is picked from the parameter menu.
bool transport_interactive(State state) {
    return state == kStateIdleFileLoaded || state == kStatePlaying || state == kStatePaused
        || state == kStatePlaybackFinished;
}

}  // namespace

// ---------------------------------------------------------------------------
// File loading
// ---------------------------------------------------------------------------

namespace {

// Zero the per-track rolling windows so a stale window does
// not leak across a fresh load.
void reset_track_buffers() {
    for (auto& window : g_track_buffers) {
        window.window_offset = kInvalidWindow;
        window.window_size   = 0;
    }
}

// Reset parser state + pending-event cache when we begin playing a new
// file. Rolling buffers are reset separately (cheaper to keep warm
// across re-parse of the same file after stop).
void reset_track_cursors() {
    for (uint8_t i = 0; i < op::samples::smf::kMaxTracks; ++i) {
        g_tracks[i]        = op::samples::smf::TrackCursor {};
        g_pending[i]       = op::samples::smf::Event {};
        g_pending_valid[i] = false;
    }
    g_track_count = 0;
}

// Decode the first event on every track, which is what playback compares to find
// the one that falls due next. A track whose first event will not decode is
// simply over before it starts.
void prime_pending_events() {
    if (g_file_handle < 0) return;
    const op::samples::midi_player::StorageFileReader reader {g_file_handle, g_file_size};
    for (uint8_t i = 0; i < g_track_count; ++i) {
        CachedTrackReader cached {reader, g_track_buffers[i]};
        op::samples::smf::Event event {};
        const bool decoded = op::samples::smf::next_event(cached, &g_tracks[i], &g_header, &event);
        g_pending[i]       = event;
        g_pending_valid[i] = decoded;
    }
}

// Safe at any point, whether a file is open or not.
void close_loaded_file() {
    if (g_file_handle >= 0) {
        if (op::api) op::api->storage_file_close(g_file_handle);
        g_file_handle = -1;
    }
    g_file_size = 0;
    reset_track_buffers();
}

// Work out how long the file is by walking its chunk headers and adding them up,
// since nothing else reports the size. Fails on a malformed header, or on a file
// that claims more bytes than the cap allows.
bool probe_file_size(uint32_t* out_total) {
    using op::samples::midi_player::StorageFileReader;
    const StorageFileReader probe {g_file_handle, 0xFFFFFFFFu};

    // A busy read is not a malformed file, so it raises the flag and the load is
    // left to try again.
    uint8_t header_bytes[14];
    {
        const int32_t read_count = probe.read_at(0, header_bytes, 14);
        if (read_count == kStorageBusy) {
            g_storage_busy = true;
            return false;
        }
        if (read_count != 14) return false;
    }

    op::samples::smf::Header probe_header {};
    if (!op::samples::smf::parse_header(header_bytes, 14, &probe_header)) return false;

    uint32_t total = 14;
    for (uint16_t i = 0; i < probe_header.track_count; ++i) {
        uint8_t chunk[8];
        const int32_t read_count = probe.read_at(total, chunk, 8);
        if (read_count == kStorageBusy) {
            g_storage_busy = true;
            return false;
        }
        if (read_count != 8) return false;
        if (chunk[0] != 'M' || chunk[1] != 'T' || chunk[2] != 'r' || chunk[3] != 'k') {
            return false;
        }
        const uint32_t length = op::samples::smf::read_be32(chunk, 4);
        // A declared length is checked against the cap before it is added, so the
        // running total keeps its footing whatever the file claims.
        if (length > op::samples::smf::kMaxFileSize || total > op::samples::smf::kMaxFileSize - 8 - length) {
            return false;
        }
        total += 8 + length;
        if (total > op::samples::smf::kMaxFileSize) return false;
    }
    *out_total = total;
    return true;
}

bool filename_equal(const char* a, const char* b) noexcept {
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

void copy_filename(char* dst, const char* src, std::size_t dst_size) noexcept {
    std::size_t i = 0;
    while (src[i] != '\0' && i + 1 < dst_size) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

// Open the picked file and read its headers. Called each tick, and does its work
// only when the pick has changed.
//
// Storage that reports itself busy is simply not ready, so every step below
// leaves the loaded name as it was and lets the next tick start the load over,
// where a genuine failure moves the mode to Error.
void try_load_selected(const char* requested_filename) {
    if (!requested_filename) return;
    if (filename_equal(requested_filename, g_loaded_filename)) return;

    // Kept so a busy read can put the loaded name back and try again next tick.
    char prior_filename[sizeof(g_loaded_filename)];
    copy_filename(prior_filename, g_loaded_filename, sizeof(prior_filename));

    // Clear the way first, so a load that fails leaves nothing half-read behind.
    close_loaded_file();
    reset_track_cursors();
    copy_filename(g_loaded_filename, requested_filename, sizeof(g_loaded_filename));

    if (requested_filename[0] == '\0') {
        g_state = kStateIdleNoFile;
        return;
    }

    g_file_handle = op::api->storage_file_open(g_loaded_filename);
    if (g_file_handle == kStorageBusy) {
        g_storage_busy = true;
        // Roll the name back, so the pick reads as fresh again and the next tick
        // retries the open.
        copy_filename(g_loaded_filename, prior_filename, sizeof(g_loaded_filename));
        return;
    }
    if (g_file_handle < 0) {
        op::log(op::LogLevel::Warn, "BAD MIDI FILE");
        g_state = kStateError;
        return;
    }

    uint32_t total = 0;
    if (!probe_file_size(&total)) {
        if (g_storage_busy) {
            close_loaded_file();
            copy_filename(g_loaded_filename, prior_filename, sizeof(g_loaded_filename));
            return;
        }
        op::log(op::LogLevel::Warn, "BAD MIDI FILE");
        close_loaded_file();
        g_state = kStateError;
        return;
    }
    g_file_size = total;

    // Finding the tracks only reads the headers, so one reader covers it.
    using op::samples::midi_player::StorageFileReader;
    StorageFileReader loader {g_file_handle, g_file_size};
    if (!op::samples::smf::load_file(loader, &g_header, g_tracks, &g_track_count)) {
        if (g_storage_busy) {
            close_loaded_file();
            copy_filename(g_loaded_filename, prior_filename, sizeof(g_loaded_filename));
            return;
        }
        op::log(op::LogLevel::Warn, "BAD MIDI FILE");
        close_loaded_file();
        g_state = kStateError;
        return;
    }

    // A file with more tracks than the cap allows loads with the first sixteen, so
    // say as much. The header count is read back from the file and compared with
    // the clamped one the parser kept.
    {
        uint8_t header_bytes[14];
        if (loader.read_at(0, header_bytes, 14) == 14) {
            const uint16_t declared = op::samples::smf::read_be16(header_bytes, 10);
            if (declared > op::samples::smf::kMaxTracks) {
                op::log(op::LogLevel::Warn, "midi_player: playing the first 16 tracks");
            }
        }
    }

    prime_pending_events();
    // The headers are read but the events are not, so start the load over.
    if (g_storage_busy) {
        close_loaded_file();
        reset_track_cursors();
        copy_filename(g_loaded_filename, prior_filename, sizeof(g_loaded_filename));
        return;
    }
    g_state           = kStateIdleFileLoaded;
    g_selected_button = 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

namespace {

// Where the song has reached, in the file's own ticks.
//
// Following the device clock, that is the beats gone by times the ticks in a
// beat. On the file's own tempo it is the microseconds gone by, scaled by the
// Where the device clock has reached, in beats. It runs at 24 pulses to the beat,
// and the fraction carries the position between two of them.
//
// The screen reads the clock as well, so this can run twice in a tick. The
// fraction only ever climbs within a pulse and resets on the next edge, which
// holds the two readings to within a fraction of a pulse.
float clock_beat() {
    const op::sdk::ClockInterpolator::Position position = g_clock.now();
    return (static_cast<float>(position.pulse) + position.frac) / 24.0f;
}

// A song position in beats, so it can be compared against the clock.
float ticks_to_beats(uint32_t ticks) {
    const float per_quarter = g_header.ticks_per_quarter > 0 ? static_cast<float>(g_header.ticks_per_quarter)
                                                             : 1.0f;
    return static_cast<float>(ticks) / per_quarter;
}

// tempo the file declares. A paused transport holds where it stopped.
uint32_t song_position_ticks(uint32_t tick_us, bool follow_clock) {
    if (g_state == kStatePaused) return g_paused_tick;
    if (g_state != kStatePlaying) return 0;

    if (follow_clock) {
        const float beat_delta = clock_beat() - g_play_start_beat;
        if (beat_delta <= 0.0f) return 0;
        const float ticks = beat_delta * static_cast<float>(g_header.ticks_per_quarter);
        if (ticks < 0.0f) return 0;
        return static_cast<uint32_t>(ticks);
    }

    if (tick_us < g_play_start_us) return 0;
    const uint64_t elapsed_us = static_cast<uint64_t>(tick_us - g_play_start_us);
    const uint64_t tempo      = g_header.tempo_us_per_quarter ? g_header.tempo_us_per_quarter : 500000u;
    return static_cast<uint32_t>((elapsed_us * static_cast<uint64_t>(g_header.ticks_per_quarter)) / tempo);
}

// Send every event that has fallen due and move each track on to its next. The
// parser has already dealt with the events that are not notes, so everything
// arriving here goes straight out.
void advance_playback(uint32_t now_tick) {
    if (g_track_count == 0 || g_file_handle < 0) return;
    bool any_active = false;
    const op::samples::midi_player::StorageFileReader reader {g_file_handle, g_file_size};

    for (uint8_t i = 0; i < g_track_count; ++i) {
        CachedTrackReader cached {reader, g_track_buffers[i]};
        while (g_pending_valid[i] && g_pending[i].tick <= now_tick) {
            const auto& event = g_pending[i];
            if (event.length == 3) {
                op::api->send_midi(kAnyOutput, event.status, event.data1, event.data2);
                // Keep the active-notes set in sync with what
                // was actually emitted so a subsequent transport exit can
                // panic only the channels that hold ringing notes.
                track_emitted_event(event.status, event.data1, event.data2);
            } else if (event.length == 2) {
                op::api->send_midi(kAnyOutput, event.status, event.data1, 0);
                track_emitted_event(event.status, event.data1, 0);
            }
            op::samples::smf::Event next {};
            const bool decoded = op::samples::smf::next_event(cached, &g_tracks[i], &g_header, &next);
            // A busy read leaves the pending event where it was, so the track picks
            // it up again next tick.
            if (g_storage_busy) {
                any_active = true;
                break;
            }
            g_pending[i]       = next;
            g_pending_valid[i] = decoded;
        }
        // A track that merely could not read this tick has not ended.
        if (g_storage_busy) {
            any_active = true;
            break;
        }
        if (g_pending_valid[i]) any_active = true;
        // A track can run out of events without ever saying so.
        if (!g_tracks[i].ended && !g_pending_valid[i]) {
            g_tracks[i].ended = true;
        }
    }

    // Every track has run out, so the song is over. A well-formed file closes its
    // own notes, and a truncated one may not, so the synth is silenced either way.
    // The file stays open and is wound back to the top, ready for the next play.
    if (!any_active) {
        silence_active_channels();
        g_state = kStatePlaybackFinished;
        reset_track_cursors();
        reset_track_buffers();
        const op::samples::midi_player::StorageFileReader reader {g_file_handle, g_file_size};
        if (op::samples::smf::load_file(reader, &g_header, g_tracks, &g_track_count)) {
            prime_pending_events();
        }
        g_selected_button = 0;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void init() {
    g_state              = kStateIdleNoFile;
    g_file_handle        = -1;
    g_file_size          = 0;
    g_loaded_filename[0] = '\0';
    g_selected_button    = 0;
    g_play_start_us      = 0;
    g_play_start_beat    = 0.0f;
    g_paused_tick        = 0;
    reset_track_cursors();
    reset_track_buffers();
    clear_active_notes();
}

void process(OpMidiMessage* /*msgs*/, uint8_t /*count*/, uint32_t tick_us) {
    if (!op::api) return;

    g_storage_busy = false;

    const auto file = param<MidiFile>();
    try_load_selected(file.filename);

    if (g_state != kStatePlaying) return;

    // The load is not finished, so there is nothing to play from yet.
    if (g_storage_busy) return;

    const bool follow       = param<ClockSource>() != 0;
    const uint32_t now_tick = song_position_ticks(tick_us, follow);
    advance_playback(now_tick);
}

void destroy() {
    // Unloading mid-song leaves the synth quiet, and the silencing goes out first
    // while there is still an api to send it through.
    silence_active_channels();
    close_loaded_file();
    g_state = kStateIdleNoFile;
    reset_track_cursors();
    g_loaded_filename[0] = '\0';
    g_selected_button    = 0;
    clear_active_notes();
}

// ---------------------------------------------------------------------------
// Custom UI
// ---------------------------------------------------------------------------

namespace {

constexpr uint16_t kScreenW = 128;

// A windowed custom UI paints into the screen less its status bar, so the canvas
// is 128 x 53. Four rows with a 1px margin and 3px between them fill it:
// 1 + 8 + 3 + 13 + 3 + 8 + 3 + 13 = 52.
constexpr uint16_t kFilenameY  = 1;
constexpr uint16_t kTimeY      = 12;
constexpr uint16_t kClockSrcY  = 28;
constexpr uint16_t kButtonRowY = 39;

// Each icon button occupies a 17x13 cell: the 7x7 symbol sprite is centred
// inside it - 5px of horizontal padding and 3px vertical on each side - and
// a selected button gains a border around the cell. The cell is wider than
// tall so the highlight box does not crowd the sprite left/right.
constexpr uint16_t kButtonCellW = 17;
constexpr uint16_t kButtonCellH = 13;

// A space is drawn narrower than a glyph in both fonts.
constexpr uint16_t kStdGlyphAdv   = 6;
constexpr uint16_t kStdSpaceAdv   = 4;
constexpr uint16_t kLargeGlyphAdv = 8;
constexpr uint16_t kLargeSpaceAdv = 6;

// How wide a string comes out, counting its spaces at their narrower width. The
// time row carries two of them, and a width that missed that would sit the row
// off-center by several pixels.
uint16_t rendered_width(const char* text, uint16_t glyph_adv, uint16_t space_adv) {
    uint16_t width = 0;
    for (; text && *text; ++text) {
        width += (*text == ' ') ? space_adv : glyph_adv;
    }
    return width;
}

// Center a string on the screen. The last glyph leaves a pixel of trailing gap,
// so it is the ink that is centered and the margins come out even.
uint16_t center_text_x(uint16_t advance_w) {
    const uint16_t ink = advance_w > 1u ? static_cast<uint16_t>(advance_w - 1u) : advance_w;
    return (kScreenW > ink) ? static_cast<uint16_t>((kScreenW - ink) / 2u) : 0u;
}

// Format "MM:SS" into a 6-byte buffer (exclusive of NUL). `total_us` is
// the count of microseconds to encode, we cap at 99:59 so pathological
// long files don't spill past the 5-glyph slot.
void format_mmss(uint32_t total_us, char out[6]) {
    uint32_t total_seconds = total_us / 1'000'000u;
    if (total_seconds > 99u * 60u + 59u) total_seconds = 99u * 60u + 59u;
    const uint32_t minutes = total_seconds / 60u;
    const uint32_t seconds = total_seconds % 60u;
    out[0]                 = '0' + (minutes / 10);
    out[1]                 = '0' + (minutes % 10);
    out[2]                 = ':';
    out[3]                 = '0' + (seconds / 10);
    out[4]                 = '0' + (seconds % 10);
    out[5]                 = '\0';
}

// Convert an SMF tick count to microseconds at an explicit us-per-quarter
// tempo + the file's ticks_per_quarter. 64-bit intermediate avoids overflow for
// long files.
uint32_t ticks_to_us_at_tempo(uint32_t ticks, uint32_t us_per_quarter) {
    if (g_header.ticks_per_quarter <= 0) return 0;
    const uint64_t tempo = us_per_quarter ? us_per_quarter : 500000u;
    return static_cast<uint32_t>((static_cast<uint64_t>(ticks) * tempo)
                                 / static_cast<uint64_t>(g_header.ticks_per_quarter));
}

// Convert an SMF tick count to microseconds at the file's own tempo.
uint32_t ticks_to_us(uint32_t ticks) {
    return ticks_to_us_at_tempo(ticks, g_header.tempo_us_per_quarter);
}

// How long the song runs, in the file's ticks, worked out once at play.
uint32_t g_song_length_ticks = 0;

// Set when the duration walk came up short because storage was busy. A later
// frame picks it up, so the total settles a moment after the song starts.
bool g_duration_pending = false;

// A smoothed tempo for the time row while the device clock is driving. The
// measured pulse period wobbles with whatever is sending the clock, and averaging
// it holds the readout steady. Zero until the first measurement.
float g_shown_us_per_quarter = 0.0f;

void recompute_total_duration() {
    g_song_length_ticks = 0;
    g_duration_pending  = false;
    if (g_file_size == 0 || g_file_handle < 0) return;
    if (g_loaded_filename[0] == '\0') return;

    // This walk reads the whole file, so the open and every read in it can come
    // back busy while the other core is writing. Clearing the flag first makes
    // the check after the walk describe this pass alone.
    g_storage_busy = false;

    // The screen works this out while playback may be reading the file on the
    // other core, and a handle carries one read position, so this walk opens its
    // own and leaves the one playback is using alone.
    const int32_t handle = op::api->storage_file_open(g_loaded_filename);
    if (handle == kStorageBusy) {
        g_duration_pending = true;
        return;
    }
    if (handle < 0) return;

    using op::samples::midi_player::StorageFileReader;
    const StorageFileReader reader {handle, g_file_size};

    op::samples::smf::Header header {};
    op::samples::smf::TrackCursor tracks[op::samples::smf::kMaxTracks] {};
    uint8_t track_count = 0;
    if (!op::samples::smf::load_file(reader, &header, tracks, &track_count)) {
        op::api->storage_file_close(handle);
        g_duration_pending = g_storage_busy;
        return;
    }

    // Windows of their own, so the walk leaves the ones playback is reading
    // through untouched. They are static because 16 KB is far too much to put on
    // the stack.
    static TrackBuffer scratch[op::samples::smf::kMaxTracks];
    for (auto& window : scratch) {
        window.window_offset = kInvalidWindow;
        window.window_size   = 0;
    }

    uint32_t max_tick = 0;
    for (uint8_t i = 0; i < track_count; ++i) {
        CachedTrackReader cached {reader, scratch[i]};
        op::samples::smf::Event event {};
        while (op::samples::smf::next_event(cached, &tracks[i], &header, &event)) {
            if (event.tick > max_tick) max_tick = event.tick;
        }
    }

    op::api->storage_file_close(handle);

    // A read that answered busy leaves the walk short of the last event, so the
    // tick it reached describes a song that ends early.
    if (g_storage_busy) {
        g_duration_pending = true;
        return;
    }

    g_song_length_ticks = max_tick;
}

// The 7x7 transport symbols, a byte to a column. The play and pause glyph carries
// both a bar and a triangle, since the one button does both and reads right
// whichever the transport is about to do.
constexpr uint16_t kIconSize                = 7;
constexpr uint8_t kIconPlayPause[kIconSize] = {
    0x7F, 0x00, 0x7F, 0x7F, 0x3E, 0x1C, 0x08,
};
constexpr uint8_t kIconStop[kIconSize] = {
    0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
};

// The symbol is centered in its cell, and the selected button gains a border.
void draw_icon_button(uint16_t cell_x, uint16_t cell_y, const uint8_t* icon, bool selected) {
    constexpr uint16_t kOffX = (kButtonCellW - kIconSize) / 2;  // = 5
    constexpr uint16_t kOffY = (kButtonCellH - kIconSize) / 2;  // = 3
    op::draw::draw_bitmap(cell_x + kOffX, cell_y + kOffY, icon, kIconSize, kIconSize);
    if (selected) {
        op::draw::draw_rect(cell_x, cell_y, kButtonCellW, kButtonCellH);
    }
}

// Two 17x13 cells with a 16px gap, centered across the screen.
void render_button_row() {
    constexpr uint16_t kGap   = 16;
    constexpr uint16_t kTotal = kButtonCellW * 2 + kGap;
    const uint16_t x0         = (kScreenW - kTotal) / 2;
    const uint16_t x1         = x0 + kButtonCellW + kGap;

    draw_icon_button(x0, kButtonRowY, kIconPlayPause,
                     /*selected=*/(g_selected_button == 0));
    draw_icon_button(x1, kButtonRowY, kIconStop,
                     /*selected=*/(g_selected_button == 1));
}

// 21 glyphs reach across the screen, so a longer name is cut short.
void render_filename_row() {
    if (g_state == kStateIdleNoFile || g_state == kStateError) {
        op::draw::draw_text(2, kFilenameY, "(no file selected)");
        return;
    }
    char truncated[24];
    op::sdk::text::truncate_with_ellipsis(g_loaded_filename, 21, truncated, sizeof(truncated));
    op::draw::draw_text(2, kFilenameY, truncated);
}

// Elapsed and total, as MM:SS / MM:SS. Once the song is over the elapsed half
// holds the full duration, so the row stays where the music left it.
void render_time_row(uint32_t tick_us, bool follow) {
    if (g_state == kStateError) {
        const char* label = "Error";
        op::draw::draw_text_large(center_text_x(rendered_width(label, kLargeGlyphAdv, kLargeSpaceAdv)),
                                  kTimeY, label);
        return;
    }
    if (g_state == kStateIdleNoFile) {
        const char* label = "00:00 / 00:00";
        op::draw::draw_text_large(center_text_x(rendered_width(label, kLargeGlyphAdv, kLargeSpaceAdv)),
                                  kTimeY, label);
        return;
    }

    // The times shown follow whatever is driving the music. On the file's own
    // tempo that is what the file declares. On the device clock it is the measured
    // pulse period, twenty-four to the quarter note, smoothed so the readout does
    // not twitch. Until the clock has been measured, the file's tempo stands in.
    uint32_t us_per_quarter = g_header.tempo_us_per_quarter ? g_header.tempo_us_per_quarter : 500000u;
    if (follow) {
        const uint32_t period = g_clock.pulse_period_us();
        if (period > 0) {
            const float measured   = period * 24u;
            g_shown_us_per_quarter = (g_shown_us_per_quarter <= 0.0f)
                ? measured
                : g_shown_us_per_quarter + (measured - g_shown_us_per_quarter) * 0.10f;
            us_per_quarter         = static_cast<uint32_t>(g_shown_us_per_quarter);
        }
    }

    uint32_t current_tick = 0;
    if (g_state == kStatePlaying) {
        current_tick = song_position_ticks(tick_us, follow);
    } else if (g_state == kStatePaused) {
        current_tick = g_paused_tick;
    } else if (g_state == kStatePlaybackFinished) {
        current_tick = g_song_length_ticks;
    }

    char text[14];  // "MM:SS / MM:SS" and its terminator
    format_mmss(ticks_to_us_at_tempo(current_tick, us_per_quarter), text);
    text[5] = ' ';
    text[6] = '/';
    text[7] = ' ';
    format_mmss(ticks_to_us_at_tempo(g_song_length_ticks, us_per_quarter), text + 8);
    op::draw::draw_text_large(center_text_x(rendered_width(text, kLargeGlyphAdv, kLargeSpaceAdv)), kTimeY,
                              text);
}

void render_clock_source_row(bool follow) {
    if (g_state == kStateError) return;
    const char* label = follow ? "Clock: External" : "Clock: File";
    op::draw::draw_text(center_text_x(rendered_width(label, kStdGlyphAdv, kStdSpaceAdv)), kClockSrcY, label);
}

}  // namespace

void ui_render() {
    if (!op::api) return;

    // A duration walk that lost its reads to a busy store gets another go here,
    // where the value is about to be drawn and storage has usually freed up.
    if (g_duration_pending) recompute_total_duration();

    // The canvas arrives cleared each frame.
    const bool follow = op::api->get_param_value(0) != 0;
    render_filename_row();
    render_time_row(op::api->get_tick(), follow);
    render_clock_source_row(follow);
    render_button_row();
}

// The mode branches on the gesture type alone, and the active input map decides
// which encoder and which press produce each one. Change cycles the Clock Source,
// Scroll moves along the buttons, and Enter presses the one selected.
void ui_gesture(uint8_t /*encoder_id*/,
                op::Gesture /*gesture*/,
                op::GestureType gesture_type,
                int16_t value) {
    if (!op::api) return;

    if (gesture_type == op::GestureType::Change) {
        if (value == 0) return;
        const int32_t current = op::api->get_param_value(0);

        // The two clock sources measure the position from different origins, both
        // set at play. Switching mid-song carries both origins back to where the
        // song has actually reached, so the new source reads that same position and
        // only the rate it runs at changes from here.
        if (g_state == kStatePlaying) {
            const bool was_following    = (current != 0);
            const uint32_t now_us       = op::api->get_tick();
            const uint32_t current_tick = song_position_ticks(now_us, was_following);
            const uint32_t current_us   = ticks_to_us(current_tick);
            g_play_start_us             = (now_us >= current_us) ? (now_us - current_us) : 0u;

            g_play_start_beat = clock_beat() - ticks_to_beats(current_tick);
        }

        op::api->set_param_value(0, current ? 0 : 1);
        return;
    }

    if (gesture_type == op::GestureType::Scroll) {
        if (!transport_interactive(g_state)) return;
        int32_t next = g_selected_button + value;
        if (next < 0) next = 0;
        if (next > kButtonCount - 1) next = kButtonCount - 1;
        g_selected_button = next;
        return;
    }

    if (gesture_type == op::GestureType::Enter) {
        if (!transport_interactive(g_state)) return;
        if (g_selected_button >= kButtonCount) return;

        if (g_selected_button == 1) {
            // Stop winds the song back to its beginning, which leaves the releases
            // for any sounding notes behind it, so they are silenced first.
            silence_active_channels();
            g_state           = kStateIdleFileLoaded;
            g_selected_button = 0;
            reset_track_cursors();
            reset_track_buffers();
            if (g_file_handle >= 0) {
                const op::samples::midi_player::StorageFileReader reader {g_file_handle, g_file_size};
                if (op::samples::smf::load_file(reader, &g_header, g_tracks, &g_track_count)) {
                    prime_pending_events();
                }
            }
            g_paused_tick = 0;
            return;
        }

        // Button 0 plays, pauses, and resumes.
        if (g_state == kStatePlaying) {
            // The position is kept, so the screen holds it and a resume starts from
            // it. A pause stops the song mid-note, so the notes it was holding are
            // silenced.
            const bool follow = op::api->get_param_value(0) != 0;
            g_paused_tick     = song_position_ticks(op::api->get_tick(), follow);
            silence_active_channels();
            g_state           = kStatePaused;
            g_selected_button = 0;
            return;
        }

        if (g_state == kStatePaused) {
            // Both origins move back by where the song had reached, so the position
            // they read is the one the pause left and the music carries straight on
            // from it.
            const uint32_t paused_us = ticks_to_us(g_paused_tick);
            const uint32_t now_us    = op::api->get_tick();
            g_play_start_us          = (now_us >= paused_us) ? (now_us - paused_us) : 0u;

            g_play_start_beat = clock_beat() - ticks_to_beats(g_paused_tick);
        } else {
            // Playing a song that has finished starts it again from the top.
            if (g_state == kStatePlaybackFinished) {
                reset_track_cursors();
                reset_track_buffers();
                if (g_file_handle >= 0) {
                    const op::samples::midi_player::StorageFileReader reader {g_file_handle, g_file_size};
                    if (op::samples::smf::load_file(reader, &g_header, g_tracks, &g_track_count)) {
                        prime_pending_events();
                    }
                }
                g_paused_tick = 0;
            }
            g_play_start_us        = op::api->get_tick();
            g_play_start_beat      = clock_beat();
            g_shown_us_per_quarter = 0.0f;  // measure the clock's tempo afresh
        }
        g_state           = kStatePlaying;
        g_selected_button = 0;
        recompute_total_duration();
        return;
    }

    // The file is picked from the parameter menu, so nothing else lands here.
}

OP_MODE_REGISTER(init, process, destroy);
OP_MODE_REGISTER_UI(ui_render, ui_gesture);
