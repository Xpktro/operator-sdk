#pragma once
/// @file midi.h
/// @brief MIDI helper library (scales, chords, CC constants).
///
/// Header-only, opt-in, so usage is on demand.
///
/// Convention: note numbers use the standard MIDI scheme (middle C = 60,
/// note 0 = C-1). Note-name formatting uses sharps only (never flats) for
/// simplicity.

#include <cstdint>

/// @namespace op::sdk::midi
/// @brief MIDI note-name formatting and related helpers.
namespace op::sdk::midi {

// ---------------------------------------------------------------------------
// Note-name formatting
// ---------------------------------------------------------------------------

/// @brief Write the human-readable name for a MIDI note into ``out``.
/// @param note Note number in [0, 127].
/// @param out Buffer of at least 5 bytes. On return holds e.g. ``"C4"`` or
///            ``"F#9"`` NUL-terminated. Sharps are always used for simplicity.
///
/// Octave convention: MIDI note 60 = C4 (so 0 = C-1 in strict MIDI).
inline void note_name(uint8_t note, char out[5]) {
    if (!out) return;
    static constexpr const char* kNames[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
    };
    const uint8_t pc = note % 12;
    const int8_t oct = static_cast<int8_t>((note / 12)) - 1;  // MIDI octave
    const char* name = kNames[pc];
    uint8_t pos      = 0;
    out[pos++]       = name[0];
    if (name[1] == '#') out[pos++] = '#';
    if (oct < 0) {
        out[pos++] = '-';
        out[pos++] = static_cast<char>('0' - oct);  // -1 -> '1', etc.
    } else if (oct < 10) {
        out[pos++] = static_cast<char>('0' + oct);
    } else {
        out[pos++] = static_cast<char>('0' + oct / 10);
        out[pos++] = static_cast<char>('0' + oct % 10);
    }
    out[pos] = '\0';
}

// ---------------------------------------------------------------------------
// Scale tables (constexpr, stored once per TU they're referenced in).
//
// Each table lists the semitone offsets from the root for one octave.
// Paired ``kScale*Size`` constants give the number of degrees to make
// bounds-checked iteration trivial:
//     for (uint8_t i = 0; i < kScaleMajorSize; ++i) { ... kScaleMajor[i] ... }
// ---------------------------------------------------------------------------

/// @brief Major scale (Ionian).
inline constexpr uint8_t kScaleMajor[]   = {0, 2, 4, 5, 7, 9, 11};
inline constexpr uint8_t kScaleMajorSize = sizeof(kScaleMajor) / sizeof(kScaleMajor[0]);

/// @brief Natural minor scale (Aeolian).
inline constexpr uint8_t kScaleNaturalMinor[]   = {0, 2, 3, 5, 7, 8, 10};
inline constexpr uint8_t kScaleNaturalMinorSize = sizeof(kScaleNaturalMinor) / sizeof(kScaleNaturalMinor[0]);

/// @brief Harmonic minor scale.
inline constexpr uint8_t kScaleHarmonicMinor[]   = {0, 2, 3, 5, 7, 8, 11};
inline constexpr uint8_t kScaleHarmonicMinorSize = sizeof(kScaleHarmonicMinor)
    / sizeof(kScaleHarmonicMinor[0]);

/// @brief Melodic minor scale (ascending).
inline constexpr uint8_t kScaleMelodicMinor[]   = {0, 2, 3, 5, 7, 9, 11};
inline constexpr uint8_t kScaleMelodicMinorSize = sizeof(kScaleMelodicMinor) / sizeof(kScaleMelodicMinor[0]);

/// @brief Dorian mode.
inline constexpr uint8_t kScaleDorian[]   = {0, 2, 3, 5, 7, 9, 10};
inline constexpr uint8_t kScaleDorianSize = sizeof(kScaleDorian) / sizeof(kScaleDorian[0]);

/// @brief Mixolydian mode.
inline constexpr uint8_t kScaleMixolydian[]   = {0, 2, 4, 5, 7, 9, 10};
inline constexpr uint8_t kScaleMixolydianSize = sizeof(kScaleMixolydian) / sizeof(kScaleMixolydian[0]);

/// @brief Blues (hexatonic) scale.
inline constexpr uint8_t kScaleBlues[]   = {0, 3, 5, 6, 7, 10};
inline constexpr uint8_t kScaleBluesSize = sizeof(kScaleBlues) / sizeof(kScaleBlues[0]);

/// @brief Whole-tone scale.
inline constexpr uint8_t kScaleWholeTone[]   = {0, 2, 4, 6, 8, 10};
inline constexpr uint8_t kScaleWholeToneSize = sizeof(kScaleWholeTone) / sizeof(kScaleWholeTone[0]);

/// @brief Major pentatonic scale.
inline constexpr uint8_t kScalePentatonicMajor[]   = {0, 2, 4, 7, 9};
inline constexpr uint8_t kScalePentatonicMajorSize = sizeof(kScalePentatonicMajor)
    / sizeof(kScalePentatonicMajor[0]);

/// @brief Minor pentatonic scale.
inline constexpr uint8_t kScalePentatonicMinor[]   = {0, 3, 5, 7, 10};
inline constexpr uint8_t kScalePentatonicMinorSize = sizeof(kScalePentatonicMinor)
    / sizeof(kScalePentatonicMinor[0]);

/// @brief Chromatic (12-tone) scale.
inline constexpr uint8_t kScaleChromatic[]   = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
inline constexpr uint8_t kScaleChromaticSize = sizeof(kScaleChromatic) / sizeof(kScaleChromatic[0]);

// ---------------------------------------------------------------------------
// Chord voicing
// ---------------------------------------------------------------------------

/// @brief Copy a chord voicing into ``out``, transposed by ``octave_shift``
///        octaves and clamped to the MIDI range [0, 127].
/// @param input_notes Source chord tones (MIDI note numbers).
/// @param count Number of source notes.
/// @param octave_shift Signed number of octaves to transpose (-10..+10 typical).
/// @param out Destination array of at least ``count`` bytes.
/// @return Number of notes written, which is ``count`` unless ``input_notes`` or
///         ``out`` is null (then 0). Notes that would fall outside [0,127] are
///         clamped rather than dropped.
inline uint8_t voice_chord(const uint8_t* input_notes, uint8_t count, int8_t octave_shift, uint8_t* out) {
    if (!input_notes || !out) return 0;
    const int16_t shift = static_cast<int16_t>(octave_shift) * 12;
    for (uint8_t i = 0; i < count; ++i) {
        int16_t n = static_cast<int16_t>(input_notes[i]) + shift;
        if (n < 0) n = 0;
        if (n > 127) n = 127;
        out[i] = static_cast<uint8_t>(n);
    }
    return count;
}

// ---------------------------------------------------------------------------
// Useful MIDI Control Change numbers
// ---------------------------------------------------------------------------

/// @brief Modulation Wheel (CC 1).
inline constexpr uint8_t kCCModWheel = 1;
/// @brief Breath controller (CC 2).
inline constexpr uint8_t kCCBreath = 2;
/// @brief Foot pedal (CC 4).
inline constexpr uint8_t kCCFootPedal = 4;
/// @brief Portamento Time (CC 5).
inline constexpr uint8_t kCCPortamentoTime = 5;
/// @brief Data Entry MSB (CC 6).
inline constexpr uint8_t kCCDataEntry = 6;
/// @brief Channel Volume (CC 7).
inline constexpr uint8_t kCCVolume = 7;
/// @brief Balance (CC 8).
inline constexpr uint8_t kCCBalance = 8;
/// @brief Pan (CC 10).
inline constexpr uint8_t kCCPan = 10;
/// @brief Expression (CC 11).
inline constexpr uint8_t kCCExpression = 11;
/// @brief Effect Controller 1 (CC 12).
inline constexpr uint8_t kCCEffect1 = 12;
/// @brief Effect Controller 2 (CC 13).
inline constexpr uint8_t kCCEffect2 = 13;
/// @brief Damper / Sustain pedal (CC 64).
inline constexpr uint8_t kCCSustain = 64;
/// @brief Portamento on/off (CC 65).
inline constexpr uint8_t kCCPortamento = 65;
/// @brief Sostenuto (CC 66).
inline constexpr uint8_t kCCSostenuto = 66;
/// @brief Soft pedal (CC 67).
inline constexpr uint8_t kCCSoftPedal = 67;
/// @brief Legato footswitch (CC 68).
inline constexpr uint8_t kCCLegato = 68;
/// @brief Reset all controllers (CC 121).
inline constexpr uint8_t kCCReset = 121;
/// @brief All Sound Off (CC 120).
inline constexpr uint8_t kCCAllSoundOff = 120;
/// @brief All Notes Off (CC 123).
inline constexpr uint8_t kCCAllNotesOff = 123;
/// @brief Local control on/off (CC 122).
inline constexpr uint8_t kCCLocalControl = 122;

}  // namespace op::sdk::midi
