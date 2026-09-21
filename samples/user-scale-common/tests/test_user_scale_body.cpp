// The body both User Scale modes share, doctest behavior suite.
//
// The screen, the gestures and the note mapping all live in the shared body, so
// they are covered here once, against the mode next to this file. Each shell
// carries a suite of its own for the part that is its own.
//
// The mode is driven through the SDK sim harness and read back the way a player
// meets it, through the framebuffer and the retuned note.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <operator_sdk_sim.h>
#include <operator_sdk.h>

#include "../user_scale_mode.h"

#include <cstdint>

OP_MODE_UNDER_TEST();

namespace us = op::samples::user_scale;

namespace {

// The scale a CurrentScale of kCustomScaleMarker reads out of CustomScale.
constexpr int32_t kCustomScale = us::kCustomScaleMarker;

constexpr int32_t kMajor      = 1;
constexpr int32_t kChromatic  = 0;
constexpr int32_t kWholetone  = 22;
constexpr int32_t kPentaMajor = 12;
constexpr int32_t kPentaMinor = 13;

// The major scale packed as the twelve cells, which an edit starts from.
constexpr int32_t kMajorMask = 0xAB5;

void setup_mode() {
    op::sim::reset_state();
    // The load-time defaults land before the mode boots, so a fresh init reads
    // CurrentScale as Major.
    op::sim::apply_param_defaults(kParams, kParamCount);
    op::sim::set_current_mode_name("user_scale");
    mode_init(op::sim::get_api());
}

OpMidiMessage press_note(uint8_t note, uint8_t port = 0) {
    OpMidiMessage message {
        /*status*/ 0x90,
        /*data1*/ note,
        /*data2*/ 100,
        /*port*/ port,
        /*length*/ 3,
    };
    mode_process(&message, 1, op::sim::get_api()->get_tick());
    return message;
}

OpMidiMessage release_note(uint8_t note, uint8_t port = 0) {
    OpMidiMessage message {
        /*status*/ 0x80,
        /*data1*/ note,
        /*data2*/ 64,
        /*port*/ port,
        /*length*/ 3,
    };
    mode_process(&message, 1, op::sim::get_api()->get_tick());
    return message;
}

// The key goes down and comes back up, so the next key to fold onto the same note
// meets a mode with nothing held and sounds it afresh.
OpMidiMessage play_note(uint8_t note) {
    const OpMidiMessage pressed = press_note(note);
    release_note(note);
    return pressed;
}

// A message the mode takes out of the stream comes back with its status cleared.
bool suppressed(const OpMidiMessage& message) {
    return message.status == 0;
}

// The cells are rebuilt on the next dispatch, so an empty batch is enough to pick
// up a scale written straight into the params.
void reseed() {
    mode_process(nullptr, 0, op::sim::get_api()->get_tick());
}

// The mode branches on the gesture type alone, and the active input map decides
// which encoder and which press produce each one.
void scroll(int16_t value) {
    op::sim::ui_gesture(kEncoderLeft, op::Gesture::Rotate, op::GestureType::Scroll, value);
}
void change(int16_t value) {
    op::sim::ui_gesture(kEncoderRight, op::Gesture::Rotate, op::GestureType::Change, value);
}
void enter() {
    op::sim::ui_gesture(kEncoderRight, op::Gesture::ShortPress, op::GestureType::Enter, 0);
}
void back() {
    op::sim::ui_gesture(kEncoderLeft, op::Gesture::ShortPress, op::GestureType::Back, 0);
}

using Framebuffer = uint8_t[op::sim::kFramebufferBytes];

void render(uint8_t framebuffer[op::sim::kFramebufferBytes]) {
    mode_ui_render();
    op::sim::capture_framebuffer(framebuffer);
}

uint32_t lit_pixels(const uint8_t* framebuffer, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint32_t count = 0;
    for (uint16_t row = y; row < y + h && row < 64; ++row) {
        const uint16_t page = row / 8;
        const uint8_t bit   = 1u << (row % 8);
        for (uint16_t column = x; column < x + w && column < 128; ++column) {
            if (framebuffer[page * 128 + column] & bit) ++count;
        }
    }
    return count;
}

bool same_pixels(const uint8_t* first, const uint8_t* second) {
    for (std::size_t i = 0; i < op::sim::kFramebufferBytes; ++i) {
        if (first[i] != second[i]) return false;
    }
    return true;
}

bool same_region(
    const uint8_t* first, const uint8_t* second, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    for (uint16_t row = y; row < y + h; ++row) {
        const uint16_t page = row / 8;
        const uint8_t bit   = 1u << (row % 8);
        for (uint16_t column = x; column < x + w; ++column) {
            const bool a = (first[page * 128 + column] & bit) != 0;
            const bool b = (second[page * 128 + column] & bit) != 0;
            if (a != b) return false;
        }
    }
    return true;
}

// The title carries an underline on the row below its glyphs while it holds focus.
// A title-cased name dips a descender or two into that row, so the run of the
// underline itself is what the count has to clear.
bool title_focused(const uint8_t* framebuffer) {
    return lit_pixels(framebuffer, 0, 16, 128, 1) > 16;
}

// The discs the keyboard draws for C and for D, from kKeyMarkers.
uint32_t key_c_disc(const uint8_t* framebuffer) {
    return lit_pixels(framebuffer, 35, 37, 4, 4);
}
uint32_t key_d_disc(const uint8_t* framebuffer) {
    return lit_pixels(framebuffer, 44, 37, 4, 4);
}

}  // namespace

// ---------------------------------------------------------------------------
// The scale the params name
// ---------------------------------------------------------------------------

TEST_CASE("the notes come out in the scale CurrentScale names") {
    setup_mode();
    const auto* api = op::sim::get_api();

    api->set_param_value(us::kCurrentScaleSlot, kMajor);
    reseed();
    constexpr bool kInMajor[12] = {
        true, false, true, false, true, true, false, true, false, true, false, true,
    };
    for (uint8_t note = 60; note < 72; ++note) {
        CHECK(kInMajor[play_note(note).data1 % 12]);
    }

    api->set_param_value(us::kCurrentScaleSlot, kWholetone);
    reseed();
    constexpr bool kInWholetone[12] = {
        true, false, true, false, true, false, true, false, true, false, true, false,
    };
    for (uint8_t note = 60; note < 72; ++note) {
        CHECK(kInWholetone[play_note(note).data1 % 12]);
    }
    mode_destroy();
}

TEST_CASE("a scale the player drew is read out of CustomScale") {
    setup_mode();
    const auto* api = op::sim::get_api();

    api->set_param_value(us::kCurrentScaleSlot, kCustomScale);
    api->set_param_value(us::kCustomScaleSlot, 0x089);  // C, D# and G
    reseed();

    constexpr bool kEnabled[12] = {
        true, false, false, true, false, false, false, true, false, false, false, false,
    };
    for (uint8_t note = 60; note < 72; ++note) {
        CHECK(kEnabled[play_note(note).data1 % 12]);
    }
    mode_destroy();
}

TEST_CASE("init opens on the scale the params were left holding") {
    setup_mode();  // no param writes, so the defaults stand

    constexpr bool kInMajor[12] = {
        true, false, true, false, true, true, false, true, false, true, false, true,
    };
    for (uint8_t note = 60; note < 72; ++note) {
        CHECK(kInMajor[play_note(note).data1 % 12]);
    }
    mode_destroy();
}

TEST_CASE("the params are read fresh on every dispatch") {
    setup_mode();
    const auto* api = op::sim::get_api();

    api->set_param_value(us::kCurrentScaleSlot, kPentaMinor);
    reseed();

    api->set_param_value(us::kTransposeSlot, +5);
    CHECK(play_note(60).data1 == 65);
    api->set_param_value(us::kTransposeSlot, -3);
    CHECK(play_note(60).data1 == 57);

    api->set_param_value(us::kTransposeSlot, 0);
    api->set_param_value(us::kCurrentScaleSlot, kChromatic);
    CHECK(play_note(61).data1 == 61);  // C# is in the scale
    api->set_param_value(us::kCurrentScaleSlot, kWholetone);
    CHECK(play_note(61).data1 == 62);  // C# rounds up to D
    mode_destroy();
}

TEST_CASE("the notes come out in a pentatonic minor scale") {
    setup_mode();
    op::sim::get_api()->set_param_value(us::kCurrentScaleSlot, kPentaMinor);
    reseed();

    constexpr bool kInPentaMinor[12] = {
        true, false, false, true, false, true, false, true, false, false, true, false,
    };
    for (uint8_t note = 60; note < 72; ++note) {
        CHECK(kInPentaMinor[play_note(note).data1 % 12]);
    }
    mode_destroy();
}

// ---------------------------------------------------------------------------
// Keys that fold onto one note
// ---------------------------------------------------------------------------

TEST_CASE("two keys folding onto one note sound it once and hold it") {
    setup_mode();  // Major with a Root of C, so C# and D both sound as D

    CHECK(press_note(61).data1 == 62);
    CHECK(suppressed(press_note(62)));

    CHECK(suppressed(release_note(61)));

    const auto last_off = release_note(62);
    CHECK_FALSE(suppressed(last_off));
    CHECK(last_off.data1 == 62);
    mode_destroy();
}

TEST_CASE("the fold holds whichever of the two keys lifts first") {
    setup_mode();

    press_note(61);
    press_note(62);

    // The note stops on the last lift, whichever of the two keys that turns out to be.
    CHECK(suppressed(release_note(62)));

    const auto last_off = release_note(61);
    CHECK_FALSE(suppressed(last_off));
    CHECK(last_off.data1 == 62);
    mode_destroy();
}

TEST_CASE("three keys folding in a pentatonic scale hold the note to the last release") {
    setup_mode();
    op::sim::get_api()->set_param_value(us::kCurrentScaleSlot, kPentaMajor);
    reseed();

    // F, F# and G all round up to the G the scale holds.
    CHECK(press_note(65).data1 == 67);
    CHECK(suppressed(press_note(66)));
    CHECK(suppressed(press_note(67)));

    CHECK(suppressed(release_note(66)));
    CHECK(suppressed(release_note(65)));

    const auto last_off = release_note(67);
    CHECK_FALSE(suppressed(last_off));
    CHECK(last_off.data1 == 67);
    mode_destroy();
}

TEST_CASE("a note-off for a key that was never pressed still comes out mapped") {
    setup_mode();

    const auto released = release_note(61);
    CHECK_FALSE(suppressed(released));
    CHECK(released.data1 == 62);
    mode_destroy();
}

TEST_CASE("a Root change under a held key leaves the release on the pitch that sounded") {
    setup_mode();
    CHECK(press_note(61).data1 == 62);

    op::sim::get_api()->set_param_value(us::kRootSlot, 2);  // rooted on D

    const auto released = release_note(61);
    CHECK_FALSE(suppressed(released));
    CHECK(released.data1 == 62);

    // The Root move did take, so the same key pressed afresh lands elsewhere.
    CHECK(press_note(61).data1 == 61);
    mode_destroy();
}

TEST_CASE("the same note held on two inputs sounds and stops on each of them") {
    setup_mode();

    CHECK(press_note(61, 0).data1 == 62);
    CHECK(press_note(61, 1).data1 == 62);

    const auto first_off = release_note(61, 0);
    CHECK_FALSE(suppressed(first_off));
    CHECK(first_off.data1 == 62);

    const auto second_off = release_note(61, 1);
    CHECK_FALSE(suppressed(second_off));
    CHECK(second_off.data1 == 62);
    mode_destroy();
}

// ---------------------------------------------------------------------------
// The screen
// ---------------------------------------------------------------------------

TEST_CASE("the screen opens on the title and the keyboard") {
    setup_mode();
    Framebuffer framebuffer {};
    render(framebuffer);

    CHECK(lit_pixels(framebuffer, 0, 3, 128, 13) > 0);     // the title
    CHECK(lit_pixels(framebuffer, 32, 22, 64, 22) > 100);  // the keyboard outline
    mode_destroy();
}

TEST_CASE("the title takes focus with an underline and holds its place") {
    setup_mode();
    Framebuffer unfocused {};
    render(unfocused);

    scroll(-1);  // keyboard -> title
    Framebuffer focused {};
    render(focused);

    CHECK(same_region(unfocused, focused, 0, 3, 128, 13));  // the glyphs stay put
    CHECK(title_focused(focused));
    CHECK_FALSE(title_focused(unfocused));
    mode_destroy();
}

TEST_CASE("the bottom row keeps its labels where they are") {
    setup_mode();
    const auto* api = op::sim::get_api();

    Framebuffer keyboard_focus {};
    render(keyboard_focus);

    scroll(+1);  // keyboard -> Root
    Framebuffer root_focus {};
    render(root_focus);

    // The Root label holds its columns and the caret lights the gutter to its left.
    CHECK(same_region(keyboard_focus, root_focus, 10, 52, 47, 8));
    CHECK(lit_pixels(root_focus, 5, 52, 5, 8) > lit_pixels(keyboard_focus, 5, 52, 5, 8));

    scroll(+1);  // Root -> Transpose
    Framebuffer transpose_focus {};
    render(transpose_focus);
    CHECK(lit_pixels(transpose_focus, 59, 52, 5, 8) > lit_pixels(root_focus, 59, 52, 5, 8));

    // The Transpose value is drawn from the right, so a value that grows a
    // character reaches further left and leaves the label where it was.
    api->set_param_value(us::kTransposeSlot, 0);
    Framebuffer two_chars {};
    render(two_chars);

    api->set_param_value(us::kTransposeSlot, 36);
    Framebuffer three_chars {};
    render(three_chars);

    CHECK(same_region(two_chars, three_chars, 58, 52, 42, 8));  // the label holds
    CHECK(lit_pixels(two_chars, 112, 52, 6, 8) > 0);            // both end at 118
    CHECK(lit_pixels(three_chars, 112, 52, 6, 8) > 0);
    CHECK(lit_pixels(two_chars, 118, 52, 10, 8) == 0);
    CHECK(lit_pixels(three_chars, 118, 52, 10, 8) == 0);
    CHECK(lit_pixels(two_chars, 100, 52, 6, 8) == 0);   // "+0" starts at 106
    CHECK(lit_pixels(three_chars, 100, 52, 6, 8) > 0);  // "+36" starts at 100
    mode_destroy();
}

// ---------------------------------------------------------------------------
// Moving around and editing
// ---------------------------------------------------------------------------

TEST_CASE("Scroll moves between the fields and comes round again") {
    setup_mode();

    scroll(-1);  // keyboard -> title
    Framebuffer title {};
    render(title);
    REQUIRE(title_focused(title));

    scroll(-1);  // title -> Transpose, round the end
    Framebuffer transpose {};
    render(transpose);
    CHECK_FALSE(title_focused(transpose));
    CHECK(lit_pixels(transpose, 59, 52, 5, 8) > 0);  // the Transpose caret

    scroll(+1);  // Transpose -> title, round the end
    Framebuffer back_to_title {};
    render(back_to_title);
    CHECK(title_focused(back_to_title));
    mode_destroy();
}

TEST_CASE("Change moves the key cursor and comes round again") {
    setup_mode();  // the screen opens on the keyboard, cursor on C

    Framebuffer at_c {};
    render(at_c);

    change(+1);
    Framebuffer at_c_sharp {};
    render(at_c_sharp);
    CHECK_FALSE(same_pixels(at_c, at_c_sharp));  // the cursor moved

    for (int i = 0; i < 11; ++i) change(+1);  // twelve steps in all
    Framebuffer full_turn {};
    render(full_turn);
    CHECK(same_pixels(at_c, full_turn));

    // One step down from C lands on B, which is also where eleven steps up land.
    change(-1);
    Framebuffer at_b_stepping_down {};
    render(at_b_stepping_down);

    change(+1);  // back to C
    for (int i = 0; i < 11; ++i) change(+1);
    Framebuffer at_b_stepping_up {};
    render(at_b_stepping_up);
    CHECK(same_pixels(at_b_stepping_down, at_b_stepping_up));
    mode_destroy();
}

TEST_CASE("Change on the title cycles the scale") {
    setup_mode();
    const auto* api = op::sim::get_api();
    api->set_param_value(us::kCurrentScaleSlot, kMajor);

    scroll(-1);  // keyboard -> title
    change(+1);

    CHECK(api->get_param_value(us::kCurrentScaleSlot) != kMajor);
    mode_destroy();
}

TEST_CASE("editing a preset copies it into the custom scale") {
    setup_mode();
    const auto* api = op::sim::get_api();
    api->set_param_value(us::kCurrentScaleSlot, kMajor);
    reseed();

    enter();  // the cursor opens on C, so C flips

    CHECK(api->get_param_value(us::kCurrentScaleSlot) == kCustomScale);
    CHECK(api->get_param_value(us::kCustomScaleSlot) == (kMajorMask ^ 0x001));
    mode_destroy();
}

TEST_CASE("a custom scale survives a cycle away and back") {
    setup_mode();
    const auto* api = op::sim::get_api();
    api->set_param_value(us::kCurrentScaleSlot, kMajor);
    reseed();

    enter();  // flips C, which moves the scale to Custom
    const int32_t edited = api->get_param_value(us::kCustomScaleSlot);
    REQUIRE(api->get_param_value(us::kCurrentScaleSlot) == kCustomScale);

    scroll(-1);  // keyboard -> title
    change(+1);  // Custom -> the first preset
    change(-1);  // and back to Custom
    REQUIRE(api->get_param_value(us::kCurrentScaleSlot) == kCustomScale);
    CHECK(api->get_param_value(us::kCustomScaleSlot) == edited);

    // The scale that plays is the one that was edited, so a C lands on some other
    // degree.
    reseed();
    CHECK(play_note(60).data1 % 12 != 0);
    mode_destroy();
}

TEST_CASE("Enter puts a focused field back where it started") {
    setup_mode();
    const auto* api = op::sim::get_api();

    scroll(+1);  // keyboard -> Root
    scroll(+1);  // Root -> Transpose
    api->set_param_value(us::kTransposeSlot, 7);
    enter();
    CHECK(api->get_param_value(us::kTransposeSlot) == 0);

    scroll(-1);  // Transpose -> Root
    api->set_param_value(us::kRootSlot, 5);
    enter();
    CHECK(api->get_param_value(us::kRootSlot) == 0);
    mode_destroy();
}

// ---------------------------------------------------------------------------
// The two keyboard views
// ---------------------------------------------------------------------------

TEST_CASE("the keyboard draws the scale as it sounds") {
    setup_mode();
    const auto* api = op::sim::get_api();

    api->set_param_value(us::kCurrentScaleSlot, kCustomScale);
    api->set_param_value(us::kCustomScaleSlot, 0x001);  // the root cell alone
    api->set_param_value(us::kRootSlot, 2);             // rooted on D
    reseed();

    scroll(+1);  // off the keyboard, so the cursor ring leaves the discs alone
    Framebuffer framebuffer {};
    render(framebuffer);

    CHECK(key_d_disc(framebuffer) > 0);
    CHECK(key_c_disc(framebuffer) == 0);
    mode_destroy();
}

TEST_CASE("editing a key flips the note it draws") {
    setup_mode();
    const auto* api = op::sim::get_api();

    api->set_param_value(us::kCurrentScaleSlot, kCustomScale);
    api->set_param_value(us::kCustomScaleSlot, 0x000);
    api->set_param_value(us::kRootSlot, 2);  // rooted on D
    reseed();

    change(+1);
    change(+1);  // the cursor onto D
    enter();

    // D is the root, so the cell it draws is cell 0.
    CHECK(api->get_param_value(us::kCustomScaleSlot) == 0x001);
    mode_destroy();
}

TEST_CASE("Back turns the keyboard between the two views") {
    setup_mode();
    const auto* api = op::sim::get_api();

    api->set_param_value(us::kCurrentScaleSlot, kCustomScale);
    api->set_param_value(us::kCustomScaleSlot, 0x001);
    api->set_param_value(us::kRootSlot, 2);  // rooted on D
    reseed();

    back();      // the scale drawn from its root
    scroll(+1);  // off the keyboard, so the discs stand alone
    Framebuffer from_root {};
    render(from_root);
    CHECK(key_c_disc(from_root) > 0);
    CHECK(key_d_disc(from_root) == 0);

    scroll(-1);  // back onto the keyboard
    back();      // and the scale as it sounds again
    scroll(+1);
    Framebuffer as_it_sounds {};
    render(as_it_sounds);
    CHECK(key_d_disc(as_it_sounds) > 0);
    CHECK(key_c_disc(as_it_sounds) == 0);
    mode_destroy();
}

TEST_CASE("the playing follows Root in either keyboard view") {
    setup_mode();
    const auto* api = op::sim::get_api();

    api->set_param_value(us::kCurrentScaleSlot, kCustomScale);
    api->set_param_value(us::kCustomScaleSlot, 0x001);  // the root cell alone
    api->set_param_value(us::kRootSlot, 2);             // rooted on D
    reseed();

    back();                                // the scale drawn from its root
    CHECK(play_note(60).data1 % 12 == 2);  // a C still sounds as a D
    mode_destroy();
}

TEST_CASE("with the keyboard drawn from its root an edit flips the key itself") {
    setup_mode();
    const auto* api = op::sim::get_api();

    api->set_param_value(us::kCurrentScaleSlot, kCustomScale);
    api->set_param_value(us::kCustomScaleSlot, 0x000);
    api->set_param_value(us::kRootSlot, 2);  // rooted on D
    reseed();

    back();  // the scale drawn from its root
    change(+1);
    change(+1);  // the cursor onto D
    enter();

    // The key stands for itself in this view, so D is cell 2.
    CHECK(api->get_param_value(us::kCustomScaleSlot) == 0x004);
    mode_destroy();
}
