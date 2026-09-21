#pragma once
/// @file
/// The mock device a mode is tested against.
///
/// Stands in for the hardware so a mode can be exercised in unit tests on a host
/// machine. It carries a mock OperatorApi, a virtual clock, capture queues for the
/// MIDI a mode sends, a framebuffer, and a small read-only store, so a test drives
/// a mode exactly as the device does and reads back exactly what a player would
/// see and hear.
///
/// Every test begins with reset_state(), which is what keeps one case from leaking
/// into the next.
///
/// @code
///     #include <doctest.h>
///     #include <operator_sdk_sim.h>
///
///     OP_MODE_UNDER_TEST();
///
///     TEST_CASE("the mode plays the note it is given") {
///         op::sim::reset_state();
///         mode_init(op::sim::get_api());
///
///         OpMidiMessage message {0x90, 60, 100, 0, 3};
///         mode_process(&message, 1, 0);
///
///         OpMidiMessage sent[4];
///         CHECK(op::sim::drain_outgoing_midi(0, sent, 4) == 1);
///     }
/// @endcode

// This one header brings in OperatorApi and OpMidiMessage, which is what the
// harness surface below needs. A mode's own code includes <operator_sdk.h> as
// usual.
#include <operator_sdk/abi/mode_api.h>  // OperatorApi, OpMidiMessage

#include <cstddef>
#include <cstdint>

// A forward declaration is all apply_param_defaults needs for its pointer.
namespace op::modes {
struct ParamSpec;
}  // namespace op::modes

/// @namespace op::sim
/// @brief The mock device a mode is tested against on a host machine.
namespace op::sim {

/// The size of the framebuffer, so a capture buffer can be stack-allocated.
///
/// The display is 128 columns across 8 pages of 8 rows, one bit per pixel.
constexpr std::size_t kFramebufferBytes = 1024;

// ---------------------------------------------------------------------------
// The device
// ---------------------------------------------------------------------------

/// The mock OperatorApi, ready to hand to a mode's init.
///
/// The pointer is stable for the life of the process and survives reset_state(),
/// so a fixture can hold on to it.
const OperatorApi* get_api();

/// Put the device back to how it starts: the clock at zero, the queues and the
/// framebuffer empty, every param zero, no seeded files.
///
/// Call this at the top of every test. The tick starts at zero, so a mode that
/// seeds a random sequence from it runs the same way every time.
void reset_state();

/// Seed the params with the defaults a mode declares, the way the device does
/// before it starts one.
///
/// reset_state() zeroes the params, because the harness holds no table of its own.
/// A mode whose first read expects its declared default (a Numeric that starts at
/// 64, an Enum that starts on its second option) needs this, called after
/// reset_state() and before mode_init.
///
/// Slots are walked the way the device walks them, so what is seeded lines up with
/// what get_param_value(slot) reads back. A Section holds no value and takes no
/// slot. A Range or NoteRange takes two, seeded low then high, so the pair starts
/// as a valid range.
void apply_param_defaults(const ::op::modes::ParamSpec* specs, std::uint8_t count);

// ---------------------------------------------------------------------------
// The clock
// ---------------------------------------------------------------------------

/// Move the virtual clock forward, in microseconds. api->get_tick() reads the
/// total. It only ever moves forward.
void advance_tick(std::uint32_t microseconds);

/// Set what api->get_pulse_count() reads.
///
/// The pulse count is the timing a mode works from, at 24 pulses to the beat, and
/// a mode finds the beat and the bar inside it by modulus.
void set_pulse_count(std::uint32_t pulse);

/// Set the pulse count in beats. A beat is 24 pulses, so a beat of 2.5 is a pulse
/// count of 60.
void set_beat_position(float beat);

/// Set what api->get_clock_state() reads. The value is a clock state: 0 for
/// Active (a clock is running), 1 for NoClock (idle), or 2 for Mode (the clock
/// source is a mode with no clock generator loaded).
void set_clock_state(std::uint8_t state);

/// Read the period a mode last asked for with api->set_clock_period_us().
///
/// This is how a clock-generating mode is checked: at 120 BPM it should ask for
/// 20833 microseconds. Reads 0 until a mode asks for something, as on the device.
std::uint32_t get_last_clock_period_us();

// ---------------------------------------------------------------------------
// The outputs
// ---------------------------------------------------------------------------

/// Take one output away, so a mode meets an output that is not there. Every output
/// is present after reset_state().
void set_output_active(std::uint8_t output_index, bool active);

/// Read back the MIDI a mode sent to one output, up to max_messages of it, and
/// empty the queue.
///
/// Returns how many messages were written into out.
std::uint16_t drain_outgoing_midi(std::uint8_t output_index, OpMidiMessage* out, std::uint16_t max_messages);

// ---------------------------------------------------------------------------
// The screen
// ---------------------------------------------------------------------------

/// Copy the framebuffer out, as it stands after every draw a mode has made since
/// the last reset_state().
///
/// The layout is the device's: byte page * 128 + column, lowest bit at the top row.
void capture_framebuffer(std::uint8_t out[kFramebufferBytes]);

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

/// Put a file where a mode will find it, at `/extras/<mode name>/<filename>`.
///
/// Seeding the same file twice overwrites it.
void seed_extras_file(const char* mode_name,
                      const char* filename,
                      const std::uint8_t* data,
                      std::uint32_t size);

/// Pick a file for a FilePicker param, as a player would on the device.
///
/// Storage is read-only and a mode never lists a directory, so a FilePicker mode is
/// stood up with both halves: seed_extras_file puts the contents in place, and this
/// says which file was picked. Call it after mode_init and before the first
/// mode_process, so the mode's first read sees it. A null or empty filename clears
/// the slot back to empty.
void pick_filename(std::uint8_t param_id, const char* filename);

/// Name the mode, which is what scopes its storage to `/extras/<mode name>/`.
/// Reads as "default" until it is set.
void set_current_mode_name(const char* mode_name);

}  // namespace op::sim

// ---------------------------------------------------------------------------
// Reaching the mode under test
// ---------------------------------------------------------------------------

/// Declare everything a mode exports, so a test can call it.
///
/// Put it once at the top of a test file, under the includes. It brings in the
/// lifecycle (mode_init, mode_process, mode_destroy), the screen (mode_ui_render,
/// mode_ui_gesture) and the param tables (kParams, kParamCount, kParamStrings), and
/// the linker takes only what the test calls.
///
/// It also declares op::sim::ui_gesture, which takes the scoped op::Gesture and
/// op::GestureType enums, so a test says the gesture it means and the narrowing to
/// the ABI's bytes happens in one place.
///
/// @code
///     OP_MODE_UNDER_TEST();
///
///     TEST_CASE("the mode boots") { mode_init(op::sim::get_api()); }
/// @endcode
#define OP_MODE_UNDER_TEST()                                                                                 \
    extern "C" {                                                                                             \
    /* Lifecycle */                                                                                          \
    void mode_init(const OperatorApi* api);                                                                  \
    void mode_process(OpMidiMessage* msgs, std::uint8_t count, std::uint32_t tick);                          \
    void mode_destroy();                                                                                     \
    /* The screen. A declarative mode leaves these alone, and the linker resolves                            \
     * only what a test calls. */                                                                            \
    void mode_ui_render();                                                                                   \
    void mode_ui_gesture(std::uint8_t encoder_id,                                                            \
                         std::uint8_t gesture,                                                               \
                         std::uint8_t gesture_type,                                                          \
                         std::int16_t value);                                                                \
    /* The param tables, as unsized arrays, so a test indexes them without knowing                           \
     * how many params the mode declares. */                                                                 \
    extern const std::uint8_t kParamCount;                                                                   \
    extern const ::op::modes::ParamSpec kParams[];                                                           \
    extern const char kParamStrings[];                                                                       \
    }                                                                                                        \
    /* The gesture export takes the ABI's bytes. This forwarder takes the scoped                             \
     * enums, so a call site says Scroll and Enter rather than the numbers behind                            \
     * them. Both enums are uint8_t-backed, so the conversion keeps the value. */                            \
    namespace op::sim {                                                                                      \
        inline void ui_gesture(std::uint8_t encoder_id,                                                      \
                               ::op::Gesture gesture,                                                        \
                               ::op::GestureType gesture_type,                                               \
                               std::int16_t value) {                                                         \
            ::mode_ui_gesture(encoder_id, static_cast<std::uint8_t>(gesture),                                \
                              static_cast<std::uint8_t>(gesture_type), value);                               \
        }                                                                                                    \
    }                                                                                                        \
    using _op_mode_under_test_decl_t = void /* consumes the trailing ';' */
