// The mock OperatorApi behind the harness. Every entry a mode calls is a stub
// here whose effect a test reads back through operator_sdk_sim.h.
//
// Storage goes to MockExchangeStore (mock_store.h), which puts the files under
// exchange_root() scoped to the mode's own /extras/ folder. Drawing goes to
// MockDisplay (mock_display.h), which holds the framebuffer bits.

#include "operator_sdk_sim.h"
#include "harness_internal.h"

#include "mock_display.h"
#include "mock_store.h"
// ParamSpec and ParamKind for apply_param_defaults.
#include <operator_sdk/abi/mode_param.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <filesystem>
#include <span>
#include <string>

namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

constexpr std::uint8_t kMaxOutputs = 9;  // OUT 1..8 at index 0..7, USB at index 8
constexpr std::uint8_t kMaxInputs  = 5;  // IN 1..4 at index 0..3, USB at index 4
// The port a message carries when there is no input behind it, which covers
// everything a mode generates. It sits outside the input range and the output
// range both, so a reader tells a generated message from an arrival by value
// alone. The device stamps the same value.
constexpr std::uint8_t kPortGenerated = 0xFF;
constexpr std::size_t kOutQueueDepth  = 32;
constexpr std::size_t kMaxParams      = 64;
constexpr std::size_t kCurrentModeMax = 32;
// One fixed-length filename buffer per param id, which is all a test needs to
// stand in for a picked file.
constexpr std::size_t kMaxFilenameBytes = 64;

struct MidiRing {
    OpMidiMessage msgs[kOutQueueDepth] {};
    std::uint16_t head  = 0;  // push index
    std::uint16_t tail  = 0;  // pop index
    std::uint16_t count = 0;

    void push(const OpMidiMessage& message) {
        // Drop when full. A test that overruns the queue asks for more than the
        // harness keeps.
        if (count >= kOutQueueDepth) return;
        msgs[head] = message;
        head       = static_cast<std::uint16_t>((head + 1) % kOutQueueDepth);
        ++count;
    }
    bool pop(OpMidiMessage& out) {
        if (count == 0) return false;
        out  = msgs[tail];
        tail = static_cast<std::uint16_t>((tail + 1) % kOutQueueDepth);
        --count;
        return true;
    }
    void clear() {
        head = tail = count = 0;
    }
};

struct State {
    // Timing. The pulse count is the canonical input a mode reads.
    std::uint32_t tick_us         = 0;
    std::uint32_t pulse_count     = 0;  // 24 PPQ
    std::uint32_t clock_period_us = 0;  // the last period a mode asked for
    std::uint8_t clock_state      = 0;  // kClockStateActive

    MidiRing outgoing[kMaxOutputs];

    // Held as the inactive set, so value-init (all false) means every output is
    // active and no constructor is needed. Tests flip one with set_output_active.
    bool output_inactive[kMaxOutputs] = {};

    op::sim::MockDisplay display;

    // One int32 slot per param, flat.
    std::int32_t params[kMaxParams] = {};

    // The filename a FilePicker param resolves to. An empty buffer is the empty
    // FilePickerValue the device starts a mode with.
    char picked_filename[kMaxParams][kMaxFilenameBytes] = {};

    // Scopes the storage paths to /extras/<mode name>/.
    char mode_name[kCurrentModeMax] = "default";
};

State g_state;

// Resolve the on-disk root used by the mock store for the current mode's
// /extras/ scope. The mock store roots its files at op::sim::exchange_root()
// (a per-process scratch tree), and this path resolves under it.
fs::path current_extras_dir() {
    fs::path dir(op::sim::exchange_root());
    dir /= "extras";
    dir /= g_state.mode_name;
    return dir;
}

// ---------------------------------------------------------------------------
// The stubs, one per OperatorApi entry.
// ---------------------------------------------------------------------------

bool stub_send_midi(std::uint8_t output_idx, std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
    if (output_idx >= kMaxOutputs) return false;
    // A send to an output that is not there goes nowhere, as on the device.
    if (g_state.output_inactive[output_idx]) return false;
    OpMidiMessage message {};
    message.status = status;
    message.data1  = data1;
    message.data2  = data2;
    message.port   = kPortGenerated;
    // Infer length from status nibble (MIDI 1.0): 0xC0/0xD0 are 2-byte,
    // system-realtime (>=0xF8) are 1-byte, others default to 3-byte.
    if (status >= 0xF8)
        message.length = 1;
    else if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0)
        message.length = 2;
    else
        message.length = 3;
    g_state.outgoing[output_idx].push(message);
    return true;
}

// The mock records what reaches an output and holds no chain of modes behind
// it, so a message that skips the modes on an output arrives here exactly as
// one that passes through them does. Both entries share one recording for that
// reason, and a test reads the same queue whichever the mode called.
bool stub_send_midi_direct(std::uint8_t output_idx,
                           std::uint8_t status,
                           std::uint8_t data1,
                           std::uint8_t data2) {
    return stub_send_midi(output_idx, status, data1, data2);
}

// A derived message is handed to the routing matrix on the device, which decides
// the outputs it reaches from the input it names. The mock carries no matrix and
// no input traffic for it to join, so the range check is the whole of what there
// is to stand in for, and the message is not recorded against any output, since
// naming one would state a destination only the matrix can decide. The harness
// holds no mode type either, so it cannot refuse the call the way the device
// refuses it for an output mode. A mode whose derived messages need asserting is
// tested on the device.
bool stub_send_midi_from(std::uint8_t input_idx, std::uint8_t, std::uint8_t, std::uint8_t) {
    return input_idx < kMaxInputs;
}

std::int32_t stub_send_sysex(std::uint8_t output_idx,
                             const std::uint8_t* data,
                             std::uint16_t len,
                             std::uint8_t flags) {
    // Transactional contract: len on success (0 for a terminator-only call), -1
    // on an invalid output or argument combination. The mock queue is unbounded,
    // so it never reports kOutputBusy.
    if (output_idx >= kMaxOutputs) return -1;
    if (data == nullptr && len > 0) return -1;
    // A call with no payload carries only the terminator, so it needs the end
    // flag, matching the device.
    if (len == 0 && (flags & kSysExEnd) == 0) return -1;
    if (len == 0) return 0;
    // A sysex send is recorded as one message carrying the first bytes of the
    // body, which is enough to assert that a mode sent one and what it opened
    // with. The full body is not kept.
    OpMidiMessage message {};
    message.status = 0xF0;
    message.data1  = data[0];
    message.data2  = len > 1 ? data[1] : 0;
    message.port   = kPortGenerated;
    message.length = static_cast<std::uint8_t>(len > 3 ? 3 : len);
    g_state.outgoing[output_idx].push(message);
    return len;
}

std::uint32_t stub_get_tick() {
    return g_state.tick_us;
}
// The pulse count is what a mode reads to find the beat. Beats, bars and steps
// are all derived from it, at 24 PPQ.
std::uint32_t stub_get_pulse_count() {
    return g_state.pulse_count;
}
// A period of 0 turns the clock off. The mock holds the device's accepted range,
// so a mode that asks for a period outside it sees the same refusal here.
void stub_set_clock_period_us(std::uint32_t period_us) {
    if (period_us != 0u && (period_us < 100u || period_us > 1'000'000'000u)) return;
    g_state.clock_period_us = period_us;
}

// Storage reads are scoped under /extras/<mode name>/, the only place a mode
// reaches on the device.
std::int32_t stub_storage_read(const char* path, std::uint8_t* buffer, std::uint32_t size) {
    if (!path || !buffer) return -1;
    fs::path full_path = current_extras_dir() / (path[0] == '/' ? path + 1 : path);
    std::FILE* file    = std::fopen(full_path.string().c_str(), "rb");
    if (!file) return -1;
    auto bytes_read = std::fread(buffer, 1, size, file);
    std::fclose(file);
    return static_cast<std::int32_t>(bytes_read);
}

// Storage is read-only, so a mode never lists a directory. To stand a FilePicker
// mode up, seed the file's contents with seed_extras_file and seed the picked
// filename with pick_filename, which together are what the device leaves behind
// once a player has picked a row.

// ---------------------------------------------------------------------------
// Handle-based streaming-storage stubs. open() resolves a mode-scoped path
// (prepending /extras/<mode>/) and delegates to op::sim::MockExchangeStore for the
// 16-slot handle pool. read, seek and close are thin passthroughs to the same
// store.
//
// The harness uses a process-global store instance whose mount root lives at
// op::sim::exchange_root(). seed_extras_file and current_extras_dir keep the
// /extras/<mode>/ layout consistent with that root.
// ---------------------------------------------------------------------------
op::sim::MockExchangeStore g_streaming_store;
bool g_streaming_store_mounted = false;

void ensure_streaming_store_mounted() {
    if (g_streaming_store_mounted) return;
    g_streaming_store.mount();  // idempotent, creates the exchange root
    g_streaming_store_mounted = true;
}

// Build the /extras/<mode_name>/<rel> path the mock store expects. Rejects
// absolute paths, dot-prefixed files and ".." traversal, mirroring the device's
// mode-path resolution.
bool build_scoped_path(const char* rel, std::string* out) {
    if (!rel || rel[0] == '\0') return false;
    if (rel[0] == '/') return false;  // absolute
    // Checked at every component, not only the front, because the name the
    // device judges is the last one: "sub/.hidden" lands as ".hidden".
    for (const char* p = rel; *p; ++p) {
        if ((p == rel || p[-1] == '/') && p[0] == '.') return false;
    }
    for (const char* cursor = rel; cursor[0] && cursor[1]; ++cursor) {
        if (cursor[0] == '.' && cursor[1] == '.') return false;
    }
    // Path consumed by the store's open() is relative to its root
    // (op::sim::exchange_root()): /extras/<mode_name>/<rel>.
    std::string scoped = "/extras/";
    scoped += g_state.mode_name;
    scoped += "/";
    scoped += rel;
    *out = std::move(scoped);
    return true;
}

std::int32_t stub_storage_file_open(const char* path) {
    if (!path) return -1;
    ensure_streaming_store_mounted();
    std::string scoped;
    if (!build_scoped_path(path, &scoped)) return -1;
    auto fh = g_streaming_store.open(scoped.c_str());
    if (fh == op::sim::kInvalidFile) return -1;
    // The mock store does not model per-slot handle ownership tagging. Host
    // tests open and close handles directly with no owning mode object.
    return fh;
}

std::int32_t stub_storage_file_read(std::int32_t handle, std::uint8_t* buffer, std::uint32_t size) {
    if (!buffer) return -1;
    return g_streaming_store.read(handle, std::span<std::uint8_t>(buffer, size));
}

std::int32_t stub_storage_file_seek(std::int32_t handle, std::uint32_t offset) {
    return g_streaming_store.seek(handle, offset);
}

std::int32_t stub_storage_file_close(std::int32_t handle) {
    auto result = g_streaming_store.close(handle);
    return (result == op::sim::StoreError::Ok) ? 0 : -1;
}

// The drawing goes through MockDisplay, which owns the framebuffer bits.
void stub_draw_rect(std::uint16_t x, std::uint16_t y, std::uint16_t w, std::uint16_t h) {
    g_state.display.draw_rect(x, y, w, h, true);
}
void stub_fill_rect(std::uint16_t x, std::uint16_t y, std::uint16_t w, std::uint16_t h, bool on) {
    g_state.display.fill_rect(x, y, w, h, on);
}
void stub_draw_text(std::uint16_t x, std::uint16_t y, const char* text) {
    if (text) g_state.display.draw_text(x, y, text);
}
void stub_draw_text_large(std::uint16_t x, std::uint16_t y, const char* text) {
    if (text) g_state.display.draw_text_large(x, y, text);
}
void stub_draw_bitmap(
    std::uint16_t x, std::uint16_t y, const std::uint8_t* data, std::uint16_t w, std::uint16_t h) {
    if (!data) return;
    // Size = w cols * ceil(h/8) pages, matching the page-addressed bitmap
    // format consumed by Display::draw_bitmap.
    const std::size_t byte_count = static_cast<std::size_t>(w) * ((h + 7) / 8);
    g_state.display.draw_bitmap(x, y, std::span<const std::uint8_t>(data, byte_count), w, h);
}

// The gesture boundary is push-model. A test drives a mode's screen by calling its
// ui_gesture export, so there is nothing here to poll.

// A log line goes to stderr with its level. It takes the typed LogLevel so the
// signature matches the OperatorApi::log ABI.
void stub_log(LogLevel level, const char* message) {
    if (!message) message = "(null)";
    const char* tag = "log";
    switch (level) {
        case LogLevel::Debug: tag = "debug"; break;
        case LogLevel::Info: tag = "info"; break;
        case LogLevel::Warn: tag = "warn"; break;
        case LogLevel::Error: tag = "error"; break;
    }
    std::fprintf(stderr, "[sdk-sim:%s] %s\n", tag, message);
}

// The params, one int32 slot each.
std::int32_t stub_get_param_value(std::uint8_t param_id) {
    if (param_id >= kMaxParams) return 0;
    return g_state.params[param_id];
}
void stub_set_param_value(std::uint8_t param_id, std::int32_t value) {
    if (param_id >= kMaxParams) return;
    g_state.params[param_id] = value;
}

// Typed FilePicker accessor. The harness stores filenames in
// g_state.picked_filename[param_id]. Tests seed via
// op::sim::pick_filename(param_id, name). Returns 0 (empty NUL-only string)
// when nothing has been picked for this param, the same shape as the
// device's empty FilePicker default.
//
// The harness holds no spec table, so it does not know a param's kind here. A
// test that wants the "not a FilePicker" branch must either drive the real spec
// table through OP_MODE_PARAMS or set the filename buffer to a known value and
// assert via FilePickerValue::empty().
std::int32_t stub_get_param_filename(std::uint8_t param_id, char* buffer, std::uint32_t buffer_size) {
    if (!buffer || buffer_size == 0) return -1;
    buffer[0] = '\0';
    if (param_id >= kMaxParams) return -1;
    const char* stored_name = g_state.picked_filename[param_id];
    if (stored_name[0] == '\0') return 0;
    std::uint32_t i = 0;
    while (stored_name[i] != '\0' && i + 1 < buffer_size) {
        buffer[i] = stored_name[i];
        ++i;
    }
    buffer[i] = '\0';
    return static_cast<std::int32_t>(i);
}

std::uint8_t stub_get_clock_state() {
    return g_state.clock_state;
}

bool stub_output_active(std::uint8_t output_idx) {
    if (output_idx >= kMaxOutputs) return false;
    return !g_state.output_inactive[output_idx];
}

// ---------------------------------------------------------------------------
// One-time OperatorApi table init
// ---------------------------------------------------------------------------

OperatorApi g_api {};
bool g_api_initialized = false;

void init_api() {
    g_api.send_midi           = stub_send_midi;
    g_api.send_midi_direct    = stub_send_midi_direct;
    g_api.send_midi_from      = stub_send_midi_from;
    g_api.send_sysex          = stub_send_sysex;
    g_api.output_active       = stub_output_active;
    g_api.get_tick            = stub_get_tick;
    g_api.get_pulse_count     = stub_get_pulse_count;
    g_api.set_clock_period_us = stub_set_clock_period_us;
    g_api.storage_read        = stub_storage_read;
    g_api.storage_file_open   = stub_storage_file_open;
    g_api.storage_file_read   = stub_storage_file_read;
    g_api.storage_file_seek   = stub_storage_file_seek;
    g_api.storage_file_close  = stub_storage_file_close;
    g_api.draw_rect           = stub_draw_rect;
    g_api.fill_rect           = stub_fill_rect;
    g_api.draw_text           = stub_draw_text;
    g_api.draw_text_large     = stub_draw_text_large;
    g_api.draw_bitmap         = stub_draw_bitmap;
    g_api.log                 = stub_log;
    g_api.get_param_value     = stub_get_param_value;
    g_api.set_param_value     = stub_set_param_value;
    g_api.get_param_filename  = stub_get_param_filename;
    g_api.get_clock_state     = stub_get_clock_state;
    g_api_initialized         = true;
}

}  // anonymous namespace

// ===========================================================================
// Public harness API (op::sim namespace)
// ===========================================================================

namespace op::sim {

const OperatorApi* get_api() {
    if (!g_api_initialized) init_api();
    return &g_api;
}

void reset_state() {
    // Zero the queues and counters. The api table is left untouched, so a
    // pointer from get_api() stays valid across a reset.
    g_state.tick_us         = 0;
    g_state.pulse_count     = 0;
    g_state.clock_period_us = 0;
    g_state.clock_state     = 0;
    for (auto& queue : g_state.outgoing) queue.clear();
    std::memset(g_state.output_inactive, 0, sizeof(g_state.output_inactive));
    g_state.display.clear();
    std::memset(g_state.params, 0, sizeof(g_state.params));
    // Clear every per-param FilePicker filename buffer
    // so test isolation is preserved across reset_state() calls.
    std::memset(g_state.picked_filename, 0, sizeof(g_state.picked_filename));
    std::strncpy(g_state.mode_name, "default", sizeof(g_state.mode_name) - 1);
    g_state.mode_name[sizeof(g_state.mode_name) - 1] = '\0';

    // Remove the /extras/ tree under the sim exchange root, so files one test
    // seeds do not leak into the next. Best-effort.
    fs::path extras_root(op::sim::exchange_root());
    extras_root /= "extras";
    std::error_code ec;
    if (fs::exists(extras_root, ec)) {
        fs::remove_all(extras_root, ec);
    }
}

void apply_param_defaults(const ::op::modes::ParamSpec* specs, std::uint8_t count) {
    if (!specs) return;
    // Walk the slots the way the device does, so what lands here lines up with what
    // get_param_value(slot) reads back. Each spec takes value_slot_count slots, a
    // Section or Action takes none, and a Range or NoteRange takes two, its default
    // then its max.
    std::size_t slot = 0;
    for (std::uint8_t i = 0; i < count && slot < kMaxParams; ++i) {
        const auto& spec = specs[i];
        // A Section or Action spec holds no value slot, so skip it without
        // consuming one.
        if (spec.kind == static_cast<std::uint8_t>(::op::modes::ParamKind::Section)
            || spec.kind == static_cast<std::uint8_t>(::op::modes::ParamKind::Action)) {
            continue;
        }
        // A scalar leaves value_slot_count at 0, which counts as one slot.
        const std::uint8_t slots_for_spec = spec.value_slot_count ? spec.value_slot_count : 1;
        g_state.params[slot]              = spec.default_value;
        if (slots_for_spec == 2 && (slot + 1) < kMaxParams) {
            // Range/NoteRange: high slot defaults to max_value so the initial
            // (low, high) pair is a valid, non-empty range.
            g_state.params[slot + 1] = spec.max_value;
        }
        slot += slots_for_spec;
    }
}

void advance_tick(std::uint32_t microseconds) {
    g_state.tick_us += microseconds;
}

// A beat is 24 pulses, so this is the pulse count said in beats.
void set_beat_position(float beat) {
    if (beat < 0.0f) beat = 0.0f;
    g_state.pulse_count = static_cast<std::uint32_t>(beat * 24.0f);
}

void set_pulse_count(std::uint32_t pulse) {
    g_state.pulse_count = pulse;
}

void set_clock_state(std::uint8_t state) {
    g_state.clock_state = state;
}

void set_output_active(std::uint8_t output_index, bool active) {
    if (output_index >= kMaxOutputs) return;
    g_state.output_inactive[output_index] = !active;
}

std::uint32_t get_last_clock_period_us() {
    return g_state.clock_period_us;
}

std::uint16_t drain_outgoing_midi(std::uint8_t output_index, OpMidiMessage* out, std::uint16_t max_messages) {
    if (output_index >= kMaxOutputs || !out || max_messages == 0) return 0;
    std::uint16_t drained = 0;
    OpMidiMessage message {};
    while (drained < max_messages && g_state.outgoing[output_index].pop(message)) {
        out[drained++] = message;
    }
    return drained;
}

// capture_framebuffer defined in framebuffer_capture.cpp

void seed_extras_file(const char* mode_name,
                      const char* filename,
                      const std::uint8_t* data,
                      std::uint32_t size) {
    if (!mode_name || !filename) return;
    fs::path dir(op::sim::exchange_root());
    dir /= "extras";
    dir /= mode_name;
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path file_path = dir / filename;
    std::FILE* file    = std::fopen(file_path.string().c_str(), "wb");
    if (!file) return;
    if (data && size > 0) {
        std::fwrite(data, 1, size, file);
    }
    std::fclose(file);
}

void set_current_mode_name(const char* mode_name) {
    if (!mode_name) return;
    std::strncpy(g_state.mode_name, mode_name, sizeof(g_state.mode_name) - 1);
    g_state.mode_name[sizeof(g_state.mode_name) - 1] = '\0';
}

// Seed the filename returned by api->get_param_filename(param_id, ...).
// A null or empty filename clears the slot.
void pick_filename(std::uint8_t param_id, const char* filename) {
    if (param_id >= kMaxParams) return;
    char* slot = g_state.picked_filename[param_id];
    if (!filename || filename[0] == '\0') {
        slot[0] = '\0';
        return;
    }
    std::size_t i = 0;
    while (filename[i] != '\0' && i + 1 < kMaxFilenameBytes) {
        slot[i] = filename[i];
        ++i;
    }
    slot[i] = '\0';
}

// ---------------------------------------------------------------------------
// Internal accessors consumed by framebuffer_capture.cpp. These are not
// declared in the public header to keep the harness surface stable.
// ---------------------------------------------------------------------------

namespace detail {

    std::uint8_t* framebuffer_bytes() {
        return g_state.display.framebuffer();
    }

}  // namespace detail

}  // namespace op::sim
