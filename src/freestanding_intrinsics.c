// The memory and division intrinsics a mode links against.
//
// GCC emits calls to memmove, memset and memcpy for aggregate copies and
// initializations, and __aeabi_uldivmod for 64-bit division on 32-bit ARM. A mode
// links with -nostdlib, so these are unresolved unless the SDK carries them, and
// linking libgcc for one of them risks pulling in the whole soft-float tower a
// freestanding mode must not have. This file is linked into every .opm and is not
// something a mode calls itself.

#include <stddef.h>
#include <stdint.h>

// --- memcpy ---------------------------------------------------------------
// Non-overlapping byte copy, returning dst. A byte at a time is enough, since GCC
// emits memcpy for the small copies it did not unroll. The `used` attribute keeps
// it through the optimizer, which cannot see the call the compiler synthesized.
__attribute__((used)) void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d       = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}

// --- memset ---------------------------------------------------------------
// Writes `c & 0xFF` to n bytes at dst. Returns dst.
__attribute__((used)) void* memset(void* dst, int c, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    unsigned char v  = (unsigned char)c;
    for (size_t i = 0; i < n; ++i) d[i] = v;
    return dst;
}

// --- memmove --------------------------------------------------------------
// Overlap-safe byte copy. Picks forward or backward direction based on
// pointer ordering.
__attribute__((used)) void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d       = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (size_t i = 0; i < n; ++i) d[i] = s[i];
    } else {
        for (size_t i = n; i != 0; --i) d[i - 1] = s[i - 1];
    }
    return dst;
}

// --- __aeabi_uldivmod -----------------------------------------------------
// 64-bit unsigned division and modulo, which the compiler emits at every 64-bit
// divide and remainder. The ARM EABI helper returns its result in registers:
//
//     in:  r0:r1 = num (low:high)
//          r2:r3 = den (low:high)
//     out: r0:r1 = quot
//          r2:r3 = rem
//     callee-saved: r4-r8, r10, r11 must be preserved
//
// A plain C function cannot express that. One returning a 16-byte struct lowers
// under AAPCS to "caller allocates the output, passes its pointer in r0, the real
// args shift along by one", which is a different ABI from the register-return
// helper and faults when the compiler calls it as the helper.
//
// So __aeabi_uldivmod is a naked Thumb-2 trampoline that marshals the helper's
// registers into a C long-division helper with an out-pointer signature, which has
// a plain-C lowering and so runs both on the host (for the algorithm test) and on
// ARM (as the trampoline's target). A POST_BUILD gate (check_uldivmod_abi.py)
// disassembles the symbol in each mode and rejects the struct-return shape.

// The long-division helper. Its out-pointer signature is host-portable, and the ARM
// trampoline marshals the helper-ABI registers into it.
//
// Under OPERATOR_SDK_HOST_TEST it is non-static, so the host test can reach it;
// otherwise it is local to this file. `used` either way, because on ARM its only
// caller is the trampoline's inline-asm `bl`, which the optimizer cannot see, so
// without it the helper could be stripped and the asm would not link.
#ifndef OPERATOR_SDK_HOST_TEST
static
#endif
    __attribute__((used)) void
    op_uldivmod_impl(uint64_t* quot_out, uint64_t* rem_out, uint64_t num, uint64_t den) {
    if (den == 0) {
        *quot_out = 0;
        *rem_out  = 0;
        return;
    }
    uint64_t quot = 0;
    uint64_t rem  = 0;
    for (int i = 63; i >= 0; --i) {
        rem = (rem << 1) | ((num >> i) & 1u);
        if (rem >= den) {
            rem -= den;
            quot |= (uint64_t)1 << i;
        }
    }
    *quot_out = quot;
    *rem_out  = rem;
}

#if defined(__arm__) && !defined(__aarch64__)
// Naked Thumb-2 trampoline implementing the real ARM EABI helper ABI.
// 32-bit ARM only. The host (including macOS Apple Silicon, which
// defines __aarch64__) compiles only the C helper above and the host
// test calls it directly via OPERATOR_SDK_HOST_TEST=1.
//
// On entry:
//     r0:r1 = num,  r2:r3 = den
// AAPCS-32 lowering for op_uldivmod_impl(q*, r*, uint64_t, uint64_t):
//     r0      = &quot   (1st arg, pointer)
//     r1      = &rem    (2nd arg, pointer)
//     r2:r3   = num     (3rd arg, 64-bit; r2 is even reg, valid pair)
//     stack[0:7] = den  (4th arg, 64-bit, 8-byte aligned at sp+0)
//
// Stack frame (16-byte register save + 24-byte scratch = 40 bytes total;
// sp 8-aligned throughout, assumed 8-aligned on entry per AAPCS-32
// public-interface contract):
//
//     push {r4, r5, r6, lr}      (16B; r6 is alignment pad; r4,r5 hold num)
//     sub  sp, sp, #24           (24B scratch:
//                                  [sp+ 0 .. sp+ 7] = den stack-arg slab
//                                  [sp+ 8 .. sp+15] = quot scratch
//                                  [sp+16 .. sp+23] = rem scratch)
//     ...                        (set up args, call helper)
//     ldrd r0, r1, [sp,#8]       (return quot in r0:r1)
//     ldrd r2, r3, [sp,#16]      (return rem  in r2:r3)
//     add  sp, sp, #24
//     pop  {r4, r5, r6, pc}
__attribute__((naked, used)) void __aeabi_uldivmod(void) {
    __asm__ volatile("push   {r4, r5, r6, lr}      \n"  // save callee-saved + lr (16B, sp stays 8-aligned)
                     "sub    sp, sp, #24           \n"  // allocate {den_stack[8], quot[8], rem[8]}
                     "mov    r4, r0                \n"  // r4 = num_lo  (preserve across pointer setup)
                     "mov    r5, r1                \n"  // r5 = num_hi
                     "str    r2, [sp, #0]          \n"  // stack arg 4 lo = den_lo
                     "str    r3, [sp, #4]          \n"  // stack arg 4 hi = den_hi
                     "add    r0, sp, #8            \n"  // arg 1 = &quot
                     "add    r1, sp, #16           \n"  // arg 2 = &rem
                     "mov    r2, r4                \n"  // arg 3 lo = num_lo
                     "mov    r3, r5                \n"  // arg 3 hi = num_hi
                     "bl     op_uldivmod_impl      \n"
                     "ldrd   r0, r1, [sp, #8]      \n"  // return quot in r0:r1
                     "ldrd   r2, r3, [sp, #16]     \n"  // return rem  in r2:r3
                     "add    sp, sp, #24           \n"
                     "pop    {r4, r5, r6, pc}      \n");
}
#endif
