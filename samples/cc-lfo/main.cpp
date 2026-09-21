// CC LFO sample mode.
//
// Output declarative generator with four independent LFO slots, each sweeping a
// Control Change on the mode's bound output. Two SDK techniques carry the
// sample. The first is declarative visibility, where one Source Enum per slot
// gates that slot's other rows through .watch (detailed at the parameter block).
// The second is an integer phase oscillator, where Free advances off get_tick()
// and Sync off the 24-PPQN pulse grid, with the quarter-wave sine LUT in
// waveforms.h.

#include <operator_sdk.h>
#include <operator_sdk/params.h>
#include <operator_sdk/timing.h>  // op::sdk::ClockInterpolator

#include "waveforms.h"

#include <cstdint>

// --- Parameters (four LFOs, flat form throughout) ---
//
// 4 Sections x (Source + 6 gated fields) = 32 parameter entries, under the
// kMaxParamsPerMode = 36 cap. Each LFO has a Source Enum {Off, Free, Sync}
// (always visible inside its Section), CC/Shape/Low/High gated on Source != Off,
// Rate Hz gated on Source == Free, and Rate Beat gated on Source == Sync.
//
// Every Section and every field is declared inline constexpr at file scope so
// a sibling has an identifier to reference via .watch = WatcherObj.
//
// Rate Beat has 20 options from slow to fast, encoded as beats-per-cycle times
// 24 by ratebeat_to_bpc24(). Whole numbers are measures, fractions are note
// values. default_ = 12 selects "1/4", one cycle per beat.

// ---- LFO 1 ----
inline constexpr op::params::Section Lfo1Sec {.name = "LFO 1"};
inline constexpr op::params::Enum Lfo1Source {
    .name     = "Source",
    .options  = {"Off", "Free", "Sync"},
    .default_ = 0,
    .parent   = Lfo1Sec,
};
inline constexpr op::params::Numeric Lfo1Cc {
    .name         = "CC",
    .min          = 0,
    .max          = 127,
    .default_     = 1,
    .parent       = Lfo1Sec,
    .visible_when = {.watch = Lfo1Source, .op = op::params::Op::Neq, .value = 0}
};
inline constexpr op::params::Enum Lfo1Shape {
    .name         = "Shape",
    .options      = {"Sine", "Triangle", "Saw", "Inv Saw", "Square", "S&H"},
    .default_     = 0,
    .parent       = Lfo1Sec,
    .visible_when = {.watch = Lfo1Source, .op = op::params::Op::Neq, .value = 0}
};
inline constexpr op::params::Numeric Lfo1Low {
    .name         = "Low",
    .min          = 0,
    .max          = 127,
    .default_     = 0,
    .parent       = Lfo1Sec,
    .visible_when = {.watch = Lfo1Source, .op = op::params::Op::Neq, .value = 0}
};
inline constexpr op::params::Numeric Lfo1High {
    .name         = "High",
    .min          = 0,
    .max          = 127,
    .default_     = 127,
    .parent       = Lfo1Sec,
    .visible_when = {.watch = Lfo1Source, .op = op::params::Op::Neq, .value = 0}
};
inline constexpr op::params::Numeric Lfo1RateHz {
    .name           = "Rate Hz",
    .min            = 1,
    .max            = 2000,
    .default_       = 100,
    .decimal_places = 2,
    .parent         = Lfo1Sec,
    .visible_when   = {.watch = Lfo1Source, .op = op::params::Op::Eq, .value = 1}
};
inline constexpr op::params::Enum Lfo1RateBeat {
    .name         = "Rate Beat",
    .options      = {"16",  "16T",  "8",   "8T",   "4",   "4T",   "2",    "2T",    "1",    "1T",
                     "1/2", "1/2T", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T"},
    .default_     = 12,
    .parent       = Lfo1Sec,
    .visible_when = {.watch = Lfo1Source, .op = op::params::Op::Eq, .value = 2}
};

// ---- LFO 2 ----
inline constexpr op::params::Section Lfo2Sec {.name = "LFO 2"};
inline constexpr op::params::Enum Lfo2Source {
    .name     = "Source",
    .options  = {"Off", "Free", "Sync"},
    .default_ = 0,
    .parent   = Lfo2Sec,
};
inline constexpr op::params::Numeric Lfo2Cc {
    .name         = "CC",
    .min          = 0,
    .max          = 127,
    .default_     = 2,
    .parent       = Lfo2Sec,
    .visible_when = {.watch = Lfo2Source, .op = op::params::Op::Neq, .value = 0}
};
inline constexpr op::params::Enum Lfo2Shape {
    .name         = "Shape",
    .options      = {"Sine", "Triangle", "Saw", "Inv Saw", "Square", "S&H"},
    .default_     = 0,
    .parent       = Lfo2Sec,
    .visible_when = {.watch = Lfo2Source, .op = op::params::Op::Neq, .value = 0}
};
inline constexpr op::params::Numeric Lfo2Low {
    .name         = "Low",
    .min          = 0,
    .max          = 127,
    .default_     = 0,
    .parent       = Lfo2Sec,
    .visible_when = {.watch = Lfo2Source, .op = op::params::Op::Neq, .value = 0}
};
inline constexpr op::params::Numeric Lfo2High {
    .name         = "High",
    .min          = 0,
    .max          = 127,
    .default_     = 127,
    .parent       = Lfo2Sec,
    .visible_when = {.watch = Lfo2Source, .op = op::params::Op::Neq, .value = 0}
};
inline constexpr op::params::Numeric Lfo2RateHz {
    .name           = "Rate Hz",
    .min            = 1,
    .max            = 2000,
    .default_       = 100,
    .decimal_places = 2,
    .parent         = Lfo2Sec,
    .visible_when   = {.watch = Lfo2Source, .op = op::params::Op::Eq, .value = 1}
};
inline constexpr op::params::Enum Lfo2RateBeat {
    .name         = "Rate Beat",
    .options      = {"16",  "16T",  "8",   "8T",   "4",   "4T",   "2",    "2T",    "1",    "1T",
                     "1/2", "1/2T", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T"},
    .default_     = 12,
    .parent       = Lfo2Sec,
    .visible_when = {.watch = Lfo2Source, .op = op::params::Op::Eq, .value = 2}
};

// ---- LFO 3 ----
inline constexpr op::params::Section Lfo3Sec {.name = "LFO 3"};
inline constexpr op::params::Enum Lfo3Source {
    .name     = "Source",
    .options  = {"Off", "Free", "Sync"},
    .default_ = 0,
    .parent   = Lfo3Sec,
};
inline constexpr op::params::Numeric Lfo3Cc {
    .name         = "CC",
    .min          = 0,
    .max          = 127,
    .default_     = 3,
    .parent       = Lfo3Sec,
    .visible_when = {.watch = Lfo3Source, .op = op::params::Op::Neq, .value = 0}
};
inline constexpr op::params::Enum Lfo3Shape {
    .name         = "Shape",
    .options      = {"Sine", "Triangle", "Saw", "Inv Saw", "Square", "S&H"},
    .default_     = 0,
    .parent       = Lfo3Sec,
    .visible_when = {.watch = Lfo3Source, .op = op::params::Op::Neq, .value = 0}
};
inline constexpr op::params::Numeric Lfo3Low {
    .name         = "Low",
    .min          = 0,
    .max          = 127,
    .default_     = 0,
    .parent       = Lfo3Sec,
    .visible_when = {.watch = Lfo3Source, .op = op::params::Op::Neq, .value = 0}
};
inline constexpr op::params::Numeric Lfo3High {
    .name         = "High",
    .min          = 0,
    .max          = 127,
    .default_     = 127,
    .parent       = Lfo3Sec,
    .visible_when = {.watch = Lfo3Source, .op = op::params::Op::Neq, .value = 0}
};
inline constexpr op::params::Numeric Lfo3RateHz {
    .name           = "Rate Hz",
    .min            = 1,
    .max            = 2000,
    .default_       = 100,
    .decimal_places = 2,
    .parent         = Lfo3Sec,
    .visible_when   = {.watch = Lfo3Source, .op = op::params::Op::Eq, .value = 1}
};
inline constexpr op::params::Enum Lfo3RateBeat {
    .name         = "Rate Beat",
    .options      = {"16",  "16T",  "8",   "8T",   "4",   "4T",   "2",    "2T",    "1",    "1T",
                     "1/2", "1/2T", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T"},
    .default_     = 12,
    .parent       = Lfo3Sec,
    .visible_when = {.watch = Lfo3Source, .op = op::params::Op::Eq, .value = 2}
};

// ---- LFO 4 ----
inline constexpr op::params::Section Lfo4Sec {.name = "LFO 4"};
inline constexpr op::params::Enum Lfo4Source {
    .name     = "Source",
    .options  = {"Off", "Free", "Sync"},
    .default_ = 0,
    .parent   = Lfo4Sec,
};
inline constexpr op::params::Numeric Lfo4Cc {
    .name         = "CC",
    .min          = 0,
    .max          = 127,
    .default_     = 4,
    .parent       = Lfo4Sec,
    .visible_when = {.watch = Lfo4Source, .op = op::params::Op::Neq, .value = 0}
};
inline constexpr op::params::Enum Lfo4Shape {
    .name         = "Shape",
    .options      = {"Sine", "Triangle", "Saw", "Inv Saw", "Square", "S&H"},
    .default_     = 0,
    .parent       = Lfo4Sec,
    .visible_when = {.watch = Lfo4Source, .op = op::params::Op::Neq, .value = 0}
};
inline constexpr op::params::Numeric Lfo4Low {
    .name         = "Low",
    .min          = 0,
    .max          = 127,
    .default_     = 0,
    .parent       = Lfo4Sec,
    .visible_when = {.watch = Lfo4Source, .op = op::params::Op::Neq, .value = 0}
};
inline constexpr op::params::Numeric Lfo4High {
    .name         = "High",
    .min          = 0,
    .max          = 127,
    .default_     = 127,
    .parent       = Lfo4Sec,
    .visible_when = {.watch = Lfo4Source, .op = op::params::Op::Neq, .value = 0}
};
inline constexpr op::params::Numeric Lfo4RateHz {
    .name           = "Rate Hz",
    .min            = 1,
    .max            = 2000,
    .default_       = 100,
    .decimal_places = 2,
    .parent         = Lfo4Sec,
    .visible_when   = {.watch = Lfo4Source, .op = op::params::Op::Eq, .value = 1}
};
inline constexpr op::params::Enum Lfo4RateBeat {
    .name         = "Rate Beat",
    .options      = {"16",  "16T",  "8",   "8T",   "4",   "4T",   "2",    "2T",    "1",    "1T",
                     "1/2", "1/2T", "1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T"},
    .default_     = 12,
    .parent       = Lfo4Sec,
    .visible_when = {.watch = Lfo4Source, .op = op::params::Op::Eq, .value = 2}
};

OP_MODE_PARAMS(Lfo1Sec,
               Lfo1Source,
               Lfo1Cc,
               Lfo1Shape,
               Lfo1Low,
               Lfo1High,
               Lfo1RateHz,
               Lfo1RateBeat,
               Lfo2Sec,
               Lfo2Source,
               Lfo2Cc,
               Lfo2Shape,
               Lfo2Low,
               Lfo2High,
               Lfo2RateHz,
               Lfo2RateBeat,
               Lfo3Sec,
               Lfo3Source,
               Lfo3Cc,
               Lfo3Shape,
               Lfo3Low,
               Lfo3High,
               Lfo3RateHz,
               Lfo3RateBeat,
               Lfo4Sec,
               Lfo4Source,
               Lfo4Cc,
               Lfo4Shape,
               Lfo4Low,
               Lfo4High,
               Lfo4RateHz,
               Lfo4RateBeat);

// --- Engine internals ------------------------------------------------------

namespace {

// --- Tunables -------------------------------------------------------------
constexpr uint8_t kSlotCount       = 4;
constexpr uint8_t kAnyOutput       = 0;  // unused output argument
constexpr int32_t kShapeSampleHold = 5;  // matches options list index

// Shape enum integer values.
constexpr int32_t kShapeSine     = 0;
constexpr int32_t kShapeTriangle = 1;
constexpr int32_t kShapeSaw      = 2;
constexpr int32_t kShapeInvSaw   = 3;
constexpr int32_t kShapeSquare   = 4;

// --- Per-slot state -------------------------------------------------------
//
// kRateKeyUnset marks a slot as "no rate seen yet". The first evaluation must
// not read as a rate change, or the phase would re-anchor on the very first
// tick. INT32_MIN works as the sentinel because no real rate key can land on
// it, since a synced key is bpc_x24 in [4,1536] and a free key is rate_hz >= 1.
constexpr int32_t kRateKeyUnset = -2147483647 - 1;  // INT32_MIN

struct SlotState {
    int32_t last_cc_value  = -1;  // sentinel -> first evaluation always emits
    uint32_t sh_step_index = 0;   // S&H step counter, bumped on each cycle wrap
    uint16_t last_phase    = 0;   // previous phase, to detect an S&H cycle wrap
    bool primed            = false;

    // Rate-change re-anchoring. When the rate parameter changes, phase_offset is
    // recomputed so the emitted phase is unchanged at that instant and keeps
    // sweeping smoothly, so editing rate live never jumps the CC value.
    // last_rate_key holds the rate last seen for this slot (bpc_x24 under Sync,
    // rate_hz under Free, kRateKeyUnset until the first evaluation), and
    // phase_offset is added (mod 1024) to the base phase before shaping.
    int32_t last_rate_key = kRateKeyUnset;
    uint16_t phase_offset = 0;

    // Emission throttle (kTotalEmitBudgetHz has the fair-share rationale).
    // last_emit_us is the tick_us of this slot's most recent send_midi, and
    // emit_primed stays false until the first emit so a freshly activated slot's
    // first changed value is never throttled.
    uint32_t last_emit_us = 0;
    bool emit_primed      = false;
};

// One SlotState per LFO. File-scope .bss, so the loader clones the array per
// instance, and init()/destroy() reset every field to its cold-start value.
SlotState g_slots[kSlotCount];

// S&H shares a single seed across slots so two S&H slots at the same rate
// diverge anyway (via sh_step_index). Seeded from get_tick() in init(), with a
// non-zero default because xorshift locks on a zero seed.
uint32_t g_sh_seed = 0x1234ABCDu;

// --- Per-instance transport pulse origin ----------------------------------
//
// Synced phase is measured relative to this origin, not the absolute 24-PPQN
// counter, which free-runs from boot with an arbitrary phase against the musical
// bar, so on its own an LFO cycle would not line up with the downbeat.
// Snapshotting an origin on MIDI transport and measuring (pulse_now -
// g_pulse_origin) re-anchors a multi-measure cycle to bar 1. A 0 origin means
// "no transport seen yet", so the phase runs from the absolute count until the
// first Start or Stop.
//
// Both Start (0xFA) and Stop (0xFC) re-snapshot the origin, since MIDI has no
// true Pause and typical hardware rewinds its sequencer on Stop, so the mode
// does too. Continue (0xFB) is deliberately ignored.
uint32_t g_pulse_origin = 0;

// --- Per-instance clock interpolator --------------------------------------
//
// Smooths the synced phase between 24-PPQN pulse edges so it sweeps
// continuously. See op::sdk::ClockInterpolator in operator_sdk/timing.h.
//
// now() advances the interpolator state, so process() calls it exactly once and
// reuses the result for every synced slot that tick.
op::sdk::ClockInterpolator g_clock;

// Per-slot fair-share throttle with a polite total budget.
//
// The instance keeps a total emission budget and splits it evenly across the
// active slots, so each active slot's minimum emit interval is active_count x
// (1'000'000 / kTotalEmitBudgetHz) us and the combined emission stays about
// kTotalEmitBudgetHz no matter how many LFOs run (1 LFO -> 500/s, 2 -> 250/s
// each, 4 -> 125/s each). 500 CC/s is about half of what an output carries,
// leaving the rest for notes and other CC. On USB it is negligible.
//
// The interval math stays in 32-bit. 1'000'000 / 500 = 2000 us per LFO, and
// times active_count (<= 4) is at most 8000 us, so nothing overflows.
constexpr uint32_t kTotalEmitBudgetHz  = 500;
constexpr uint32_t kEmitPeriodPerLfoUs = 1'000'000u / kTotalEmitBudgetHz;

// --- Param-slot access ----------------------------------------------------
//
// Each slot's params live at slot_index * 7 in the value-slot array, and
// api->get_param_value(slot_index * 7 + offset) reads the ABI slot directly. Each
// LFO occupies seven value slots (Sections contribute 0). Offsets must match
// the OP_MODE_PARAMS declaration order above.
constexpr uint8_t kParamsPerSlot = 7;
constexpr uint8_t kOffSource     = 0;
constexpr uint8_t kOffCc         = 1;
constexpr uint8_t kOffShape      = 2;
constexpr uint8_t kOffLow        = 3;
constexpr uint8_t kOffHigh       = 4;
constexpr uint8_t kOffRateHz     = 5;
constexpr uint8_t kOffRateBeat   = 6;

// Source enum option indices (mirror the options list order in the param
// table above).
constexpr int32_t kSourceOff  = 0;
constexpr int32_t kSourceFree = 1;
constexpr int32_t kSourceSync = 2;

int32_t slot_param(uint8_t slot, uint8_t offset) {
    return op::api->get_param_value(static_cast<uint8_t>(slot * kParamsPerSlot + offset));
}

}  // namespace (pause anonymous namespace for testable phase math)

// --- Phase math (pure integer, -nostdlib safe) ----------------------------
//
// These functions are at file scope (not in an anonymous namespace) so the
// host doctest suite can forward-declare and call them directly.
//
// The synced phase comes straight from the integer pulse counter (see
// phase_sync_on_interp below).

// RateBeat option index -> beats-per-cycle times 24 (the encoding fed to
// phase_sync_on_interp). One full LFO cycle spans (bpc_x24 / 24) beats. The
// 20-option list runs slow to fast, where whole numbers are measures (4 beats
// each), fractions are note values, and "T" is triplet. The table below pairs
// each option with its bpc_x24.
int32_t ratebeat_to_bpc24(int32_t option_index) {
    static constexpr int32_t kRateBeatBpc24[20] = {
        1536, 1152,  // 16,   16T
        768,  576,   // 8,    8T
        384,  288,   // 4,    4T
        192,  144,   // 2,    2T
        96,   72,    // 1,    1T
        48,   32,    // 1/2,  1/2T
        24,   16,    // 1/4,  1/4T
        12,   8,     // 1/8,  1/8T
        6,    4,     // 1/16, 1/16T
        3,    2,     // 1/32, 1/32T
    };
    int32_t index = option_index;
    if (index < 0) index = 0;
    if (index > 19) index = 19;
    return kRateBeatBpc24[index];
}

// sync=on phase. Drive the 10-bit phase from the continuous 24-PPQN pulse
// counter with sub-pulse smoothing. The times-24 encoding makes bpc_x24 ==
// pulses-per-cycle at 24 PPQN, so one cycle spans exactly bpc_x24 pulses and the
// integer phase within it is ((pulse_now mod bpc_x24) * 1024) / bpc_x24, mod
// 1024, continuous across bar lines and wrapping only at the true cycle length.
// For bpc_x24 = 192 ("2"), pulse 96 gives 512, half the cycle, and pulse 192
// wraps back to 0.
//
// frac blends in the sub-pulse position (from op::sdk::ClockInterpolator) so the
// phase sweeps continuously between pulse edges while staying clock-locked. It is
// the interpolator's progress toward the next pulse, in [0, 1).
//   progress   = (pulse_now mod bpc) + frac           (in [0, bpc))
//   phase_1024 = floor(progress * 1024 / bpc)  mod 1024
//
// The product (in_cycle + frac) * 1024 < 1'572'864 stays well inside float's
// exact-integer range (<= 16'777'216), and the single divide is done in float.
// frac is clamped to [0, 1) here in case a caller passes an out-of-range value.
uint16_t phase_sync_on_interp(uint32_t pulse_now, float frac, int32_t bpc_x24) {
    if (bpc_x24 <= 0) return 0;
    const uint32_t bpc      = static_cast<uint32_t>(bpc_x24);
    const uint32_t in_cycle = pulse_now % bpc;
    // Clamp frac into [0, 1) so the sub-pulse sweep never leaks into the next
    // integer pulse (the interpolator already clamps, this is belt-and-braces).
    float clamped_frac = frac;
    if (clamped_frac < 0.0f) clamped_frac = 0.0f;
    if (clamped_frac >= 1.0f) clamped_frac = 0.99999f;
    const float progress     = static_cast<float>(in_cycle) + clamped_frac;  // [0, bpc)
    const float phase_f      = progress * 1024.0f / static_cast<float>(bpc);
    const uint32_t phase_num = static_cast<uint32_t>(phase_f);
    return phase_num & 1023u;
}

// sync=off phase. rate is centi-Hz (rate = 100 -> 1 Hz), and the 10-bit phase
// wraps every 1/f seconds. Splitting tick_us into a quotient and residual keeps
// the arithmetic in 32-bit. The residual is in [0, 99'999] and, with the
// kRateMax = 2'000 clamp below, residual * rate fits in uint32 (199'998'000 <
// 2^32).
uint16_t phase_sync_off(uint32_t tick_us, int32_t rate) {
    if (rate <= 0) return 0;
    // Defensive clamp on rate upper bound. kParams declares rate max = 2'000
    // (20.00 Hz), but a corrupt param buffer must not cause silent uint32
    // overflow below.
    constexpr uint32_t kRateMax = 2'000u;
    uint32_t clamped_rate       = static_cast<uint32_t>(rate);
    if (clamped_rate > kRateMax) clamped_rate = kRateMax;

    const uint32_t quotient  = tick_us / 100'000u;
    const uint32_t residual  = tick_us - quotient * 100'000u;  // tick_us mod 100'000
    const uint32_t mixed     = residual * clamped_rate;        // fits uint32
    const uint32_t fine      = mixed / 100'000u;
    const uint32_t phase_num = quotient * clamped_rate + fine;  // natural wrap
    return phase_num & 1023u;
}

// Compute the phase offset that keeps the emitted phase continuous across a rate
// change. At the change instant the old curve emits
//   emitted_old = (base_old + offset_old) mod 1024
// and the new curve's base is base_new. To hold the emitted phase we need
//   offset_new = (emitted_old - base_new) mod 1024
// so that (base_new + offset_new) == emitted_old. Pure 10-bit modular
// arithmetic (no division). Adding 1024 before the mask keeps the subtraction
// non-negative.
uint16_t reanchor_offset(uint16_t base_old, uint16_t offset_old, uint16_t base_new) {
    const uint32_t emitted_old = (base_old + offset_old) & 1023u;
    const uint32_t offset_new  = (emitted_old + 1024u - (base_new & 1023u)) & 1023u;
    return offset_new;
}

namespace {  // resume anonymous namespace

// --- Shape dispatch -------------------------------------------------------

uint8_t evaluate_shape(int32_t shape, uint16_t phase_1024, SlotState& state, uint32_t seed) {
    switch (shape) {
        case kShapeSine: return op::samples::cc_lfo::sine_u7(phase_1024);
        case kShapeTriangle: return op::samples::cc_lfo::triangle_u7(phase_1024);
        case kShapeSaw: return op::samples::cc_lfo::saw_u7(phase_1024);
        case kShapeInvSaw: return op::samples::cc_lfo::inv_saw_u7(phase_1024);
        case kShapeSquare: return op::samples::cc_lfo::square_u7(phase_1024);
        case kShapeSampleHold:
            // S&H holds its value for a whole cycle. We detect a wrap by
            // comparing the current phase against the last observed one. When
            // phase has decreased (wrapped 1023 -> 0) the step index advances
            // and a new random value is sampled.
            if (state.primed && phase_1024 < state.last_phase) {
                ++state.sh_step_index;
            }
            state.primed     = true;
            state.last_phase = phase_1024;
            return op::samples::cc_lfo::sample_hold_u7(seed, state.sh_step_index);
        default: return 127;  // center (should not happen)
    }
}

// --- Per-slot evaluation --------------------------------------------------

void evaluate_slot(uint8_t slot_index,
                   uint32_t tick_us,
                   uint32_t pulse_now,
                   float pulse_frac,
                   uint32_t slot_emit_interval_us) {
    // Source gates the whole LFO. Off means no emit, Free is free-run (Rate Hz
    // via get_tick), Sync is clock-synced (Rate Beat via the pulse grid).
    const int32_t source = slot_param(slot_index, kOffSource);
    if (source == kSourceOff) return;

    const int32_t cc_number = slot_param(slot_index, kOffCc);
    if (cc_number < 0 || cc_number > 127) return;

    const int32_t shape    = slot_param(slot_index, kOffShape);
    const int32_t low_raw  = slot_param(slot_index, kOffLow);
    const int32_t high_raw = slot_param(slot_index, kOffHigh);

    // Clamp low/high to MIDI range and normalize so low <= high (UI should
    // prevent crossing, but persisted or corrupted state could invert it).
    int32_t low  = low_raw < 0 ? 0 : (low_raw > 127 ? 127 : low_raw);
    int32_t high = high_raw < 0 ? 0 : (high_raw > 127 ? 127 : high_raw);
    if (high < low) {
        const int32_t temp = low;
        low                = high;
        high               = temp;
    }

    SlotState& state = g_slots[slot_index];

    // Compute the base phase from the active clock and the rate key for this
    // evaluation. Sync uses bpc_x24 off the pulse counter, Free uses rate_hz off
    // the microsecond tick, and both feed the re-anchor logic below. Synced
    // pulses are measured relative to g_pulse_origin (re-snapped on transport in
    // process()) so the cycle tracks the downbeat. Unsigned modular subtraction
    // is wrap-safe across counter rollover, and is computed once here so the
    // rate-change re-anchor below recomputes the old base on the same relative
    // clock.
    const uint32_t relative_pulse = pulse_now - g_pulse_origin;

    uint16_t base_phase;
    int32_t rate_key;
    if (source == kSourceSync) {
        // RateBeat picks a measure or subdivision, mapped to bpc_x24. At 24 PPQN
        // that equals pulses-per-cycle, so the phase comes straight from the
        // pulse count, continuous across bar lines. pulse_frac fills the gap
        // between pulse edges so the synced sweep stays smooth (see
        // phase_sync_on_interp).
        rate_key   = ratebeat_to_bpc24(slot_param(slot_index, kOffRateBeat));
        base_phase = phase_sync_on_interp(relative_pulse, pulse_frac, rate_key);
    } else {
        // Free mode. RateHz (centi-Hz) drives the free-run phase accumulator.
        // Any non-Off, non-Sync source lands here.
        rate_key   = slot_param(slot_index, kOffRateHz);
        base_phase = phase_sync_off(tick_us, rate_key);
    }

    // Re-anchor the phase offset whenever the rate key changes (bpc_x24 under
    // Sync, rate_hz under Free), so the emitted phase is unchanged at the change
    // instant and keeps sweeping smoothly, with no value jump when editing rate
    // live. A Source switch also changes the key, so this path keeps that
    // transition continuous too. The first evaluation (key == unset) never
    // re-anchors. This only changes which phase is shaped, not whether or when we
    // emit.
    if (state.last_rate_key != rate_key) {
        if (state.last_rate_key != kRateKeyUnset) {
            // Recompute the old base at this same instant to find what we were
            // about to emit, then match it on the new base. For Sync, use the
            // same interpolated fraction so continuity is measured at the
            // identical sub-pulse instant.
            uint16_t base_old;
            if (source == kSourceSync) {
                base_old = phase_sync_on_interp(relative_pulse, pulse_frac, state.last_rate_key);
            } else {
                base_old = phase_sync_off(tick_us, state.last_rate_key);
            }
            state.phase_offset = reanchor_offset(base_old, state.phase_offset, base_phase);
        }
        state.last_rate_key = rate_key;
    }

    const uint16_t phase_1024 = (base_phase + state.phase_offset) & 1023u;

    const uint8_t raw_shape = evaluate_shape(shape, phase_1024, g_slots[slot_index], g_sh_seed);

    // Map raw_shape 0..254 into [low, high]. The divisor is 254 because every
    // shape evaluator caps at 254, the sine LUT by mirror geometry
    // (127 +/- 127 = 0..254) and saw and triangle by explicit clamp. The 127
    // half-point keeps center emission within 1 CC unit of (low + high) / 2.
    const int32_t span     = high - low;
    const int32_t scaled   = low + (raw_shape * span) / 254;
    const uint8_t cc_value = scaled;

    // Dedupe identical consecutive CC values per slot. Only compare here, do not
    // record last_cc_value yet (see the ordering note under the throttle gate).
    if (cc_value == g_slots[slot_index].last_cc_value) return;

    // Fair-share throttle. Defer this emit if the slot sent one less than
    // slot_emit_interval_us ago (its equal share of the total budget, computed
    // once per process(), see kTotalEmitBudgetHz). Unsigned subtraction is
    // wrap-safe across get_tick() rollover.
    if (state.emit_primed && (tick_us - state.last_emit_us) < slot_emit_interval_us) {
        // Leave last_cc_value unchanged on the deferred path. The value was not
        // sent, so a later tick must re-detect it as changed and retry. Do not
        // stamp last_emit_us either.
        return;
    }

    // Gate passed, so this is a real emission. Record the per-slot value (so
    // the next identical value dedupes) and stamp this slot's emit timestamp.
    state.last_cc_value = cc_value;
    state.last_emit_us  = tick_us;
    state.emit_primed   = true;

    op::api->send_midi(kAnyOutput,
                       /*status=*/0xB0,
                       /*data1=*/static_cast<uint8_t>(cc_number),
                       /*data2=*/cc_value);
}

}  // namespace

// --- Lifecycle -------------------------------------------------------------

void init() {
    // Reset every per-instance field to its cold-start value (see each
    // declaration for what the reset guarantees).
    for (uint8_t i = 0; i < kSlotCount; ++i) {
        g_slots[i] = SlotState {};
    }
    g_sh_seed      = 0x1234ABCDu;
    g_pulse_origin = 0;
    g_clock        = op::sdk::ClockInterpolator {};
    if (op::api) {
        const uint32_t tick = op::api->get_tick();
        if (tick != 0) g_sh_seed = tick ^ 0xDEADBEEFu;
    }
}

void process(OpMidiMessage* messages, uint8_t count, uint32_t tick_us) {
    if (!op::api) return;

    // Sample the interpolator once per tick (now() advances its state, so call
    // it exactly once). position.pulse is the integer pulse count and
    // position.frac the sub-pulse fraction in [0, 1) that smooths the synced
    // phase between edges (see phase_sync_on_interp).
    const op::sdk::ClockInterpolator::Position position = g_clock.now();
    const uint32_t pulse_now                            = position.pulse;
    const float pulse_frac                              = position.frac;

    // Watch MIDI transport and re-anchor the synced phase to the downbeat. Start
    // (0xFA) and Stop (0xFC) both re-snapshot the pulse origin (MIDI has no true
    // Pause, and typical hardware rewinds on Stop). Continue (0xFB) is ignored.
    // This generator only observes the realtime status bytes and passes the batch
    // through untouched. The scan is cheap since realtime bytes are single-byte
    // with no data payload.
    bool transport_reset = false;
    for (uint8_t i = 0; messages != nullptr && i < count; ++i) {
        const uint8_t status = messages[i].status;
        if (status == 0xFA || status == 0xFC) transport_reset = true;
    }
    if (transport_reset) {
        // Snapping g_pulse_origin to now makes relative_pulse 0, restarting the
        // synced cycle at the shape's start on the downbeat. This intentional
        // re-zero must win over the rate-change smoothing, so clear phase_offset
        // for Sync slots (Free slots are wall-clock and left untouched).
        // last_rate_key stays as-is, so the next evaluation does not re-anchor
        // and the offset stays 0.
        g_pulse_origin = pulse_now;
        for (uint8_t i = 0; i < kSlotCount; ++i) {
            if (slot_param(i, kOffSource) == kSourceSync) {
                g_slots[i].phase_offset = 0;
            }
        }
    }

    // Count active slots (Source != Off) so the total budget splits evenly,
    // giving each active slot an interval of active_count x kEmitPeriodPerLfoUs
    // (see kTotalEmitBudgetHz). Clamp to >= 1 so the interval is never zero. With
    // all 4 active the interval is 8000 us. Each slot self-limits, so a plain
    // 0..3 loop suffices.
    uint8_t active_count = 0;
    for (uint8_t i = 0; i < kSlotCount; ++i) {
        if (slot_param(i, kOffSource) != kSourceOff) ++active_count;
    }
    if (active_count == 0) active_count = 1;
    const uint32_t slot_emit_interval_us = static_cast<uint32_t>(active_count) * kEmitPeriodPerLfoUs;

    for (uint8_t slot_index = 0; slot_index < kSlotCount; ++slot_index) {
        evaluate_slot(slot_index, tick_us, pulse_now, pulse_frac, slot_emit_interval_us);
    }
}

void destroy() {
    // Clear every per-instance field so a later reload starts cold. The
    // SlotState{} assignment also clears each slot's throttle state.
    for (uint8_t i = 0; i < kSlotCount; ++i) {
        g_slots[i] = SlotState {};
    }
    g_pulse_origin = 0;
    g_clock        = op::sdk::ClockInterpolator {};
}

OP_MODE_REGISTER(init, process, destroy);
