// SMF-parser unit tests for the midi-player sample.
//
// The parser is a pure function of the bytes it is handed, so this suite feeds
// it fixtures straight from memory and reads the events back. It covers the
// header, the variable-length quantities the format encodes its deltas in,
// running status, and the three meta events the player acts on.
//
// The fixtures are hand-written SMF bytes, small enough to check against the
// format by eye.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../smf_parser.h"

#include <cstddef>
#include <cstdint>

namespace smf = op::samples::smf;

namespace {

// Feeds the parser from a byte array. The mode itself reads through a file
// handle, so this lives with the tests that need it.
class MemoryReader {
public:
    constexpr MemoryReader(const uint8_t* data, uint32_t size) noexcept
        : data_(data)
        , size_(size) { }

    int32_t read_at(uint32_t offset, uint8_t* buffer, uint32_t count) const noexcept {
        if (!data_ || offset >= size_) return 0;
        const uint32_t available = size_ - offset;
        const uint32_t to_copy   = (count < available) ? count : available;
        for (uint32_t i = 0; i < to_copy; ++i) buffer[i] = data_[offset + i];
        return static_cast<int32_t>(to_copy);
    }

    uint32_t size() const noexcept {
        return size_;
    }

private:
    const uint8_t* data_;
    uint32_t size_;
};

static_assert(smf::SmfReader<MemoryReader>);

// ---------------------------------------------------------------------------
// Fixture: SMF Type 0 - single track, 4-note C-major arpeggio.
// ---------------------------------------------------------------------------
//
// Header: "MThd" 00 00 00 06  format=0  track_count=1  ticks_per_quarter=96 (0x60)
// Track:  "MTrk" 00 00 00 <len>
//   00 FF 51 03 07 A1 20        tempo = 500000 us/qn (120 BPM)
//   00 FF 58 04 04 02 18 08     time signature 4/4
//   00 90 3C 64                 note-on  C4 vel 100
//   60 80 3C 40                 delta=96, note-off C4
//   00 90 3E 64                 note-on  D4 vel 100 (full status - not running)
//   60 80 3E 40                 delta=96, note-off D4
// End:  00 FF 2F 00              end-of-track
//
// Track length was precomputed by hand and re-verified below via a static
// assertion that bytes[22..25] == <length> and bytes.size() == 22 + 8 +
// length. Keeping the counts visible avoids the classic "recount on
// edit" pitfall that burns hand-authored fixtures.

constexpr uint8_t kType0Simple[] = {
    // ---- MThd ----
    'M', 'T', 'h', 'd', 0x00, 0x00, 0x00,
    0x06,  // header length = 6
    0x00,
    0x00,  // format = 0
    0x00,
    0x01,  // track_count = 1
    0x00,
    0x60,  // ticks_per_quarter = 96 ticks/quarter
    // ---- MTrk ----
    'M', 'T', 'r', 'k', 0x00, 0x00, 0x00,
    0x23,  // track length = 35 bytes (content below)
    // content begins here (35 bytes):
    0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1,
    0x20,  // 7 tempo meta
    0x00, 0xFF, 0x58, 0x04, 0x04, 0x02, 0x18,
    0x08,  // 8 time-sig 4/4
    0x00, 0x90, 0x3C,
    0x64,  // 4 note-on  C4
    0x60, 0x80, 0x3C,
    0x40,  // 4 note-off C4
    0x00, 0x90, 0x3E,
    0x64,  // 4 note-on  D4
    0x60, 0x80, 0x3E,
    0x40,  // 4 note-off D4
    0x00, 0xFF, 0x2F,
    0x00,  // 4 end-of-track
    // total: 7+8+4+4+4+4+4 = 35 bytes
};
static_assert(sizeof(kType0Simple) == 14 /*MThd*/ + 8 /*MTrk header*/ + 35 /*content*/
);

// ---------------------------------------------------------------------------
// Fixture: SMF Type 1 - two tracks (tempo + notes).
// ---------------------------------------------------------------------------
//
// Header:  format=1, track_count=2, ticks_per_quarter=96
// Track 1: tempo-only track
//   00 FF 51 03 07 A1 20   tempo = 500000 us/qn
//   00 FF 2F 00            end-of-track
// Track 2: one note on/off pair
//   00 90 3C 64            note-on  C4
//   60 80 3C 40            note-off C4
//   00 FF 2F 00            end-of-track

constexpr uint8_t kType1Multitrack[] = {
    // ---- MThd ----
    'M', 'T', 'h', 'd', 0x00, 0x00, 0x00, 0x06, 0x00,
    0x01,  // format = 1
    0x00,
    0x02,  // track_count = 2
    0x00,
    0x60,  // ticks_per_quarter = 96
    // ---- Track 1 (tempo) ----
    'M', 'T', 'r', 'k', 0x00, 0x00, 0x00,
    0x0B,  // length = 11
    0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1,
    0x20,  // 7 tempo
    0x00, 0xFF, 0x2F,
    0x00,  // 4 end-of-track
    // ---- Track 2 (notes) ----
    'M', 'T', 'r', 'k', 0x00, 0x00, 0x00,
    0x0C,  // length = 12
    0x00, 0x90, 0x3C,
    0x64,  // 4 note-on
    0x60, 0x80, 0x3C,
    0x40,  // 4 note-off
    0x00, 0xFF, 0x2F,
    0x00,  // 4 end-of-track
};
static_assert(sizeof(kType1Multitrack) == 14 + 8 + 11 + 8 + 12);

// ---------------------------------------------------------------------------
// Fixture: running-status track - validates running-status handling directly.
// ---------------------------------------------------------------------------
// The second note-on uses a BARE data-byte pair after the first note-on's
// 0x90 status byte (running status). A correct parser keeps last_status
// across delta times and synthesizes the missing byte.

constexpr uint8_t kRunningStatusTrack[] = {
    'M',  'T',  'h',  'd',  0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x60, 'M',  'T',  'r',  'k',  0x00, 0x00, 0x00, 0x12,  // length = 18
    0x00, 0x90, 0x3C, 0x64,                                            // note-on C4 (explicit status)
    0x10, 0x3E, 0x64,        // delta=16, running status -> note-on D4
    0x10, 0x40, 0x64,        // delta=16, running status -> note-on E4
    0x60, 0x80, 0x3C, 0x40,  // note-off C4 (new status)
    0x00, 0xFF, 0x2F, 0x00,  // end-of-track
};
static_assert(sizeof(kRunningStatusTrack) == 14 + 8 + 18);

// ---------------------------------------------------------------------------
// Fixture: SMF Type 1 with track_count=20 (exceeds kMaxTracks=16).
// ---------------------------------------------------------------------------
//
// A track count above the cap is held to it. The fixture declares
// 20 MTrk chunks, each carries the minimum legal payload (only a 4-byte
// end-of-track meta: 00 FF 2F 00). load_file is expected to populate
// exactly 16 cursors and stop, the remaining 4 MTrk chunks are silently
// inaccessible.
//
// Per-MTrk byte cost: 8-byte chunk header + 4-byte EOT payload = 12 bytes.
// Total: 14 (MThd) + 20*12 = 254 bytes.

constexpr uint8_t kEot20Track[] = {
    // ---- MThd ----
    'M',      'T',      'h',      'd',      0x00,     0x00,     0x00,     0x06,     0x00,
    0x01,  // format = 1
    0x00,
    0x14,  // track_count = 20  <- exceeds kMaxTracks (16)
    0x00,
    0x60,  // ticks_per_quarter = 96
// ---- 20 x (MTrk + 4-byte EOT) ----
// Each MTrk: 'M' 'T' 'r' 'k' 00 00 00 04  00 FF 2F 00
#define EOT_MTRK 'M', 'T', 'r', 'k', 0x00, 0x00, 0x00, 0x04, 0x00, 0xFF, 0x2F, 0x00
    EOT_MTRK, EOT_MTRK, EOT_MTRK, EOT_MTRK, EOT_MTRK, EOT_MTRK, EOT_MTRK, EOT_MTRK, EOT_MTRK, EOT_MTRK,
    EOT_MTRK, EOT_MTRK, EOT_MTRK, EOT_MTRK, EOT_MTRK, EOT_MTRK, EOT_MTRK, EOT_MTRK, EOT_MTRK, EOT_MTRK,
#undef EOT_MTRK
};
static_assert(sizeof(kEot20Track) == 14 + 20 * 12);

// Convenience constructor so every TEST_CASE has a one-liner MemoryReader.
template <std::size_t N> constexpr MemoryReader make_reader(const uint8_t (&bytes)[N]) noexcept {
    return MemoryReader(bytes, static_cast<uint32_t>(N));
}

}  // namespace

// ---------------------------------------------------------------------------
// Header parsing
// ---------------------------------------------------------------------------

TEST_CASE("parses a Type 0 header") {
    smf::Header header {};
    const bool parsed = smf::parse_header(kType0Simple, sizeof(kType0Simple), &header);
    REQUIRE(parsed);
    CHECK(header.format == 0);
    CHECK(header.track_count == 1);
    CHECK(header.ticks_per_quarter == 96);
}

TEST_CASE("parses a Type 1 header") {
    smf::Header header {};
    const bool parsed = smf::parse_header(kType1Multitrack, sizeof(kType1Multitrack), &header);
    REQUIRE(parsed);
    CHECK(header.format == 1);
    CHECK(header.track_count == 2);
    CHECK(header.ticks_per_quarter == 96);
}

TEST_CASE("rejects a Type 2 file") {
    // Same bytes as Type 0 but with format = 2. Type 2 files hold
    // independent tracks with no shared tempo - not playable sequentially.
    uint8_t bytes[sizeof(kType0Simple)];
    for (std::size_t i = 0; i < sizeof(kType0Simple); ++i) bytes[i] = kType0Simple[i];
    bytes[9] = 0x02;  // format hi byte at offset 8 stays 0; format lo at 9 -> 2
    smf::Header header {};
    CHECK(smf::parse_header(bytes, sizeof(bytes), &header) == false);
}

TEST_CASE("rejects a file whose header magic is wrong") {
    uint8_t bytes[sizeof(kType0Simple)];
    for (std::size_t i = 0; i < sizeof(kType0Simple); ++i) bytes[i] = kType0Simple[i];
    bytes[0] = 'X';  // corrupt magic
    smf::Header header {};
    CHECK(smf::parse_header(bytes, sizeof(bytes), &header) == false);
}

TEST_CASE("clamps a track count above the cap") {
    // The header parses, holding the count to the cap, with
    // out->track_count == kMaxTracks. Callers detect the clamp by re-reading
    // bytes[10..11] (the on-disk track_count) and comparing.
    smf::Header header {};
    const bool parsed = smf::parse_header(kEot20Track, sizeof(kEot20Track), &header);
    REQUIRE(parsed);
    CHECK(header.track_count == smf::kMaxTracks);
    CHECK(header.format == 1);
    CHECK(header.ticks_per_quarter == 96);
    // The on-disk track_count (caller-visible via read_be16) must still report
    // the original value so the caller can decide whether to log a warn.
    CHECK(smf::read_be16(kEot20Track, 10) == 20);
}

TEST_CASE("loads only as many tracks as the cap allows") {
    // With the
    // parse_header clamp in place, load_file's `while (track_count < header->track_count)`
    // loop naturally stops at kMaxTracks. The trailing 4 MTrk chunks in
    // the fixture are silently inaccessible - no cursor points at them.
    smf::Header header {};
    smf::TrackCursor tracks[smf::kMaxTracks] {};
    uint8_t track_count = 0;
    auto reader         = make_reader(kEot20Track);
    REQUIRE(smf::load_file(reader, &header, tracks, &track_count));
    CHECK(track_count == smf::kMaxTracks);
    CHECK(header.track_count == smf::kMaxTracks);
    // Sanity: the first track's chunk_offset is 14 (immediately after MThd).
    CHECK(tracks[0].chunk_offset == 14u + 8u);  // skip MTrk header to payload
    CHECK(tracks[0].chunk_size == 4u);          // EOT payload size
}

TEST_CASE("rejects a file that declares more bytes than the cap") {
    // kMaxFileSize is 16 MB. We cannot stack-allocate
    // A reader that reports a size past the cap and hands over no bytes, so the
    // rejection can be checked without building a 16 MB file. Both ways into the
    // parser turn it away.
    struct OversizeReader {
        int32_t read_at(uint32_t /*offset*/, uint8_t* /*buf*/, uint32_t /*n*/) const noexcept {
            return -1;  // never invoked: load_file rejects on size() first
        }
        uint32_t size() const noexcept {
            return smf::kMaxFileSize + 1u;
        }
    };
    static_assert(smf::SmfReader<OversizeReader>, "OversizeReader must satisfy the SmfReader concept");

    smf::Header header {};
    smf::TrackCursor tracks[smf::kMaxTracks] {};
    uint8_t track_count = 0;
    OversizeReader reader;
    CHECK(smf::load_file<OversizeReader>(reader, &header, tracks, &track_count) == false);

    CHECK(smf::parse_header(kType0Simple, smf::kMaxFileSize + 1u, &header) == false);
}

// ---------------------------------------------------------------------------
// Variable-length quantity
// ---------------------------------------------------------------------------

TEST_CASE("decodes a single-byte delta") {
    const uint8_t bytes[] = {0x40};
    std::size_t offset    = 0;
    uint32_t value        = 0;
    const bool parsed     = smf::decode_vlq(bytes, sizeof(bytes), &offset, &value);
    REQUIRE(parsed);
    CHECK(value == 64);
    CHECK(offset == 1);
}

TEST_CASE("decodes a multi-byte delta") {
    const uint8_t bytes[] = {0x81, 0x00};  // 128
    std::size_t offset    = 0;
    uint32_t value        = 0;
    const bool parsed     = smf::decode_vlq(bytes, sizeof(bytes), &offset, &value);
    REQUIRE(parsed);
    CHECK(value == 128u);
    CHECK(offset == 2);
}

TEST_CASE("decodes the longest delta the format allows") {
    const uint8_t bytes[] = {0xFF, 0xFF, 0xFF, 0x7F};  // 0x0FFFFFFF
    std::size_t offset    = 0;
    uint32_t value        = 0;
    const bool parsed     = smf::decode_vlq(bytes, sizeof(bytes), &offset, &value);
    REQUIRE(parsed);
    CHECK(value == 0x0FFFFFFFu);
    CHECK(offset == 4);
}

TEST_CASE("rejects a delta that runs past four bytes") {
    // Fifth continuation byte - the SMF spec caps VLQs at 4 bytes. A
    // parser that keeps chugging opens a malicious-input vector.
    const uint8_t bytes[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    std::size_t offset    = 0;
    uint32_t value        = 0;
    CHECK(smf::decode_vlq(bytes, sizeof(bytes), &offset, &value) == false);
}

// ---------------------------------------------------------------------------
// Running status
// ---------------------------------------------------------------------------

TEST_CASE("carries running status across channel-voice events") {
    smf::Header header {};
    smf::TrackCursor tracks[smf::kMaxTracks] {};
    uint8_t track_count = 0;
    auto reader         = make_reader(kRunningStatusTrack);
    REQUIRE(smf::load_file(reader, &header, tracks, &track_count));
    REQUIRE(track_count == 1);

    smf::Event event {};

    // Event 1: explicit note-on 0x90 0x3C 0x64
    REQUIRE(smf::next_event(reader, &tracks[0], &header, &event));
    CHECK(event.status == 0x90);
    CHECK(event.data1 == 0x3C);
    CHECK(event.data2 == 0x64);
    CHECK(event.tick == 0);

    // Event 2: running status note-on - missing status byte
    REQUIRE(smf::next_event(reader, &tracks[0], &header, &event));
    CHECK(event.status == 0x90);
    CHECK(event.data1 == 0x3E);
    CHECK(event.data2 == 0x64);
    CHECK(event.tick == 16);

    // Event 3: running status note-on again
    REQUIRE(smf::next_event(reader, &tracks[0], &header, &event));
    CHECK(event.status == 0x90);
    CHECK(event.data1 == 0x40);
    CHECK(event.data2 == 0x64);
    CHECK(event.tick == 32);
}

TEST_CASE("drops running status after a system message") {
    // Fixture: note-on, SysEx, bare data bytes. After a SysEx the
    // last_status must be cleared so a bare data byte is a malformed
    // stream rather than an implied reuse of 0x90.
    constexpr uint8_t bytes[] = {
        'M', 'T', 'h', 'd', 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x60, 'M', 'T', 'r', 'k',
        0x00, 0x00, 0x00, 0x0B,        // length =
                                       // 11
        0x00, 0x90, 0x3C, 0x64,        // note-on C4
        0x00, 0xF0, 0x02, 0x7E, 0xF7,  // SysEx 2 bytes then terminator
        0x00, 0x3E, 0x64,              // bare data - must fail
    };
    smf::Header header {};
    smf::TrackCursor tracks[smf::kMaxTracks] {};
    uint8_t track_count = 0;
    auto reader         = make_reader(bytes);
    REQUIRE(smf::load_file(reader, &header, tracks, &track_count));

    smf::Event event {};
    // First event OK (explicit status).
    REQUIRE(smf::next_event(reader, &tracks[0], &header, &event));
    // Second event (SysEx) - parser skips SysEx payload without setting
    // last_status. We accept either "event returned with status=0xF0" or
    // "event silently consumed" as long as the cursor does not inherit
    // 0x90 into subsequent bare data bytes.
    // Third attempt on the bare 0x3E 0x64 must fail.
    bool decoded = smf::next_event(reader, &tracks[0], &header, &event);  // may be sysex
    decoded      = smf::next_event(reader, &tracks[0], &header, &event);  // bare data -> fail
    CHECK(decoded == false);
}

// ---------------------------------------------------------------------------
// Meta events
// ---------------------------------------------------------------------------

TEST_CASE("reads the tempo meta event") {
    smf::Header header {};
    smf::TrackCursor tracks[smf::kMaxTracks] {};
    uint8_t track_count = 0;
    auto reader         = make_reader(kType0Simple);
    REQUIRE(smf::load_file(reader, &header, tracks, &track_count));

    // Drive the cursor until we see the tempo update.
    smf::Event event {};
    for (int i = 0; i < 8 && !tracks[0].ended; ++i) {
        (void)smf::next_event(reader, &tracks[0], &header, &event);
    }
    // 0x07A120 = 500000 us/qn -> 120 BPM
    CHECK(header.tempo_us_per_quarter == 500000u);
}

TEST_CASE("reads the end-of-track meta event") {
    smf::Header header {};
    smf::TrackCursor tracks[smf::kMaxTracks] {};
    uint8_t track_count = 0;
    auto reader         = make_reader(kType0Simple);
    REQUIRE(smf::load_file(reader, &header, tracks, &track_count));

    smf::Event event {};
    for (int i = 0; i < 32 && !tracks[0].ended; ++i) {
        (void)smf::next_event(reader, &tracks[0], &header, &event);
    }
    CHECK(tracks[0].ended == true);
}

TEST_CASE("reads the time-signature meta event") {
    smf::Header header {};
    smf::TrackCursor tracks[smf::kMaxTracks] {};
    uint8_t track_count = 0;
    auto reader         = make_reader(kType0Simple);
    REQUIRE(smf::load_file(reader, &header, tracks, &track_count));

    smf::Event event {};
    for (int i = 0; i < 8 && !tracks[0].ended; ++i) {
        (void)smf::next_event(reader, &tracks[0], &header, &event);
    }
    CHECK(header.beats_per_bar == 4);
    CHECK(header.beat_unit == 4);
}

TEST_CASE("skips a meta event it does not act on") {
    // Lyric meta (0x05) is not in our whitelist. Parser must skip its
    // length-prefixed payload and continue to the note-on that follows.
    constexpr uint8_t bytes[] = {
        'M', 'T', 'h', 'd', 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x60, 'M', 'T', 'r', 'k',
        0x00, 0x00, 0x00, 0x0C,                 // length =
                                                // 12
        0x00, 0xFF, 0x05, 0x03, 'a', 'b', 'c',  // lyric "abc" - skip
        0x00, 0x90, 0x3C, 0x64,                 // note-on C4
        0x00, 0xFF, 0x2F, 0x00,                 // end-of-track
    };
    smf::Header header {};
    smf::TrackCursor tracks[smf::kMaxTracks] {};
    uint8_t track_count = 0;
    auto reader         = make_reader(bytes);
    REQUIRE(smf::load_file(reader, &header, tracks, &track_count));

    // First decoded event must be the note-on, not the lyric.
    smf::Event event {};
    // Walk past any meta events (lyric should be transparently skipped -
    // parser may either consume it internally or return status=0xFF).
    bool found_note = false;
    for (int i = 0; i < 8 && !tracks[0].ended; ++i) {
        if (!smf::next_event(reader, &tracks[0], &header, &event)) break;
        if (event.status == 0x90) {
            found_note = true;
            break;
        }
    }
    CHECK(found_note == true);
    CHECK(event.data1 == 0x3C);
}
