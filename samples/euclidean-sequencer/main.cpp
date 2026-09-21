// Euclidean Sequencer sample mode.
//
// Output mode with a custom two-screen UI. Four independent sequences each hold a
// (Steps, Events, Offset) triple, spread their onsets with the Bresenham
// distributor in euclidean.h, and emit a note-on at every onset boundary.
//
// A bar is 96 pulses (24 PPQ across 4 beats), and each sequence divides that bar
// into its own `Steps` equal slots. A 16-step sequence therefore ticks 16 times a
// bar and a 12-step sequence ticks 12 times, which is what lets the four layer
// into polyrhythms. On a step boundary the sequence releases its previous note and
// plays a fresh one when the new step carries an onset.
//
// Step math is integer throughout, so `floor_beat_times_n` derives the step index
// from the bar fraction without a call to floor() or fmod().

#include <operator_sdk.h>
#include <operator_sdk/draw.h>  // op::draw::* int-accepting draw wrappers (opt-in)
#include <operator_sdk/params.h>
#include <operator_sdk/text.h>
#include <operator_sdk/timing.h>  // op::sdk::ClockInterpolator, op::sdk::StepTracker (opt-in)

#include "euclidean.h"

#include <bit>
#include <cstdint>

// --- Parameters ------------------------------------------------------------
//
// Four sequences of three params each. Events is clamped in process() to the
// sequence's current Steps.

inline constexpr op::params::Numeric S1Steps {.name = "Seq 1 Steps", .min = 1, .max = 32, .default_ = 16};
inline constexpr op::params::Numeric S1Events {.name = "Seq 1 Events", .min = 0, .max = 32, .default_ = 4};
inline constexpr op::params::Numeric S1Offset {.name = "Seq 1 Offset", .min = 0, .max = 31, .default_ = 0};
inline constexpr op::params::Numeric S2Steps {.name = "Seq 2 Steps", .min = 1, .max = 32, .default_ = 16};
inline constexpr op::params::Numeric S2Events {.name = "Seq 2 Events", .min = 0, .max = 32, .default_ = 4};
inline constexpr op::params::Numeric S2Offset {.name = "Seq 2 Offset", .min = 0, .max = 31, .default_ = 0};
inline constexpr op::params::Numeric S3Steps {.name = "Seq 3 Steps", .min = 1, .max = 32, .default_ = 16};
inline constexpr op::params::Numeric S3Events {.name = "Seq 3 Events", .min = 0, .max = 32, .default_ = 4};
inline constexpr op::params::Numeric S3Offset {.name = "Seq 3 Offset", .min = 0, .max = 31, .default_ = 0};
inline constexpr op::params::Numeric S4Steps {.name = "Seq 4 Steps", .min = 1, .max = 32, .default_ = 16};
inline constexpr op::params::Numeric S4Events {.name = "Seq 4 Events", .min = 0, .max = 32, .default_ = 4};
inline constexpr op::params::Numeric S4Offset {.name = "Seq 4 Offset", .min = 0, .max = 31, .default_ = 0};

OP_MODE_PARAMS(S1Steps,
               S1Events,
               S1Offset,
               S2Steps,
               S2Events,
               S2Offset,
               S3Steps,
               S3Events,
               S3Offset,
               S4Steps,
               S4Events,
               S4Offset);

namespace {

// --- Tunables / constants -------------------------------------------------

constexpr uint8_t kSeqCount        = 4;
constexpr uint8_t kMaxSteps        = 32;
constexpr uint8_t kAnyOutput       = 0;  // unused output argument
constexpr uint8_t kDefaultVelocity = 100;

// One base note per sequence, laid out as kick, snare, closed hat and open hat so
// a single instance drives a basic drum machine with no further setup.
constexpr uint8_t kBaseNote[kSeqCount] = {36, 38, 40, 42};

// --- Per-sequence state ---------------------------------------------------

struct SeqState {
    bool pattern[kMaxSteps] = {};

    // The params the current pattern was built from. A change to any of them
    // triggers a rebuild.
    int32_t cached_steps  = -1;
    int32_t cached_events = -1;
    int32_t cached_offset = -1;

    // Which step the sequence is on, and whether the grid has moved it there.
    op::sdk::StepTracker steps;

    // The sounding voice, with 0 meaning silent.
    uint8_t active_note = 0;
};

SeqState g_seq[kSeqCount];

// The four sequences share one bar position, so one interpolator serves the mode.
// It smooths the bar fraction between pulse edges, which sweeps the UI playhead
// continuously. Step firing reads the same fraction through an integer floor, so
// it lands on a step boundary either way.
op::sdk::ClockInterpolator g_clock_interp {};

// --- Screen state (UI) ----------------------------------------------------

uint8_t g_screen                = 0;  // 0 = main, 1 = detail
uint8_t g_selected_seq          = 0;  // the row on main, the sequence on detail
uint8_t g_selected_detail_field = 0;  // 0 = Steps, 1 = Events, 2 = Offset

// --- Param slot lookup ----------------------------------------------------
//
// The (steps, events, offset) slot index for each sequence, resolved at compile
// time from the OP_MODE_PARAMS bundle so call sites stay independent of
// declaration order, and stored in arrays for indexing by g_selected_seq.

inline constexpr uint8_t kStepsSlot[kSeqCount] = {
    param_slot_info<S1Steps>().slot,
    param_slot_info<S2Steps>().slot,
    param_slot_info<S3Steps>().slot,
    param_slot_info<S4Steps>().slot,
};
inline constexpr uint8_t kEventsSlot[kSeqCount] = {
    param_slot_info<S1Events>().slot,
    param_slot_info<S2Events>().slot,
    param_slot_info<S3Events>().slot,
    param_slot_info<S4Events>().slot,
};
inline constexpr uint8_t kOffsetSlot[kSeqCount] = {
    param_slot_info<S1Offset>().slot,
    param_slot_info<S2Offset>().slot,
    param_slot_info<S3Offset>().slot,
    param_slot_info<S4Offset>().slot,
};

// --- Integer-only floor(beat x n) -------------------------------------
//
// Take the float apart by hand and multiply the mantissa before shifting, so the
// sub-beat precision survives into the integer result. Used to derive
// current_step = floor(measure_position x Steps).
int32_t floor_beat_times_n(float beat_position, uint32_t multiplier) {
    uint32_t bits = std::bit_cast<uint32_t>(beat_position);
    if (bits & 0x80000000u) return 0;
    uint32_t exponent = (bits >> 23) & 0xFFu;
    if (exponent == 0u) return 0;
    if (exponent >= 0xFFu) return 0;

    const uint32_t mantissa = (bits & 0x7FFFFFu) | 0x800000u;
    const int32_t shift     = static_cast<int32_t>(exponent) - 127 - 23;

    uint64_t product = static_cast<uint64_t>(mantissa) * static_cast<uint64_t>(multiplier);
    if (shift >= 0)
        product <<= shift;
    else
        product >>= -shift;
    if (product > 0x7FFFFFFFull) return 0x7FFFFFFF;
    return static_cast<int32_t>(product);
}

// Rebuild `sequence.pattern` from the (steps, events, offset) triple. The caller
// has already checked steps > 0.
void rebuild_pattern(SeqState& sequence, uint8_t steps, uint8_t events, uint8_t offset) {
    op::samples::euclidean::euclidean_bresenham(events, steps, offset, sequence.pattern);
    sequence.cached_steps  = steps;
    sequence.cached_events = events;
    sequence.cached_offset = offset;
}

// The (steps, events, offset) triple the engine actually runs on, clamped to its
// legal ranges.
struct ResolvedParams {
    uint8_t steps, events, offset;
};

ResolvedParams resolve_params(uint8_t index) {
    if (!op::api) return {16, 4, 0};
    int32_t steps  = op::api->get_param_value(kStepsSlot[index]);
    int32_t events = op::api->get_param_value(kEventsSlot[index]);
    int32_t offset = op::api->get_param_value(kOffsetSlot[index]);

    if (steps < 1) steps = 1;
    if (steps > kMaxSteps) steps = kMaxSteps;
    if (events < 0) events = 0;
    if (events > steps) events = steps;
    if (offset < 0) offset = 0;
    if (steps > 0) offset %= steps;

    return {static_cast<uint8_t>(steps), static_cast<uint8_t>(events), static_cast<uint8_t>(offset)};
}

// Release the sounding voice on this sequence.
void release_voice(SeqState& sequence) {
    if (sequence.active_note != 0) {
        op::api->send_midi(kAnyOutput, 0x80, sequence.active_note, /*velocity=*/0);
        sequence.active_note = 0;
    }
}

// The bar fraction in [0, 1), interpolated so it sweeps continuously between pulse
// edges. The clamp keeps a pulse-95 sample with a fraction approaching 1 from
// reaching unity.
float measure_position() {
    const op::sdk::ClockInterpolator::Position position = g_clock_interp.now();
    float fraction = (static_cast<float>(position.pulse % 96u) + position.frac) / 96.0f;
    if (fraction >= 1.0f) fraction = 0.99999f;
    return fraction;
}

// The step a sequence of `steps` slots is on at `fraction` of the bar.
int32_t step_at(float fraction, uint8_t steps) {
    int32_t step = floor_beat_times_n(fraction, steps);
    if (step >= steps) step = steps - 1;
    return step;
}

}  // namespace

// --- Lifecycle -------------------------------------------------------------

void init() {
    for (uint8_t i = 0; i < kSeqCount; ++i) {
        g_seq[i] = SeqState {};
    }
    g_screen                = 0;
    g_selected_seq          = 0;
    g_selected_detail_field = 0;
}

void process(OpMidiMessage* /*messages*/, uint8_t /*count*/, uint32_t /*tick_us*/) {
    if (!op::api) return;

    // now() advances the interpolator, so process() calls it once per tick.
    const float fraction = measure_position();

    for (uint8_t i = 0; i < kSeqCount; ++i) {
        SeqState& sequence = g_seq[i];
        const auto params  = resolve_params(i);

        if (params.steps != sequence.cached_steps || params.events != sequence.cached_events
            || params.offset != sequence.cached_offset) {
            rebuild_pattern(sequence, params.steps, params.events, params.offset);
        }

        const int32_t current_step = step_at(fraction, params.steps);

        if (sequence.steps.observe(current_step) == op::sdk::StepEdge::Crossed) {
            // A step boundary, so close the previous voice and open a new one when
            // this step carries an onset.
            release_voice(sequence);

            if (sequence.pattern[current_step]) {
                const uint8_t note = kBaseNote[i];
                op::api->send_midi(kAnyOutput, /*status=*/0x90, note, kDefaultVelocity);
                sequence.active_note = note;
            }
        }
    }
}

void destroy() {
    if (op::api) {
        for (uint8_t i = 0; i < kSeqCount; ++i) release_voice(g_seq[i]);
    }
    for (uint8_t i = 0; i < kSeqCount; ++i) g_seq[i] = SeqState {};
    g_screen                = 0;
    g_selected_seq          = 0;
    g_selected_detail_field = 0;
}

// --- Custom UI -------------------------------------------------------------

namespace {

constexpr uint16_t kScreenW = 128;

// The four main-screen rows spread evenly down the content area, 11 px tall with
// 2 px between them.
constexpr uint16_t kRowY[kSeqCount] = {1, 14, 27, 40};
constexpr uint16_t kRowHeight       = 11;
constexpr uint16_t kLabelX          = 2;

// step_x() anchors the first and last dot at these two x positions and spreads the
// intermediate ones between them, so the right-hand padding stays even at any step
// count. The last-dot anchor is pulled in far enough that the 5-px playhead arrow
// clears the row's selection outline at x = 127.
constexpr uint16_t kDotsXOrigin = 38;
constexpr uint16_t kDotsXEnd    = 123;
constexpr uint16_t kDotsSpanW   = kDotsXEnd - kDotsXOrigin;  // 85

// The detail screen draws the same row across a wider area with 3-px dots.
constexpr uint16_t kDetailDotsXOrigin = 4;
constexpr uint16_t kDetailDotsXEnd    = 124;
constexpr uint16_t kDetailDotsSpanW   = kDetailDotsXEnd - kDetailDotsXOrigin;  // 120

// Spread `total` dots across [origin, origin + span_w], putting step 0 at the
// origin and the last step at the far end. A lone dot sits in the middle.
inline uint16_t step_x(uint8_t step, uint8_t total, uint16_t origin, uint16_t span_w) {
    if (total <= 1) {
        return origin + span_w / 2u;
    }
    return origin + (span_w * step) / (total - 1u);
}

// The detail screen stacks a title, a trigger row and three param rows down the
// content area.
constexpr uint16_t kDetailTitleY      = 3;
constexpr uint16_t kDetailRowYTrigger = 15;
constexpr uint16_t kDetailRowYStep    = 26;
constexpr uint16_t kDetailRowYEvents  = 34;
constexpr uint16_t kDetailRowYOffset  = 42;

// The playhead is a 5x2 upward trapezoid below the dot line, a 3-px stub over a
// 5-px base, matching the device's digit-edit cursor.
void draw_playhead(uint16_t x, int y_top, int y_bot) {
    op::draw::fill_rect(x - 1, y_top, /*w=*/3, /*h=*/1, /*on=*/true);
    op::draw::fill_rect(x - 2, y_bot, /*w=*/5, /*h=*/1, /*on=*/true);
}

void draw_main_row(uint8_t index, const ResolvedParams& params, int32_t live_step) {
    const uint16_t y = kRowY[index];

    // The label is a constant string with the one sequence digit patched in.
    char label[] = "SEQ 1:";
    label[4]     = '1' + index;
    op::draw::draw_text(kLabelX, y + 2, label);

    SeqState& sequence = g_seq[index];
    if (params.steps > 0) {
        for (uint8_t step = 0; step < params.steps; ++step) {
            const uint16_t x = step_x(step, params.steps, kDotsXOrigin, kDotsSpanW);
            if (sequence.pattern[step]) {
                op::draw::fill_rect(x, y + 4, /*w=*/2, /*h=*/2, /*on=*/true);
            } else {
                op::draw::fill_rect(x + 1, y + 5, /*w=*/1, /*h=*/1,
                                    /*on=*/true);
            }
        }
        if (live_step >= 0 && live_step < params.steps) {
            const uint16_t x = step_x(static_cast<uint8_t>(live_step), params.steps, kDotsXOrigin,
                                      kDotsSpanW);
            draw_playhead(x, y + 7, y + 8);
        }
    }

    if (index == g_selected_seq) {
        op::draw::draw_rect(0, y, kScreenW, kRowHeight);
    }
}

void draw_detail_screen() {
    const uint8_t index = g_selected_seq;
    const auto params   = resolve_params(index);

    // The title is a constant string with the one sequence digit patched in.
    char title[]           = "SEQ 1 EDITOR";
    title[4]               = '1' + index;
    const uint16_t title_x = op::sdk::text::align_center_x(title, kScreenW);
    op::draw::draw_text(title_x, kDetailTitleY, title);

    if (params.steps > 0) {
        SeqState& sequence = g_seq[index];
        for (uint8_t step = 0; step < params.steps; ++step) {
            const uint16_t x = step_x(step, params.steps, kDetailDotsXOrigin, kDetailDotsSpanW);
            if (sequence.pattern[step]) {
                op::draw::fill_rect(x, kDetailRowYTrigger, 3, 3, /*on=*/true);
            } else {
                op::draw::fill_rect(x + 1, kDetailRowYTrigger + 1, 1, 1,
                                    /*on=*/true);
            }
        }
        const int32_t live_step = step_at(measure_position(), params.steps);
        if (live_step >= 0) {
            const uint16_t x = step_x(static_cast<uint8_t>(live_step), params.steps, kDetailDotsXOrigin,
                                      kDetailDotsSpanW);
            // The 3-px dots run two rows lower than the main screen's, so the arrow
            // sits 4 px below the trigger row.
            draw_playhead(x + 1, kDetailRowYTrigger + 4, kDetailRowYTrigger + 5);
        }
    }

    // Each param row carries a "> " marker when it is the selected one.
    auto draw_param_row = [&](uint16_t y, uint8_t row_index, const char* label, int32_t value) {
        char buffer[24];
        op::sdk::text::format_str(row_index == g_selected_detail_field ? "> " : "  ", buffer, sizeof(buffer));
        op::sdk::text::append_str(buffer, sizeof(buffer), label);
        op::sdk::text::append_uint(buffer, sizeof(buffer), static_cast<uint32_t>(value));
        op::draw::draw_text(2, y, buffer);
    };
    draw_param_row(kDetailRowYStep, 0, "STEPS:  ", params.steps);
    draw_param_row(kDetailRowYEvents, 1, "EVENTS: ", params.events);
    draw_param_row(kDetailRowYOffset, 2, "OFFSET: ", params.offset);
}

}  // namespace

void ui_render() {
    if (!op::api) return;

    if (g_screen == 0) {
        const float fraction = measure_position();
        for (uint8_t i = 0; i < kSeqCount; ++i) {
            const auto params = resolve_params(i);
            int32_t live_step = -1;
            if (params.steps > 0) {
                live_step = step_at(fraction, params.steps);
            }
            draw_main_row(i, params, live_step);
        }
    } else {
        draw_detail_screen();
    }
}

void ui_gesture(uint8_t encoder_id, op::Gesture gesture, op::GestureType gesture_type, int16_t value) {
    // Branch on gesture_type, the role the active Input Map preset resolved the
    // gesture to, so the mode follows whichever preset the user runs. Rotation
    // direction stays in `value` and is the same under every preset.
    (void)encoder_id;
    (void)gesture;
    if (!op::api) return;

    if (g_screen == 0) {
        // Main screen.
        if (gesture_type == op::GestureType::Scroll) {
            int32_t next = g_selected_seq + value;
            if (next < 0) next = 0;
            if (next > kSeqCount - 1) next = kSeqCount - 1;
            g_selected_seq = next;
        } else if (gesture_type == op::GestureType::Enter) {
            g_screen                = 1;
            g_selected_detail_field = 0;
        }
        return;
    }

    // Detail screen.
    if (gesture_type == op::GestureType::Scroll) {
        int32_t next = g_selected_detail_field + value;
        if (next < 0) next = 0;
        if (next > 2) next = 2;
        g_selected_detail_field = next;
    } else if (gesture_type == op::GestureType::Back) {
        g_screen = 0;
    } else if (gesture_type == op::GestureType::Change) {
        // Edit the selected param of the selected sequence.
        const uint8_t index = g_selected_seq;
        uint8_t slot        = 0;
        int32_t lowest = 0, highest = 0;
        switch (g_selected_detail_field) {
            case 0:
                slot    = kStepsSlot[index];
                lowest  = 1;
                highest = kMaxSteps;
                break;
            case 1:
                slot    = kEventsSlot[index];
                lowest  = 0;
                highest = kMaxSteps;
                break;
            default:
                slot    = kOffsetSlot[index];
                lowest  = 0;
                highest = kMaxSteps - 1;
                break;
        }
        int32_t current = op::api->get_param_value(slot);
        current += value;
        if (current < lowest) current = lowest;
        if (current > highest) current = highest;
        op::api->set_param_value(slot, current);
    } else if (gesture_type == op::GestureType::Enter) {
        // Reset the selected sequence to its defaults.
        const uint8_t index = g_selected_seq;
        op::api->set_param_value(kStepsSlot[index], 16);
        op::api->set_param_value(kEventsSlot[index], 4);
        op::api->set_param_value(kOffsetSlot[index], 0);
    }
}

OP_MODE_REGISTER(init, process, destroy);
OP_MODE_REGISTER_UI(ui_render, ui_gesture);
