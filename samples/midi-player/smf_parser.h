#pragma once
// Streaming reader for Standard MIDI Files.
//
// Covers what a player needs. Type 0 and Type 1 files, channel-voice events with
// running status, and three meta events: tempo, end of track, and time signature.
// Any other meta event is stepped over, and so is sysex. Type 2 files hold tracks
// that keep their own time, which has no meaning for a player, so they are turned
// away at the header.
//
// Running status is where a hand-written parser usually comes undone. A status
// byte may be left out when it repeats, so the last one is remembered and put
// back. A system message clears that memory, which keeps a stray data byte after
// a sysex from reading as a note.
//
// The parser takes its bytes through a reader, so the same code runs over an open
// file on the device and over an array in a test. It holds no bytes of its own,
// and reads each one through the reader as it needs it.

#include <cstddef>
#include <cstdint>
#include <concepts>
#include <type_traits>

namespace op::samples::smf {

// A ceiling on the size a file may declare, well past any real one. Since the
// bytes are streamed, a large ceiling costs nothing until they are read.
constexpr uint32_t kMaxFileSize = 16u * 1024u * 1024u;  // 16 MB

// A file may declare more tracks than this, and the first sixteen are the ones
// that play. The caller can see that happen by reading the count from the header
// itself and comparing it with the one that comes back clamped.
constexpr uint8_t kMaxTracks = 16;

// The format writes a delta in at most four bytes, so a fifth is malformed.
constexpr uint8_t kVlqMaxBytes = 4;

// --- What the file says about itself -----------------------------------------

// The header, and the tempo and time signature as the meta events move them. The
// tempo starts at the format's default of 500000 us per quarter, which is 120 BPM.
// The time signature is kept as the numbers it reads as, so a denominator of 4 is
// a 4 and not the power of two the file stores.
struct Header {
    uint16_t format;                // 0 or 1
    uint16_t track_count;           // held to kMaxTracks
    int16_t ticks_per_quarter;      // the resolution the deltas are written in
    uint32_t tempo_us_per_quarter;  // the tempo meta events move this
    uint8_t beats_per_bar;          // the time signature's upper number
    uint8_t beat_unit;              // its lower number, as the number it reads as
};

// Where one track has reached. The parser keeps one per track and moves each
// along on its own, so a multi-track file merges by the tick each event falls on.
//
// last_status is the running-status byte, kept so an event that leaves its status
// out can have it put back, and cleared by a system message.
struct TrackCursor {
    uint32_t chunk_offset;   // where the track's bytes start in the file
    uint32_t chunk_size;     // how many bytes it has
    uint32_t read_offset;    // how far into them the parser has read
    uint32_t absolute_tick;  // where the track has reached in the song
    uint8_t last_status;     // 0 when no running status is in effect
    bool ended;
};

// An event to play. The parser deals with the meta events itself, so only notes
// and the other channel-voice messages come back out here.
//
// tick is where the event falls in the song, not how far it is from the last one.
// length tells a two-byte message from a three-byte one.
struct Event {
    uint32_t tick;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    uint8_t length;
};

// --- Reading the bytes -------------------------------------------------------

// The format writes its numbers big-endian. The caller has the bounds.
inline uint16_t read_be16(const uint8_t* data, std::size_t offset) {
    return (static_cast<uint16_t>(data[offset]) << 8) | static_cast<uint16_t>(data[offset + 1]);
}

inline uint32_t read_be32(const uint8_t* data, std::size_t offset) {
    return (static_cast<uint32_t>(data[offset]) << 24) | (static_cast<uint32_t>(data[offset + 1]) << 16)
        | (static_cast<uint32_t>(data[offset + 2]) << 8) | static_cast<uint32_t>(data[offset + 3]);
}

// A delta is written seven bits to a byte, with the top bit set on every byte but
// the last, and the format allows it four bytes. Reading a fifth means the stream
// is malformed, and so does running out of bytes before the last one arrives.
inline bool decode_vlq(const uint8_t* data, std::size_t size, std::size_t* offset, uint32_t* value) {
    uint32_t decoded  = 0;
    std::size_t index = *offset;
    for (uint8_t i = 0; i < kVlqMaxBytes; ++i) {
        if (index >= size) return false;
        const uint8_t byte = data[index++];
        decoded            = (decoded << 7) | (byte & 0x7F);
        if ((byte & 0x80) == 0) {
            *offset = index;
            *value  = decoded;
            return true;
        }
    }
    return false;
}

// What the parser needs of a byte source. Anything that can read at an offset and
// say how long it is will do, which is how the same parser runs over a file on the
// device and over an array in a test.
template <typename Reader>
concept SmfReader = requires(Reader reader, uint32_t offset, uint8_t* buffer, uint32_t count) {
    { reader.read_at(offset, buffer, count) } -> std::same_as<int32_t>;
    { reader.size() } -> std::same_as<uint32_t>;
};

// The same delta decode, taking its bytes through the reader.
template <SmfReader Reader>
inline bool decode_vlq_via_reader(Reader& reader, TrackCursor* cursor, uint32_t* value) {
    uint32_t decoded = 0;
    for (uint8_t i = 0; i < kVlqMaxBytes; ++i) {
        if (cursor->read_offset >= cursor->chunk_size) return false;
        uint8_t byte = 0;
        if (reader.read_at(cursor->chunk_offset + cursor->read_offset, &byte, 1) != 1) return false;
        ++cursor->read_offset;
        decoded = (decoded << 7) | (byte & 0x7F);
        if ((byte & 0x80) == 0) {
            *value = decoded;
            return true;
        }
    }
    return false;
}

// --- The header --------------------------------------------------------------

// Read the header at the start of the file, which carries the format, the track
// count, and the ticks in a quarter note, and seeds the tempo and time signature
// with the defaults the format calls for.
//
// A file is turned away when it declares more bytes than the cap allows, when the
// magic or the declared header length is wrong, when the format is one the player
// has no way to follow, when the timing is in frames instead of ticks, or when it
// carries no tracks at all.
//
// A file declaring more tracks than the cap allows keeps its first sixteen, and
// the rest go unread. A caller wanting to know that happened can read the count
// out of the header itself and compare it with the one that comes back here.
inline bool parse_header(const uint8_t* bytes, std::size_t size, Header* header) {
    if (!bytes || !header) return false;
    if (size > kMaxFileSize) return false;
    if (size < 14) return false;

    if (bytes[0] != 'M' || bytes[1] != 'T' || bytes[2] != 'h' || bytes[3] != 'd') {
        return false;
    }
    const uint32_t header_length = read_be32(bytes, 4);
    if (header_length != 6) return false;

    const uint16_t format            = read_be16(bytes, 8);
    const uint16_t declared_tracks   = read_be16(bytes, 10);
    const uint16_t ticks_per_quarter = read_be16(bytes, 12);

    if (format > 1) return false;                   // a Type 2 file has no single timeline
    if (declared_tracks == 0) return false;         // nothing to play
    if (ticks_per_quarter & 0x8000u) return false;  // the timing is in frames, not ticks
    if (ticks_per_quarter == 0) return false;

    header->format               = format;
    header->track_count          = (declared_tracks > kMaxTracks) ? kMaxTracks : declared_tracks;
    header->ticks_per_quarter    = static_cast<int16_t>(ticks_per_quarter);
    header->tempo_us_per_quarter = 500000u;  // the format's default, which is 120 BPM
    header->beats_per_bar        = 4;
    header->beat_unit            = 4;
    return true;
}

// --- The tracks --------------------------------------------------------------

// Find the tracks and leave a cursor on each one, ready to read from. Only the
// headers are read here, so the tracks themselves stay on disk until the music
// asks for them. tracks[] has room for kMaxTracks of them.
template <SmfReader Reader>
inline bool load_file(Reader& reader, Header* header, TrackCursor tracks[], uint8_t* track_count) {
    if (!header || !tracks || !track_count) return false;

    const uint32_t file_size = reader.size();
    if (file_size > kMaxFileSize) return false;
    if (file_size < 14) return false;

    uint8_t header_bytes[14];
    if (reader.read_at(0, header_bytes, 14) != 14) return false;
    if (!parse_header(header_bytes, 14, header)) return false;

    uint32_t offset = 14;
    uint8_t found   = 0;
    while (found < header->track_count) {
        if (found >= kMaxTracks) return false;
        if (offset + 8 > file_size) return false;

        uint8_t chunk[8];
        if (reader.read_at(offset, chunk, 8) != 8) return false;
        // The tracks follow the header directly, with nothing in between.
        if (chunk[0] != 'M' || chunk[1] != 'T' || chunk[2] != 'r' || chunk[3] != 'k') {
            return false;
        }
        const uint32_t length = read_be32(chunk, 4);
        offset += 8;
        // The declared length is checked against what is left of the file, so a
        // track claiming more than there is gets no further.
        if (length > file_size || offset > file_size - length) return false;

        tracks[found].chunk_offset  = offset;
        tracks[found].chunk_size    = length;
        tracks[found].read_offset   = 0;
        tracks[found].absolute_tick = 0;
        tracks[found].last_status   = 0;
        tracks[found].ended         = false;

        offset += length;
        ++found;
    }
    *track_count = found;
    return true;
}

// --- The events --------------------------------------------------------------

namespace detail {

    // How many data bytes a channel-voice message carries. Program change and
    // channel aftertouch take one, and the rest take two.
    inline uint8_t voice_message_data_bytes(uint8_t status) {
        switch (status & 0xF0) {
            case 0xC0:  // program change
            case 0xD0:  // channel aftertouch
                return 1;
            default: return 2;
        }
    }

}  // namespace detail

// Move the cursor to the next event to play, dealing with meta events and sysex
// on the way. The cursor comes back sitting past what it read.
//
// Returns false at the end of the track, which also marks the cursor ended, and on
// a track that runs out mid-event, a malformed delta, or a data byte arriving with
// no status to go on.
//
// The parser reads a byte at a time through the reader, and the caller is free to
// put a cache behind it, which is what keeps playback off the flash for every byte.
template <SmfReader Reader>
inline bool next_event(Reader& reader, TrackCursor* cursor, Header* header, Event* event) {
    if (!cursor || !header || !event) return false;
    if (cursor->ended) return false;

    // A real track carries a handful of meta events at most before it reaches a
    // note, so the search for one is bounded and a malformed file ends rather than
    // spinning.
    for (int guard = 0; guard < 64; ++guard) {
        if (cursor->read_offset >= cursor->chunk_size) return false;

        uint32_t delta = 0;
        if (!decode_vlq_via_reader(reader, cursor, &delta)) return false;
        cursor->absolute_tick += delta;

        if (cursor->read_offset >= cursor->chunk_size) return false;
        uint8_t byte = 0;
        if (reader.read_at(cursor->chunk_offset + cursor->read_offset, &byte, 1) != 1) {
            return false;
        }

        // A meta event, which the parser acts on or steps over itself.
        if (byte == 0xFF) {
            ++cursor->read_offset;
            cursor->last_status = 0;  // a meta event breaks the running-status chain

            if (cursor->read_offset >= cursor->chunk_size) return false;
            uint8_t meta_type = 0;
            if (reader.read_at(cursor->chunk_offset + cursor->read_offset, &meta_type, 1) != 1) {
                return false;
            }
            ++cursor->read_offset;

            uint32_t meta_length = 0;
            if (!decode_vlq_via_reader(reader, cursor, &meta_length)) return false;
            if (cursor->read_offset + meta_length > cursor->chunk_size) return false;

            // The tempo takes three bytes and the time signature four, so four is
            // all that is ever read. Any other meta event is stepped over.
            uint8_t payload[4]     = {};
            const uint32_t to_read = (meta_length < 4) ? meta_length : 4;
            if (to_read > 0) {
                if (reader.read_at(cursor->chunk_offset + cursor->read_offset, payload, to_read)
                    != static_cast<int32_t>(to_read)) {
                    return false;
                }
            }
            cursor->read_offset += meta_length;

            if (meta_type == 0x2F) {
                cursor->ended = true;
                return false;
            }
            if (meta_type == 0x51 && meta_length == 3) {
                header->tempo_us_per_quarter = (static_cast<uint32_t>(payload[0]) << 16)
                    | (static_cast<uint32_t>(payload[1]) << 8) | static_cast<uint32_t>(payload[2]);
            } else if (meta_type == 0x58 && meta_length == 4) {
                // The lower number is written as the power of two it stands for, so a
                // 2 means a quarter note. It is turned back into the number here, and
                // held to 64 so a malformed exponent stays inside the shift.
                header->beats_per_bar  = payload[0];
                const uint8_t exponent = payload[1];
                header->beat_unit      = 1u << (exponent > 6 ? 6 : exponent);
            }
            continue;
        }

        // A sysex message, which the player steps over. It leaves no running status
        // behind it, so a bare data byte after one fails rather than reading as a note.
        if (byte == 0xF0 || byte == 0xF7) {
            ++cursor->read_offset;
            cursor->last_status = 0;

            uint32_t sysex_length = 0;
            if (!decode_vlq_via_reader(reader, cursor, &sysex_length)) return false;
            if (cursor->read_offset + sysex_length > cursor->chunk_size) return false;
            cursor->read_offset += sysex_length;
            continue;
        }

        // A byte with its high bit set is a status byte. A byte without one is
        // running status: the event left its status out, so the last one stands in,
        // and this byte is already data. Arriving here with no status to stand in is
        // malformed.
        uint8_t status;
        if (byte & 0x80) {
            status = byte;
            ++cursor->read_offset;
            cursor->last_status = status;
        } else {
            if (cursor->last_status == 0) return false;
            status = cursor->last_status;
        }

        const uint8_t data_count = detail::voice_message_data_bytes(status);
        if (cursor->read_offset + data_count > cursor->chunk_size) return false;

        uint8_t data[2] = {};
        if (data_count > 0) {
            if (reader.read_at(cursor->chunk_offset + cursor->read_offset, data, data_count) != data_count) {
                return false;
            }
            cursor->read_offset += data_count;
        }

        event->tick   = cursor->absolute_tick;
        event->status = status;
        event->data1  = data[0];
        event->data2  = data[1];
        event->length = 1 + data_count;
        return true;
    }
    return false;
}

// For a caller holding the header const. The tempo and time signature still move
// as the meta events carrying them go by, so a header that must stay put has to be
// copied first.
template <SmfReader Reader>
inline bool next_event(Reader& reader, TrackCursor* cursor, const Header* header, Event* event) {
    return next_event(reader, cursor, const_cast<Header*>(header), event);
}

}  // namespace op::samples::smf
