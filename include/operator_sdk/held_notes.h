#pragma once
/// @file held_notes.h
/// @brief Held-note registry for modes that rewrite note pitch (opt-in, header-only).
///
/// A mode that maps incoming notes onto other pitches can send two keys to the same
/// output note. ``op::sdk::HeldNotes`` remembers which keys are down and which pitch
/// each one went out on. That lets a mode sound an output note once, keep it ringing
/// while any key mapped to it is still held, and release it at the pitch it was
/// actually emitted on even if the mapping has moved in the meantime.
///
/// One instance holds ``Capacity`` entries. An entry is four bytes with no payload
/// and eight with a 32-bit one, so the default 64 entries take 256 or 512 bytes of
/// RAM. Both are template parameters, so a mode that holds fewer notes or carries
/// more per note picks its own.
///
/// Every scan is bounded by the keys currently down, so a mode pays for the notes it
/// is holding and an instance holding none does no work at all.
///
/// Include on demand. This header is deliberately not pulled in by
/// ``operator_sdk.h``.

#include <cstdint>

namespace op::sdk {

/// @brief The payload of a registry that carries nothing beyond the note itself.
struct NoNotePayload {};

/// @brief Remembers the notes a mode is sounding and the pitch each one went out on.
///
/// The key is the port, the channel and the incoming note number together. A global
/// mode sees every input in one batch and each message carries the port it arrived
/// on, so the port keeps a note held on one input distinct from the same note
/// arriving on another.
///
/// ``Payload`` rides along with each entry and comes back untouched. A mode that
/// recomputes everything from the pitch alone leaves it at ``NoNotePayload`` and
/// keeps four-byte entries. A mode that has to reproduce something it derived at
/// press time, a tuning offset for instance, names its own type here and gets it
/// handed back on release.
///
/// Every member carries a default initializer, so an instance needs no constructor
/// and is safe to declare at file scope.
template <uint8_t Capacity = 64, typename Payload = NoNotePayload>
class HeldNotes {
public:
    /// @brief One key that is down, and what the mode sounded for it.
    struct Entry {
        uint8_t port        = 0;  ///< The input the note arrived on.
        uint8_t channel     = 0;  ///< The MIDI channel nibble, 0 to 15.
        uint8_t input_note  = 0;  ///< The note number the key sent.
        uint8_t output_note = 0;  ///< The note number the mode emitted.
        [[no_unique_address]] Payload payload {};  ///< Whatever the mode recorded at press time.
    };

    /// @brief Whether ``press`` recorded the key, and whether the note should sound.
    struct PressResult {
        bool tracked;       ///< The press has an entry, so its release will be recognized.
        bool first_holder;  ///< Nothing else on this port and channel holds that output note.
    };

    /// @brief Whether ``release`` found the key, and whether the note should stop.
    struct ReleaseResult {
        bool found;           ///< A matching press was on record.
        bool last_holder;     ///< No other key on this port and channel still holds that output note.
        uint8_t output_note;  ///< The pitch recorded at press time, 0 when ``found`` is false.
        Payload payload;      ///< The payload recorded at press time, a default one when ``found`` is false.
    };

    /// @brief Record a key going down and report whether it should sound.
    /// @param port The input the note arrived on.
    /// @param channel The MIDI channel nibble, 0 to 15.
    /// @param input_note The note number the key sent.
    /// @param output_note The note number the mode is about to emit.
    /// @param payload Anything the mode wants handed back when the key lifts.
    /// @return ``first_holder`` true the first time an output note is claimed on this
    ///         port and channel, which is the moment to emit the note-on.
    ///
    /// A key already down keeps the entry it has and reports ``first_holder`` false,
    /// so the pitch its release carries stays the one that is sounding.
    ///
    /// A press the registry has no room for reports ``tracked`` false and
    /// ``first_holder`` true, so the note still sounds. Its release comes back with
    /// ``found`` false, leaving the mode to work the pitch out from its current
    /// mapping. Only that one note goes without an entry, and nothing already on
    /// record is displaced to make room.
    PressResult press(uint8_t port,
                      uint8_t channel,
                      uint8_t input_note,
                      uint8_t output_note,
                      Payload payload = {}) {
        bool output_note_held = false;
        for (uint8_t i = 0; i < count_; ++i) {
            const Entry& entry = entries_[i];
            if (entry.port != port || entry.channel != channel) continue;
            if (entry.input_note == input_note) return PressResult {true, false};
            if (entry.output_note == output_note) output_note_held = true;
        }

        if (count_ >= Capacity) return PressResult {false, true};

        entries_[count_] = Entry {port, channel, input_note, output_note, payload};
        ++count_;
        return PressResult {true, !output_note_held};
    }

    /// @brief Take a press back and report whether the note should stop.
    /// @param port The input the note arrived on.
    /// @param channel The MIDI channel nibble, 0 to 15.
    /// @param input_note The note number the key sent.
    /// @return ``last_holder`` true when the release gives up the output note for
    ///         good, which is the moment to emit the note-off on ``output_note``.
    ///
    /// The newest entry matching the key is the one removed, and the entries left
    /// keep the order they were pressed in.
    ReleaseResult release(uint8_t port, uint8_t channel, uint8_t input_note) {
        for (uint8_t i = count_; i > 0; --i) {
            const uint8_t index = i - 1;
            const Entry& entry  = entries_[index];
            if (entry.port != port || entry.channel != channel || entry.input_note != input_note) {
                continue;
            }

            const uint8_t output_note = entry.output_note;
            const Payload payload     = entry.payload;

            for (uint8_t slot = index; slot + 1 < count_; ++slot) {
                entries_[slot] = entries_[slot + 1];
            }
            --count_;

            bool output_note_held = false;
            for (uint8_t j = 0; j < count_; ++j) {
                const Entry& remaining = entries_[j];
                if (remaining.port == port && remaining.channel == channel
                    && remaining.output_note == output_note) {
                    output_note_held = true;
                    break;
                }
            }
            return ReleaseResult {true, !output_note_held, output_note, payload};
        }
        return ReleaseResult {false, false, 0, Payload {}};
    }

    /// @brief The most recently pressed key still held on a channel, or null.
    ///
    /// The channel alone is the key here, with the port left out. A channel's pitch
    /// bend reaches every note sounding on it whichever input that note arrived
    /// from, so a mode restoring per-channel state wants all of them.
    const Entry* newest_on_channel(uint8_t channel) const {
        for (uint8_t i = count_; i > 0; --i) {
            const Entry& entry = entries_[i - 1];
            if (entry.channel == channel) return &entry;
        }
        return nullptr;
    }

    /// @brief Forget every held note.
    ///
    /// A mode calls this as it starts and as it stops, so each run begins with
    /// nothing held.
    void clear() {
        for (uint8_t i = 0; i < count_; ++i) entries_[i] = Entry {};
        count_ = 0;
    }

private:
    Entry entries_[Capacity] {};
    uint8_t count_ = 0;
};

}  // namespace op::sdk
