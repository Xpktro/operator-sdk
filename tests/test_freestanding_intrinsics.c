// Unit test for the intrinsics shim. The shim's memcpy, memset and memmove are
// renamed here so they do not collide with the host libc at link time.
//
// __aeabi_uldivmod is ARM-only, so what runs on the host is its long-division
// helper, op_uldivmod_impl, which the shim exposes under OPERATOR_SDK_HOST_TEST.
// The trampoline that marshals the helper ABI into it runs on ARM, where a
// POST_BUILD gate checks its shape.

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

void* op_test_memcpy(void* dst, const void* src, size_t n);
void* op_test_memset(void* dst, int c, size_t n);
void* op_test_memmove(void* dst, const void* src, size_t n);

// The long-division helper the ARM trampoline calls into, exposed under
// OPERATOR_SDK_HOST_TEST so the host test can exercise it directly.
void op_uldivmod_impl(uint64_t* quot_out, uint64_t* rem_out, uint64_t num, uint64_t den);

static int failures = 0;
#define EXPECT(cond, label)                                                                                  \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__);                                        \
            ++failures;                                                                                      \
        }                                                                                                    \
    } while (0)

int main(void) {
    // memcpy: non-overlapping
    {
        char src[8] = "ABCDEFGH";
        char dst[9] = {0};
        op_test_memcpy(dst, src, 8);
        EXPECT(strcmp(dst, "ABCDEFGH") == 0, "memcpy basic");
    }
    // memset: fill with 0x42
    {
        char buf[4] = {0};
        op_test_memset(buf, 0x42, 4);
        EXPECT(buf[0] == 0x42 && buf[1] == 0x42 && buf[2] == 0x42 && buf[3] == 0x42, "memset fill");
    }
    // memmove: overlap, dst > src (backward path)
    {
        char buf[8] = "ABCDEFGH";
        op_test_memmove(buf + 2, buf, 6);
        EXPECT(memcmp(buf, "ABABCDEF", 8) == 0, "memmove backward overlap");
    }
    // memmove: overlap, dst < src (forward path)
    {
        char buf[8] = "ABCDEFGH";
        op_test_memmove(buf, buf + 2, 6);
        EXPECT(memcmp(buf, "CDEFGHGH", 8) == 0, "memmove forward overlap");
    }
    // memmove: n == 0 is a no-op
    {
        char buf[4] = "ABCD";
        op_test_memmove(buf + 1, buf, 0);
        EXPECT(memcmp(buf, "ABCD", 4) == 0, "memmove n=0 noop");
    }
    // memmove: dst == src is a no-op
    {
        char buf[4] = "WXYZ";
        op_test_memmove(buf, buf, 4);
        EXPECT(memcmp(buf, "WXYZ", 4) == 0, "memmove dst==src noop");
    }

    // -----------------------------------------------------------------
    // op_uldivmod_impl: the long-division helper, at its boundaries.
    // -----------------------------------------------------------------
    {
        uint64_t q, r;
        // case 1: small / small
        op_uldivmod_impl(&q, &r, (uint64_t)7, (uint64_t)3);
        EXPECT(q == 2 && r == 1, "uldivmod 7/3");

        // case 2: large / small  (1<<40) / 7
        //   1099511627776 / 7 = 157073089682 rem 2  (Python: divmod(1<<40, 7))
        op_uldivmod_impl(&q, &r, (uint64_t)1 << 40, (uint64_t)7);
        EXPECT(q == 157073089682ull && r == 2, "uldivmod (1<<40)/7");

        // case 3: small / large  5 / (1<<40)
        op_uldivmod_impl(&q, &r, (uint64_t)5, (uint64_t)1 << 40);
        EXPECT(q == 0 && r == 5, "uldivmod 5/(1<<40)");

        // case 4: divide-by-zero comes back as {0, 0}
        op_uldivmod_impl(&q, &r, (uint64_t)42, (uint64_t)0);
        EXPECT(q == 0 && r == 0, "uldivmod 42/0 deterministic");

        // case 5: UINT64_MAX / 2
        op_uldivmod_impl(&q, &r, (uint64_t)0xFFFFFFFFFFFFFFFFull, (uint64_t)2);
        EXPECT(q == 0x7FFFFFFFFFFFFFFFull && r == 1, "uldivmod UINT64_MAX/2");

        // case 6: UINT64_MAX / UINT64_MAX
        op_uldivmod_impl(&q, &r, (uint64_t)0xFFFFFFFFFFFFFFFFull, (uint64_t)0xFFFFFFFFFFFFFFFFull);
        EXPECT(q == 1 && r == 0, "uldivmod UINT64_MAX/UINT64_MAX");

        // case 7: n / 1
        op_uldivmod_impl(&q, &r, (uint64_t)123456789ull, (uint64_t)1);
        EXPECT(q == 123456789ull && r == 0, "uldivmod 123456789/1");
    }

    return failures == 0 ? 0 : 1;
}
