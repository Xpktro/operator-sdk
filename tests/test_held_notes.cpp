// The held-note registry, doctest behavior suite.
//
// op::sdk::HeldNotes reads no clock and no ABI, so each case presses and releases
// keys directly and checks what the registry reports back.
//
// This is a host test, so it is free of the freestanding rule.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <operator_sdk/held_notes.h>

#include <cstdint>

using op::sdk::HeldNotes;

// ---------------------------------------------------------------------------
// One key at a time
// ---------------------------------------------------------------------------

TEST_CASE("a lone press sounds and its release stops the note") {
    HeldNotes<> held;

    const auto pressed = held.press(0, 0, 61, 62);
    CHECK(pressed.tracked);
    CHECK(pressed.first_holder);

    const auto released = held.release(0, 0, 61);
    CHECK(released.found);
    CHECK(released.last_holder);
    CHECK(released.output_note == 62);
}

TEST_CASE("a release for a key that was never pressed is not found") {
    HeldNotes<> held;
    held.press(0, 0, 61, 62);

    const auto released = held.release(0, 0, 64);
    CHECK_FALSE(released.found);
    CHECK(released.output_note == 0);
}

TEST_CASE("a repeat press of a held key keeps the pitch it was emitted on") {
    HeldNotes<> held;
    held.press(0, 0, 61, 62);

    // The mapping has moved under the key, and the note that is sounding has not.
    const auto repeat = held.press(0, 0, 61, 67);
    CHECK(repeat.tracked);
    CHECK_FALSE(repeat.first_holder);

    const auto released = held.release(0, 0, 61);
    CHECK(released.found);
    CHECK(released.last_holder);
    CHECK(released.output_note == 62);
}

// ---------------------------------------------------------------------------
// Keys that fold onto one note
// ---------------------------------------------------------------------------

TEST_CASE("two keys folding onto one note sound it once and stop it on the last release") {
    HeldNotes<> held;

    CHECK(held.press(0, 0, 61, 62).first_holder);
    CHECK_FALSE(held.press(0, 0, 62, 62).first_holder);

    const auto first_off = held.release(0, 0, 61);
    CHECK(first_off.found);
    CHECK_FALSE(first_off.last_holder);

    const auto second_off = held.release(0, 0, 62);
    CHECK(second_off.found);
    CHECK(second_off.last_holder);
    CHECK(second_off.output_note == 62);
}

TEST_CASE("the fold holds whichever of the two keys lifts first") {
    HeldNotes<> held;

    held.press(0, 0, 61, 62);
    held.press(0, 0, 62, 62);

    // The note stops on the last lift, whichever of the two keys that turns out to be.
    CHECK_FALSE(held.release(0, 0, 62).last_holder);
    CHECK(held.release(0, 0, 61).last_holder);
}

TEST_CASE("three keys folding onto one note hold it until the last of them lifts") {
    HeldNotes<> held;

    CHECK(held.press(0, 0, 65, 67).first_holder);
    CHECK_FALSE(held.press(0, 0, 66, 67).first_holder);
    CHECK_FALSE(held.press(0, 0, 67, 67).first_holder);

    CHECK_FALSE(held.release(0, 0, 66).last_holder);
    CHECK_FALSE(held.release(0, 0, 65).last_holder);

    const auto last_off = held.release(0, 0, 67);
    CHECK(last_off.last_holder);
    CHECK(last_off.output_note == 67);
}

// ---------------------------------------------------------------------------
// What makes two notes distinct
// ---------------------------------------------------------------------------

TEST_CASE("the same note on two ports is two notes") {
    HeldNotes<> held;

    CHECK(held.press(0, 0, 61, 62).first_holder);
    CHECK(held.press(1, 0, 61, 62).first_holder);

    CHECK(held.release(0, 0, 61).last_holder);
    CHECK(held.release(1, 0, 61).last_holder);
}

TEST_CASE("the same note on two channels is two notes") {
    HeldNotes<> held;

    CHECK(held.press(0, 0, 61, 62).first_holder);
    CHECK(held.press(0, 5, 61, 62).first_holder);

    CHECK(held.release(0, 5, 61).last_holder);
    CHECK(held.release(0, 0, 61).last_holder);
}

// ---------------------------------------------------------------------------
// A full registry
// ---------------------------------------------------------------------------

TEST_CASE("a press past capacity still sounds and its release is not found") {
    HeldNotes<4> held;

    for (uint8_t note = 60; note < 64; ++note) {
        CHECK(held.press(0, 0, note, note).tracked);
    }

    const auto past_capacity = held.press(0, 0, 64, 64);
    CHECK_FALSE(past_capacity.tracked);
    CHECK(past_capacity.first_holder);

    // The mode works the pitch of that one out for itself, and the four on record
    // still answer.
    CHECK_FALSE(held.release(0, 0, 64).found);
    for (uint8_t note = 60; note < 64; ++note) {
        const auto released = held.release(0, 0, note);
        CHECK(released.found);
        CHECK(released.output_note == note);
    }
}

// ---------------------------------------------------------------------------
// Asking what is still held
// ---------------------------------------------------------------------------

TEST_CASE("newest_on_channel finds the last press on the channel whatever port it came from") {
    HeldNotes<> held;

    held.press(0, 0, 60, 60);
    held.press(1, 0, 64, 64);
    held.press(0, 3, 67, 67);

    const auto* on_channel_zero = held.newest_on_channel(0);
    REQUIRE(on_channel_zero != nullptr);
    CHECK(on_channel_zero->output_note == 64);
    CHECK(on_channel_zero->port == 1);

    const auto* on_channel_three = held.newest_on_channel(3);
    REQUIRE(on_channel_three != nullptr);
    CHECK(on_channel_three->output_note == 67);

    CHECK(held.newest_on_channel(9) == nullptr);

    // With the newest released, the press under it becomes the newest on the channel.
    held.release(1, 0, 64);
    const auto* next_newest = held.newest_on_channel(0);
    REQUIRE(next_newest != nullptr);
    CHECK(next_newest->output_note == 60);
}

TEST_CASE("clear leaves nothing held") {
    HeldNotes<> held;

    held.press(0, 0, 61, 62);
    held.press(0, 0, 64, 65);
    held.clear();

    CHECK(held.newest_on_channel(0) == nullptr);
    CHECK_FALSE(held.release(0, 0, 61).found);
    // The same key presses as a first holder again, so the clear left no hold behind.
    CHECK(held.press(0, 0, 61, 62).first_holder);
}

// ---------------------------------------------------------------------------
// A registry that carries a payload
// ---------------------------------------------------------------------------

TEST_CASE("a payload comes back exactly as it went in") {
    HeldNotes<8, float> held;

    held.press(0, 0, 60, 60, -49.5f);
    held.press(0, 0, 64, 64, 12.25f);

    const auto* newest = held.newest_on_channel(0);
    REQUIRE(newest != nullptr);
    CHECK(newest->payload == 12.25f);

    const auto released = held.release(0, 0, 64);
    CHECK(released.found);
    CHECK(released.payload == 12.25f);

    // Releasing one key leaves the other key's payload as it was recorded.
    const auto* remaining = held.newest_on_channel(0);
    REQUIRE(remaining != nullptr);
    CHECK(remaining->payload == -49.5f);

    // A release with no press on record hands back a default payload.
    const auto missing = held.release(0, 0, 72);
    CHECK_FALSE(missing.found);
    CHECK(missing.payload == 0.0f);
}
