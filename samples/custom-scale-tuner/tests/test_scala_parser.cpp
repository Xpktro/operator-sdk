// Scala-parser unit tests for the custom-scale-tuner sample.
//
// Real-world .scl files arrive with a UTF-8 BOM, CRLF line endings, comment
// lines, ratios, and outright malformed content, so the parser accepts the first
// four and rejects the last without ever crashing. These cases cover a
// well-formed 12-TET scale, ratio entries (num/den and bare integer), BOM and
// CRLF handling, comment lines at any position, invalid counts, malformed pitch
// entries, the cents and ratio conversion paths, fractional truncation beyond 3
// digits, and negative cents.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../scala.h"

#include <cstring>

using op::samples::scala::entry_to_cents_x1000;
using op::samples::scala::parse;
using op::samples::scala::Scale;
using op::samples::scala::ScaleEntry;

namespace {

// Treats the provided C-string as the full byte payload of a .scl file. strlen()
// is safe here because these payloads never embed a NUL byte. The BOM case builds
// its payload with explicit std::string assembly below.
bool parse_cstr(const char* text, Scale* out_scale) {
    return parse(text, std::strlen(text), out_scale);
}

// Absolute value helper for integer tolerance checks.
int32_t iabs(int32_t value) {
    return value < 0 ? -value : value;
}

}  // namespace

TEST_CASE("12-tet scale") {
    // Canonical 12-tone equal temperament in cents. Each step is 100.0 cents,
    // the octave (2/1) lands at 1200.0 cents. The leading "!" line carries a
    // human-readable comment and must be ignored by the parser.
    const char* payload = "! 12-tet\n"
                          "12-TET\n"
                          " 12\n"
                          "  100.0\n"
                          "  200.0\n"
                          "  300.0\n"
                          "  400.0\n"
                          "  500.0\n"
                          "  600.0\n"
                          "  700.0\n"
                          "  800.0\n"
                          "  900.0\n"
                          "  1000.0\n"
                          "  1100.0\n"
                          "  1200.0\n";

    Scale scale {};
    REQUIRE(parse_cstr(payload, &scale));
    CHECK(scale.count == 12);
    CHECK(scale.entries[0].is_cents == true);
    CHECK(scale.entries[0].cents_x1000 == 100000);
    CHECK(scale.entries[11].cents_x1000 == 1200000);
    // Description is the first non-comment line.
    CHECK(std::string(scale.description) == "12-TET");
}

TEST_CASE("ratio scale") {
    // A three-degree scale of ratios, including the bare-integer form ("2" == 2/1)
    // that Scala permits.
    const char* payload = "Just-intonation triad\n"
                          "3\n"
                          " 3/2\n"
                          " 5/4\n"
                          " 2\n";

    Scale scale {};
    REQUIRE(parse_cstr(payload, &scale));
    CHECK(scale.count == 3);

    CHECK(scale.entries[0].is_cents == false);
    CHECK(scale.entries[0].num == 3);
    CHECK(scale.entries[0].den == 2);

    CHECK(scale.entries[1].is_cents == false);
    CHECK(scale.entries[1].num == 5);
    CHECK(scale.entries[1].den == 4);

    CHECK(scale.entries[2].is_cents == false);
    CHECK(scale.entries[2].num == 2);
    CHECK(scale.entries[2].den == 1);
}

TEST_CASE("UTF-8 BOM stripped") {
    // A real-world file authored on Windows with a UTF-8 BOM prefix. The
    // parser must strip the three-byte BOM transparently, otherwise the
    // description line would start with mojibake and the int-count line
    // parse would fail downstream.
    std::string payload;
    payload.push_back(static_cast<char>(0xEF));
    payload.push_back(static_cast<char>(0xBB));
    payload.push_back(static_cast<char>(0xBF));
    payload += "Scale with BOM\n";
    payload += "1\n";
    payload += " 100.0\n";

    Scale scale {};
    REQUIRE(parse(payload.data(), payload.size(), &scale));
    CHECK(scale.count == 1);
    CHECK(std::string(scale.description) == "Scale with BOM");
    CHECK(scale.entries[0].cents_x1000 == 100000);
}

TEST_CASE("CRLF line endings") {
    // Windows-style line terminators. Every terminator form (\r\n, \n, \r) counts
    // as a single line break, and the parser's trailing-\r strip handles \r\n.
    const char* payload = "CRLF scale\r\n"
                          "2\r\n"
                          " 100.0\r\n"
                          " 1200.0\r\n";

    Scale scale {};
    REQUIRE(parse_cstr(payload, &scale));
    CHECK(scale.count == 2);
    CHECK(std::string(scale.description) == "CRLF scale");
    CHECK(scale.entries[0].cents_x1000 == 100000);
    CHECK(scale.entries[1].cents_x1000 == 1200000);
}

TEST_CASE("comment lines skipped") {
    // '!' comment lines may appear anywhere in the file, interleaved with pitch
    // entries. The parser skips them without consuming a pitch slot, so the final
    // count matches `count`.
    const char* payload = "! top-of-file comment\n"
                          "Mixed comments\n"
                          "! comment between description and count\n"
                          "2\n"
                          "! comment between count and first pitch\n"
                          "100.0\n"
                          "! comment between pitches\n"
                          "200.0\n";

    Scale scale {};
    REQUIRE(parse_cstr(payload, &scale));
    CHECK(scale.count == 2);
    CHECK(scale.entries[0].cents_x1000 == 100000);
    CHECK(scale.entries[1].cents_x1000 == 200000);
}

TEST_CASE("invalid count rejected") {
    // A count of 0 or negative is meaningless, since Scala files describe at least
    // one degree. A count above 64 would overrun the fixed entries[] buffer, so the
    // parser rejects it.
    Scale scale {};

    CHECK_FALSE(parse_cstr("desc\n0\n", &scale));
    CHECK_FALSE(parse_cstr("desc\n-1\n", &scale));
    CHECK_FALSE(parse_cstr("desc\n65\n", &scale));
}

TEST_CASE("malformed pitch rejected") {
    // Non-numeric entries, zero denominators, and zero numerators are all invalid
    // under the Scala spec, so the parser rejects them and never emits nonsense
    // retuning output.
    Scale scale {};

    // A non-numeric token makes fs_atoi() yield 0, which the num/den > 0 guard
    // rejects.
    CHECK_FALSE(parse_cstr("desc\n1\nabc\n", &scale));
    // Zero denominator (division by zero) is rejected.
    CHECK_FALSE(parse_cstr("desc\n1\n1/0\n", &scale));
    // A zero numerator is rejected, since log2(0) is -inf.
    CHECK_FALSE(parse_cstr("desc\n1\n0/1\n", &scale));
}

TEST_CASE("entry_to_cents_x1000 cents path") {
    // Cents-to-cents_x1000 is identity for the cents path, verify both a
    // mid-octave entry (100 cents = 100000 cx1000) and the octave boundary
    // (1200 cents = 1200000 cx1000).
    ScaleEntry entry {.is_cents = true, .cents_x1000 = 100000, .num = 0, .den = 0};
    CHECK(entry_to_cents_x1000(entry) == 100000);

    entry.cents_x1000 = 1200000;
    CHECK(entry_to_cents_x1000(entry) == 1200000);
}

TEST_CASE("entry_to_cents_x1000 ratio path") {
    // The perfect fifth 3/2 is log2(1.5) * 1200000 = 701955. The table-based
    // integer computation must be within +/-2 of the exact value.
    ScaleEntry entry {.is_cents = false, .cents_x1000 = 0, .num = 3, .den = 2};
    auto cents = entry_to_cents_x1000(entry);
    CHECK(cents >= 701953);
    CHECK(cents <= 701957);

    // The octave (2/1) is exactly 1200000 cents_x1000, an integer octave boundary.
    entry.num = 2;
    entry.den = 1;
    CHECK(entry_to_cents_x1000(entry) == 1200000);
}

// --- Ratio-to-cents and fixed-point parsing cases ---

TEST_CASE("cents 100.5 parses to cents_x1000 == 100500") {
    const char* payload = "Test\n"
                          "1\n"
                          " 100.5\n";

    Scale scale {};
    REQUIRE(parse_cstr(payload, &scale));
    CHECK(scale.entries[0].is_cents == true);
    CHECK(scale.entries[0].cents_x1000 == 100500);
}

TEST_CASE("ratio 3/2 returns cents_x1000 within tolerance of 701955") {
    // True log2(1.5) * 1200 * 1000 = 701955.0, the table + linear interp
    // must be within +/-2.
    ScaleEntry entry {.is_cents = false, .cents_x1000 = 0, .num = 3, .den = 2};
    auto cents = entry_to_cents_x1000(entry);
    CHECK(iabs(cents - 701955) <= 2);
}

TEST_CASE("ratio 2/1 returns cents_x1000 == 1200000 exactly") {
    // The integer octave is an exact table boundary, so it must be exactly 1200000.
    ScaleEntry entry {.is_cents = false, .cents_x1000 = 0, .num = 2, .den = 1};
    CHECK(entry_to_cents_x1000(entry) == 1200000);
}

TEST_CASE("ratio 1/1 (bare integer 1) parses to cents_x1000 == 0") {
    const char* payload = "Unison test\n"
                          "1\n"
                          " 1\n";

    Scale scale {};
    REQUIRE(parse_cstr(payload, &scale));
    CHECK(scale.entries[0].is_cents == false);
    CHECK(scale.entries[0].num == 1);
    CHECK(scale.entries[0].den == 1);
    CHECK(entry_to_cents_x1000(scale.entries[0]) == 0);
}

TEST_CASE("entry_to_cents_x1000 with direct cents entry") {
    ScaleEntry entry {.is_cents = true, .cents_x1000 = 50000, .num = 0, .den = 0};
    CHECK(entry_to_cents_x1000(entry) == 50000);
}

TEST_CASE("entry_to_cents_x1000 ratio 9/8 within tolerance of 203910") {
    // True log2(9/8) * 1200000 = 203910.0, must be within +/-2.
    ScaleEntry entry {.is_cents = false, .cents_x1000 = 0, .num = 9, .den = 8};
    auto cents = entry_to_cents_x1000(entry);
    CHECK(iabs(cents - 203910) <= 2);
}

TEST_CASE("negative cents -50.25 parses to cents_x1000 == -50250") {
    const char* payload = "Neg test\n"
                          "1\n"
                          " -50.25\n";

    Scale scale {};
    REQUIRE(parse_cstr(payload, &scale));
    CHECK(scale.entries[0].is_cents == true);
    CHECK(scale.entries[0].cents_x1000 == -50250);
}

TEST_CASE("parse_cents_x1000 handles truncation beyond 3 fractional digits") {
    // Input "1.2345" should yield 1234 (truncation at 3 digits, not rounding).
    // The Scala spec recommends 6 decimals but the integer path truncates at 3.
    const char* payload = "Truncation test\n"
                          "1\n"
                          " 1.2345\n";

    Scale scale {};
    REQUIRE(parse_cstr(payload, &scale));
    CHECK(scale.entries[0].is_cents == true);
    CHECK(scale.entries[0].cents_x1000 == 1234);
}
