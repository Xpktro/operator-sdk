#pragma once
// The body both User Scale modes share.
//
// The two differ only in whether their params are held per output or once for the
// device, so the screen, the gestures and the note mapping all live here and each
// mode is a shell around them.
//
// The params are declared per mode by the macro at the foot of this file, so the
// code here reaches them by slot. The slots below follow the order the macro
// declares them in.

#include <operator_sdk.h>
#include <operator_sdk/draw.h>
#include <operator_sdk/held_notes.h>
#include <operator_sdk/params.h>
#include <operator_sdk/text.h>

#include "keyboard_sprites.h"
#include "user_scale.h"

#include <cstddef>
#include <cstdint>

// The macro that declares the params also declares op::api, and it runs after this
// header, so the name is made visible here for the code below to reach.
namespace op {
extern const OperatorApi* api;
}

namespace op::samples::user_scale {

// These follow the order the macro declares the params in.
inline constexpr uint8_t kRootSlot         = 0;
inline constexpr uint8_t kTransposeSlot    = 1;
inline constexpr uint8_t kCurrentScaleSlot = 2;
inline constexpr uint8_t kCustomScaleSlot  = 3;

// --- The screen --------------------------------------------------------------

inline constexpr uint16_t kScreenW = 128;
inline constexpr uint16_t kScreenH = 64;

// The title, the keyboard and the Root and Transpose row, spaced down the screen.
// The keyboard is 64 wide, so it sits centered at 32.
inline constexpr uint16_t kTitleY     = 3;
inline constexpr uint16_t kKeyboardX  = 32;
inline constexpr uint16_t kKeyboardY  = 22;
inline constexpr uint16_t kBottomRowY = 52;

inline constexpr uint8_t kCellCount = 12;

// A CurrentScale of this reads the scale out of CustomScale instead.
inline constexpr uint8_t kCustomScaleMarker = kPresetCount;

// Where each key's marker is drawn, in screen coordinates. The margins are set by
// eye, so the ring sits a pixel clear of the key's bottom line and the disc two.
//
// A marker lands on a row that is not a multiple of eight, which is what has it
// blend into the key interior it is drawn over. Moving one of these rows means
// checking that still holds.
struct KeyMarker {
    uint16_t ring_x;
    uint16_t ring_y;
    uint16_t disc_x;
    uint16_t disc_y;
};

// Indexed by chromatic key 0..11 (C, C#, D, D#, E, F, F#, G, G#, A, A#, B).
inline constexpr KeyMarker kKeyMarkers[kCellCount] = {
    {34, 36, 35, 37}, // C
    {39, 27, 40, 28}, // C#
    {43, 36, 44, 37}, // D
    {48, 27, 49, 28}, // D#
    {52, 36, 53, 37}, // E
    {61, 36, 62, 37}, // F
    {66, 27, 67, 28}, // F#
    {70, 36, 71, 37}, // G
    {75, 27, 76, 28}, // G#
    {79, 36, 80, 37}, // A
    {84, 27, 85, 28}, // A#
    {88, 36, 89, 37}, // B
};

// --- What the mode remembers -------------------------------------------------
//
// The scale, the root and the transpose are all held in params. What is kept here
// is where the screen is and the mask the note mapping reads.

enum Field : uint8_t {
    kFieldPreset    = 0,
    kFieldKeyboard  = 1,
    kFieldRoot      = 2,
    kFieldTranspose = 3,
    kFieldCount     = 4,
};

inline uint8_t g_field      = kFieldKeyboard;  // the screen opens on the keyboard
inline uint8_t g_key_cursor = 0;               // the key the cursor rests on

// The scale as the screen draws it and as the note mapping reads it.
inline bool g_cells[kCellCount] = {};
inline uint16_t g_mask          = 0;

// A value no param can hold, so the first look always rebuilds the cells.
inline constexpr int32_t kNotSeededYet = -0x7FFFFFFF;

// The param values the cells were built from, so they are rebuilt only when one
// of them moves.
inline int32_t g_seeded_current_scale = kNotSeededYet;
inline int32_t g_seeded_custom_scale  = kNotSeededYet;

// How the keyboard draws the scale, which is a matter of drawing alone. On, the
// shape turns with Root, so the lit keys are the notes that sound and editing a
// key flips the note under it. Off, the shape is drawn from the root itself,
// whichever note that is.
inline bool g_visual_transpose = true;

// The keys that are down and the note each one sounded. Several keys can fold onto
// one note, so the record is what says which notes are ringing.
inline op::sdk::HeldNotes<> g_held_notes;

inline uint8_t live_root() {
    if (!op::api) return 0;
    const int32_t root = op::api->get_param_value(kRootSlot);
    return (root >= 0 && root < 12) ? root : 0;
}

// The cell a key draws and edits. With the scale drawn as it sounds, the key is
// read from the root, and otherwise it is the key itself. The screen and the edit
// go through this together, so the key that lights is the one that flips.
inline uint8_t key_to_cell(uint8_t key, uint8_t root) {
    return g_visual_transpose ? (key + 12u - root) % 12u : key;
}

// Rebuild the cells from the params. A CurrentScale that names no preset reads the
// scale out of CustomScale, which carries twelve bits.
inline void seed_cells_from_params() {
    if (!op::api) return;
    const int32_t current = op::api->get_param_value(kCurrentScaleSlot);
    if (current >= 0 && current < kPresetCount) {
        load_preset(current, g_cells);
    } else {
        const uint16_t custom = op::api->get_param_value(kCustomScaleSlot) & 0x0FFF;
        for (uint8_t i = 0; i < kCellCount; ++i) {
            g_cells[i] = ((custom >> i) & 0x1u) != 0u;
        }
    }
    g_mask                 = cells_to_mask(g_cells);
    g_seeded_current_scale = current;
    g_seeded_custom_scale  = op::api->get_param_value(kCustomScaleSlot);
}

// True when the params have moved since the cells were last built from them.
inline bool params_moved() {
    return op::api->get_param_value(kCurrentScaleSlot) != g_seeded_current_scale
        || op::api->get_param_value(kCustomScaleSlot) != g_seeded_custom_scale;
}

// Flip the note the cursor rests on. Editing a preset copies it into the custom
// scale and moves there, so the presets stay as they ship.
inline void toggle_cursor_note() {
    const uint16_t current = g_mask;
    if (is_preset(current)) {
        op::api->set_param_value(kCurrentScaleSlot, kCustomScaleMarker);
    }
    const uint8_t cell  = key_to_cell(g_key_cursor, live_root());
    const uint16_t next = (current ^ (1u << cell)) & 0x0FFF;
    op::api->set_param_value(kCustomScaleSlot, next);
    seed_cells_from_params();
}

// --- Drawing the screen ------------------------------------------------------

// The name of the scale, or Custom for one the player has made. The title takes
// focus with an underline drawn beneath it.
inline void draw_title() {
    const int32_t current = op::api->get_param_value(kCurrentScaleSlot);
    const char* name = (current >= 0 && current < kPresetCount) ? kPresets[current].name.data() : "Custom";

    const uint16_t title_x = op::sdk::text::large_align_center_x(name, kScreenW);
    op::draw::draw_text_large(title_x, kTitleY, name);

    if (g_field == kFieldPreset) {
        // The large font stands 13 tall and leaves a pixel of space after each
        // glyph, so the line is drawn a pixel short of the full advance and ends
        // where the last glyph does.
        const uint16_t advance = op::sdk::text::large_text_width(name);
        const int width        = advance > 0 ? advance - 1 : 0;
        op::draw::fill_rect(title_x, kTitleY + 13, width, 1, /*on=*/true);
    }
}

// A key in the scale carries a disc, the key under the cursor carries a ring, and
// a key that is both carries the ring with a core. The cursor shows only while the
// keyboard has focus.
inline void draw_key_markers() {
    const bool keyboard_focused = (g_field == kFieldKeyboard);
    const uint8_t root          = live_root();
    for (uint8_t key = 0; key < kCellCount; ++key) {
        const bool in_scale     = g_cells[key_to_cell(key, root)];
        const bool under_cursor = keyboard_focused && key == g_key_cursor;
        const KeyMarker marker  = kKeyMarkers[key];

        if (in_scale && under_cursor) {
            op::draw::draw_bitmap(marker.ring_x, marker.ring_y, kNoteOnCursorSprite, kNoteOnCursorW,
                                  kNoteOnCursorH);
        } else if (in_scale) {
            op::draw::draw_bitmap(marker.disc_x, marker.disc_y, kNoteOnSprite, kNoteOnW, kNoteOnH);
        } else if (under_cursor) {
            op::draw::draw_bitmap(marker.ring_x, marker.ring_y, kCursorSprite, kCursorW, kCursorH);
        }
    }
}

// The bottom row, Root on the left and Transpose on the right. Each label stays
// put and its value is drawn beside it, so a value that grows a character does not
// shift the label. The focused one is marked with a caret.
inline void draw_root_and_transpose(uint8_t root,
                                    int32_t transpose,
                                    bool root_focused,
                                    bool transpose_focused) {
    constexpr uint16_t kRootLabelX      = 10;
    constexpr uint16_t kRootValueX      = 40;
    constexpr uint16_t kTransposeLabelX = 64;
    constexpr uint16_t kTransposeRight  = kScreenW - kRootLabelX;

    op::draw::draw_text(kRootLabelX, kBottomRowY, "Root:");
    op::draw::draw_text(kRootValueX, kBottomRowY, root < 12 ? kRootNoteNames[root] : "?");
    if (root_focused) op::draw::draw_text(5, kBottomRowY, ">");

    op::draw::draw_text(kTransposeLabelX, kBottomRowY, "Trans:");

    // The transpose always carries its sign, and it reaches three characters at
    // either end of its range.
    char value[4];
    op::sdk::text::format_int(transpose, value, sizeof(value), /*force_plus=*/true);
    const uint16_t width = op::sdk::text::text_width(value);
    op::draw::draw_text(width >= kTransposeRight ? 0 : kTransposeRight - width, kBottomRowY, value);

    if (transpose_focused) {
        op::draw::draw_text(kTransposeLabelX - 5, kBottomRowY, ">");
    }
}

// --- The behavior each shell forwards to -------------------------------------

inline void impl_init() {
    seed_cells_from_params();
    g_field            = kFieldKeyboard;
    g_key_cursor       = 0;
    g_visual_transpose = true;
    g_held_notes.clear();
}

// Swallow a message. Clearing the status is what marks it consumed.
inline void suppress(OpMidiMessage& message) {
    message.status = 0;
}

// The held-note record carries the pitch each key went out on and a release is sent
// to that, so moving Root or Transpose under a held key still stops the note that is
// ringing.
inline void impl_process(OpMidiMessage* messages, uint8_t count, uint32_t /*tick_us*/) {
    if (!op::api) return;

    // The scale can also be edited from the param page.
    if (params_moved()) seed_cells_from_params();

    if (count == 0) return;

    const uint8_t root     = live_root();
    const int8_t transpose = op::api->get_param_value(kTransposeSlot);
    const uint16_t mask    = g_mask;

    for (uint8_t i = 0; i < count; ++i) {
        OpMidiMessage& message = messages[i];
        const uint8_t kind     = message.status & 0xF0;
        const uint8_t channel  = message.status & 0x0F;
        const bool is_note_on  = kind == 0x90 && message.data2 != 0;
        const bool is_note_off = kind == 0x80 || (kind == 0x90 && message.data2 == 0);

        if (is_note_on) {
            const uint8_t mapped = retune_note(message.data1, mask, root, transpose);
            if (g_held_notes.press(message.port, channel, message.data1, mapped).first_holder) {
                message.data1 = mapped;
            } else {
                suppress(message);
            }
        } else if (is_note_off) {
            const auto released = g_held_notes.release(message.port, channel, message.data1);
            if (!released.found) {
                // A key the mode never saw go down still has to stop.
                message.data1 = retune_note(message.data1, mask, root, transpose);
            } else if (released.last_holder) {
                message.data1 = released.output_note;
            } else {
                suppress(message);
            }
        }
    }
}

inline void impl_destroy() {
    for (uint8_t i = 0; i < kCellCount; ++i) g_cells[i] = false;
    g_mask                 = 0;
    g_seeded_current_scale = kNotSeededYet;
    g_seeded_custom_scale  = kNotSeededYet;
    g_field                = kFieldKeyboard;
    g_key_cursor           = 0;
    g_visual_transpose     = true;
    g_held_notes.clear();
}

inline void impl_ui_render() {
    if (!op::api) return;

    if (params_moved()) seed_cells_from_params();

    op::draw::fill_rect(0, 0, kScreenW, kScreenH, /*on=*/false);

    draw_title();

    // The keyboard first, then the markers over its key interiors.
    op::draw::draw_bitmap(kKeyboardX, kKeyboardY, kKeyboardSprite, kKeyboardW, kKeyboardH);
    draw_key_markers();

    draw_root_and_transpose(live_root(), op::api->get_param_value(kTransposeSlot),
                            /*root_focused=*/g_field == kFieldRoot,
                            /*transpose_focused=*/g_field == kFieldTranspose);
}

// The mode branches on the gesture type alone, and the active input map decides
// which encoder and which press produce each one.
//
// Scroll moves between the four fields, Change edits the one in focus, and Enter
// flips the note under the cursor on the keyboard or puts the other fields back to
// where they started. Back turns the keyboard between drawing the scale as it
// sounds and drawing it from its root.
inline void impl_ui_gesture(uint8_t /*encoder_id*/,
                            op::Gesture /*gesture*/,
                            op::GestureType gesture_type,
                            int16_t value) {
    if (!op::api) return;

    if (gesture_type == op::GestureType::Scroll) {
        g_field = ((g_field + value) % kFieldCount + kFieldCount) % kFieldCount;
        return;
    }

    if (gesture_type == op::GestureType::Back) {
        if (g_field == kFieldKeyboard) {
            g_visual_transpose = !g_visual_transpose;
        }
        return;
    }

    if (gesture_type == op::GestureType::Change) {
        switch (g_field) {
            case kFieldPreset: {
                // The presets cycle round, with the custom scale after the last of
                // them.
                const int32_t places = kPresetCount + 1;
                int32_t next         = op::api->get_param_value(kCurrentScaleSlot) + value;
                next                 = ((next % places) + places) % places;
                op::api->set_param_value(kCurrentScaleSlot, next);
                seed_cells_from_params();
                break;
            }
            case kFieldKeyboard: {
                g_key_cursor = ((g_key_cursor + value) % kCellCount + kCellCount) % kCellCount;
                break;
            }
            case kFieldRoot: {
                const int32_t next = ((op::api->get_param_value(kRootSlot) + value) % 12 + 12) % 12;
                op::api->set_param_value(kRootSlot, next);
                break;
            }
            case kFieldTranspose: {
                int32_t next = op::api->get_param_value(kTransposeSlot) + value;
                if (next < -36) next = -36;
                if (next > 36) next = 36;
                op::api->set_param_value(kTransposeSlot, next);
                break;
            }
        }
        return;
    }

    if (gesture_type == op::GestureType::Enter) {
        switch (g_field) {
            case kFieldKeyboard: toggle_cursor_note(); break;
            case kFieldPreset:
                op::api->set_param_value(kCurrentScaleSlot, 1);  // back to major
                seed_cells_from_params();
                break;
            case kFieldRoot: op::api->set_param_value(kRootSlot, 0); break;
            case kFieldTranspose: op::api->set_param_value(kTransposeSlot, 0); break;
        }
    }
}

}  // namespace op::samples::user_scale

// The params, declared per mode. The bit handed in says whether each output holds
// its own scale or the device holds one for all of them.
//
// The order here is the order the slot constants at the top of this file follow, so
// a param can be added at the end and the ones already saved still point where they
// did.
#define OP_USER_SCALE_DEFINE_PARAMS(PER_INSTANCE)                                                            \
    inline constexpr op::params::Enum Root {                                                                 \
        .name            = "Root",                                                                           \
        .options         = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"},                \
        .default_        = 0,                                                                                \
        .is_per_instance = (PER_INSTANCE),                                                                   \
    };                                                                                                       \
    inline constexpr op::params::Numeric Transpose {                                                         \
        .name            = "Transpose",                                                                      \
        .min             = -36,                                                                              \
        .max             = +36,                                                                              \
        .default_        = 0,                                                                                \
        .is_per_instance = (PER_INSTANCE),                                                                   \
    };                                                                                                       \
    inline constexpr op::params::Numeric CurrentScale {                                                      \
        .name            = "CurrentScale",                                                                   \
        .min             = 0,                                                                                \
        .max             = op::samples::user_scale::kCustomScaleMarker,                                      \
        .default_        = 1, /* Major */                                                                    \
        .is_per_instance = (PER_INSTANCE),                                                                   \
        .visible_when    = op::params::hidden(),                                                             \
    };                                                                                                       \
    inline constexpr op::params::Numeric CustomScale {                                                       \
        .name            = "CustomScale",                                                                    \
        .min             = 0,                                                                                \
        .max             = 4095, /* the twelve cells */                                                      \
        .default_        = 0,                                                                                \
        .is_per_instance = (PER_INSTANCE),                                                                   \
        .visible_when    = op::params::hidden(),                                                             \
    };                                                                                                       \
    OP_MODE_PARAMS(Root, Transpose, CurrentScale, CustomScale)
