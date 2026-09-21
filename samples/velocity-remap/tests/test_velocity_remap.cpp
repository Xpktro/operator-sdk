// Velocity Remap sample, doctest behavior suite.
//
// Two things to pin down. process() has to reshape real note-ons and leave every
// other message alone, and the mode's own screen has to move focus, edit, reset,
// and toggle Constant off the four gesture types it is sent.
//
// The curve itself is covered in test_velocity_curve.cpp. Here build_lut only
// supplies the expected output for a given input, so process() can be held to the
// same table it reads on the device.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <operator_sdk.h>
#include <operator_sdk_sim.h>

#include "velocity_curve.h"

#include <cstdint>

OP_MODE_UNDER_TEST();

namespace {

// Slot order follows OP_MODE_PARAMS(Minimum, Maximum, Tension, Compand, Constant)
// in main.cpp. Every param is a scalar, so each takes one slot.
constexpr uint8_t kSlotMinimum  = 0;
constexpr uint8_t kSlotMaximum  = 1;
constexpr uint8_t kSlotTension  = 2;
constexpr uint8_t kSlotCompand  = 3;
constexpr uint8_t kSlotConstant = 4;

// The gesture type is what the mode branches on. The encoder and the press that
// produced it are left to the active input map and the mode ignores both, so any
// encoder will do here.
constexpr uint8_t kAnyEncoder = 0;

void boot_with(int minimum, int maximum, int tension, int compand) {
    op::sim::reset_state();
    op::sim::apply_param_defaults(kParams, kParamCount);
    mode_init(op::sim::get_api());

    const auto* api = op::sim::get_api();
    api->set_param_value(kSlotMinimum, minimum);
    api->set_param_value(kSlotMaximum, maximum);
    api->set_param_value(kSlotTension, tension);
    api->set_param_value(kSlotCompand, compand);
}

void scroll(int16_t steps) {
    op::sim::ui_gesture(kAnyEncoder, op::Gesture::Rotate, op::GestureType::Scroll, steps);
}
void change(int16_t steps) {
    op::sim::ui_gesture(kAnyEncoder, op::Gesture::Rotate, op::GestureType::Change, steps);
}
void enter() {
    op::sim::ui_gesture(kAnyEncoder, op::Gesture::ShortPress, op::GestureType::Enter, 0);
}
void back() {
    op::sim::ui_gesture(kAnyEncoder, op::Gesture::ShortPress, op::GestureType::Back, 0);
}

int param(uint8_t slot) {
    return static_cast<int>(op::sim::get_api()->get_param_value(slot));
}

OpMidiMessage note_on(uint8_t pitch, uint8_t velocity) {
    return OpMidiMessage {0x90, pitch, velocity, /*port=*/0, /*length=*/3};
}

}  // namespace

// --- Processing --------------------------------------------------------------

TEST_CASE("a note-on comes out at the velocity the curve maps it to") {
    boot_with(/*minimum=*/1, /*maximum=*/127, /*tension=*/0, /*compand=*/50);
    uint8_t lut[128];
    build_lut(lut, 1, 127, 0, 50);

    OpMidiMessage message = note_on(60, 64);
    mode_process(&message, 1, 0);

    CHECK(static_cast<int>(message.data2) == static_cast<int>(lut[64]));
    CHECK(static_cast<int>(message.status) == 0x90);
    CHECK(static_cast<int>(message.data1) == 60);
}

// Everything that is not a real note-on keeps its bytes, which is what holds a
// note-off at velocity 0 and leaves the rest of the stream expressive.
TEST_CASE("messages other than note-ons pass through untouched") {
    boot_with(1, 127, 0, 50);

    OpMidiMessage note_off {0x80, 60, 64, 0, 3};
    OpMidiMessage released {0x90, 60, 0, 0, 3};  // note-on status at velocity 0
    OpMidiMessage control {0xB0, 7, 64, 0, 3};
    OpMidiMessage clock {0xF8, 0, 0, 0, 1};

    mode_process(&note_off, 1, 0);
    mode_process(&released, 1, 0);
    mode_process(&control, 1, 0);
    mode_process(&clock, 1, 0);

    CHECK(static_cast<int>(note_off.data2) == 64);
    CHECK(static_cast<int>(released.data2) == 0);
    CHECK(static_cast<int>(control.data1) == 7);
    CHECK(static_cast<int>(control.data2) == 64);
    CHECK(static_cast<int>(clock.status) == 0xF8);
}

// Constant collapses the curve to a flat line at Maximum, so how hard the note
// was played stops mattering and later Maximum edits move the whole output.
TEST_CASE("Constant sends every note at the Maximum velocity") {
    boot_with(/*minimum=*/40, /*maximum=*/100, /*tension=*/60, /*compand=*/40);
    op::sim::get_api()->set_param_value(kSlotConstant, 1);

    for (int velocity : {1, 30, 64, 100, 127}) {
        OpMidiMessage message = note_on(60, static_cast<uint8_t>(velocity));
        mode_process(&message, 1, 0);
        CHECK(static_cast<int>(message.data2) == 100);
    }

    op::sim::get_api()->set_param_value(kSlotMaximum, 70);
    OpMidiMessage message = note_on(60, 64);
    mode_process(&message, 1, 0);
    CHECK(static_cast<int>(message.data2) == 70);
}

// --- Screen ------------------------------------------------------------------

TEST_CASE("Change edits the focused field") {
    boot_with(1, 127, 0, 0);
    change(+5);
    CHECK(param(kSlotMinimum) == 6);
    CHECK(param(kSlotMaximum) == 127);
}

TEST_CASE("Scroll moves the focus to the next field") {
    boot_with(1, 127, 0, 0);
    scroll(+1);
    change(-5);
    CHECK(param(kSlotMaximum) == 122);
    CHECK(param(kSlotMinimum) == 1);
}

TEST_CASE("Enter resets the focused field to its default") {
    boot_with(1, 127, 0, 0);
    change(+40);
    CHECK(param(kSlotMinimum) == 41);
    enter();
    CHECK(param(kSlotMinimum) == 1);
}

// Turning Constant on hands Minimum the Maximum value so it survives the toggle
// and reappears there when Constant goes off again.
TEST_CASE("Back toggles Constant and holds Minimum at the Maximum") {
    boot_with(/*minimum=*/20, /*maximum=*/90, /*tension=*/0, /*compand=*/0);

    back();
    CHECK(param(kSlotConstant) == 1);
    CHECK(param(kSlotMinimum) == 90);

    back();
    CHECK(param(kSlotConstant) == 0);
}

// Minimum leaves the screen while Constant is on, so the focus steps over it onto
// the fields that remain.
TEST_CASE("Constant hides Minimum so the focus skips it") {
    boot_with(1, 127, 0, 0);
    back();  // Constant on, focus moves off the hidden Minimum onto Maximum

    scroll(-1);  // wraps backward past Minimum onto Compand
    change(+10);
    CHECK(param(kSlotCompand) == 10);
    CHECK(param(kSlotMinimum) == param(kSlotMaximum));
}
