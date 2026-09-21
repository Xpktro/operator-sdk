// User Scale (Output) sample, doctest behavior suite.
//
// The screen, the gestures and the note mapping are the shared body's, and they
// are covered by the suite that sits with it. What the shell adds is the param
// table and the five entry points it hands over, so that is what is checked here.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <operator_sdk_sim.h>
#include <operator_sdk.h>

#include "../../user-scale-common/user_scale_mode.h"

#include <cstdint>

OP_MODE_UNDER_TEST();

namespace us = op::samples::user_scale;

TEST_CASE("the shell hands every entry point to the shared body") {
    op::sim::reset_state();
    op::sim::apply_param_defaults(kParams, kParamCount);
    op::sim::set_current_mode_name("user_scale_output");
    const auto* api = op::sim::get_api();

    // init reaches the body, which opens on the scale the params name.
    mode_init(api);
    REQUIRE(api->get_param_value(us::kCurrentScaleSlot) == 1);  // Major

    // process reaches it, so a C# rounds up to the D that Major holds.
    OpMidiMessage message {
        /*status*/ 0x90,
        /*data1*/ 61,
        /*data2*/ 100,
        /*port*/ 0,
        /*length*/ 3,
    };
    mode_process(&message, 1, api->get_tick());
    CHECK(message.data1 == 62);

    // ui_render reaches it, so the keyboard is drawn.
    uint8_t framebuffer[op::sim::kFramebufferBytes] {};
    mode_ui_render();
    op::sim::capture_framebuffer(framebuffer);
    uint32_t lit = 0;
    for (uint16_t row = 22; row < 44; ++row) {
        const uint16_t page = row / 8;
        const uint8_t bit   = 1u << (row % 8);
        for (uint16_t column = 32; column < 96; ++column) {
            if (framebuffer[page * 128 + column] & bit) ++lit;
        }
    }
    CHECK(lit > 100);

    // ui_gesture reaches it, so an Enter on the keyboard edits the scale, which
    // moves it to Custom.
    op::sim::ui_gesture(kEncoderRight, op::Gesture::ShortPress, op::GestureType::Enter, 0);
    CHECK(api->get_param_value(us::kCurrentScaleSlot) == us::kCustomScaleMarker);

    mode_destroy();
}
