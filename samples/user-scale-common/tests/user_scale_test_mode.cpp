// The shared body wired up as a mode, so the behavior suite has one to drive.
//
// A mode is a param table and the entry points it exports, which is all either
// User Scale shell adds to the body. This carries the same shape, and it is built
// the way a shell is, so the suite exercises the body as it really runs.

#include "../user_scale_mode.h"

OP_USER_SCALE_DEFINE_PARAMS(0);

namespace us = op::samples::user_scale;

void init() {
    us::impl_init();
}
void process(OpMidiMessage* messages, uint8_t count, uint32_t tick_us) {
    us::impl_process(messages, count, tick_us);
}
void destroy() {
    us::impl_destroy();
}
void ui_render() {
    us::impl_ui_render();
}
void ui_gesture(uint8_t encoder_id, op::Gesture gesture, op::GestureType gesture_type, int16_t value) {
    us::impl_ui_gesture(encoder_id, gesture, gesture_type, value);
}

OP_MODE_REGISTER(init, process, destroy);
OP_MODE_REGISTER_UI(ui_render, ui_gesture);
