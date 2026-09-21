// Master Clock sample, doctest behavior suite.
//
// The mode turns a tempo into two observable things, the clock period it arms
// and the stream of clock messages it sends, one per pulse to every output. The
// suite drives the pulse counter by hand and reads both back.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <operator_sdk.h>
#include <operator_sdk_sim.h>

#include <cstdint>

OP_MODE_UNDER_TEST();

namespace {

constexpr uint8_t kSlotBpm    = 0;
constexpr uint8_t kSlotBypass = 1;

// The 8 physical outputs plus USB, so a single pulse produces nine messages.
constexpr uint8_t kNumOutputs = 9;

// The clock stops climbing after a quarter note's worth of pulses.
constexpr uint32_t kMaxCatchupBurst = 24;

void set_param(uint8_t slot, int32_t value) {
    op::sim::get_api()->set_param_value(slot, value);
}

void tick() {
    mode_process(nullptr, 0, op::sim::get_api()->get_tick());
}

// Every clock message the mode has sent since the last call, across all outputs.
uint32_t clock_messages() {
    OpMidiMessage buffer[64] {};
    uint32_t total = 0;
    for (uint8_t output = 0; output < kNumOutputs; ++output) {
        while (true) {
            const auto count = op::sim::drain_outgoing_midi(output, buffer, 64);
            if (count == 0) break;
            for (uint16_t i = 0; i < count; ++i) {
                if (buffer[i].status == 0xF8) ++total;
            }
        }
    }
    return total;
}

// Load the mode at a tempo and run one tick, which arms the clock. Leaves the
// message queues drained so a case only sees what it goes on to provoke.
void boot_at(int32_t bpm_fixed, uint32_t pulse = 0) {
    op::sim::reset_state();
    op::sim::apply_param_defaults(kParams, kParamCount);
    op::sim::set_pulse_count(pulse);
    mode_init(op::sim::get_api());
    set_param(kSlotBpm, bpm_fixed);
    tick();
    clock_messages();
}

}  // namespace

// --- Sending the clock -------------------------------------------------------

TEST_CASE("a tick with no new pulse sends nothing") {
    boot_at(12000, /*pulse=*/100);

    tick();

    CHECK(clock_messages() == 0);
    mode_destroy();
}

TEST_CASE("every pulse sends one clock message to each output") {
    boot_at(12000);

    op::sim::set_pulse_count(1);
    tick();
    CHECK(clock_messages() == kNumOutputs);

    op::sim::set_pulse_count(6);
    tick();
    CHECK(clock_messages() == 5u * kNumOutputs);

    mode_destroy();
}

// The pulse counter restarts from zero on its own schedule, and the raw delta
// across that restart reads as billions of pulses. The burst is capped so the
// restart costs a quarter note of clock at worst.
TEST_CASE("the pulse counter restarting does not flood the outputs") {
    constexpr uint32_t kJustBeforeRestart = 4'294'965'599u;
    boot_at(12000, kJustBeforeRestart);

    op::sim::set_pulse_count(0);
    tick();

    CHECK(clock_messages() == kMaxCatchupBurst * kNumOutputs);
    mode_destroy();
}

// --- Tempo -------------------------------------------------------------------

// The period is 250_000_000 / BPM, and it has to stay inside the window the
// device accepts across the whole range the param can reach.
TEST_CASE("BPM sets the clock period") {
    boot_at(12000);  // 120.00 BPM
    CHECK(op::sim::get_last_clock_period_us() == 20833u);

    set_param(kSlotBpm, 14000);  // 140.00 BPM
    tick();
    CHECK(op::sim::get_last_clock_period_us() == 17857u);

    set_param(kSlotBpm, 2000);  // 20.00 BPM, the slowest the param goes
    tick();
    const uint32_t slowest = op::sim::get_last_clock_period_us();
    CHECK(slowest == 125'000u);

    set_param(kSlotBpm, 30000);  // 300.00 BPM, the fastest
    tick();
    const uint32_t fastest = op::sim::get_last_clock_period_us();
    CHECK(fastest == 8333u);

    CHECK(fastest >= 100u);
    CHECK(slowest <= 1'000'000'000u);

    mode_destroy();
}

TEST_CASE("a tempo change sends no clock message of its own") {
    boot_at(12000);

    set_param(kSlotBpm, 14000);
    tick();

    CHECK(clock_messages() == 0);
    mode_destroy();
}

// --- Bypass and unloading ----------------------------------------------------

TEST_CASE("Bypass stops the clock") {
    boot_at(12000);

    set_param(kSlotBypass, 1);
    op::sim::set_pulse_count(5);
    tick();

    CHECK(clock_messages() == 0);
    CHECK(op::sim::get_last_clock_period_us() == 0u);
    mode_destroy();
}

// Bypass carries the baseline forward, so the pulses that went by while it was
// on stay behind the mode when it comes back.
TEST_CASE("leaving Bypass resumes without a catch-up burst") {
    boot_at(12000);

    set_param(kSlotBypass, 1);
    op::sim::set_pulse_count(500);
    tick();
    clock_messages();

    set_param(kSlotBypass, 0);
    tick();
    CHECK(clock_messages() == 0);

    // The clock picks up again from where the counter now stands.
    op::sim::set_pulse_count(501);
    tick();
    CHECK(clock_messages() == kNumOutputs);

    mode_destroy();
}

TEST_CASE("unloading disarms the clock") {
    boot_at(12000);
    REQUIRE(op::sim::get_last_clock_period_us() == 20833u);

    mode_destroy();

    CHECK(op::sim::get_last_clock_period_us() == 0u);
}
