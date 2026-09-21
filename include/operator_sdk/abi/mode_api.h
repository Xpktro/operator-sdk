#pragma once
/// @file mode_api.h
/// @brief The Operator function table a mode calls into, plus the mode export
///        signatures and the input gesture ABI.
///
/// OperatorApi is the versioned struct of function pointers the firmware hands
/// a mode at init. A mode keeps the pointer and calls through it to send MIDI,
/// read timing and clock state, read and write scoped storage, draw to the
/// display, log, and read and write its declarative parameters.
/// A mode resolves an entry by its position, so the table only ever grows. New
/// entries are appended, and existing ones keep their position and their type.
/// A mode built against one SDK release keeps loading on later firmware, which
/// offers it entries it never calls.

#include <cstdint>

extern "C" {

/// @brief A single MIDI message passed to a mode's process function.
/// A fixed C layout: a status byte, two data bytes, the source port, and a
/// valid-length count.
struct OpMidiMessage {
    uint8_t status;   ///< Status byte: message type in the high nibble, channel in the low nibble.
                      ///< Clearing it to 0 takes the message out of the batch.
    uint8_t data1;    ///< First data byte (for example note number or controller).
    uint8_t data2;    ///< Second data byte (for example velocity or value).
    uint8_t port;     ///< Input this message arrived on. 0..3 are the physical inputs, 4 is USB.
                      ///< A generated message arrived on no input and carries 0xFF.
    uint8_t length;   ///< Valid byte count, 1 to 3.
};

/// @brief Clock authority state returned by get_clock_state.
/// A mode polls this to decide whether the pulse counter is advancing, whether
/// it should generate its own timing, and whether the user has delegated clock
/// authority to a loaded mode.
enum OpClockState : uint8_t {
    kClockStateActive  = 0,  // clock messages observed -> pulse counter advancing
    kClockStateNoClock = 1,  // no clock observed -> pulse counter idle
    kClockStateMode    = 2,  // Clock Source = Mode but no clock-gen mode loaded
};

/// @brief Severity level for a log line passed to OperatorApi::log.
/// A mode picks the level that matches the message's importance. The Log
/// Monitor view renders one letter per level (D / I / W / E) and can filter by
/// level, so choosing an accurate level makes a mode's output easier to read
/// and filter. Prefer the scoped alias op::LogLevel.
enum class LogLevel : uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3 };

/// @brief The function table a mode calls into.
/// Entries are grouped by concern: MIDI I/O, Timing, Storage (scoped read and
/// write), Display, Logging, Parameters, and Clock. SDK version 1 defines
/// 28 entries, and their order below is part of the ABI.
struct OperatorApi {
    // --- MIDI I/O ---
    /// @brief Queue a 1 to 3 byte MIDI message for output.
    /// The message joins that output's traffic, so the modes loaded after this
    /// one there are handed it and can transform it, hold it back, or answer
    /// it.
    ///
    /// The output takes it only if it passes that kind of message, so one set
    /// to pass clock alone takes no notes. Timing clock (0xF8) goes to the
    /// output on its own and joins no mode's batch.
    /// @param output_idx Output for global modes (0..7 = OUT 1..8, 8 = USB).
    ///        Ignored for output modes, which always reach their bound output.
    /// @param status MIDI status byte.
    /// @param data1 First data byte.
    /// @param data2 Second data byte (ignored for 1 and 2 byte messages).
    /// @return True if the message was queued, false if it was dropped.
    bool (*send_midi)(uint8_t output_idx, uint8_t status, uint8_t data1, uint8_t data2);
    /// @brief Queue a 1 to 3 byte MIDI message straight to the output.
    /// The modes loaded after this one on that output are never handed it, so
    /// it reaches the wire untouched. This suits a timing reference or a tap
    /// reporting what an output is doing.
    ///
    /// Only those modes are skipped. The output still takes the message only if
    /// it passes that kind of message.
    /// @param output_idx Output for global modes (0..7 = OUT 1..8, 8 = USB).
    ///        Ignored for output modes, which always reach their bound output.
    /// @param status MIDI status byte.
    /// @param data1 First data byte.
    /// @param data2 Second data byte (ignored for 1 and 2 byte messages).
    /// @return True if the message was queued, false if it was dropped.
    bool (*send_midi_direct)(uint8_t output_idx, uint8_t status, uint8_t data1,
                             uint8_t data2);
    /// @brief Queue a 1 to 3 byte MIDI message caused by one this mode was handed.
    /// The message joins the traffic on the input named by `input_idx`, so it
    /// is routed exactly as a message arriving there would be.
    ///
    /// It carries `input_idx` as its port, and the global modes loaded after
    /// this one are handed it.
    ///
    /// Global modes only. An output mode runs once routing has decided where
    /// everything goes, so the call is refused there and answers false.
    /// @param input_idx Input the causing message arrived on (0..3 = IN 1..4, 4 = USB).
    /// @param status MIDI status byte.
    /// @param data1 First data byte.
    /// @param data2 Second data byte (ignored for 1 and 2 byte messages).
    /// @return True if the message was queued, false if it was dropped.
    bool (*send_midi_from)(uint8_t input_idx, uint8_t status, uint8_t data1, uint8_t data2);
    /// @brief Queue a system-exclusive message, or a fragment of one, for output.
    /// A short message goes out in a single call. A long one is built up over
    /// several calls, and `flags` says where each call sits in the message.
    ///
    /// - `kSysExStart` together with `kSysExEnd` sends a whole message.
    /// - `kSysExStart` on its own opens a message that later calls continue.
    /// - Neither bit set continues the open message with more bytes.
    /// - `kSysExEnd` on its own closes it.
    ///
    /// Everything sent between the opening and the closing call reaches the
    /// output as one message. Each call queues its whole fragment or queues
    /// nothing at all, so a call answered with @ref kOutputBusy leaves the open
    /// message untouched and the same fragment goes out again on a later
    /// `process()`.
    ///
    /// One call carries at most 253 payload bytes to a physical output and 448
    /// to the USB output, so 253 bytes per call reaches any output. A message
    /// longer than that goes out over several calls.
    ///
    /// A message ends by itself after a full second in which none of it reached
    /// the output. A refused call sends nothing, so it counts towards that
    /// second, and a retry that keeps being refused ends the message it is
    /// retrying. Calls after that answer -1 and a new message has to be opened.
    /// @param output_idx Output index. An output mode passes its own bound output.
    /// @param data The message bytes, without the opening 0xF0 and the closing
    ///        0xF7, which are added around the message.
    /// @param len Number of bytes in data. Zero pairs with kSysExEnd to close a
    ///        message that has no bytes left to send.
    /// @param flags Where this call sits in the message (see the list above).
    /// @return `len` when the whole fragment was queued (0 when the call only
    ///         closes the message), @ref kOutputBusy when the output could not
    ///         take it right now, @ref kOutputTooLarge when the fragment is
    ///         bigger than that output can ever carry, or -1 for an invalid
    ///         output, an output not set to pass system-exclusive, a mode not
    ///         allowed to send SysEx, or a call continuing a message that is
    ///         over.
    int32_t (*send_sysex)(uint8_t output_idx, const uint8_t* data, uint16_t len,
                          uint8_t flags);
    /// @brief Report whether an output can currently emit MIDI.
    /// True when output_idx is in range, is not user-disabled, and is present.
    /// The USB output reports true while USB MIDI is active. A mode rotating
    /// between outputs uses this to pick one that can take the message.
    /// @param output_idx Output index to query.
    /// @return True if the output can emit MIDI right now.
    bool (*output_active)(uint8_t output_idx);

    // --- Timing ---
    /// @brief Monotonic microsecond counter since boot.
    /// @return Microseconds since boot.
    uint32_t (*get_tick)();
    /// @brief Monotonic 24 PPQ pulse counter since boot.
    /// Advances when the current clock source emits a clock message.
    /// @return The pulse count.
    uint32_t (*get_pulse_count)();
    /// @brief Program the internal clock generator's period.
    /// Only effective when the caller is a clock generating mode and Clock
    /// Source is set to Mode. A period of 0 disables the clock, and setting the
    /// same period again is a no-op.
    /// @param period_us Clock period in microseconds.
    void     (*set_clock_period_us)(uint32_t period_us);

    // --- Storage scoped: read family ---
    // Path arguments are relative names. Every read is routed under
    // /extras/{mode_name}/, and cross-mode access is rejected.
    /// @brief Read up to size bytes from a scoped path into buf.
    /// @param path Relative path under the mode's storage area.
    /// @param buf Destination buffer.
    /// @param size Maximum bytes to read.
    /// @return Bytes read, or a negative error code.
    int32_t  (*storage_read)(const char* path, uint8_t* buf, uint32_t size);
    /// @brief Open a handle for streaming reads under a scoped path.
    /// The handle stays valid across process() calls until it is closed or the
    /// mode is destroyed (which auto-closes it).
    /// @param path Relative path under the mode's storage area.
    /// @return A non-negative handle on success, or -1 on error.
    int32_t (*storage_file_open) (const char* path);
    /// @brief Read up to size bytes from an open handle.
    /// @param handle A handle from storage_file_open.
    /// @param buf Destination buffer.
    /// @param size Maximum bytes to read.
    /// @return Bytes read (may be less than size near the end of file), 0 at end
    ///         of file, or -1 on error.
    int32_t (*storage_file_read) (int32_t handle, uint8_t* buf, uint32_t size);
    /// @brief Move an open handle's read cursor.
    /// Seeking past the end of the file is allowed, and later reads return 0.
    /// @param handle A handle from storage_file_open.
    /// @param offset New absolute read offset.
    /// @return 0 on success, or -1 on error.
    int32_t (*storage_file_seek) (int32_t handle, uint32_t offset);
    /// @brief Close an open handle.
    /// This is not idempotent: closing an already closed handle returns -1.
    /// @param handle A handle from storage_file_open.
    /// @return 0 on success, or -1 on an invalid handle.
    int32_t (*storage_file_close)(int32_t handle);

    // --- Storage scoped: write family ---
    // Paths are relative names under the mode's own storage area, the same as
    // the read family above. A write takes a while, so changes accumulate while
    // process() runs and go out from an input gesture or an explicit save.
    /// @brief Open a handle for streaming writes under a scoped path.
    /// The handle stays valid across process() calls until it is closed or the
    /// mode is destroyed (which auto-closes it).
    /// @param path Relative path under the mode's storage area.
    /// @param open_mode @ref kStorageOpenTruncate creates or overwrites from
    ///                  empty, @ref kStorageOpenOverwrite overwrites in place
    ///                  from offset 0.
    /// @return A non-negative handle on success, -1 on error, or @ref
    ///         kStorageBusy while the device is busy with flash.
    int32_t (*storage_file_open_write)(const char* path, uint8_t open_mode);
    /// @brief Write size bytes from buf to an open write handle.
    /// @param handle A handle from storage_file_open_write.
    /// @param buf Source bytes.
    /// @param size Number of bytes to write.
    /// @return Bytes written, -1 on error, @ref kStorageBusy while the device is
    ///         busy with flash, or @ref kStorageNoSpace when the store is full.
    int32_t (*storage_file_write)(int32_t handle, const uint8_t* buf, uint32_t size);
    /// @brief Delete a file under a scoped path.
    /// @param path Relative path under the mode's storage area.
    /// @return 0 on success, -1 on error, or @ref kStorageBusy while the device is
    ///         busy with flash.
    int32_t (*storage_delete)(const char* path);
    /// @brief List the mode's stored files into out as NUL-separated names.
    /// @param pattern Extension filter (for example ".scl"), or empty for all
    ///                files.
    /// @param out Destination buffer for the packed name list. The list is
    ///            clipped to fit.
    /// @param out_size Size of out.
    /// @return The number of entries listed, -1 on error, or @ref kStorageBusy
    ///         while the device is busy with flash.
    int32_t (*storage_enumerate)(const char* pattern, char* out, uint32_t out_size);

    // --- Display ---
    /// @brief Draw a 1 pixel outline rectangle.
    /// @param x Left edge in pixels.
    /// @param y Top edge in pixels.
    /// @param w Width in pixels.
    /// @param h Height in pixels.
    void (*draw_rect)(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    /// @brief Draw a filled rectangle of on or off pixels.
    /// @param x Left edge in pixels.
    /// @param y Top edge in pixels.
    /// @param w Width in pixels.
    /// @param h Height in pixels.
    /// @param on True sets the pixels, false clears them.
    void (*fill_rect)(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool on);
    /// @brief Draw text in the 5x7 standard font.
    /// @param x Left edge of the first glyph in pixels.
    /// @param y Top edge in pixels.
    /// @param text NUL-terminated string to draw.
    void (*draw_text)(uint16_t x, uint16_t y, const char* text);
    /// @brief Draw text in the 8x12 large font.
    /// @param x Left edge of the first glyph in pixels.
    /// @param y Top edge in pixels.
    /// @param text NUL-terminated string to draw.
    void (*draw_text_large)(uint16_t x, uint16_t y, const char* text);
    /// @brief Draw a packed 1-bit bitmap.
    /// @param x Left edge in pixels.
    /// @param y Top edge in pixels.
    /// @param data Page-addressed 1-bit pixel data.
    /// @param w Bitmap width in pixels.
    /// @param h Bitmap height in pixels.
    void (*draw_bitmap)(uint16_t x, uint16_t y, const uint8_t* data,
                        uint16_t w, uint16_t h);
    /// @brief Turn a single pixel on or off. Out-of-range coordinates are a no-op.
    /// @param x Column in pixels.
    /// @param y Row in pixels.
    /// @param on True sets the pixel, false clears it.
    void (*set_pixel)(uint16_t x, uint16_t y, bool on);

    // --- Logging ---
    /// @brief Emit a log line, viewable in the Log Monitor view.
    /// @param level A LogLevel severity (op::LogLevel::Debug / Info / Warn /
    ///        Error). The Log Monitor renders it as a D / I / W / E prefix and
    ///        can filter on it.
    /// @param message Null-terminated text to log.
    void (*log)(LogLevel level, const char* message);

    // --- Parameters ---
    /// @brief Read the current int32 value of a declarative parameter.
    /// @param param_id The parameter's value slot index.
    /// @return The current value.
    int32_t (*get_param_value)(uint8_t param_id);
    /// @brief Write a declarative parameter value.
    /// The new value is published within one frame.
    /// @param param_id The parameter's value slot index.
    /// @param value The value to store.
    void    (*set_param_value)(uint8_t param_id, int32_t value);
    /// @brief Resolve a FilePicker parameter to its selected filename.
    /// Copies the null-terminated filename into buf, truncated to buf_size with
    /// the NUL always preserved when buf_size is greater than 0. Prefer the
    /// typed param<FilePickerSpec>() accessor from
    /// <operator_sdk/params.h>, which wraps this entry and returns a
    /// FilePickerValue.
    /// @param param_id The FilePicker parameter id.
    /// @param buf Destination buffer for the filename.
    /// @param buf_size Size of buf.
    /// @return Bytes written excluding the NUL (0 means no file picked yet), or -1 on a validation error.
    int32_t (*get_param_filename)(uint8_t param_id, char* buf, uint32_t buf_size);

    // --- Clock ---
    /// @brief Read the current clock authority state.
    /// @return One of OpClockState as a uint8_t.
    uint8_t (*get_clock_state)();
};

static_assert(sizeof(OperatorApi) == 28 * sizeof(void*),
              "OperatorApi must contain exactly 28 function pointer entries");

/// @brief The current SDK and function table version.
/// A mode built against this version or an earlier one runs here.
static const uint16_t kOperatorSdkVersion = 1;

/// @brief Storage result meaning the device is busy with flash right now.
/// Any storage call can answer it when the other core is mid-write. A call
/// answered with it succeeds on a later `process()`.
static const int32_t kStorageBusy = -2;

/// @brief Storage result meaning the store is out of space.
/// Returned by @ref OperatorApi::storage_file_write "storage_file_write".
/// The store stays full until the user frees space on it.
static const int32_t kStorageNoSpace = -3;

/// @brief send_sysex result meaning the output cannot take the fragment now.
/// Returned by @ref OperatorApi::send_sysex "send_sysex" when the fragment plus
/// its opening and closing bytes will not fit right now. Nothing was queued and
/// the open message is left intact, so the same fragment goes out again on a
/// later `process()`.
static const int32_t kOutputBusy = -2;

/// @brief send_sysex result meaning the fragment is bigger than the output can carry.
/// Returned by @ref OperatorApi::send_sysex "send_sysex" when the fragment plus
/// its opening and closing bytes is larger than the output can ever carry, so
/// the message needs splitting into smaller ones. A retry answers the same
/// however long the wait. Nothing was queued and any open message is left
/// intact.
static const int32_t kOutputTooLarge = -3;

/// @brief open_mode that creates the file, or overwrites an existing one from empty.
static const uint8_t kStorageOpenTruncate  = 0;
/// @brief open_mode that keeps the existing bytes and writes in place from offset 0.
static const uint8_t kStorageOpenOverwrite = 1;

/// @brief Signature of a mode's mode_init export, called once at load with the API table.
typedef void (*ModeInitFn)(const OperatorApi* api);
/// @brief Signature of a mode's mode_process export, called with the incoming MIDI messages.
typedef void (*ModeProcessFn)(OpMidiMessage* msgs, uint8_t count, uint32_t tick);
/// @brief Signature of a mode's mode_destroy export, called once at unload.
typedef void (*ModeDestroyFn)(void);
/// @brief Signature of a mode's mode_migrate_config export, called to upgrade persisted config.
typedef void (*ModeMigrateConfigFn)(const uint8_t* old_data, uint16_t old_version);

// Custom UI exports. A mode provides these to draw its own screen.
/// @brief Signature of a mode's mode_ui_render export, called each frame to draw the custom UI.
typedef void (*ModeUiRenderFn)(void);
/// @brief Signature of a mode's mode_ui_gesture export, a flat input event handler.
/// gesture_type carries the meaning resolved through the input map (Scroll,
/// Change, Enter, Back), which is what most modes read. encoder_id and gesture
/// carry the raw event for a mode that wants map-independent behavior. value is
/// the signed rotation step with acceleration applied for a rotate, and 0 for
/// presses.
typedef void (*ModeUiGestureFn)(uint8_t encoder_id, uint8_t gesture,
                                uint8_t gesture_type, int16_t value);

/// @brief Signature of a mode's SysEx handler, called with inbound SysEx.
/// The handler runs only when the mode declares `HANDLES_SYSEX`. A short
/// message arrives in a single call carrying both flag bits. A long one arrives
/// as several fragments across multiple calls, in order, the first flagged
/// kSysExStart and the last kSysExEnd. port is the input the message arrived on,
/// whichever kind of mode receives it, so a mode reassembling a dump can keep two
/// senders apart.
/// Returning true on the fragment flagged kSysExStart consumes the message, so
/// it is not sent on and no later mode in the chain sees the rest of it. Returning
/// false lets it continue to the outputs. A mode that exports no handler passes
/// every message. The decision is read on the start fragment and held
/// through kSysExEnd, so the return value of a later fragment is ignored.
typedef bool (*ModeOnSysexFn)(uint8_t port, const uint8_t* data,
                              uint16_t len, uint8_t flags);

/// @brief mode_on_sysex flags bit marking the first fragment of a message.
/// The consume-or-pass choice is made on this fragment and held through
/// kSysExEnd, so the message is handled whole either way.
static const uint8_t kSysExStart = 0x01;
/// @brief mode_on_sysex flags bit marking the last fragment of a message.
static const uint8_t kSysExEnd   = 0x02;

/// @brief Signature of the callback an Action parameter runs.
/// Declaring the button with op::params::Action wires it up.
typedef void (*ModeOnSelectFn)(void);

/// @brief Encoder id for the left encoder, passed to mode_ui_gesture.
static const uint8_t kEncoderLeft  = 0;
/// @brief Encoder id for the right encoder, passed to mode_ui_gesture.
static const uint8_t kEncoderRight = 1;

// The op::Gesture and op::GestureType scoped enums are defined in namespace op
// just below this extern "C" block. A mode compares the uint8_t gesture and
// gesture_type arguments against them.

}  // extern "C"

namespace op {

/// @brief Scoped alias for the log severity enum, so mode code writes
/// op::LogLevel::Warn alongside the other op:: scoped enums. It names the same
/// type as the extern "C" LogLevel above (identical uint8_t layout).
using LogLevel = ::LogLevel;

/// @brief Physical gesture kind for an input event.
/// For Rotate, mode_ui_gesture's value is the signed step count (positive
/// clockwise, negative counter-clockwise, magnitude includes acceleration).
/// For the presses value is 0. The order is ABI byte-stable.
enum class Gesture : uint8_t {
    Rotate     = 0,
    ShortPress = 1,
    LongPress  = 2,
};

/// @brief Input-map resolved gesture type for an input event.
/// A mode reads gesture_type for input-map-aware behavior, or reads encoder_id
/// and gesture for raw, map-independent behavior. The two long-press types are
/// firmware-reserved: the firmware intercepts them before dispatch, so a mode
/// never observes them on any field. Which physical long-press maps to which
/// reserved type follows the active Input Map. The order is ABI byte-stable.
enum class GestureType : uint8_t {
    None        = 0,
    Scroll      = 1,  // list/cursor navigation (rotate)
    Change      = 2,  // value edit (rotate)
    Enter       = 3,  // activate / confirm (short-press)
    Back        = 4,  // cancel / pop (short-press)
    EnterParams = 5,  // open Parameters page (long-press, reserved)
    ExitUi      = 6,  // exit custom UI (long-press, reserved)
};

}  // namespace op
