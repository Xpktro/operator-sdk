// The opt-in helper headers, doctest behavior suite.
//
// Value and behavior coverage for the helpers a mode opts into
// (operator_sdk/{text,timing,midi,math,log,storage}.h).
//
// Two things this suite cannot check from inside one translation unit, which
// includes every helper, are checked in CMakeLists.txt instead: that each header
// stands alone, through per-header compile-only targets, and that the umbrella does
// not pull them in, through the will-fail compile probes.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cstdint>
#include <cstring>  // std::strcmp, host-side assertions only

#include <operator_sdk/text.h>
#include <operator_sdk/timing.h>
#include <operator_sdk/midi.h>
#include <operator_sdk/math.h>
#include <operator_sdk/log.h>
#include <operator_sdk/storage.h>

// op::log, op::write_file and ClockInterpolator name op::api via an extern
// declaration. The single definition lives here, and the cases below point it
// at their own mocks.
namespace op {
const OperatorApi* api = nullptr;
}

// ---------------------------------------------------------------------------
// text.h: the width math and the formatters
// ---------------------------------------------------------------------------

TEST_CASE("text.h: pixel width matches the device renderer") {
    using namespace op::sdk::text;
    CHECK(text_width("ABC") == 18);  // 3 glyphs * 6px advance
    CHECK(text_width("A B") == 16);  // SPACE is 4px, not 6
    CHECK(text_width(nullptr) == 0);
    CHECK(large_text_width("ABC") == 24);     // 3 * 8px
    CHECK(align_center_x("ABC", 128) == 55);  // (128 - 18) / 2
    CHECK(align_center_x("ABC", 10) == 0);    // wider than box -> 0
    CHECK(kFontStdAdvance == 6);
    CHECK(kFontLargeAdvance == 8);
}

TEST_CASE("text.h: integer + string composition") {
    using namespace op::sdk::text;
    char buf[32];

    format_int(-42, buf, sizeof buf);
    CHECK(std::strcmp(buf, "-42") == 0);
    format_int(7, buf, sizeof buf, /*force_plus=*/true);
    CHECK(std::strcmp(buf, "+7") == 0);
    format_uint(4294967295u, buf, sizeof buf);
    CHECK(std::strcmp(buf, "4294967295") == 0);

    format_str("n", buf, sizeof buf);
    append_str(buf, sizeof buf, " = ");
    append_int(buf, sizeof buf, 3);
    CHECK(std::strcmp(buf, "n = 3") == 0);

    static const char* const kLabels[] = {"1/4", "1/8"};
    format_str("Rate: ", buf, sizeof buf);
    append_enum(buf, sizeof buf, 1, kLabels, 2);
    CHECK(std::strcmp(buf, "Rate: 1/8") == 0);
    format_str("Rate: ", buf, sizeof buf);
    append_enum(buf, sizeof buf, 9, kLabels, 2);  // out of range -> fallback
    CHECK(std::strcmp(buf, "Rate: ?") == 0);
}

TEST_CASE("text.h: append_fixed formats a fixed-point decimal") {
    using namespace op::sdk::text;
    char buf[16];

    auto fixed = [&](int32_t v, uint8_t d) {
        buf[0] = '\0';
        append_fixed(buf, sizeof buf, v, d);
        return buf;
    };
    CHECK(std::strcmp(fixed(1250, 2), "12.50") == 0);
    CHECK(std::strcmp(fixed(-5, 2), "-0.05") == 0);  // sign + zero-padded frac
    CHECK(std::strcmp(fixed(5, 2), "0.05") == 0);
    CHECK(std::strcmp(fixed(100, 2), "1.00") == 0);
    CHECK(std::strcmp(fixed(0, 2), "0.00") == 0);
    CHECK(std::strcmp(fixed(42, 0), "42") == 0);  // 0 decimals -> plain int
}

// ---------------------------------------------------------------------------
// timing.h: the subdivision math
// ---------------------------------------------------------------------------

TEST_CASE("timing.h: subdivision + grid + swing math") {
    using namespace op::sdk::timing;
    // 120 BPM: quarter = 500000us, eighth = 250000us, triplet-8th = 166666us.
    CHECK(subdivision_us(kSubdiv_4, 120) == 500000u);
    CHECK(subdivision_us(kSubdiv_8, 120) == 250000u);
    CHECK(subdivision_us(kSubdiv_16, 120) == 125000u);
    CHECK(subdivision_us(kSubdivTripletEighth, 120) == 166666u);
    CHECK(subdivision_us(kSubdiv_4, 0) == 0u);  // bpm 0 guard

    CHECK(quantize_to_grid(1025, 1000) == 1000u);
    CHECK(quantize_to_grid(1999, 1000) == 1000u);
    CHECK(quantize_to_grid(500, 0) == 500u);  // subdiv 0 passthrough

    CHECK(swing_offset(100, 50, 100) == 0);   // straight feel
    CHECK(swing_offset(100, 75, 100) == 25);  // off-beat delayed
    CHECK(swing_offset(200, 75, 100) == 0);   // on-beat untouched
}

// ---------------------------------------------------------------------------
// midi.h: note names, scales, chords and CC constants
// ---------------------------------------------------------------------------

TEST_CASE("midi.h: note-name formatting (sharps, MIDI octave)") {
    using namespace op::sdk::midi;
    char n[5];
    note_name(60, n);
    CHECK(std::strcmp(n, "C4") == 0);
    note_name(61, n);
    CHECK(std::strcmp(n, "C#4") == 0);
    note_name(69, n);
    CHECK(std::strcmp(n, "A4") == 0);
    note_name(0, n);
    CHECK(std::strcmp(n, "C-1") == 0);
    note_name(127, n);
    CHECK(std::strcmp(n, "G9") == 0);
}

TEST_CASE("midi.h: scale tables carry the canonical degrees") {
    using namespace op::sdk::midi;
    CHECK(kScaleMajorSize == 7);
    const uint8_t expect[] = {0, 2, 4, 5, 7, 9, 11};
    for (uint8_t i = 0; i < kScaleMajorSize; ++i) CHECK(kScaleMajor[i] == expect[i]);
    CHECK(kScaleChromaticSize == 12);
    CHECK(kScaleBluesSize == 6);
    CHECK(kScalePentatonicMinorSize == 5);
}

TEST_CASE("midi.h: chord voicing transposes and clamps to [0,127]") {
    using namespace op::sdk::midi;
    const uint8_t in[] = {60, 64, 67};
    uint8_t out[3];
    CHECK(voice_chord(in, 3, 1, out) == 3);
    CHECK(out[0] == 72);
    CHECK(out[1] == 76);
    CHECK(out[2] == 79);

    const uint8_t low[] = {0};
    uint8_t lo[1];
    voice_chord(low, 1, -1, lo);
    CHECK(lo[0] == 0);  // clamped, not wrapped
    const uint8_t high[] = {127};
    uint8_t hi[1];
    voice_chord(high, 1, 1, hi);
    CHECK(hi[0] == 127);  // clamped
}

TEST_CASE("midi.h: CC constants") {
    using namespace op::sdk::midi;
    CHECK(kCCModWheel == 1);
    CHECK(kCCSustain == 64);
    CHECK(kCCAllNotesOff == 123);
}

TEST_CASE("math.h: value mapping") {
    using namespace op::sdk::math;
    CHECK(map_linear(50, 0, 100, 0, 1000) == 500);
    CHECK(map_linear(0, 0, 100, 200, 300) == 200);
    CHECK(map_linear(5, 10, 10, 7, 9) == 7);  // collapsed input range -> out_lo

    // Curve maps hit their range endpoints exactly.
    CHECK(map_exp(0.0f, 0.0f, 1.0f, 10.0f, 20.0f) == doctest::Approx(10.0f));
    CHECK(map_exp(1.0f, 0.0f, 1.0f, 10.0f, 20.0f) == doctest::Approx(20.0f));
    CHECK(map_log(0.0f, 0.0f, 1.0f, 10.0f, 20.0f) == doctest::Approx(10.0f));
    CHECK(map_log(1.0f, 0.0f, 1.0f, 10.0f, 20.0f) == doctest::Approx(20.0f));

    // Midpoint bends the expected way relative to the linear midpoint (50).
    CHECK(map_exp(0.5f, 0.0f, 1.0f, 0.0f, 100.0f) < 50.0f);  // exp sits below
    CHECK(map_log(0.5f, 0.0f, 1.0f, 0.0f, 100.0f) > 50.0f);  // log sits above
}

// ---------------------------------------------------------------------------
// log.h: op::log composes a label and a number and forwards to the ABI
// ---------------------------------------------------------------------------

namespace {
char g_log_last[128];
void capture_log(::LogLevel, const char* m) {
    // The SDK's own freestanding copy, so the capture path pulls in no libc.
    op::sdk::text::format_str(m, g_log_last, sizeof g_log_last);
}
}  // namespace

TEST_CASE("log.h: op::log forwards message and formats labeled values") {
    OperatorApi mock {};
    mock.log = &capture_log;
    op::api  = &mock;

    op::log(op::LogLevel::Info, "hello");
    CHECK(std::strcmp(g_log_last, "hello") == 0);

    op::log(op::LogLevel::Warn, "count", 3);
    CHECK(std::strcmp(g_log_last, "count: 3") == 0);

    op::log(op::LogLevel::Info, "rate", 1250, 2);
    CHECK(std::strcmp(g_log_last, "rate: 12.50") == 0);

    op::api = nullptr;
}

TEST_CASE("log.h: op::log is a safe no-op before op::api is set") {
    op::api = nullptr;
    // With no api set, op::log returns without touching it.
    op::log(op::LogLevel::Error, "should not crash", 1);
    CHECK(op::api == nullptr);
}

// ---------------------------------------------------------------------------
// storage.h: op::write_file drives the three write entries
// ---------------------------------------------------------------------------

namespace {

enum class StorageCall { Open, Write, Close };

constexpr int kMaxStorageCalls = 8;
StorageCall g_storage_calls[kMaxStorageCalls];
int g_storage_call_count = 0;

const char* g_open_path   = nullptr;
uint8_t g_open_mode       = 0xFF;
int32_t g_open_result     = 0;

int32_t g_write_handle    = -1;
uint32_t g_write_size     = 0;
uint8_t g_written_bytes[16];
int32_t g_write_result    = 0;

int32_t g_close_handle    = -1;

void record_storage_call(StorageCall call) {
    if (g_storage_call_count < kMaxStorageCalls) g_storage_calls[g_storage_call_count++] = call;
}

int32_t mock_open_write(const char* path, uint8_t open_mode) {
    record_storage_call(StorageCall::Open);
    g_open_path = path;
    g_open_mode = open_mode;
    return g_open_result;
}

int32_t mock_write(int32_t handle, const uint8_t* buf, uint32_t size) {
    record_storage_call(StorageCall::Write);
    g_write_handle = handle;
    g_write_size   = size;
    for (uint32_t i = 0; i < size && i < sizeof(g_written_bytes); ++i) g_written_bytes[i] = buf[i];
    return g_write_result;
}

int32_t mock_close(int32_t handle) {
    record_storage_call(StorageCall::Close);
    g_close_handle = handle;
    return 0;
}

// Wires the three write entries and clears what the mocks last recorded.
OperatorApi make_storage_mock(int32_t open_result, int32_t write_result) {
    g_storage_call_count = 0;
    g_open_path          = nullptr;
    g_open_mode          = 0xFF;
    g_open_result        = open_result;
    g_write_handle       = -1;
    g_write_size         = 0;
    g_write_result       = write_result;
    g_close_handle       = -1;

    OperatorApi mock {};
    mock.storage_file_open_write = &mock_open_write;
    mock.storage_file_write      = &mock_write;
    mock.storage_file_close      = &mock_close;
    return mock;
}

}  // namespace

TEST_CASE("storage.h: write_file opens then writes the caller's bytes then closes") {
    OperatorApi mock = make_storage_mock(/*open_result=*/7, /*write_result=*/3);
    op::api          = &mock;

    const uint8_t payload[] = {0x11, 0x22, 0x33};
    const int32_t written   = op::write_file("clip.bin", payload, sizeof(payload));

    REQUIRE(g_storage_call_count == 3);
    CHECK(g_storage_calls[0] == StorageCall::Open);
    CHECK(g_storage_calls[1] == StorageCall::Write);
    CHECK(g_storage_calls[2] == StorageCall::Close);

    CHECK(std::strcmp(g_open_path, "clip.bin") == 0);
    CHECK(g_open_mode == kStorageOpenTruncate);
    CHECK(g_write_handle == 7);  // the handle open handed back
    CHECK(g_close_handle == 7);
    REQUIRE(g_write_size == sizeof(payload));
    for (uint32_t i = 0; i < sizeof(payload); ++i) CHECK(g_written_bytes[i] == payload[i]);
    CHECK(written == 3);

    op::api = nullptr;
}

TEST_CASE("storage.h: write_file closes the file and returns the store's result") {
    OperatorApi mock = make_storage_mock(/*open_result=*/4, /*write_result=*/kStorageNoSpace);
    op::api          = &mock;

    const uint8_t payload[] = {0xAB};
    const int32_t written   = op::write_file("rec.bin", payload, sizeof(payload));

    REQUIRE(g_storage_call_count == 3);
    CHECK(g_storage_calls[2] == StorageCall::Close);
    CHECK(written == kStorageNoSpace);

    op::api = nullptr;
}
