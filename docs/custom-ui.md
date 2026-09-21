# Custom UI {#custom-ui}

When a param page is not enough, a per-step visualization, a keyboard, a transport button row, a mode can draw its own screen and handle input directly. Make a custom UI only when declarative parameters (see @ref parameters) cannot completely express what you need.

## When to go custom

| Stay declarative when | Go custom when |
|-----------------------|----------------|
| All your state fits Numeric / Bool / Enum / Range / NoteRange / FilePicker params | You need per-note or per-step visual feedback |
| The user only tunes values | The button set changes with your mode's state |
| No per-step visual feedback is needed | You render a bar, graph, keyboard, or grid |
| A standard gesture map is fine | You need to override the default rotate-edits-selected behavior |

## Custom UI Hooks

A custom UI adds two functions, registered with @ref OP_MODE_REGISTER_UI. Both run on Core 0. Their signatures are in @ref mode-lifecycle.

- `ui_render` draws the screen, once per display frame.
- `ui_gesture` receives one input event at a time.

## Drawing

Every pixel on the 128x64 OLED is drawn through one of the @ref OperatorApi functions below. Instead of having direct access to the display framebuffer, you use the API and the firmware renders into the display itself.

| Function | Purpose |
|----------|---------|
| @ref OperatorApi::draw_rect "draw_rect(x, y, w, h)" | 1-pixel outline rectangle |
| @ref OperatorApi::fill_rect "fill_rect(x, y, w, h, on)" | Fill with ON (white) or OFF (black) pixels |
| @ref OperatorApi::draw_text "draw_text(x, y, text)" | 5x7 standard font, 6px advance, 8px line height |
| @ref OperatorApi::draw_text_large "draw_text_large(x, y, text)" | 8x12 large font, 9px advance, 13px line height |
| @ref OperatorApi::draw_bitmap "draw_bitmap(x, y, data, w, h)" | Blit a packed 1-bit bitmap |
| @ref OperatorApi::set_pixel "set_pixel(x, y, on)" | Turn a single pixel ON or OFF |

Auxiliary helpers (header-only, opt-in), in `<operator_sdk/text.h>`: `text_width` and `large_text_width` (pixel width of a string), `align_center_x` and `large_align_center_x` (x-offset to center text in a box), `truncate_with_ellipsis` (safe truncate with `...`), and `word_wrap` (greedy word-wrapping).

### Fonts

Two bitmap fonts ship in firmware. There's currently no API-supported way to add or change the system-provided fonts.

| Font | Glyph | Advance | Line Height | Typical Use |
|------|-------|---------|-------------|-------------|
| Standard | 5x7 px | 6 px | 8 px | Labels, hint rows, values, menu items |
| Large | 8x12 px | 9 px | 13 px | Titles, primary readout, large numbers |

On the 128 px wide display that is 21 standard characters or 14 large characters per line before overflow. Respect those ceilings when writing UI copy.

### Bitmaps

@ref OperatorApi::draw_bitmap "draw_bitmap" blits a packed 1-bit image `w` pixels wide and `h` pixels tall, with its top-left corner at `(x, y)`. A set bit draws an ON pixel.

The data is page-addressed. The image is split into horizontal pages of 8 rows each (`ceil(h / 8)` pages). Each byte holds the 8 vertical pixels of one column within a page, least-significant bit at the top. Bytes run column by column within a page, then page by page: the byte at index `p * w + col` holds column `col` of page `p`, and its bit `r` is the pixel at row `p * 8 + r`. The buffer is exactly `w * ceil(h / 8)` bytes, and any unused high bits in the final partial page are zero.

A tiny example is a 3x3 diagonal from top-left to bottom-right. It fits in a single page (`ceil(3 / 8)` is 1), so there is one byte per column and bit 0 is the top row:

```cpp
// bit 0 (LSB) is the top row; one byte per column
static const uint8_t diagonal[3] = {
    0b00000001,   // column 0: pixel in row 0
    0b00000010,   // column 1: pixel in row 1
    0b00000100,   // column 2: pixel in row 2
};
op::api->draw_bitmap(x, y, diagonal, 3, 3);
```

For a larger image, the User Scale sample (`samples/user-scale-common/keyboard_sprites.h`) is a worked example.

### Layout zones

Your UI gets one of two layouts, chosen with `FLAGS FULLSCREEN_UI` in `op_add_mode()` (see @ref reference-build):

| Zone | Without the flag | With the flag |
|------|------------------|---------------|
| y = 0..8 | Firmware status bar | Yours |
| y = 9..63 | Your content (128x55) | Yours |
| Total | 128 x 55 content area | 128 x 64 full screen |

## Handling input

Your `ui_gesture` runs once per input event:

```cpp
void ui_gesture(uint8_t control, op::Gesture gesture, op::GestureType type, int16_t value);
```

- `control` is which physical control the event came from (0 = left, 1 = right).
- `gesture` is the raw event, an @ref op::Gesture value (`Rotate`, `ShortPress`, or `LongPress`).
- `type` is the input-mapped intent, an @ref op::GestureType value (`Scroll`, `Change`, `Enter`, or `Back`).
- `value` is the signed step for a rotation (acceleration already applied), or 0 for a press.

### Read intent, not raw controls

The recommended way is to react to `type`. Your UI then follows whatever input map the user has set, without you hard-coding which control does what:

- `Scroll` is a rotation meant as navigation. Move a cursor or selection by `value`.
- `Change` is a rotation meant as editing. Adjust the selected value by `value`.
- `Enter` is a short press meant as confirm or drill-in.
- `Back` is a short press meant as cancel or go-up.

```cpp
void ui_gesture(uint8_t control, op::Gesture gesture, op::GestureType type, int16_t value) {
    if (!op::api) return;
    if (type == op::GestureType::Scroll) {
        cursor_ += value;               // rotate to move the cursor
    } else if (type == op::GestureType::Change) {
        edit_selected(value);           // rotate to edit the selected value
    } else if (type == op::GestureType::Enter) {
        confirm();                      // short press to confirm
    }
}
```

All sample modes handle input exactly like this, editing their parameters with @ref OperatorApi::set_param_value "set_param_value" (see @ref parameters).

### Raw, map-independent input

For something like an X/Y sketch pad, ignore `type` and read `control` and `gesture` directly: left or right control, `Rotate` or `ShortPress`, and the signed `value`. Use this only when your interaction genuinely does not fit the standard intents.

### The reserved gesture

@warning The long press on the left control is reserved by the firmware as the universal escape from any custom UI back to the system menu. Your mode never sees it (the reserved @ref op::GestureType values `EnterParams` and `ExitUi` are intercepted before dispatch), so never rely on a left long-press.

## Examples

Four shipped samples cover the space:

- **User Scale** (`samples/user-scale-global/`), a fullscreen 12-cell keyboard.
- **Quantizer** (`samples/quantizer/`), a windowed UI with a large readout and a beat-synced bar.
- **Euclidean Sequencer** (`samples/euclidean-sequencer/`), a two-screen UI with an overview and a detail editor.
- **MIDI Player** (`samples/midi-player/`), a transport button row that changes with playback state.

Copy the closest one when starting a custom-UI mode. They share the conventions of `template-custom-ui/` and add the domain-specific rendering.
