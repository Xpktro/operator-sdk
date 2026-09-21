// Custom Scale Tuner sample mode.
//
// Retunes notes to the nearest degree of a user-supplied Scala (.scl) microtonal
// scale, emitting the retuned note on the output this instance is routed to. It
// shares parsing and retune math with the header-only helpers in this directory
// (scala.h and retune.h).

#include <operator_sdk.h>
#include <operator_sdk/held_notes.h>
#include <operator_sdk/params.h>
#include <operator_sdk/log.h>  // op::log(...) ergonomic logging wrappers

#include "scala.h"
#include "retune.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

// --- Parameters ------------------------------------------------------------

inline constexpr op::params::FilePicker ScaleFile {
    .name      = "Scale File",
    .extension = ".scl",
};

inline constexpr op::params::Note RootKey {
    .name     = "Root Key",
    .min      = 0,
    .max      = 127,
    .default_ = 69,
};

inline constexpr op::params::Numeric RootFreqHz {
    .name           = "Root Freq Hz",
    .min            = 2000,
    .max            = 48000,
    .default_       = 44000,
    .step           = 10,
    .decimal_places = 2,
};

inline constexpr op::params::Numeric BendRange {
    .name     = "Bend Range",
    .min      = 1,
    .max      = 48,
    .default_ = 2,  // +/-2 semitones, the MIDI 1.0 default.
};

// The held-note record and the MPE allocator each track only their own path, so a
// note sounded under one setting and released under the other can hang.
inline constexpr op::params::Bool MpeMode {
    .name            = "MPE",
    .default_        = false,
    .is_per_instance = 1,
};

OP_MODE_PARAMS(ScaleFile, RootKey, RootFreqHz, BendRange, MpeMode);

namespace {

// --- Tunables / constants -------------------------------------------------

constexpr uint32_t kMaxScalaFileBytes = 8192;

// Unused output argument, since an output mode reaches the output it was loaded
// on.
constexpr uint8_t kAnyOutput = 0;

// --- Mode state -----------------------------------------------------------

op::samples::scala::Scale g_scale;
bool g_scale_valid = false;
// Cache the last-loaded filename to detect when the user picks a different .scl
// in the FilePicker between process() calls. An empty string means no scale is
// loaded yet. The 65 is ScaleFile's max_filename_len (64) plus a NUL.
char g_loaded_filename[65]                                       = {};
uint8_t g_active_mpe[op::samples::tuner::kMpeMemberChannelCount] = {0};
char g_scale_bytes[kMaxScalaFileBytes];

// --- Pitch-bend composition state -----------------------------------------
// Bend state is per channel (16 entries). MPE mode uses channels 2..16
// (nibbles 1..15) and non-MPE uses whichever channel the input came in on.
// Total 16 * (sizeof(float)+sizeof(bool)) ~= 80 bytes.
float g_retune_cents[16]  = {};
bool g_channel_active[16] = {};
float g_user_bend_cents   = 0.0f;

// --- Held notes (non-MPE only) --------------------------------------------
// Channel pitch bend is global per MIDI channel, and in non-MPE one channel may
// hold several sustaining notes. Each key that is down records the note that went
// out and the retune offset it went out with, so a release stops the pitch that is
// actually sounding and hands the channel back to whatever is still held on it.
//
// Several keys can retune to the same whole note. The record counts them, so that
// note sounds once and holds until the last of them lifts.
//
// MPE allocates a dedicated member channel per note, so each channel holds at most
// one note and the MPE branches of the emit helpers consult none of this.
op::sdk::HeldNotes<64, float> g_held_notes;

// Compare two NUL-terminated filenames without pulling in libc strcmp.
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

// Resolve the picked file directly. requested_filename is the user's pick (via
// param<ScaleFile>().filename), opened through storage_read. Empty string means
// no file picked yet.
void maybe_reload_scale(const char* requested_filename) {
    if (!requested_filename) return;
    if (filename_equal(requested_filename, g_loaded_filename)) return;

    g_scale_valid = false;

    if (requested_filename[0] == '\0') {
        copy_filename(g_loaded_filename, requested_filename, sizeof(g_loaded_filename));
        return;
    }

    const int32_t bytes_read = op::api->storage_read(
        requested_filename, reinterpret_cast<uint8_t*>(g_scale_bytes), kMaxScalaFileBytes);

    // Storage was busy and read nothing. The name stays uncached, so the pick
    // reads as fresh again and the next tick retries it.
    if (bytes_read == kStorageBusy) return;

    // The outcome is settled from here, so cache the name and let this one read
    // decide whether the scale is good.
    copy_filename(g_loaded_filename, requested_filename, sizeof(g_loaded_filename));

    if (bytes_read <= 0) {
        op::log(op::LogLevel::Warn, "BAD SCALE FILE");
        return;
    }

    if (!op::samples::scala::parse(g_scale_bytes, static_cast<std::size_t>(bytes_read), &g_scale)) {
        op::log(op::LogLevel::Warn, "BAD SCALE FILE");
        return;
    }
    g_scale_valid = true;
}

// MIDI carries a 14-bit value as two 7-bit halves, the low one first.
uint16_t combine_14bit(uint8_t lsb, uint8_t msb) {
    return static_cast<uint16_t>(((msb & 0x7F) << 7) | (lsb & 0x7F));
}

// A retuned note-on is a pair of MIDI messages, the pitch bend first so the synth
// has the bend set when the note-on fires, then the note-on itself on the same
// channel. The span the 14-bit value maps onto is bend_range_semis.
void emit_pitch_bend(uint8_t channel_nibble, uint16_t bend_14bit) {
    const uint8_t status = 0xE0 | (channel_nibble & 0x0F);
    const uint8_t lsb    = bend_14bit & 0x7F;
    const uint8_t msb    = (bend_14bit >> 7) & 0x7F;
    op::api->send_midi(kAnyOutput, status, lsb, msb);
}

// Emit the composed (retune + user) bend on `channel_nibble`. This is the one
// place that consults both halves of the bend state, used by note-on emission
// and by the user-bend interception loop.
void emit_combined_bend(uint8_t channel_nibble, float bend_range_semis) {
    const float combined      = g_retune_cents[channel_nibble] + g_user_bend_cents;
    const uint16_t bend_14bit = op::samples::tuner::cents_to_pitch_bend_14bit(combined, bend_range_semis);
    emit_pitch_bend(channel_nibble, bend_14bit);
}

void emit_retuned_note_on(const OpMidiMessage& message,
                          const op::samples::tuner::RetunedNote& retuned,
                          float bend_range_semis,
                          bool mpe_on) {
    uint8_t channel_nibble = message.status & 0x0F;
    if (mpe_on) {
        const uint8_t member_channel = op::samples::tuner::mpe_allocate_channel(retuned.midi_note,
                                                                                g_active_mpe);
        if (member_channel == op::samples::tuner::kMpeNoFreeChannel) return;
        channel_nibble = (member_channel - 1) & 0x0F;
    } else {
        // The note attacks on the first key that reaches it. A second key retuning
        // to the same whole note joins the one already ringing.
        const auto pressed = g_held_notes.press(message.port, channel_nibble, message.data1,
                                                retuned.midi_note, retuned.cents_offset);
        if (!pressed.first_holder) return;
    }

    g_retune_cents[channel_nibble]   = retuned.cents_offset;
    g_channel_active[channel_nibble] = true;

    emit_combined_bend(channel_nibble, bend_range_semis);

    const uint8_t note_on_status = 0x90 | channel_nibble;
    op::api->send_midi(kAnyOutput, note_on_status, retuned.midi_note, message.data2);
}

void emit_retuned_note_off(const OpMidiMessage& message,
                           const op::samples::tuner::RetunedNote& retuned,
                           float bend_range_semis,
                           bool mpe_on) {
    uint8_t channel_nibble = message.status & 0x0F;
    uint8_t emitted_note   = retuned.midi_note;

    if (mpe_on) {
        const uint8_t member_channel = op::samples::tuner::mpe_find_channel_for_note(retuned.midi_note,
                                                                                     g_active_mpe);
        if (member_channel != op::samples::tuner::kMpeNoFreeChannel) {
            channel_nibble = (member_channel - 1) & 0x0F;
            op::samples::tuner::mpe_release_channel(member_channel, g_active_mpe);
        }
    } else {
        const auto released = g_held_notes.release(message.port, channel_nibble, message.data1);
        if (released.found) {
            // Another key on this channel still holds the note, so it keeps ringing.
            if (!released.last_holder) return;
            // The pitch on record is the pitch that is sounding, so a Root Key or
            // scale file change under a held note still stops the right one.
            emitted_note = released.output_note;
        }
        // A key the mode never saw go down falls through on the live mapping, so it
        // still stops.
    }

    const uint8_t note_off_status = (message.status & 0xF0) | channel_nibble;

    // Send note-off before the channel's bend changes, so the synth has released
    // the retuned pitch by then. A decaying note left ringing through the change
    // slides audibly to the next held note's bend.
    op::api->send_midi(kAnyOutput, note_off_status, emitted_note, message.data2);

    if (mpe_on) {
        // In MPE there is at most one note per member channel, so releasing it
        // deactivates the channel and there is no record to consult.
        g_channel_active[channel_nibble] = false;
        g_retune_cents[channel_nibble]   = 0.0f;
        return;
    }

    // In non-MPE, hand the channel back to whatever is still held on it, since one
    // bend serves every note sounding there. With nothing left, the bend emit is
    // skipped and the channel goes quiet.
    const auto* still_held = g_held_notes.newest_on_channel(channel_nibble);
    if (still_held != nullptr) {
        g_retune_cents[channel_nibble] = still_held->payload;
        emit_combined_bend(channel_nibble, bend_range_semis);
    } else {
        g_retune_cents[channel_nibble]   = 0.0f;
        g_channel_active[channel_nibble] = false;
    }
}

// Decode an incoming pitch-bend message into a signed cents value using the
// user's configured Bend Range as the semitone span, the inverse of
// cents_to_pitch_bend_14bit.
float decode_incoming_bend_cents(const OpMidiMessage& message, float bend_range_semis) {
    const uint16_t raw_14bit = combine_14bit(message.data1, message.data2);
    const float normalized   = (static_cast<float>(raw_14bit) - 8192.0f) / 8192.0f;
    return normalized * bend_range_semis * 100.0f;
}

// Re-emit the composed bend on every currently-active channel after the user's
// bend value has changed, so every held note follows the user's intent while
// keeping its scale-degree retune offset intact.
void reemit_combined_bend_on_active_channels(float bend_range_semis) {
    for (uint8_t channel = 0; channel < 16; ++channel) {
        if (g_channel_active[channel]) {
            emit_combined_bend(channel, bend_range_semis);
        }
    }
}

}  // namespace

// --- Lifecycle -------------------------------------------------------------

void init() {
    g_scale_valid        = false;
    g_loaded_filename[0] = '\0';
    g_user_bend_cents    = 0.0f;
    std::memset(g_active_mpe, 0, sizeof(g_active_mpe));
    std::memset(g_retune_cents, 0, sizeof(g_retune_cents));
    std::memset(g_channel_active, 0, sizeof(g_channel_active));
    g_held_notes.clear();
}

void process(OpMidiMessage* messages, uint8_t count, uint32_t /*tick_us*/) {
    if (!op::api) return;

    // The typed FilePicker accessor returns a FilePickerValue with the picked
    // .filename.
    const auto picked = param<ScaleFile>();
    maybe_reload_scale(picked.filename);

    if (count == 0) return;

    if (!g_scale_valid) {
        // No scale loaded, so leave messages untouched.
        return;
    }

    const uint8_t root_key       = static_cast<uint8_t>(param<RootKey>());
    const float bend_range_semis = static_cast<float>(param<BendRange>());
    const bool mpe_on            = param<MpeMode>() != 0;

    for (uint8_t i = 0; i < count; ++i) {
        OpMidiMessage& message  = messages[i];
        const uint8_t status_hi = message.status & 0xF0;

        if (status_hi == 0x90 && message.data2 != 0) {
            const auto retuned = op::samples::tuner::nearest_scale_note(message.data1, g_scale, root_key);
            emit_retuned_note_on(message, retuned, bend_range_semis, mpe_on);
            // Clearing the status consumes the raw note-on, leaving the retuned
            // emission as the only note that reaches the output.
            message.status = 0;
        } else if (status_hi == 0x80 || (status_hi == 0x90 && message.data2 == 0)) {
            const auto retuned = op::samples::tuner::nearest_scale_note(message.data1, g_scale, root_key);
            emit_retuned_note_off(message, retuned, bend_range_semis, mpe_on);
            message.status = 0;
        } else if (status_hi == 0xE0) {
            // Compose the user pitch bend with each active channel's retune so the
            // microtuning survives wheel input. Clearing the status consumes the raw
            // wheel, leaving the composed bend as the only one that reaches the output.
            g_user_bend_cents = decode_incoming_bend_cents(message, bend_range_semis);
            reemit_combined_bend_on_active_channels(bend_range_semis);
            message.status = 0;
        }
    }
}

void destroy() {
    std::memset(g_active_mpe, 0, sizeof(g_active_mpe));
    g_held_notes.clear();
    g_scale_valid        = false;
    g_loaded_filename[0] = '\0';
}

OP_MODE_REGISTER(init, process, destroy);
