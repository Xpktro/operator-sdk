// Master Clock sample mode.
//
// Sends MIDI Timing Clock at 24 pulses per quarter note to every output, at the
// tempo the BPM param sets. Built with the clock flag set, so it can be picked
// as the device's clock source.
//
// Setting the clock period arms a pulse counter that runs on its own, and each
// process() call sends one clock message per pulse that has come up since the
// last one.

#include <operator_sdk.h>
#include <operator_sdk/params.h>

#include <cstdint>

// --- Parameters --------------------------------------------------------------
//
// BPM is fixed point at two decimal places, so 12000 is 120.00 BPM and one step
// moves it by 0.10.

inline constexpr op::params::Numeric Bpm {
    .name           = "BPM",
    .min            = 2000,
    .max            = 30000,
    .default_       = 12000,
    .step           = 10,
    .decimal_places = 2,
};

inline constexpr op::params::Bool Bypass {
    .name     = "Bypass",
    .default_ = false,
};

OP_MODE_PARAMS(Bpm, Bypass);

namespace {

// The clock goes to all 8 physical outputs plus USB.
constexpr uint8_t kNumOutputs = 9;

// MIDI Timing Clock is defined as 24 pulses per quarter note.
constexpr uint32_t kPulsesPerQuarter = 24;

constexpr uint32_t kBpmFixedScale   = 100;
constexpr uint32_t kMicrosPerMinute = 60'000'000;

// The pulse period is kIntervalNumerator / bpm_fixed, which costs one divide per
// BPM change.
//
// The division comes first so every intermediate stays inside 32 bits. 60e6 / 24
// is exact, so the whole derivation holds in 32-bit integer math.
constexpr uint32_t kIntervalNumerator = (kMicrosPerMinute / kPulsesPerQuarter) * kBpmFixedScale;

static_assert(kIntervalNumerator == 250'000'000u, "the BPM-to-period derivation depends on this");

// The largest burst worth sending, one quarter note. The pulse counter restarts
// from zero on its own schedule, and the raw delta across that restart reads as
// billions of pulses, so the burst is bounded here.
constexpr uint32_t kMaxCatchupBurst = 24;

// Where the mode last read the pulse counter, and the tempo it last programmed.
uint32_t g_last_seen_pulse = 0;
int32_t g_last_bpm_fixed   = 0;

}  // namespace

// --- Lifecycle ---------------------------------------------------------------

void init() {
    // Start from the pulse count as it stands, so the first process() call sends
    // nothing for the pulses that came before the mode loaded.
    g_last_seen_pulse = op::api ? op::api->get_pulse_count() : 0u;
    g_last_bpm_fixed  = 0;
}

void process(OpMidiMessage* /*messages*/, uint8_t /*count*/, uint32_t /*tick_us*/) {
    if (!op::api) return;

    // Bypass stops the clock and moves the baseline up to the present, so coming
    // out of it resumes from the current moment.
    if (param<Bypass>()) {
        op::api->set_clock_period_us(0);
        g_last_seen_pulse = op::api->get_pulse_count();
        g_last_bpm_fixed  = 0;
        return;
    }

    // Reprogram only when the tempo actually moves.
    const int32_t bpm_fixed = param<Bpm>();
    if (bpm_fixed != g_last_bpm_fixed) {
        const uint32_t period_us = bpm_fixed > 0 ? kIntervalNumerator / static_cast<uint32_t>(bpm_fixed) : 0u;
        op::api->set_clock_period_us(period_us);
        g_last_bpm_fixed = bpm_fixed;
    }

    // One clock message per pulse that has come up. In the steady state that is
    // a pulse or none, since process() runs far more often than a pulse arrives.
    const uint32_t pulse_now = op::api->get_pulse_count();
    uint32_t delta           = pulse_now - g_last_seen_pulse;
    if (delta > kMaxCatchupBurst) delta = kMaxCatchupBurst;

    // The clock goes straight to each output, past the modes loaded there, so a
    // steady 24 messages a beat stays out of everyone else's work.
    for (uint32_t pulse = 0; pulse < delta; ++pulse) {
        for (uint8_t output = 0; output < kNumOutputs; ++output) {
            op::api->send_midi_direct(output, 0xF8, 0, 0);
        }
    }
    g_last_seen_pulse = pulse_now;
}

void destroy() {
    // The clock is disarmed on the way out, so it stops with the mode.
    if (op::api) op::api->set_clock_period_us(0);
    g_last_seen_pulse = 0;
    g_last_bpm_fixed  = 0;
}

OP_MODE_REGISTER(init, process, destroy);
