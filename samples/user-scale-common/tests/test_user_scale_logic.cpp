// Scale-math unit tests for the User Scale samples.
//
// The helpers in ../user_scale.h are the music the two User Scale modes make.
// A note is mapped to the nearest enabled degree at or above it, Root turns the
// scale to start on a different note, and Transpose shifts what comes out.
//
// The scenarios are small and readable on purpose, a handful of enabled degrees
// rather than a sweep of every combination.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../user_scale.h"

#include <array>
#include <cstdint>

using op::samples::user_scale::cells_to_mask;
using op::samples::user_scale::kPresetCount;
using op::samples::user_scale::kPresets;
using op::samples::user_scale::load_preset;
using op::samples::user_scale::retune_note;
using op::samples::user_scale::round_up_to_enabled_degree;

// Where the major scale sits in the shipped table.
constexpr uint8_t kMajorIndex = 1;

namespace {

// Build a 12-cell enabled array from a brace-init degree list. E.g.
// `cells_from_degrees({1, 5, 7})` enables cells 1, 5, 7 and disables the other
// nine.
std::array<bool, 12> cells_from_degrees(std::initializer_list<uint8_t> degrees) {
    std::array<bool, 12> cells {};
    for (auto degree : degrees) {
        if (degree < 12) cells[degree] = true;
    }
    return cells;
}

}  // namespace

// ---------------------------------------------------------------------------
// Mapping a note onto the scale
// ---------------------------------------------------------------------------

TEST_CASE("a note rounds up to the nearest enabled degree") {
    // With C#, F and G enabled, a D# rounds up to F and an F# rounds up to G. A
    // note already on an enabled degree stays where it is.
    const auto cells    = cells_from_degrees({1, 5, 7});
    const uint16_t mask = cells_to_mask(cells.data());

    CHECK(round_up_to_enabled_degree(3, mask) == 5);
    CHECK(round_up_to_enabled_degree(6, mask) == 7);
    CHECK(round_up_to_enabled_degree(1, mask) == 1);  // on-degree passes
    CHECK(round_up_to_enabled_degree(5, mask) == 5);  // on-degree passes
}

TEST_CASE("a note above every enabled degree wraps into the next octave") {
    // With only C# enabled, a note above it finds the C# of the next octave, which
    // the degree carries as 13. That is what keeps a rising line rising on a sparse
    // scale.
    const auto cells    = cells_from_degrees({1});
    const uint16_t mask = cells_to_mask(cells.data());

    CHECK(round_up_to_enabled_degree(5, mask) == 13);
    CHECK(round_up_to_enabled_degree(1, mask) == 1);  // on-degree passes
    CHECK(round_up_to_enabled_degree(0, mask) == 1);  // the next one up, same octave
}

TEST_CASE("a scale with nothing enabled passes notes through") {
    // Turning every degree off is something the keyboard lets a player do, and the
    // notes still come through, carrying only the Transpose.
    const uint16_t mask = 0;

    CHECK(retune_note(60, mask, /*root=*/0, /*transpose=*/0) == 60);
    CHECK(retune_note(72, mask, /*root=*/5, /*transpose=*/0) == 72);
    CHECK(retune_note(36, mask, /*root=*/3, /*transpose=*/5) == 41);
}

// ---------------------------------------------------------------------------
// Root and Transpose
// ---------------------------------------------------------------------------

TEST_CASE("a chromatic scale lands every note where it started") {
    // Every degree is enabled, so turning the scale with Root leaves it the same
    // scale. Transpose still moves the note.
    const auto cells    = cells_from_degrees({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
    const uint16_t mask = cells_to_mask(cells.data());

    CHECK(retune_note(60, mask, /*root=*/7, /*transpose=*/0) == 60);
    CHECK(retune_note(60, mask, /*root=*/0, /*transpose=*/+7) == 67);
    CHECK(retune_note(60, mask, /*root=*/5, /*transpose=*/+7) == 67);
    CHECK(retune_note(60, mask, /*root=*/0, /*transpose=*/-24) == 36);
}

TEST_CASE("Root turns C major into D major") {
    // A major scale rooted on D is D major. D stays where it is, D# climbs to E,
    // and C, which D major has no place for, climbs to C#.
    bool cells_major[12] {};
    load_preset(kMajorIndex, cells_major);
    const uint16_t mask = cells_to_mask(cells_major);

    CHECK(retune_note(62, mask, /*root=*/2, /*transpose=*/0) == 62);  // D stays D
    CHECK(retune_note(63, mask, /*root=*/2, /*transpose=*/0) == 64);  // D# -> E
    CHECK(retune_note(64, mask, /*root=*/2, /*transpose=*/0) == 64);  // E stays E
    CHECK(retune_note(65, mask, /*root=*/2, /*transpose=*/0) == 66);  // F -> F#
    CHECK(retune_note(60, mask, /*root=*/2, /*transpose=*/0) == 61);  // C -> C#
}

TEST_CASE("Transpose shifts the output whatever the Root") {
    // Transpose lands on the note the scale chose and moves it, so it stacks with
    // Root.
    bool cells_major[12] {};
    load_preset(kMajorIndex, cells_major);
    const uint16_t mask = cells_to_mask(cells_major);

    CHECK(retune_note(62, mask, /*root=*/2, /*transpose=*/+12) == 74);
    CHECK(retune_note(63, mask, /*root=*/2, /*transpose=*/+12) == 76);
    CHECK(retune_note(60, mask, /*root=*/2, /*transpose=*/-12) == 49);

    // Against a chromatic scale the shift is the only thing moving the note, so
    // the far ends of the range read exactly.
    const auto chromatic          = cells_from_degrees({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
    const uint16_t chromatic_mask = cells_to_mask(chromatic.data());
    CHECK(retune_note(60, chromatic_mask, /*root=*/0, /*transpose=*/+36) == 96);
    CHECK(retune_note(60, chromatic_mask, /*root=*/0, /*transpose=*/-36) == 24);
}

// ---------------------------------------------------------------------------
// The preset table
// ---------------------------------------------------------------------------

TEST_CASE("a preset loads as the scale it is written as") {
    // The table writes each scale in playing order, cell 0 on the left, and the
    // cells come back in that order. Major reads it end to end.
    bool cells[12] {};
    load_preset(kMajorIndex, cells);

    CHECK(cells[0] == true);    // C
    CHECK(cells[1] == false);   // C#
    CHECK(cells[2] == true);    // D
    CHECK(cells[3] == false);   // D#
    CHECK(cells[4] == true);    // E
    CHECK(cells[5] == true);    // F
    CHECK(cells[6] == false);   // F#
    CHECK(cells[7] == true);    // G
    CHECK(cells[8] == false);   // G#
    CHECK(cells[9] == true);    // A
    CHECK(cells[10] == false);  // A#
    CHECK(cells[11] == true);   // B
}

TEST_CASE("every preset has a distinct shape and a name that fits") {
    // A name has to be there and has to fit across the title row.
    for (uint8_t i = 0; i < kPresetCount; ++i) {
        const auto& name = kPresets[i].name;
        CHECK(name.size() > 0);
        CHECK(name.size() <= 14);
    }

    // Each preset carries a shape of its own, which is what lets the title name the
    // scale the player is on.
    for (uint8_t i = 0; i < kPresetCount; ++i) {
        for (uint8_t j = i + 1; j < kPresetCount; ++j) {
            CHECK(kPresets[i].mask != kPresets[j].mask);
        }
    }

    // Every preset in the table survives the trip out to cells and back.
    for (uint8_t i = 0; i < kPresetCount; ++i) {
        bool cells[12] {};
        load_preset(i, cells);
        CHECK(cells_to_mask(cells) == kPresets[i].mask);
    }
}
