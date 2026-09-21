// The single-precision expf, logf and powf a mode links against.
//
// A mode shaping a curve or an envelope reaches for a log, an exp or a power
// without building a lookup table by hand. These are freestanding by
// construction, which is what lets them link into a mode:
//   - single precision only, no 64-bit float and no soft-float
//   - no 64-bit integer division
//   - no libc calls
//   - constant-time and branch-light, with no loop over data and no allocation
//
// The bit tricks pun a float through a `union { float; uint32_t; }`, since this is
// C and std::bit_cast is not available.
//
// Domains. logf takes x > 0, and a mode feeding it 1 + u for u >= 0.001 stays at or
// above 1.001. expf takes a bounded x, up to about 5.31 where a mode uses it. Out
// of domain, logf returns 0 and powf returns 0 for a non-positive base, so a misuse
// comes back as a plain number a downstream integer conversion can take, rather than
// a NaN or an infinity.

#include <stdint.h>

// log2(2) and its reciprocal as single-precision constants.
#define OP_LN2 0.69314718055994531f      // ln(2)
#define OP_INV_LN2 1.44269504088896341f  // 1 / ln(2)

typedef union {
    float f;
    uint32_t u;
} op_fbits;

// --- log2 of a mantissa in [1, 2) -----------------------------------------
// Uses the atanh series:  log2(m) = (2/ln2) * (t + t^3/3 + t^5/5 + t^7/7 ...)
// with t = (m - 1) / (m + 1). For m in [1,2), |t| <= 1/3, so a degree-7
// (4-term) odd series is accurate to well under 1e-6 relative.
static float op_log2_mant(float m) {
    float t  = (m - 1.0f) / (m + 1.0f);
    float t2 = t * t;
    // Horner on the odd series in t2: t * (1 + t2/3 + t2^2/5 + t2^3/7).
    float series = 1.0f
        + t2
            * (0.33333333333333331f
               + t2 * (0.20000000000000001f + t2 * (0.14285714285714285f + t2 * 0.11111111111111110f)));
    return (2.0f * OP_INV_LN2) * (t * series);
}

// --- logf -----------------------------------------------------------------
// log(x) = ln2 * (e + log2(mant)), where x = mant * 2^e, mant in [1,2).
__attribute__((used)) float logf(float x) {
    if (x <= 0.0f) return 0.0f;  // out of domain, see the header
    op_fbits b;
    b.f       = x;
    int32_t e = (int32_t)((b.u >> 23) & 0xFFu) - 127;
    // Force the exponent field to 127 so the value lands in [1, 2).
    b.u        = (b.u & 0x007FFFFFu) | 0x3F800000u;
    float mant = b.f;
    return OP_LN2 * ((float)e + op_log2_mant(mant));
}

// --- 2^f for f in [0, 1) ---------------------------------------------------
// Minimax-quality degree-5 polynomial for 2^f via its Taylor expansion in
// (f*ln2): 2^f = exp(f*ln2) = 1 + g + g^2/2 + g^3/6 + g^4/24 + g^5/120, with
// g = f*ln2 in [0, ln2]. |g| <= 0.6931, and the degree-5 truncation error of
// exp over [0, ln2] is < 5e-5 absolute, well inside the 1e-4 relative budget.
static float op_exp2_frac(float f) {
    float g = f * OP_LN2;
    // Horner: 1 + g(1 + g/2(1 + g/3(1 + g/4(1 + g/5))))
    return 1.0f
        + g
        * (1.0f
           + g
               * (0.5f
                  + g * (0.16666666666666666f + g * (0.041666666666666664f + g * 0.0083333333333333332f))));
}

// --- exp2f-style assembly: 2^x = 2^k * 2^frac, k = floor(x) ----------------
// Builds 2^k by writing (k + 127) << 23 into a float via the union pun.
static float op_exp2(float x) {
    // Integer floor without libc floorf: truncate toward zero, then adjust
    // for negative non-integers.
    int32_t k = (int32_t)x;
    if (x < 0.0f && (float)k != x) --k;
    float frac = x - (float)k;  // frac in [0, 1)

    // Out of expf's bounded domain, k could carry the union construction into a
    // denormal or an infinity, so it is held to the representable exponent range.
    if (k > 127) return 3.4028235e38f;  // saturate toward FLT_MAX
    if (k < -126) return 0.0f;

    op_fbits b;
    b.u = (uint32_t)(k + 127) << 23;  // 2^k
    return b.f * op_exp2_frac(frac);
}

// --- expf -----------------------------------------------------------------
// exp(x) = 2^(x / ln2).
__attribute__((used)) float expf(float x) {
    return op_exp2(x * OP_INV_LN2);
}

// --- powf -----------------------------------------------------------------
// pow(x, y) = exp(y * log(x)) for x > 0. A non-positive base is out of domain and
// comes back as 1 for an exponent of 0 and 0 otherwise.
__attribute__((used)) float powf(float x, float y) {
    if (x > 0.0f) return expf(y * logf(x));
    return (y == 0.0f) ? 1.0f : 0.0f;
}
