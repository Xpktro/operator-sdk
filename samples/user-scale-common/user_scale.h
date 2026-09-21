#pragma once
// The music the two User Scale modes make.
//
// A scale is twelve cells, one per semitone, held as twelve bits so a scale can
// be compared, stored and recognized as a single number. Cell 0 is the root and
// cell 11 the note below the octave.
//
// A note is mapped to the nearest enabled cell at or above it. Rounding up is
// what keeps a rising line rising, and when nothing above it is enabled the
// search carries on into the next octave, so the line climbs on.

#include <bit>
#include <cstdint>
#include <string_view>

namespace op::samples::user_scale {

// --- The scales the modes ship with ------------------------------------------

// A preset is its name and its twelve cells. The name fits the title row, which
// holds fourteen characters.
struct Preset {
    std::string_view name;
    uint16_t mask;
};

// Read a scale written in playing order, cell 0 on the left, into the mask, where
// cell 0 is the lowest bit. Writing the table this way puts the shape of each
// scale in plain sight at the point it is declared.
constexpr uint16_t from_bits(const char (&bits)[13]) {
    uint16_t mask = 0;
    for (std::size_t i = 0; i < 12; ++i) {
        if (bits[i] == '1') mask = mask | (1u << i);
    }
    return mask;
}

// The presets, in the order the mode offers them. What is stored is the position
// in this table, so a scale can be added at the end and the ones already saved
// still point where they did.
inline constexpr Preset kPresets[] = {
    {.name = "Chromatic",  .mask = from_bits("111111111111")},
    {.name = "Major",      .mask = from_bits("101011010101")},
    {.name = "Dorian",     .mask = from_bits("101101010110")},
    {.name = "Phrygian",   .mask = from_bits("110101011010")},
    {.name = "Lydian",     .mask = from_bits("101010110101")},
    {.name = "Mixolydian", .mask = from_bits("101011010110")},
    {.name = "Aeolian",    .mask = from_bits("101101011010")},
    {.name = "Locrian",    .mask = from_bits("110101101010")},
    {.name = "Minor Harm", .mask = from_bits("101101011001")},
    {.name = "Minor Melo", .mask = from_bits("101101010101")},
    {.name = "Blues Maj",  .mask = from_bits("101110010100")},
    {.name = "Blues Min",  .mask = from_bits("100101110010")},
    {.name = "Penta Maj",  .mask = from_bits("101010010100")},
    {.name = "Penta Min",  .mask = from_bits("100101010010")},
    {.name = "Raga 1",     .mask = from_bits("110011011001")},
    {.name = "Arabic",     .mask = from_bits("101011101010")},
    {.name = "Spanish",    .mask = from_bits("110111011010")},
    {.name = "Gypsy",      .mask = from_bits("101100111001")},
    {.name = "Egyptian",   .mask = from_bits("101001010010")},
    {.name = "Bali",       .mask = from_bits("110100011000")},
    {.name = "Japanese",   .mask = from_bits("110001011000")},
    {.name = "Chinese",    .mask = from_bits("100010110001")},
    {.name = "Wholetone",  .mask = from_bits("101010101010")},
};

inline constexpr uint8_t kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

// The note each cell is named after, from the root.
inline constexpr const char* kRootNoteNames[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
};

// --- Cells and masks ---------------------------------------------------------
//
// The screen keeps the scale as twelve cells, one per key it draws. The presets
// and the note mapping keep it as the packed mask. These two carry it across.

inline void load_preset(uint8_t index, bool cells[12]) {
    const uint16_t mask = kPresets[index].mask;
    for (uint8_t i = 0; i < 12; ++i) {
        cells[i] = ((mask >> i) & 0x1u) != 0u;
    }
}

inline constexpr uint16_t cells_to_mask(const bool cells[12]) {
    uint16_t mask = 0;
    for (uint8_t i = 0; i < 12; ++i) {
        if (cells[i]) mask = mask | (1u << i);
    }
    return mask;
}

// True when the scale is one the mode ships. The title reads Custom for a scale
// the player has edited past any of them.
inline constexpr bool is_preset(uint16_t mask) {
    for (uint8_t i = 0; i < kPresetCount; ++i) {
        if (kPresets[i].mask == mask) return true;
    }
    return false;
}

// --- Mapping a note onto the scale -------------------------------------------

// The nearest enabled degree at or above the one handed in.
//
// The answer runs 0 to 23. Anything from 12 up means the search carried into the
// next octave, so the caller lifts the note by one.
//
// Shifting the mask down by the degree and counting the trailing zeros gives the
// distance to the next enabled cell above it. When nothing is left up there, the
// count is taken from the bottom of the mask and an octave is added.
//
// A scale with nothing enabled hands the degree straight back.
inline uint8_t round_up_to_enabled_degree(uint8_t degree, uint16_t mask) {
    if (mask == 0) return degree;

    const uint16_t above = mask >> degree;
    if (above != 0) {
        return degree + std::countr_zero(above);
    }
    return 12 + std::countr_zero(mask);
}

// Map a note onto the scale and hand back what to play, held inside 0 to 127.
//
// Root says which note the scale starts on, so a major scale with a Root of D is
// D major. Transpose then moves what comes out, in semitones, which lets the same
// rooted scale be played an octave or three from where it falls.
//
// The note is carried into the scale's own frame, mapped there, and carried back:
//
//   in_frame  = note - root
//   degree    = in_frame mod 12, always positive
//   mapped    = round_up_to_enabled_degree(degree, mask)
//   out       = (in_frame - degree) + mapped + root + transpose
//
// A scale with nothing enabled leaves the note where it is and only moves it by
// the Transpose.
inline uint8_t retune_note(uint8_t note, uint16_t mask, uint8_t root, int8_t transpose) {
    int32_t out = 0;

    if (mask == 0) {
        out = note + transpose;
    } else {
        const int32_t in_frame = note - root;

        // C++ can hand back a negative remainder, so the degree is brought round.
        const int32_t degree = ((in_frame % 12) + 12) % 12;
        const uint8_t mapped = round_up_to_enabled_degree(degree, mask);

        out = (in_frame - degree) + mapped + root + transpose;
    }

    if (out < 0) out = 0;
    if (out > 127) out = 127;
    return out;
}

}  // namespace op::samples::user_scale
