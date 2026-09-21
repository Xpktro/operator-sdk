#pragma once
// Header-only Scala `.scl` microtonal-scale parser for the Custom Scale Tuner
// sample mode.
//
// The Scala format (Huygens-Fokker,
// https://www.huygens-fokker.org/scala/scl_format.html) is the lingua franca
// of microtonal scale exchange. A .scl file declares a single octave's worth
// of pitch degrees either as cents-from-1/1 or as rational ratios (`num/den`
// or bare integer -> `N/1`). The implicit 1/1 base is not stored, so
// `entries[0]` is the first non-base degree.
//
// This parser sits alongside the mode's main.cpp and is consumed with
// `#include "scala.h"`. It is intentionally dependency-light, with no heap and no
// C++ stream headers, so the same translation unit compiles cleanly on both the
// host test harness and the device target.
//
// Robustness posture.
//   - UTF-8 BOM and CRLF line endings must be transparently
//     accepted. Many real-world .scl files are edited on Windows or carry
//     a leading BOM from text editors.
//   - Never crash on malformed input. The parser returns `false` on any
//     bounds/validation failure and leaves `*out` in an unspecified-but-safe
//     state (the caller should treat it as invalid).
//   - Fixed-size buffers throughout, 64 degrees max and a 256-byte line cap. A
//     pathological file with 10 000-entry count or 8 KB lines is rejected
//     at the structural level.
//
// Non-goals are locale-aware numeric parsing (the parsers are hand-rolled and
// integer-only), comment-position enforcement (the parser skips '!' lines
// anywhere, not just at the documented head-of-file position), and octave-closure
// verification (a .scl that does not reach 1200 cents or 2/1 is legal, since
// microtonal scales may be non-octave-repeating).
//
// All scale-entry arithmetic operates on integer cents x 1000 (int32_t), and
// ratio-to-cents conversion uses a precomputed 257-entry log2 table with linear
// interpolation, delivering +/-2 cents_x1000 accuracy. The file uses no floating
// point and no libc.

#include <cstddef>  // size_t
#include <cstdint>

namespace op::samples::scala {

// ---------------------------------------------------------------------------
// Storage limits.
// ---------------------------------------------------------------------------
// A Scale carries at most kMaxDegrees pitch entries plus a description line. The
// 64-degree cap covers every Scala file in the surveyed Huygens-Fokker archive.
// Pathological files (say a 1000-degree experimental tuning) are rejected at
// parse time, and callers log "BAD SCALE FILE" and keep passing MIDI through.
constexpr uint8_t kMaxDegrees = 64;
// The spec is silent on maximum line length, so this cap is generous enough for
// any realistic pitch entry ("123456/789012    ! inline comment") while still
// bounding the denial-of-service surface.
constexpr std::size_t kMaxLineBytes = 256;
// Description storage is a 63-byte name plus a NUL. Scala's own spec suggests
// about 40 bytes, and 64 gives comfortable room without bloating the Scale
// struct.
constexpr std::size_t kDescriptionBytes = 64;

// ---------------------------------------------------------------------------
// Hand-rolled parse helpers (detail namespace).
// ---------------------------------------------------------------------------
// Small integer and string parsers (std::atoi / std::strchr / std::atof
// equivalents). None allocate, all are noexcept by construction. Placed in a
// detail namespace so the names don't pollute the public API.

namespace detail {

    // Signed integer parse. Stops at first non-digit after optional sign.
    // Returns 0 on empty / non-numeric input (matches std::atoi contract).
    inline int32_t fs_atoi(const char* cursor) noexcept {
        if (!cursor) return 0;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        bool negative = false;
        if (*cursor == '-') {
            negative = true;
            ++cursor;
        } else if (*cursor == '+') {
            ++cursor;
        }
        int32_t accumulator = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            accumulator = accumulator * 10 + (*cursor - '0');
            ++cursor;
        }
        return negative ? -accumulator : accumulator;
    }

    // Locate the first occurrence of `target` in `text` (NUL-terminated).
    // Returns null if absent. Matches std::strchr semantics for ASCII input.
    inline const char* fs_strchr(const char* text, int target) noexcept {
        if (!text) return nullptr;
        const char ch = static_cast<char>(target);
        while (*text) {
            if (*text == ch) return text;
            ++text;
        }
        return (ch == '\0') ? text : nullptr;
    }

    // Parse a decimal cents value like "100.5" or "-50.25" or "1234" into
    // integer cents x 1000 via a fixed-point parse.
    // It accepts up to 3 fractional digits and truncates the rest. The Scala spec
    // recommends 6 decimal places, but real-world .scl files use 2 to 5, and
    // truncating at 3 keeps every plausible value inside int32.
    inline int32_t parse_cents_x1000(const char* cursor) noexcept {
        if (!cursor) return 0;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        bool negative = false;
        if (*cursor == '-') {
            negative = true;
            ++cursor;
        } else if (*cursor == '+') {
            ++cursor;
        }

        // Integer part.
        int64_t whole = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            whole = whole * 10 + (*cursor - '0');
            ++cursor;
        }
        // Fractional part, up to 3 digits, padded with zeros if fewer.
        int64_t frac        = 0;
        int32_t frac_digits = 0;
        if (*cursor == '.') {
            ++cursor;
            while (frac_digits < 3 && *cursor >= '0' && *cursor <= '9') {
                frac = frac * 10 + (*cursor - '0');
                ++cursor;
                ++frac_digits;
            }
            // Skip (truncate) any extra fractional digits.
            while (*cursor >= '0' && *cursor <= '9') ++cursor;
        }
        while (frac_digits < 3) {
            frac *= 10;
            ++frac_digits;
        }

        int64_t value = whole * 1000 + frac;
        if (negative) value = -value;
        // Clamp to int32 range defensively, microtonal cents fit in int32
        // by many orders of magnitude (1 period = 1'200'000 cents_x1000).
        if (value > 2'147'483'647LL) value = 2'147'483'647LL;
        if (value < -2'147'483'648LL) value = -2'147'483'648LL;
        return static_cast<int32_t>(value);
    }

    // ---------------------------------------------------------------------------
    // Ratio -> cents_x1000.
    // ---------------------------------------------------------------------------
    // Computes 1200000 * log2(num/den) as a signed int32 for any num,den in
    // [1, 10^6]. The implementation uses a 257-entry precomputed table of
    // log2(1 + k/256) * 1200 * 1000 for k in [0, 256], then reduces the ratio
    // to a mantissa in [1.0, 2.0) via bit shifts and interpolates linearly
    // between adjacent table entries.
    //
    // Linear interp over a 257-entry log2 table has a worst-case error of about
    // (log2 second derivative * step^2 / 8). With step = 1/256, the max error is
    // about 1.13e-6 octaves, or 1.36 millicents, well inside the +/-2 tolerance
    // this advertises to retune.h. Integer arithmetic throughout.

    // 257 entries covering k = 0..256 inclusive. Value at k is
    // round(log2(1 + k/256) * 1200 * 1000). Generated offline with
    //   import math
    //   vals = [round(math.log2(1 + k/256) * 1_200_000) for k in range(257)]
    //
    // Sentinels are k=0 -> 0, k=128 -> 701955, and k=256 -> 1200000.
    constexpr int32_t kLog2CentsX1000Table[257] = {
        0,       6749,    13473,   20170,   26841,   33487,   40108,   46703,   53273,   59818,   66339,
        72835,   79307,   85755,   92179,   98579,   104955,  111309,  117638,  123945,  130229,  136491,
        142729,  148946,  155140,  161312,  167462,  173590,  179697,  185782,  191846,  197888,  203910,
        209911,  215891,  221850,  227789,  233708,  239607,  245485,  251344,  257183,  263002,  268802,
        274582,  280344,  286086,  291809,  297513,  303199,  308865,  314514,  320144,  325756,  331349,
        336925,  342483,  348023,  353545,  359050,  364537,  370007,  375460,  380895,  386314,  391715,
        397100,  402468,  407820,  413155,  418474,  423776,  429062,  434333,  439587,  444825,  450047,
        455254,  460445,  465621,  470781,  475926,  481055,  486170,  491269,  496354,  501423,  506478,
        511518,  516543,  521554,  526550,  531532,  536500,  541453,  546393,  551318,  556229,  561127,
        566010,  570880,  575736,  580579,  585408,  590224,  595026,  599815,  604591,  609354,  614103,
        618840,  623564,  628274,  632972,  637658,  642330,  646991,  651638,  656273,  660896,  665507,
        670105,  674691,  679265,  683827,  688377,  692915,  697441,  701955,  706458,  710948,  715428,
        719895,  724352,  728796,  733230,  737652,  742063,  746462,  750851,  755228,  759594,  763950,
        768294,  772627,  776950,  781262,  785563,  789854,  794134,  798403,  802662,  806910,  811148,
        815376,  819594,  823801,  827998,  832184,  836361,  840528,  844684,  848831,  852968,  857095,
        861212,  865319,  869417,  873505,  877583,  881652,  885711,  889760,  893801,  897831,  901853,
        905865,  909868,  913861,  917846,  921821,  925787,  929744,  933693,  937632,  941562,  945483,
        949395,  953299,  957194,  961080,  964957,  968826,  972686,  976537,  980380,  984215,  988041,
        991858,  995667,  999468,  1003260, 1007045, 1010820, 1014588, 1018348, 1022099, 1025842, 1029577,
        1033304, 1037023, 1040734, 1044438, 1048133, 1051820, 1055500, 1059172, 1062836, 1066492, 1070140,
        1073781, 1077415, 1081040, 1084658, 1088269, 1091872, 1095467, 1099055, 1102636, 1106209, 1109775,
        1113334, 1116885, 1120429, 1123966, 1127495, 1131017, 1134533, 1138041, 1141542, 1145036, 1148522,
        1152002, 1155475, 1158941, 1162400, 1165852, 1169298, 1172736, 1176167, 1179592, 1183010, 1186422,
        1189826, 1193224, 1196615, 1200000};
    static_assert(kLog2CentsX1000Table[0] == 0, "log2 table k=0 sentinel");
    static_assert(kLog2CentsX1000Table[128] == 701955, "log2 table k=128 sentinel (3/2)");
    static_assert(kLog2CentsX1000Table[256] == 1200000, "log2 table k=256 sentinel");

    // Compute 1'200'000 * log2(num/den) as int32_t.
    // Precondition is num >= 1 and den >= 1.
    //
    // Divides cast to uint64/uint64, since the signed 64-bit divide helper is not
    // available and every operand reaching the divide here is non-negative by
    // construction (num and den are > 0 at entry, n, d and delta all stay >= 0
    // through normalization, and b >= a for adjacent log2 table entries), so the
    // unsigned form is semantics-preserving.
    inline int32_t ratio_to_cents_x1000(int32_t num, int32_t den) noexcept {
        if (num <= 0 || den <= 0) return 0;
        // Reduce num/den to a mantissa m in [1, 2) with integer octave shift.
        // We track "octaves" separately to keep the mantissa tightly bounded.
        int32_t octaves = 0;
        uint64_t n      = static_cast<uint64_t>(num);
        uint64_t d      = static_cast<uint64_t>(den);
        // Normalize by shifting d up until n < 2*d, or n up until n >= d.
        while (n >= 2u * d) {
            d <<= 1;
            ++octaves;
        }
        while (n < d) {
            n <<= 1;
            --octaves;
        }
        // Now d <= n < 2d, and the 8-bit fractional mantissa index is
        //   frac256 = floor((n - d) * 256 / d)  in [0, 255]
        const uint64_t delta        = n - d;
        const uint64_t frac256_full = delta * 256u;
        const int32_t frac256       = static_cast<int32_t>(frac256_full / d);
        const uint64_t frac_rem     = frac256_full - static_cast<uint64_t>(frac256) * d;
        // Linear interp between table[frac256] and table[frac256+1].
        // Both table entries are positive (monotonic from 0 up to 1'200'000)
        // and b >= a for adjacent entries, so (b - a) is representable as
        // unsigned for the subsequent multiply/divide.
        const uint64_t a      = static_cast<uint64_t>(kLog2CentsX1000Table[frac256]);
        const uint64_t b      = static_cast<uint64_t>(kLog2CentsX1000Table[frac256 + 1]);
        const uint64_t interp = a + ((b - a) * frac_rem) / d;
        return static_cast<int32_t>(octaves * 1'200'000LL + static_cast<int64_t>(interp));
    }

}  // namespace detail

// ---------------------------------------------------------------------------
// Scale data model.
// ---------------------------------------------------------------------------

// A single pitch degree with two mutually exclusive encodings, where `is_cents`
// selects which set of fields is meaningful.
//   `is_cents == true`  -> `cents_x1000` in integer cents x 1000 above 1/1.
//   `is_cents == false` -> ratio `num/den` (both > 0 per spec).
//
// The caller uses `entry_to_cents_x1000(entry)` to collapse both
// representations onto a single integer-cents-x1000 axis for retuning arithmetic.
struct ScaleEntry {
    bool is_cents;
    int32_t cents_x1000;  // integer cents x 1000 (sub-millicent precision)
    int32_t num;          // used when is_cents == false
    int32_t den;
};

// Parsed representation of a complete Scala file. `count` degrees are
// populated in `entries[0 .. count-1]`, the remaining slots are undefined
// (caller must not read them). The implicit 1/1 base is not stored.
struct Scale {
    char description[kDescriptionBytes];
    uint8_t count;
    ScaleEntry entries[kMaxDegrees];
};

// ---------------------------------------------------------------------------
// Parser.
// ---------------------------------------------------------------------------
// `parse` consumes the entire byte payload of a .scl file and populates the
// caller-provided `Scale`. It is a single function (no class, no state) to
// keep the surface trivially analyzable and to make header-only inclusion
// in multiple TUs free from ODR headaches.
//
// Returns true on success, any bounds/format failure returns false. On
// false the partially-populated Scale is unsafe to use, so treat it as completely
// invalid and fall back to passing MIDI through.

// Parse a Scala `.scl` payload of exactly `size` bytes starting at `bytes`,
// write the parsed scale into `*out`, and return true on success.
// `bytes` is the raw file content (not NUL-terminated, `size` is authoritative),
// `size` is the total byte length of the payload, and `out` is the non-null
// destination for the parsed scale.
inline bool parse(const char* bytes, std::size_t size, Scale* out) {
    if (!bytes || !out) return false;

    // --- BOM strip ---------------------------------------------------------
    // Many Windows editors prepend a UTF-8 BOM (0xEF 0xBB 0xBF). The rest
    // of the file is pure ASCII per spec, so dropping three bytes once
    // upfront is both necessary and sufficient.
    std::size_t i = 0;
    if (size >= 3 && static_cast<uint8_t>(bytes[0]) == 0xEF && static_cast<uint8_t>(bytes[1]) == 0xBB
        && static_cast<uint8_t>(bytes[2]) == 0xBF) {
        i = 3;
    }

    // --- Line reader -------------------------------------------------------
    // Reads the next non-comment, non-empty line into `line_out` (NUL-
    // terminated). Returns false only on end-of-buffer with no more lines.
    //
    // Line terminator handling accepts `\n`, `\r\n`, and the old
    // Mac-style bare `\r`. Internally we scan to the next `\r` or
    // `\n`, strip a trailing `\r` from the raw segment (covers `\r\n` and
    // bare `\r`), and advance past the terminator(s) in a way that won't
    // mis-count `\r\n` as two line breaks.
    auto read_line = [&](char* line_out) -> bool {
        while (i < size) {
            const std::size_t start = i;
            while (i < size && bytes[i] != '\n' && bytes[i] != '\r') ++i;
            std::size_t len = i - start;
            // Strip trailing '\r' from the raw segment. Handles both bare
            // '\r' (old Mac) and the '\r' half of a '\r\n' pair once the
            // '\n' branch below advances past the '\n'.
            if (len > 0 && bytes[start + len - 1] == '\r') --len;

            // Advance past the terminator byte(s). A '\r' followed by '\n' is
            // one line break, so consume both bytes here and the next call
            // starts on the following line.
            if (i < size && bytes[i] == '\r') {
                ++i;
                if (i < size && bytes[i] == '\n') ++i;
            } else if (i < size && bytes[i] == '\n') {
                ++i;
            }

            // Comment lines start with '!' and are skipped at any position.
            if (len > 0 && bytes[start] == '!') continue;

            // Copy the surviving segment (possibly empty, since the spec permits
            // an empty description line) into the caller's buffer. Cap at
            // kMaxLineBytes - 1 to leave room for the NUL.
            const std::size_t copy = (len < kMaxLineBytes - 1) ? len : (kMaxLineBytes - 1);
            // Copy byte-by-byte so the compiler emits no memcpy call, which
            // would require libc on the ARM target.
            for (std::size_t j = 0; j < copy; ++j) {
                line_out[j] = bytes[start + j];
            }
            line_out[copy] = '\0';
            return true;
        }
        return false;
    };

    char line[kMaxLineBytes];

    // --- Line 1, the description -----------------------------------------
    // The first non-comment line is the human-readable description. An
    // empty description is legal, we store an empty string in that case.
    if (!read_line(line)) return false;
    // Compute length bounded by kDescriptionBytes - 1. We don't use
    // `strnlen` because it is a POSIX extension not guaranteed to live in `std::`
    // on all toolchains. The line buffer is already NUL-terminated by `read_line`,
    // so an explicit clamp loop is both portable and clear.
    std::size_t description_length = 0;
    while (description_length < kDescriptionBytes - 1 && line[description_length] != '\0')
        ++description_length;
    // Byte loop again, for the same no-memcpy reason as read_line.
    for (std::size_t j = 0; j < description_length; ++j) {
        out->description[j] = line[j];
    }
    out->description[description_length] = '\0';

    // --- Line 2, the degree count ----------------------------------------
    // fs_atoi returns 0 on non-numeric content. Any value
    // outside 1..kMaxDegrees is a structural rejection, since a malformed count
    // must not overflow the fixed entries[] buffer.
    if (!read_line(line)) return false;
    const int degree_count = detail::fs_atoi(line);
    if (degree_count <= 0 || degree_count > kMaxDegrees) return false;
    out->count = degree_count;

    // --- Line 3 onward, the pitch entries ---------------------------------
    // Each line declares one degree. The spec says a pitch containing '.' is
    // cents (float) and anything else is a ratio, with a bare integer `N` as
    // shorthand for `N/1`.
    //
    // The spec says anything after a valid pitch value should be ignored, so
    // trailing content (inline comments, formatting notes) is allowed. The token
    // scan stops at the first space or tab after the numeric payload, so fs_atoi
    // and parse_cents_x1000 see only the pitch token.
    for (uint8_t k = 0; k < out->count; ++k) {
        if (!read_line(line)) return false;

        // Skip leading whitespace, since real files commonly indent pitch values.
        const char* cursor = line;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor == '\0') return false;  // empty line inside entries block

        // Classify by content. A '.' means cents, a '/' means a ratio, and a bare
        // digit run is an integer ratio with an implicit /1.
        bool has_dot   = false;
        bool has_slash = false;
        for (const char* scan = cursor; *scan && *scan != ' ' && *scan != '\t'; ++scan) {
            if (*scan == '.')
                has_dot = true;
            else if (*scan == '/')
                has_slash = true;
        }

        if (has_dot) {
            // parse_cents_x1000 handles the fixed-point parse. The .scl spec
            // fixes '.' as the decimal separator, and Scala files are ASCII.
            out->entries[k] = ScaleEntry {
                /*is_cents*/ true,
                /*cents_x1000*/ detail::parse_cents_x1000(cursor),
                /*num*/ 0,
                /*den*/ 0,
            };
            // The Scala spec allows negative cents (descending scales), so a
            // negative cents_x1000 is accepted.
            (void)has_slash;
        } else {
            // A ratio is num (required) plus an optional /den, and a bare integer
            // means /1.
            const int num = detail::fs_atoi(cursor);
            int den       = 1;
            if (const char* slash = detail::fs_strchr(cursor, '/'); slash != nullptr) {
                den = detail::fs_atoi(slash + 1);
            }
            // Reject zero denominator (undefined division) and zero/negative
            // numerator (log2 of 0 or negative is invalid).
            if (num <= 0 || den <= 0) return false;
            out->entries[k] = ScaleEntry {
                /*is_cents*/ false,
                /*cents_x1000*/ 0,
                /*num*/ num,
                /*den*/ den,
            };
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Pitch math.
// ---------------------------------------------------------------------------

// Convert a parsed `ScaleEntry` to its integer cents x 1000 offset from the 1/1
// base degree. The cents path returns `cents_x1000` directly and the ratio path
// returns `ratio_to_cents_x1000(num, den)`.
//
// A perfect fifth (3/2) returns 701955 (= log2(1.5) * 1200 * 1000).
// Callers can convert this to MIDI note + pitch bend, that is a downstream
// concern (see retune.h).
inline int32_t entry_to_cents_x1000(const ScaleEntry& entry) noexcept {
    return entry.is_cents ? entry.cents_x1000 : detail::ratio_to_cents_x1000(entry.num, entry.den);
}

}  // namespace op::samples::scala
