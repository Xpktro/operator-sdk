// The step boundary tracker, doctest behavior suite.
//
// op::sdk::StepTracker holds no clock of its own and reads no ABI, so each case
// feeds it step indices directly and checks the edge it reports back.
//
// This is a host test, so it is free of the freestanding rule.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <operator_sdk/timing.h>

#include <cstdint>

using op::sdk::StepEdge;
using op::sdk::StepTracker;

// ---------------------------------------------------------------------------
// The first observation
// ---------------------------------------------------------------------------

TEST_CASE("the first observation is adopted and crosses nothing") {
    StepTracker tracker;
    CHECK(tracker.started() == false);

    // A mode loading partway through step 7 has not entered step 7.
    CHECK(tracker.observe(7) == StepEdge::Adopted);
    CHECK(tracker.started() == true);
    CHECK(tracker.current() == 7);
}

TEST_CASE("step zero is adopted like any other first step") {
    StepTracker tracker;
    // Zero is the index a frozen clock position yields, and it carries no more
    // meaning on a first look than any other step.
    CHECK(tracker.observe(0) == StepEdge::Adopted);
    CHECK(tracker.current() == 0);
    CHECK(tracker.observe(0) == StepEdge::Unchanged);
}

TEST_CASE("a held position reports unchanged for as long as it holds") {
    StepTracker tracker;
    CHECK(tracker.observe(3) == StepEdge::Adopted);
    for (int tick = 0; tick < 500; ++tick) {
        CHECK(tracker.observe(3) == StepEdge::Unchanged);
    }
    CHECK(tracker.current() == 3);
}

// ---------------------------------------------------------------------------
// Crossings
// ---------------------------------------------------------------------------

TEST_CASE("a changed index crosses once and then holds") {
    StepTracker tracker;
    tracker.observe(0);
    CHECK(tracker.observe(1) == StepEdge::Crossed);
    CHECK(tracker.observe(1) == StepEdge::Unchanged);
    CHECK(tracker.observe(2) == StepEdge::Crossed);
    CHECK(tracker.current() == 2);
}

TEST_CASE("a grid that wraps to zero crosses like any other move") {
    StepTracker tracker;
    tracker.observe(14);
    CHECK(tracker.observe(15) == StepEdge::Crossed);
    // The last step of a bar gives way to the first of the next.
    CHECK(tracker.observe(0) == StepEdge::Crossed);
    CHECK(tracker.current() == 0);
}

TEST_CASE("a backward jump crosses so a rewound transport keeps firing") {
    StepTracker tracker;
    tracker.observe(9);
    CHECK(tracker.observe(2) == StepEdge::Crossed);
    CHECK(tracker.current() == 2);
}

TEST_CASE("a skipped step crosses once for the step it lands on") {
    StepTracker tracker;
    tracker.observe(0);
    // A tick that arrives late sees several steps go by and reports one edge.
    CHECK(tracker.observe(5) == StepEdge::Crossed);
    CHECK(tracker.current() == 5);
}

// ---------------------------------------------------------------------------
// Re-arming
// ---------------------------------------------------------------------------

TEST_CASE("reset arms the next observation to be adopted") {
    StepTracker tracker;
    tracker.observe(4);
    CHECK(tracker.observe(5) == StepEdge::Crossed);

    tracker.reset();
    CHECK(tracker.started() == false);
    // The step a restarted sequence resumes on is a baseline, not an entry.
    CHECK(tracker.observe(5) == StepEdge::Adopted);
    CHECK(tracker.observe(6) == StepEdge::Crossed);
}

// ---------------------------------------------------------------------------
// Independence
// ---------------------------------------------------------------------------

TEST_CASE("two trackers on their own grids never interfere") {
    StepTracker sixteenths;
    StepTracker triplets;

    sixteenths.observe(0);
    triplets.observe(0);

    CHECK(sixteenths.observe(1) == StepEdge::Crossed);
    CHECK(triplets.observe(0) == StepEdge::Unchanged);
    CHECK(sixteenths.current() == 1);
    CHECK(triplets.current() == 0);
}
