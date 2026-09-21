// Quantizer sample mode.
//
// Snaps the timing of notes to a grid. A note played a little late is held back
// and let go on the next grid point, so a loose part locks to the beat.
//
// Only the start of a note is moved. A release passes straight through, which is
// what keeps a note the length it was played and keeps it from being left
// sounding.
//
// A held note leaves the batch it arrived in and is sent afresh at the grid
// point, so the mode both transforms what passes through and plays notes of its
// own.

#include <operator_sdk.h>
#include <operator_sdk/draw.h>
#include <operator_sdk/params.h>
#include <operator_sdk/text.h>

#include <bit>
#include <cstdint>

// --- Parameters --------------------------------------------------------------
//
// The Division options carry the spaces they are drawn with, so the same strings
// serve the param page and the mode's own screen.

inline constexpr op::params::Enum Division {
    .name     = "Division",
    .options  = {"1", "1 / 2", "1 / 4", "1 / 8", "1 / 16", "1 / 32", "1 / 64"},
    .default_ = 2, // the quarter-note grid
};

inline constexpr op::params::Bool Triplet {
    .name     = "Triplet",
    .default_ = false,
};

OP_MODE_PARAMS(Division, Triplet);

namespace {

// The Division options, in the order they are declared.
constexpr int32_t kWhole         = 0;
constexpr int32_t kHalf          = 1;
constexpr int32_t kQuarter       = 2;
constexpr int32_t kEighth        = 3;
constexpr int32_t kSixteenth     = 4;
constexpr int32_t kThirtySecond  = 5;
constexpr int32_t kSixtyFourth   = 6;
constexpr int32_t kDivisionCount = 7;

// How many grid points fall in a beat, as a fraction, since a whole note spans
// four beats and a sixty-fourth spans a sixteenth of one. Triplets put three grid
// points where two would go, which is the second half of the table.
//
// The engine wants floor(beat * 2 * points_per_beat), so the numbers are kept as
// a multiplier and a divisor and the whole thing stays in integers.
struct GridScale {
    uint32_t multiplier;
    uint32_t divisor;
};
constexpr GridScale kGrid[2 * kDivisionCount] = {
    {1,  2}, // whole
    {1,  1}, // half
    {2,  1}, // quarter
    {4,  1}, // eighth
    {8,  1}, // sixteenth
    {16, 1}, // thirty-second
    {32, 1}, // sixty-fourth
    {3,  4}, // whole triplet
    {3,  2}, // half triplet
    {3,  1}, // quarter triplet
    {6,  1}, // eighth triplet
    {12, 1}, // sixteenth triplet
    {24, 1}, // thirty-second triplet
    {48, 1}, // sixty-fourth triplet
};

// Unused output argument, since an output mode reaches the output it was loaded
// on.
constexpr uint8_t kAnyOutput = 0;

// A note waiting for its grid point. A status of 0 means the slot is free.
struct Waiting {
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    uint8_t port;        // the input it arrived on, which is what matches a release to it
    int32_t grid_point;  // the one it is waiting for
};

// Eight notes can wait at once, which covers a chord struck inside one window.
constexpr uint8_t kWaitingCapacity = 8;

Waiting g_waiting[kWaitingCapacity] = {};
int32_t g_last_grid_scale           = -1;

// The pulse the grid is measured from. The clock's pulse count runs on from
// whenever the device started, so on its own it says nothing about where the bar
// is. Start and Stop both re-anchor it, since MIDI carries no pause and the gear
// that sends these rewinds on either one.
uint32_t g_anchor_pulse = 0;

// floor(beat * multiplier), read straight off the bits of the float so the mode
// carries no math library with it.
//
// The multiply comes before the shift, which is what keeps the fraction of a beat
// the grid is decided on.
int32_t floor_beat_times(float beat, uint32_t multiplier) {
    const uint32_t bits = std::bit_cast<uint32_t>(beat);
    if (bits & 0x80000000u) return 0;  // before the anchor
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    if (exponent == 0u) return 0;     // zero, or too small to carry an exponent
    if (exponent >= 0xFFu) return 0;  // not a number the grid can use

    const uint32_t mantissa = (bits & 0x7FFFFFu) | 0x800000u;
    const int32_t shift     = static_cast<int32_t>(exponent) - 127 - 23;

    uint64_t product = static_cast<uint64_t>(mantissa) * multiplier;
    if (shift >= 0) {
        product <<= shift;
    } else {
        product >>= -shift;
    }
    if (product > 0x7FFFFFFFull) return 0x7FFFFFFF;
    return static_cast<int32_t>(product);
}

// Which row of kGrid the live params point at.
uint8_t grid_scale() {
    int32_t division = param<Division>();
    if (division < 0 || division >= kDivisionCount) division = kQuarter;
    return division + (param<Triplet>() != 0 ? kDivisionCount : 0);
}

// Twice the grid point the beat has reached, so the low bit says which half of the
// window the beat is in and the rest says which grid point.
int32_t half_grid_at(float beat, uint8_t scale) {
    const GridScale& row = kGrid[scale];
    return floor_beat_times(beat, row.multiplier) / static_cast<int32_t>(row.divisor);
}

// The beats since the transport last anchored the grid. This keeps climbing past
// the end of the bar, which is what lets a note held in the last window of one bar
// wait for a grid point in the next.
float beats_since_anchor() {
    return static_cast<float>(op::api->get_pulse_count() - g_anchor_pulse) / 24.0f;
}

// The beats into the current bar, which is what the screen draws. Feeding this to
// half_grid_at lands the cursor inside the bar on its own.
float beats_into_bar() {
    return static_cast<float>((op::api->get_pulse_count() - g_anchor_pulse) % 96u) / 24.0f;
}

Waiting* free_slot() {
    for (uint8_t i = 0; i < kWaitingCapacity; ++i) {
        if (g_waiting[i].status == 0) return &g_waiting[i];
    }
    return nullptr;
}

// A note tapped and released inside one window never reached the grid point it was
// waiting for, so it is let go and its release goes with it.
void cancel_waiting_note(OpMidiMessage& release) {
    const uint8_t channel = release.status & 0x0F;
    for (uint8_t i = 0; i < kWaitingCapacity; ++i) {
        Waiting& note = g_waiting[i];
        if (note.status == 0) continue;
        if ((note.status & 0xF0) != 0x90 || note.data2 == 0) continue;
        if (note.port != release.port) continue;
        if ((note.status & 0x0F) != channel) continue;
        if (note.data1 != release.data1) continue;

        note.status    = 0;
        release.status = 0;
        return;
    }
}

// Send every note whose grid point has arrived.
void send_due(int32_t grid_point) {
    for (uint8_t i = 0; i < kWaitingCapacity; ++i) {
        Waiting& note = g_waiting[i];
        if (note.status == 0) continue;
        if (grid_point >= note.grid_point) {
            op::api->send_midi(kAnyOutput, note.status, note.data1, note.data2);
            note.status = 0;
        }
    }
}

// Let go of every note that was waiting, without sending it. A grid that has moved
// under a note would put it somewhere it was never played.
void clear_waiting() {
    for (uint8_t i = 0; i < kWaitingCapacity; ++i) {
        g_waiting[i].status = 0;
    }
}

}  // namespace

// --- Lifecycle ---------------------------------------------------------------

void init() {
    clear_waiting();
    g_last_grid_scale = -1;
    g_anchor_pulse    = 0;
}

void process(OpMidiMessage* messages, uint8_t count, uint32_t /*tick_us*/) {
    if (!op::api) return;

    const uint8_t scale = grid_scale();
    if (g_last_grid_scale >= 0 && scale != g_last_grid_scale) {
        clear_waiting();
    }
    g_last_grid_scale = scale;

    // Start and Stop both re-anchor the grid to now, so the notes line up with bar
    // one of whatever plays next. Continue is left alone. These are read here and
    // still pass through with everything else below.
    bool transport_moved = false;
    for (uint8_t i = 0; messages != nullptr && i < count; ++i) {
        if (messages[i].status == 0xFA || messages[i].status == 0xFC) transport_moved = true;
    }
    if (transport_moved) {
        g_anchor_pulse = op::api->get_pulse_count();
        clear_waiting();
    }

    const int32_t half_grid  = half_grid_at(beats_since_anchor(), scale);
    const int32_t grid_point = half_grid >> 1;
    const bool second_half   = (half_grid & 1) != 0;

    send_due(grid_point);

    if (count == 0 || messages == nullptr) return;

    for (uint8_t i = 0; i < count; ++i) {
        OpMidiMessage& message = messages[i];
        const uint8_t kind     = message.status & 0xF0;
        const bool is_note_on  = kind == 0x90 && message.data2 != 0;
        const bool is_note_off = kind == 0x80 || (kind == 0x90 && message.data2 == 0);

        if (is_note_off) {
            cancel_waiting_note(message);
            continue;
        }
        if (!is_note_on) continue;

        // A note in the first half of the window is already where it wants to be,
        // so it plays now.
        if (!second_half) continue;

        Waiting* slot = free_slot();
        if (slot == nullptr) {
            // Every slot is taken, so this note is let go. It costs a note where
            // playing it now would put it off the grid.
            message.status = 0;
            continue;
        }
        slot->status     = message.status;
        slot->data1      = message.data1;
        slot->data2      = message.data2;
        slot->port       = message.port;
        slot->grid_point = grid_point + 1;

        // It leaves the batch here and is sent again at the grid point.
        message.status = 0;
    }
}

void destroy() {
    clear_waiting();
    g_last_grid_scale = -1;
    g_anchor_pulse    = 0;
}

// --- The screen --------------------------------------------------------------
//
// The Division in large text, a bar marked with the grid and swept by a cursor on
// the beat, and whether Triplet is on.

namespace {

constexpr uint16_t kScreenWidth = 128;
constexpr uint16_t kDivisionY   = 6;
constexpr uint16_t kBarX        = 4;
constexpr uint16_t kBarY        = 25;
constexpr uint16_t kBarWidth    = 120;
constexpr uint16_t kBarHeight   = 4;
constexpr uint16_t kTripletY    = 38;

// The grid points in a bar of four, which is what the bar is divided into. Whole
// triplets run three to two bars, and the one that falls in this bar is drawn.
constexpr uint8_t kPointsPerBar[kDivisionCount]        = {1, 2, 4, 8, 16, 32, 64};
constexpr uint8_t kPointsPerBarTriplet[kDivisionCount] = {2, 3, 6, 12, 24, 48, 96};

const char* division_label(int32_t division) {
    switch (division) {
        case kWhole: return "1";
        case kHalf: return "1 / 2";
        case kQuarter: return "1 / 4";
        case kEighth: return "1 / 8";
        case kSixteenth: return "1 / 16";
        case kThirtySecond: return "1 / 32";
        case kSixtyFourth: return "1 / 64";
        default: return "?";
    }
}

// Where a grid point sits along the bar.
uint16_t point_x(uint8_t point, uint8_t points_per_bar) {
    return kBarX + (kBarWidth * point) / points_per_bar;
}

}  // namespace

void ui_render() {
    if (!op::api) return;

    // The canvas arrives cleared each frame.

    int32_t division = param<Division>();
    if (division < 0 || division >= kDivisionCount) division = kQuarter;
    const char* label = division_label(division);
    op::draw::draw_text_large(op::sdk::text::large_align_center_x(label, kScreenWidth), kDivisionY, label);

    const bool triplet   = param<Triplet>() != 0;
    const uint8_t points = triplet ? kPointsPerBarTriplet[division] : kPointsPerBar[division];

    op::draw::draw_rect(kBarX, kBarY, kBarWidth, kBarHeight);

    // The bar's own outline draws the grid points at each end, so the marks go
    // between them.
    for (uint8_t point = 1; point < points; ++point) {
        op::draw::fill_rect(point_x(point, points), kBarY, /*w=*/1, kBarHeight, /*on=*/true);
    }

    // The cursor fills the grid point the beat is on.
    const int32_t cursor = half_grid_at(beats_into_bar(), grid_scale()) >> 1;
    const uint16_t start = point_x(cursor, points);
    const uint16_t end   = point_x(cursor + 1, points);
    if (end > start) {
        op::draw::fill_rect(start, kBarY, end - start, kBarHeight, /*on=*/true);
    }

    const char* triplet_label = triplet ? "Triplet: Yes" : "Triplet: No";
    const uint16_t width      = op::sdk::text::text_width(triplet_label);
    op::draw::draw_text((kScreenWidth - width) / 2, kTripletY, triplet_label);
}

// The mode branches on the gesture type alone, and the active input map decides
// which encoder and which press produce each one.
void ui_gesture(uint8_t /*encoder_id*/,
                op::Gesture /*gesture*/,
                op::GestureType gesture_type,
                int16_t value) {
    if (!op::api) return;

    constexpr auto division_slot = param_slot_info<Division>().slot;
    constexpr auto triplet_slot  = param_slot_info<Triplet>().slot;

    if (gesture_type == op::GestureType::Scroll) {
        // The Division cycles round, so scrolling past either end comes back.
        const int32_t current = op::api->get_param_value(division_slot);
        const int32_t next    = ((current + value) % kDivisionCount + kDivisionCount) % kDivisionCount;
        op::api->set_param_value(division_slot, next);

    } else if (gesture_type == op::GestureType::Change) {
        if (value == 0) return;
        const int32_t current = op::api->get_param_value(triplet_slot);
        op::api->set_param_value(triplet_slot, current != 0 ? 0 : 1);
    }
}

OP_MODE_REGISTER(init, process, destroy);
OP_MODE_REGISTER_UI(ui_render, ui_gesture);
