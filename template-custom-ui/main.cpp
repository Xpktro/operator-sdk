// Custom UI mode template, a counter on the screen.
//
// Copy this directory to start a mode that draws its own screen. The counter is
// here to be replaced. What it shows is the shape of such a mode, the two fonts,
// centering a value with the text helpers, and branching on the gesture type.

#include <operator_sdk.h>
#include <operator_sdk/draw.h>
#include <operator_sdk/params.h>
#include <operator_sdk/text.h>

#include <cstdint>

OP_MODE_NO_PARAMS();

namespace {

constexpr uint16_t kScreenWidth = 128;

int32_t g_counter = 0;

}  // namespace

// --- Lifecycle ---------------------------------------------------------------

void init() {
    g_counter = 0;
}

void process(OpMidiMessage*, uint8_t, uint32_t) {
    // The mode draws, and the MIDI passing through it goes by untouched.
}

void destroy() {
    g_counter = 0;
}

// --- The screen --------------------------------------------------------------

void ui_render() {
    if (!op::api) return;

    char value[12];
    op::sdk::text::format_int(g_counter, value, sizeof(value));

    const uint16_t value_x = op::sdk::text::large_align_center_x(value, kScreenWidth);
    op::draw::draw_text_large(value_x, 20, value);

    op::draw::draw_text(2, 56, "Change +1   Enter reset");
}

// The mode branches on the gesture type alone, and the active input map decides
// which encoder and which press produce each one.
//
// Long-press is spoken for on both encoders. One leaves the custom UI and the other
// opens the mode's Parameters page, so a mode is sent rotates and short presses.
void ui_gesture(uint8_t /*encoder_id*/,
                op::Gesture /*gesture*/,
                op::GestureType gesture_type,
                int16_t value) {
    if (gesture_type == op::GestureType::Change) {
        g_counter += value;
    } else if (gesture_type == op::GestureType::Enter) {
        g_counter = 0;
    }
}

OP_MODE_REGISTER(init, process, destroy);
OP_MODE_REGISTER_UI(ui_render, ui_gesture);
