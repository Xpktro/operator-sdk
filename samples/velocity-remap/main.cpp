// Velocity Remap sample mode.
//
// The curve math lives in velocity_curve.h. This file is the shell around it,
// the params that shape the curve, the note-on rewrite in process(), and a
// windowed custom UI that plots the curve as it is edited.
//
// Building the curve costs two transcendentals per table entry, far more than
// the MIDI path can spend, so the table is cached and process() only indexes it.

#include <operator_sdk.h>
#include <operator_sdk/draw.h>
#include <operator_sdk/params.h>
#include <operator_sdk/text.h>

#include "velocity_curve.h"

#include <cstdint>

// --- Parameters --------------------------------------------------------------
//
// The param page shows these full names. The mode's own screen has a narrow right
// column and labels the same values Min, Max, Tns, and Cmp.
//
// Constant is declared first so Minimum's visibility gate can name it.

inline constexpr op::params::Bool Constant {.name = "Constant", .default_ = false, .is_per_instance = 1};
inline constexpr op::params::Numeric Minimum {
    .name            = "Minimum",
    .min             = 1,
    .max             = 127,
    .default_        = 1,
    .is_per_instance = 1,
    .visible_when    = {.watch = Constant, .op = op::params::Op::Eq, .value = false}
};
inline constexpr op::params::Numeric Maximum {
    .name = "Maximum", .min = 1, .max = 127, .default_ = 127, .is_per_instance = 1};
inline constexpr op::params::Numeric Tension {
    .name = "Tension", .min = -100, .max = 100, .default_ = 0, .is_per_instance = 1};
inline constexpr op::params::Numeric Compand {
    .name = "Compand", .min = -100, .max = 100, .default_ = 0, .is_per_instance = 1};

OP_MODE_PARAMS(Minimum, Maximum, Tension, Compand, Constant);

// --- Engine state ------------------------------------------------------------

namespace {

// The cached curve. process() reads g_lut[velocity] and nothing more.
inline uint8_t g_lut[128] = {};

// The param values the table was built from. INT32_MIN matches no param value,
// so the first rebuild always runs.
inline int32_t g_seeded_minimum = INT32_MIN;
inline int32_t g_seeded_maximum = INT32_MIN;
inline int32_t g_seeded_tension = INT32_MIN;
inline int32_t g_seeded_compand = INT32_MIN;

// The last note-on's input velocity, which the screen draws as a cursor.
// process() writes it and ui_render() reads it from the other core. It is one
// display byte, so a torn read moves the cursor a pixel for a single frame.
inline volatile uint8_t g_last_input_velocity = 0;

// The field the mode's screen is focused on (0=Min, 1=Max, 2=Tns, 3=Cmp).
inline uint8_t g_focused_field = 0;

// Rebuild the table when the params it was built from have moved. process()
// calls this every block, so the common path is four reads and four compares.
void rebuild_lut_if_changed() {
    if (!op::api) return;
    const int32_t maximum = param<Maximum>();
    const int32_t tension = param<Tension>();
    const int32_t compand = param<Compand>();

    // Constant flattens the curve by lifting the effective minimum up to the
    // maximum, so every input velocity lands on the same output. The stored
    // Minimum is left to the UI to follow along (sync_constant_min).
    const int32_t minimum = (param<Constant>() != 0) ? maximum : param<Minimum>();

    if (minimum == g_seeded_minimum && maximum == g_seeded_maximum && tension == g_seeded_tension
        && compand == g_seeded_compand) {
        return;
    }
    build_lut(g_lut, minimum, maximum, tension, compand);
    g_seeded_minimum = minimum;
    g_seeded_maximum = maximum;
    g_seeded_tension = tension;
    g_seeded_compand = compand;
}

// Hold the stored Minimum at Maximum while Constant is on, so it persists and
// reappears there once Constant goes off. The mode's screen and the param page
// are the two writers of a param and both run on Core 0, so this belongs on the
// UI side and runs from ui_render() and ui_gesture() only.
void sync_constant_min() {
    if (!op::api || param<Constant>() == 0) return;
    const int32_t maximum = param<Maximum>();
    if (param<Minimum>() != maximum) {
        op::api->set_param_value(param_slot_info<Minimum>().slot, maximum);
    }
}

}  // namespace

// --- Lifecycle ---------------------------------------------------------------

void init() {
    g_seeded_minimum      = INT32_MIN;
    g_seeded_maximum      = INT32_MIN;
    g_seeded_tension      = INT32_MIN;
    g_seeded_compand      = INT32_MIN;
    g_last_input_velocity = 0;
    g_focused_field       = 0;
    rebuild_lut_if_changed();
}

void process(OpMidiMessage* messages, uint8_t count, uint32_t /*tick_us*/) {
    if (!op::api) return;
    rebuild_lut_if_changed();
    if (count == 0 || messages == nullptr) return;

    for (uint8_t i = 0; i < count; ++i) {
        OpMidiMessage& message = messages[i];
        // A note-on at velocity 0 is a note-off, and only a real note-on is remapped.
        const bool is_note_on = ((message.status & 0xF0) == 0x90) && (message.data2 != 0);
        if (!is_note_on) continue;
        g_last_input_velocity = message.data2;
        message.data2         = g_lut[message.data2];
    }
}

void destroy() { }

// --- Custom UI ---------------------------------------------------------------
//
// A windowed custom UI paints into the screen less its status bar, so the canvas
// is 128 x 53 from (0,0). The left half holds a square plot of the cached curve
// with a cursor on the last note's velocity, and the right half the four param
// fields with a marker on the focused one.

namespace {

constexpr uint16_t kViewWidth  = 128;
constexpr uint16_t kViewHeight = 53;
constexpr uint16_t kHalfWidth  = kViewWidth / 2;
constexpr uint16_t kPadding    = 2;
constexpr uint8_t kFieldCount  = 4;

// The plot box is the largest square the canvas height allows, centered in the
// left half and down the canvas.
constexpr uint16_t kBoxSize = kViewHeight - 2 * kPadding;
constexpr uint16_t kBoxX    = (kHalfWidth - kBoxSize) / 2;
constexpr uint16_t kBoxY    = (kViewHeight - kBoxSize) / 2;

// The curve is plotted inside the box border, which insets it a pixel all round.
constexpr uint16_t kPlotX    = kBoxX + 1;
constexpr uint16_t kPlotY    = kBoxY + 1;
constexpr uint16_t kPlotSize = kBoxSize - 2;
constexpr uint16_t kPlotLast = kPlotSize - 1;

// The label block is centered in the right half on its widest label, and the
// focus marker sits one glyph to the left of the label text.
constexpr uint16_t kMaxLabelWidth = 40;  // "Max 127" and "Cmp +50" in the 5x7 font
constexpr uint16_t kFieldX        = kHalfWidth + (kHalfWidth - kMaxLabelWidth) / 2;
constexpr uint16_t kMarkerX       = kFieldX - 6;
constexpr uint16_t kRowPitch      = 11;
constexpr uint16_t kLineHeight    = 8;

// Plot columns span the playable velocities, so the curve meets the inner corners
// of the box at both ends.
int column_to_velocity(int column) {
    return 1 + (column * 126 + kPlotLast / 2) / kPlotLast;
}

int velocity_to_column(int velocity) {
    if (velocity < 1) velocity = 1;
    if (velocity > 127) velocity = 127;
    return ((velocity - 1) * kPlotLast + 63) / 126;
}

// Screen y runs downward, so the loudest output sits at the top of the plot.
uint16_t plot_y(int output_velocity) {
    if (output_velocity < 1) output_velocity = 1;
    if (output_velocity > 127) output_velocity = 127;
    const int row = ((output_velocity - 1) * kPlotLast + 63) / 126;
    return kPlotY + (kPlotLast - row);
}

// Compose a field's label and value, as in "Min 64" or "Tns +50". Tension and
// Compand carry their sign, since they read either side of neutral.
void field_label(uint8_t index, char* out, uint8_t out_size) {
    if (!out || out_size == 0) return;
    static const char* const kNames[kFieldCount] = {"Min", "Max", "Tns", "Cmp"};
    const bool is_signed                         = index >= 2;
    int32_t value                                = 0;
    switch (index) {
        case 0: value = param<Minimum>(); break;
        case 1: value = param<Maximum>(); break;
        case 2: value = param<Tension>(); break;
        default: value = param<Compand>(); break;
    }
    op::sdk::text::format_str("", out, out_size);
    op::sdk::text::append_enum(out, out_size, index, kNames, kFieldCount);
    op::sdk::text::append_str(out, out_size, " ");
    op::sdk::text::append_int(out, out_size, value, /*force_plus=*/is_signed);
}

struct FieldInfo {
    uint8_t slot;
    int32_t minimum;
    int32_t maximum;
    int32_t default_value;
};

FieldInfo field_info(uint8_t index) {
    switch (index) {
        case 0: return {param_slot_info<Minimum>().slot, 1, 127, 1};
        case 1: return {param_slot_info<Maximum>().slot, 1, 127, 127};
        case 2: return {param_slot_info<Tension>().slot, -100, 100, 0};
        default: return {param_slot_info<Compand>().slot, -100, 100, 0};
    }
}

// Constant slaves Minimum to Maximum, so the screen drops the field for as long
// as it is on, the same gate the param page uses.
bool field_visible(uint8_t field) {
    return !(field == 0 && param<Constant>() != 0);
}

uint8_t visible_field_count() {
    return (param<Constant>() != 0) ? (kFieldCount - 1) : kFieldCount;
}

}  // namespace

void ui_render() {
    if (!op::api) return;
    sync_constant_min();  // catches Maximum edits made from the param page
    rebuild_lut_if_changed();

    // The canvas arrives cleared each frame.
    op::draw::draw_rect(kBoxX, kBoxY, kBoxSize, kBoxSize);

    // Walk the plot columns, look the output velocity up in the cached table, and
    // set the pixel. Steep columns get a short vertical run to bridge the gap, so
    // the curve reads as one line.
    int previous_y = -1;
    for (uint16_t column = 0; column < kPlotSize; ++column) {
        const int y = plot_y(g_lut[column_to_velocity(column)]);
        const int x = kPlotX + column;
        if (previous_y < 0) {
            op::draw::set_pixel(x, y, true);
        } else {
            const int top    = previous_y < y ? previous_y : y;
            const int bottom = previous_y < y ? y : previous_y;
            for (int fill = top; fill <= bottom; ++fill) {
                op::draw::set_pixel(x, fill, true);
            }
        }
        previous_y = y;
    }

    // A dotted line up the column of the last note's velocity, so playing shows
    // where the note entered the curve.
    const uint8_t last_velocity = g_last_input_velocity;
    if (last_velocity != 0) {
        const int cursor_x = kPlotX + velocity_to_column(last_velocity);
        for (uint16_t y = kPlotY; y < kPlotY + kPlotSize; ++y) {
            if (((y - kPlotY) & 1u) == 0u) {
                op::draw::set_pixel(cursor_x, y, true);
            }
        }
    }

    // The visible fields re-pack and re-center for their count, so hiding Minimum
    // leaves the remaining three centered on the screen.
    const uint16_t group_height = (visible_field_count() - 1) * kRowPitch + kLineHeight;
    const uint16_t first_row    = (kViewHeight - group_height) / 2;
    uint8_t row                 = 0;
    for (uint8_t field = 0; field < kFieldCount; ++field) {
        if (!field_visible(field)) continue;
        const int row_y = first_row + row * kRowPitch;
        if (field == g_focused_field) {
            op::draw::draw_text(kMarkerX, row_y, ">");
        }
        char label[16];
        field_label(field, label, sizeof(label));
        op::draw::draw_text(kFieldX, row_y, label);
        ++row;
    }
}

// The mode branches on the gesture type alone. Which encoder and which press
// produce each gesture is left to the active input map.
void ui_gesture(uint8_t /*encoder_id*/,
                op::Gesture /*gesture*/,
                op::GestureType gesture_type,
                int16_t value) {
    if (!op::api) return;

    if (gesture_type == op::GestureType::Scroll) {
        if (value == 0) return;
        const int direction = value > 0 ? 1 : -1;
        uint8_t field       = g_focused_field;
        for (int step = 0; step < kFieldCount; ++step) {
            field = (field + direction + kFieldCount) % kFieldCount;
            if (field_visible(field)) break;
        }
        g_focused_field = field;

    } else if (gesture_type == op::GestureType::Change) {
        if (value == 0) return;
        const FieldInfo info = field_info(g_focused_field);
        int32_t edited       = op::api->get_param_value(info.slot) + value;
        if (edited < info.minimum) edited = info.minimum;
        if (edited > info.maximum) edited = info.maximum;
        op::api->set_param_value(info.slot, edited);
        sync_constant_min();
        rebuild_lut_if_changed();

    } else if (gesture_type == op::GestureType::Enter) {
        const FieldInfo info = field_info(g_focused_field);
        op::api->set_param_value(info.slot, info.default_value);
        sync_constant_min();
        rebuild_lut_if_changed();

    } else if (gesture_type == op::GestureType::Back) {
        // Turning Constant on takes Minimum with it and moves the focus off the
        // field that is about to leave the screen.
        const bool turn_on = (param<Constant>() == 0);
        op::api->set_param_value(param_slot_info<Constant>().slot, turn_on ? 1 : 0);
        if (turn_on) {
            op::api->set_param_value(param_slot_info<Minimum>().slot, param<Maximum>());
            if (g_focused_field == 0) g_focused_field = 1;
        }
        rebuild_lut_if_changed();
    }
}

OP_MODE_REGISTER(init, process, destroy);
OP_MODE_REGISTER_UI(ui_render, ui_gesture);
