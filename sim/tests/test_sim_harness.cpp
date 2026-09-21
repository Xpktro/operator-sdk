// The mock device, doctest behavior suite.
//
// Drives every public op::sim entry through the mock OperatorApi, so a mode's own
// suite can lean on them. A mode's behavior is covered by that mode's suite. What
// is covered here is that the device it is tested against does what it says.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <operator_sdk_sim.h>
#include <operator_sdk/abi/mode_param.h>  // ParamSpec, ParamKind for apply_param_defaults

#include <cstdint>
#include <cstring>
#include <string_view>

TEST_CASE("a sent message comes back out of the output it was sent to") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    REQUIRE(api != nullptr);

    // Send a single NoteOn on output 0 and drain it back out.
    CHECK(api->send_midi(0, 0x90, 60, 100));

    OpMidiMessage buf[4] {};
    auto n = op::sim::drain_outgoing_midi(0, buf, 4);
    CHECK(n == 1);
    CHECK(buf[0].status == 0x90);
    CHECK(buf[0].data1 == 60);
    CHECK(buf[0].data2 == 100);
    // No input is behind a message a mode sends, so it carries the marker that
    // says so. Which output holds it is what names its destination.
    CHECK(buf[0].port == 0xFF);
    CHECK(buf[0].length == 3);

    // The queue is empty after the drain.
    auto n2 = op::sim::drain_outgoing_midi(0, buf, 4);
    CHECK(n2 == 0);
}

TEST_CASE("the tick accumulates the microseconds it is advanced by") {
    op::sim::reset_state();
    op::sim::advance_tick(20833);
    CHECK(op::sim::get_api()->get_tick() == 20833u);

    op::sim::advance_tick(10);
    CHECK(op::sim::get_api()->get_tick() == 20843u);
}

TEST_CASE("a draw lands in the framebuffer that is captured") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();

    // Draw a 10x10 rectangle at the top-left. Top edge lives on page 0,
    // so the top-row pixels set bit 0 of columns 0..9 (byte value 0x01).
    api->draw_rect(0, 0, 10, 10);

    std::uint8_t fb[op::sim::kFramebufferBytes] {};
    op::sim::capture_framebuffer(fb);

    // At minimum, column 0 of page 0 must be non-zero (top-left corner pixel).
    CHECK(fb[0] != 0);
    // And column 1 of page 0 must be non-zero (top horizontal line).
    CHECK(fb[1] != 0);
    // The bottom-right corner (page 7, column 127) is well outside the 10x10
    // rect, so it stays empty.
    CHECK(fb[7 * 128 + 127] == 0);
}

TEST_CASE("a seeded file reads back through storage") {
    op::sim::reset_state();
    op::sim::set_current_mode_name("demo");

    const std::uint8_t data[4] = {1, 2, 3, 4};
    op::sim::seed_extras_file("demo", "test.scl", data, 4);

    // Read back the contents via storage_read (uses /extras/<mode>/ scope).
    std::uint8_t read_buf[8] {};
    auto bytes = op::sim::get_api()->storage_read("test.scl", read_buf, 8);
    CHECK(bytes == 4);
    CHECK(read_buf[0] == 1);
    CHECK(read_buf[3] == 4);
}

TEST_CASE("a picked filename is empty until one is picked") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();
    char buf[64]    = {'X', 'X', 'X', 'X'};
    auto n          = api->get_param_filename(0, buf, sizeof(buf));
    CHECK(n == 0);
    CHECK(buf[0] == '\0');
}

TEST_CASE("a picked filename reads back") {
    op::sim::reset_state();
    op::sim::pick_filename(3, "song.mid");

    const auto* api = op::sim::get_api();
    char buf[64] {};
    auto n = api->get_param_filename(3, buf, sizeof(buf));
    CHECK(n == 8);  // strlen("song.mid")
    CHECK(std::string_view(buf) == "song.mid");
}

TEST_CASE("each param slot holds its own picked filename") {
    op::sim::reset_state();
    op::sim::pick_filename(0, "alpha.mid");
    op::sim::pick_filename(1, "beta.mid");

    const auto* api = op::sim::get_api();
    char buf0[64] {}, buf1[64] {};
    api->get_param_filename(0, buf0, sizeof(buf0));
    api->get_param_filename(1, buf1, sizeof(buf1));
    CHECK(std::string_view(buf0) == "alpha.mid");
    CHECK(std::string_view(buf1) == "beta.mid");
}

TEST_CASE("a picked filename longer than the buffer is truncated and terminated") {
    op::sim::reset_state();
    op::sim::pick_filename(0, "abcdefghij");  // 10 chars

    const auto* api = op::sim::get_api();
    char buf[5] {};  // room for 4 chars + NUL
    auto n = api->get_param_filename(0, buf, sizeof(buf));
    CHECK(n == 4);
    CHECK(std::string_view(buf) == "abcd");
    CHECK(buf[4] == '\0');
}

TEST_CASE("a picked filename read with no buffer to read into fails") {
    op::sim::reset_state();
    op::sim::pick_filename(0, "x.mid");

    const auto* api = op::sim::get_api();
    CHECK(api->get_param_filename(0, nullptr, 64) == -1);

    char buf[8] {};
    CHECK(api->get_param_filename(0, buf, 0) == -1);
}

TEST_CASE("clearing a picked filename empties the slot") {
    op::sim::reset_state();
    op::sim::pick_filename(0, "filled.mid");
    op::sim::pick_filename(0, nullptr);  // clear

    const auto* api = op::sim::get_api();
    char buf[64]    = {'Y'};
    auto n          = api->get_param_filename(0, buf, sizeof(buf));
    CHECK(n == 0);
    CHECK(buf[0] == '\0');
}

TEST_CASE("the clock state reads back what it is set to") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();

    // Default after reset is kClockStateActive (0).
    CHECK(api->get_clock_state() == 0);

    op::sim::set_clock_state(1);  // kClockStateNoClock
    CHECK(api->get_clock_state() == 1);

    op::sim::set_clock_state(2);  // kClockStateMode
    CHECK(api->get_clock_state() == 2);
}

TEST_CASE("the pulse count reads back in pulses and in beats") {
    // A beat is 24 pulses, so the two setters reach the same counter.
    op::sim::reset_state();
    op::sim::set_beat_position(2.5f);
    CHECK(op::sim::get_api()->get_pulse_count() == 60u);  // 2.5 * 24

    // Direct pulse-count setter:
    op::sim::set_pulse_count(123u);
    CHECK(op::sim::get_api()->get_pulse_count() == 123u);
}

TEST_CASE("an output taken away refuses a send") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();

    // Every output is present after reset.
    CHECK(api->output_active(1));

    op::sim::set_output_active(1, false);
    CHECK_FALSE(api->output_active(1));

    // A send to the absent output is refused, and nothing drains from it.
    CHECK_FALSE(api->send_midi(1, 0x90, 60, 100));
    OpMidiMessage buf[4] {};
    CHECK(op::sim::drain_outgoing_midi(1, buf, 4) == 0);

    // An output still present is unaffected.
    CHECK(api->send_midi(0, 0x90, 60, 100));
    CHECK(op::sim::drain_outgoing_midi(0, buf, 4) == 1);
}

TEST_CASE("the last requested clock period reads back") {
    op::sim::reset_state();
    const auto* api = op::sim::get_api();

    // Nothing has been asked for yet.
    CHECK(op::sim::get_last_clock_period_us() == 0u);

    // 120 BPM at 24 PPQ is 20833 us per pulse.
    api->set_clock_period_us(20833);
    CHECK(op::sim::get_last_clock_period_us() == 20833u);

    // A period of 0 turns the clock off and is recorded as such.
    api->set_clock_period_us(0);
    CHECK(op::sim::get_last_clock_period_us() == 0u);
}

TEST_CASE("declared param defaults are seeded into their slots") {
    op::sim::reset_state();

    // A scalar, a Section that holds no slot, then a Range over two slots.
    ::op::modes::ParamSpec specs[3] {};
    specs[0].kind          = static_cast<std::uint8_t>(::op::modes::ParamKind::Numeric);
    specs[0].default_value = 64;  // value_slot_count 0 counts as one slot
    specs[1].kind          = static_cast<std::uint8_t>(::op::modes::ParamKind::Section);
    specs[2].kind             = static_cast<std::uint8_t>(::op::modes::ParamKind::Range);
    specs[2].value_slot_count = 2;
    specs[2].default_value    = 10;   // low slot
    specs[2].max_value        = 100;  // high slot

    op::sim::apply_param_defaults(specs, 3);

    const auto* api = op::sim::get_api();
    // The Section consumed no slot, so the Range lands right after the scalar.
    CHECK(api->get_param_value(0) == 64);   // scalar default
    CHECK(api->get_param_value(1) == 10);   // Range low
    CHECK(api->get_param_value(2) == 100);  // Range high
}
