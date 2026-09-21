// Holding the single-precision math shim to the host's own math.
//
// The shim's expf, logf and powf are compiled into this target under renamed
// symbols, so they do not collide with the host libc at link time, and the host's
// <cmath> is the reference they are checked against, to within 1e-4 relative error
// over the domains a mode feeds them:
//   - logf over x in [1.001, 201]
//   - expf over x in [0.0, 5.31]
//   - powf as expf(y * logf(x)) for x > 0

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cmath>

// The shim, compiled into this target with its names rewritten to op_test_*, so
// these bind to the shim and not the libc expf/logf/powf.
extern "C" float op_test_expf(float);
extern "C" float op_test_logf(float);
extern "C" float op_test_powf(float, float);

namespace {

// Relative error vs a non-zero oracle value. For values near zero the caller
// uses an absolute tolerance instead (see edge-case tests).
double rel_err(double got, double oracle) {
    return std::fabs(got - oracle) / std::fabs(oracle);
}

constexpr double kRelTol = 1e-4;  // <= 1e-4 relative error contract
constexpr int kSweep     = 256;   // sampled points per domain

}  // namespace

TEST_CASE("logf accuracy: <=1e-4 rel over [1.001, 201]") {
    const double lo = 1.001, hi = 201.0;
    double max_rel = 0.0;
    for (int i = 0; i < kSweep; ++i) {
        double t      = static_cast<double>(i) / (kSweep - 1);
        float x       = static_cast<float>(lo + t * (hi - lo));
        double got    = op_test_logf(x);
        double oracle = std::log(static_cast<double>(x));
        double e      = rel_err(got, oracle);
        max_rel       = std::max(max_rel, e);
        CHECK(e <= kRelTol);
    }
    CHECK(max_rel <= kRelTol);
}

TEST_CASE("expf accuracy: <=1e-4 rel over [0.0, 5.31]") {
    const double lo = 0.0, hi = 5.31;
    double max_rel = 0.0;
    for (int i = 0; i < kSweep; ++i) {
        double t      = static_cast<double>(i) / (kSweep - 1);
        float x       = static_cast<float>(lo + t * (hi - lo));
        double got    = op_test_expf(x);
        double oracle = std::exp(static_cast<double>(x));
        double e      = rel_err(got, oracle);
        max_rel       = std::max(max_rel, e);
        CHECK(e <= kRelTol);
    }
    CHECK(max_rel <= kRelTol);
}

TEST_CASE("powf accuracy: 2^10 == 1024 and composite spot-checks") {
    CHECK(rel_err(op_test_powf(2.0f, 10.0f), 1024.0) <= kRelTol);

    const float xs[] = {0.5f, 2.0f, 10.0f};
    const float ys[] = {0.5f, 2.0f, 3.0f};
    for (float x : xs) {
        for (float y : ys) {
            double got    = op_test_powf(x, y);
            double oracle = std::pow(static_cast<double>(x), static_cast<double>(y));
            CHECK(rel_err(got, oracle) <= kRelTol);
        }
    }
}

TEST_CASE("edge cases: logf(1)==0 and expf(0)==1 within 1e-6") {
    CHECK(std::fabs(static_cast<double>(op_test_logf(1.0f))) <= 1e-6);
    CHECK(std::fabs(static_cast<double>(op_test_expf(0.0f)) - 1.0) <= 1e-6);
}

TEST_CASE("powf domain guard: x<=0 returns 0 except x^0==1") {
    CHECK(op_test_powf(0.0f, 0.0f) == doctest::Approx(1.0f));
    CHECK(op_test_powf(-1.0f, 2.0f) == doctest::Approx(0.0f));
    CHECK(op_test_powf(0.0f, 3.0f) == doctest::Approx(0.0f));
}
