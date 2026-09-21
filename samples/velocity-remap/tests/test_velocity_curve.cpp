// Curve-math unit tests for the velocity-remap sample.
//
// build_lut (velocity_curve.h) is a pure function, so this suite calls it with
// exact params and reads the table back. That is the pattern for unit-testing a
// mode's curve math without driving the whole mode.
//
// The curve runs in single precision on the device. reference_curve below is the
// same pipeline in double precision, so the two can be compared and the
// single-precision table held to within one velocity step of it.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "velocity_curve.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>  // std::abs

namespace {

// The tension and compand settings the single-precision table is held against.
struct Point {
    int tension;
    int compand;
};

constexpr Point kReferencePoints[] = {
    {0,   50 },
    {0,   -50},
    {60,  0  },
    {-60, 0  },
    {60,  -50},
};

int reference_curve(int input, int minimum, int maximum, int tension, int compand) {
    const double bias      = 0.0099 * static_cast<double>(tension);
    const double anchor    = 0.5 * (bias + 1.0);
    const double schlick   = (1.0 / anchor) - 2.0;
    const double magnitude = compand < 0 ? -compand : compand;
    const double blend     = magnitude * 0.01;
    const double depth     = 200.0;
    const double log_depth = std::log(1.0 + depth);

    double x = static_cast<double>(input - 1) / 126.0;
    if (x < 0.0) x = 0.0;

    double companded = x;
    if (compand != 0) {
        double offset     = 2.0 * x - 1.0;
        const double sign = offset < 0 ? -1.0 : 1.0;
        if (offset < 0) offset = -offset;
        const double curved = compand > 0 ? std::log(1.0 + depth * offset) / log_depth
                                          : (std::exp(offset * log_depth) - 1.0) / depth;
        const double full   = 0.5 + 0.5 * sign * curved;
        companded           = x + blend * (full - x);
    }

    const double biased = tension == 0 ? companded : companded / (schlick * (1.0 - companded) + 1.0);
    const double scaled = static_cast<double>(minimum) + static_cast<double>(maximum - minimum) * biased;
    return static_cast<int>(scaled + 0.5);
}

}  // namespace

// ---------------------------------------------------------------------------
// The shape of the curve
// ---------------------------------------------------------------------------

// The default window with a neutral curve is a straight line, so every playable
// velocity comes back out as itself.
TEST_CASE("the default window passes every velocity through unchanged") {
    uint8_t lut[128];
    build_lut(lut, /*minimum=*/1, /*maximum=*/127, /*tension=*/0, /*compand=*/0);
    for (int velocity = 1; velocity <= 127; ++velocity) {
        CHECK(static_cast<int>(lut[velocity]) == velocity);
    }
}

// Positive compand widens the gap between soft and loud around the middle, so a
// low velocity lands lower and a high one lands higher. Negative compand draws
// both back toward the middle.
TEST_CASE("compand widens and narrows the spread around the middle") {
    uint8_t widened[128], narrowed[128];
    build_lut(widened, 1, 127, 0, 100);
    build_lut(narrowed, 1, 127, 0, -100);

    CHECK(static_cast<int>(widened[32]) < 32);
    CHECK(static_cast<int>(widened[96]) > 96);
    CHECK(static_cast<int>(narrowed[32]) > 32);
    CHECK(static_cast<int>(narrowed[96]) < 96);
}

// Tension bends the whole curve toward the loud or the soft end, which shows up
// clearest well away from the endpoints where the window pins it.
TEST_CASE("tension bends the curve toward loud or soft") {
    uint8_t lifted[128], lowered[128];
    build_lut(lifted, 1, 127, 100, 0);
    build_lut(lowered, 1, 127, -100, 0);

    CHECK(static_cast<int>(lifted[10]) > 10);
    CHECK(static_cast<int>(lowered[10]) < 10);
}

TEST_CASE("a minimum above the maximum flips the curve") {
    uint8_t lut[128];
    build_lut(lut, /*minimum=*/127, /*maximum=*/1, 0, 0);
    for (int velocity = 1; velocity < 127; ++velocity) {
        CHECK(static_cast<int>(lut[velocity + 1]) <= static_cast<int>(lut[velocity]));
    }
    CHECK(static_cast<int>(lut[1]) >= 126);
    CHECK(static_cast<int>(lut[127]) == 1);
}

// ---------------------------------------------------------------------------
// What holds across every setting
// ---------------------------------------------------------------------------

// Harder playing always comes out at least as loud, whatever the curve is doing.
TEST_CASE("the curve stays non-decreasing across the param range") {
    uint8_t lut[128];
    for (int compand : {-100, -50, 0, 50, 100}) {
        for (int tension : {-100, -50, 0, 50, 100}) {
            build_lut(lut, 1, 127, tension, compand);
            for (int velocity = 1; velocity < 127; ++velocity) {
                CHECK(static_cast<int>(lut[velocity + 1]) >= static_cast<int>(lut[velocity]));
            }
        }
    }
}

// The window is what keeps a remapped note audible. Both of its ends are playable
// velocities and the curve stays between them, so every entry the table hands back
// is a velocity that sounds.
TEST_CASE("the curve stays inside the output window") {
    uint8_t lut[128];
    for (int compand : {-100, 0, 100}) {
        for (int tension : {-100, 0, 100}) {
            build_lut(lut, /*minimum=*/20, /*maximum=*/100, tension, compand);
            for (int velocity = 1; velocity <= 127; ++velocity) {
                CHECK(static_cast<int>(lut[velocity]) >= 20);
                CHECK(static_cast<int>(lut[velocity]) <= 100);
            }
        }
    }
}

// The device builds the table in single precision. Hold it to within one
// velocity step of the same pipeline evaluated in double precision.
TEST_CASE("the single-precision table tracks the double-precision reference") {
    uint8_t lut[128];
    for (Point point : kReferencePoints) {
        build_lut(lut, 1, 127, point.tension, point.compand);
        for (int velocity = 0; velocity <= 127; ++velocity) {
            const int expected = reference_curve(velocity, 1, 127, point.tension, point.compand);
            CHECK(std::abs(static_cast<int>(lut[velocity]) - expected) <= 1);
        }
    }
}
