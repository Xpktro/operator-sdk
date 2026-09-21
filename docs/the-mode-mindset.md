# The Mode Mindset {#the-mode-mindset}

A mode is not a normal program. It runs inside the device's real-time MIDI loop, so it plays by a few rules a desktop program does not. This page explains those rules and the reasoning behind them. Read it once before you write anything nontrivial and the constraints elsewhere in the docs stop feeling arbitrary.

## What is always true

- Your `process()` runs on every MIDI cycle, so it must be quick and must never wait or block.
- No dynamic allocation is permitted. There is no `new` and no `malloc`. Everything your mode works with must be declared at compile time with a fixed, known size, never allocated on demand while it runs.
- Math is single-precision and integer-first. Reach for `float`, not `double`, and lean on integers where you can.
- You talk to the device only through `op::api`. You never call the hardware or firmware directly.
- Loading your mode into two slots gives two independent copies. Each keeps its own state.

The rest of this page turns those into a concrete contract.

## Freestanding constraints {#freestanding-constraints}

Your mode runs on its own, without the C standard library beneath it. That is what keeps it small and its timing predictable, but it also puts a few common things off-limits. The SDK checks this for you at build time and fails fast if you slip, so you never have to memorize the list. You just need to know the shape of it and why it is there.

### Do not use

- **`double` or double-precision math.** The device's FPU is single-precision only. Use `float` for fractional math, or (better still) integer fixed-point.
- **C library functions** such as `sin`, `pow`, `sqrt`, `atoi`, `strcmp`, `sprintf`, and `malloc`, from `<cmath>`, `<cstdlib>`, `<cstring>`, and `<cstdio>`. Reach for an SDK helper, a small hand-written function, or a precomputed lookup table instead.
- **64-bit integer division.** The device divides 32-bit integers in hardware but has no 64-bit divide. Redesign the math to stay 32-bit.

### Safe to use

- **`float` arithmetic.** Hardware-accelerated, typically single-cycle.
- **Integer arithmetic** at any width, with the single exception of 64-bit _division_. A 64-bit multiply is fine.
- **`memcpy`, `memset`, `memmove`.** The SDK provides these.
- **`expf`, `logf`, `powf`.** The SDK provides single-precision versions for curves, envelopes, and gain math. `logf(x)` expects `x > 0` and returns `0.0f` for `x <= 0`. `powf(x, y)` is defined for `x > 0`. Their `double` and base-2 cousins (`exp`, `log`, `pow`, `log2f`, and so on) are still off-limits.
- **Any SDK helper** in `operator_sdk/*.h`. These are safe here by construction.

### When you need a curve or a transcendental

Use a precomputed lookup table with linear interpolation. Tables of 32 to 257 entries are typical, and the samples show the pattern:

- `samples/custom-scale-tuner/scala.h` builds a log2 table for ratio-to-cents conversion.
- `samples/cc-lfo/main.cpp` uses a pure 32-bit integer phase accumulator, no float at all.
- `samples/delay/main.cpp` keeps its timing math to 32-bit integers with a small constexpr table.

### A few worked substitutions

**Fixed-point cents instead of `double` ratios.** Integer math gives you sub-cent precision:

```cpp
// Avoid: pulls in double math and log2
double cents = 1200.0 * log2((double)num / (double)den);

// Prefer: integer cents_x1000 via a precomputed log2 table (see scala.h)
int32_t cents_x1000 = ratio_to_cents_x1000(num, den);
```

**Integer pitch-bend math instead of float cents.** For 14-bit MIDI pitch bend in a bounded range:

```cpp
// Avoid: emits double math and lround
int16_t bend = 8192 + (int16_t)lround(cents * 8192.0 / bend_range_cents);

// Prefer: integer clamp plus factor reduction, all int32
int16_t bend = cents_to_pitch_bend_14bit(cents_x1000, bend_range_cents);
```

**A hand-rolled string compare instead of `strcmp`.** For UI label dispatch:

```cpp
// A 10-line static helper, see samples/midi-player/main.cpp
static bool fs_str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
if (fs_str_eq(label, "Play")) { /* ... */ }
```

### Why the check exists

It is easy to build a mode that runs fine on a desktop test and is quietly broken for the device, because a desktop toolchain provides all of the above silently and the device's does not. The SDK runs a build-time check so the failure surfaces the moment it happens, with a message that names the offending symbol. You do not have to remember the rule. The check remembers it for you.

## Per-instance state {#per-instance-memory}

When the same mode is loaded into two slots (for example two arpeggiators), the device gives each copy its own private state. They never share it. That isolation is guaranteed for you, and it shapes how you should store state:

- Keep mutable per-slot state in your mode's ordinary variables. Each instance gets its own copy automatically.
- Mark constants (lookup tables, string literals) `const` or `constexpr`. Constants are shared across instances, so they cost RAM only once no matter how many copies load.
- Never rely on one instance seeing another's changes. Treat every instance as if it were the only one.

You do not manage any of this yourself. The SDK's build settings and the device's loader handle it, and a build check verifies your mode was compiled the right way. If you build a mode outside the SDK's own toolchain and skip those settings, the check catches it before the mode ever reaches a device.

## See also

- [Mode Lifecycle](mode-lifecycle.md): when each hook runs and on which core.
- [Timing and Clock](timing-and-clock.md): deriving a smooth clock position without `double`.
- [Storage and Files](storage-and-files.md): the write discipline behind a saved bulk dump.
