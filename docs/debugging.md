# Debugging {#debugging}

Watch the MIDI your mode moves, and decode the build errors you are most likely to hit.

## Watching your mode's MIDI

The device has a built-in MIDI monitor screen. By design it reflects the device's **routing I/O**, the input messages the routing layer ingests and the output messages it emits as they pass through. Use it to confirm that notes and CCs your mode reacts to are actually arriving, and that the messages your mode produces are leaving on the expected output.

Two things to keep in mind so the monitor matches your expectations:

- **It is a routing view, not a complete trace of every byte your mode generates.** What you emit through `api->send_midi` is shown as it leaves on the device's outputs. Treat the monitor as a routed-I/O window rather than an internal log of your mode's own decisions.
- **Clock messages are intentionally omitted** from the monitor stream to keep it readable. If you are debugging clock-driven behavior, rely on `api->get_pulse_count()` and the clock-synced timing helpers (see [Timing and Clock](timing-and-clock.md)) rather than expecting `0xF8` ticks to appear on the monitor.

## Logging to the Log Monitor

The device has a **Log Monitor** screen that shows log lines as they happen, filterable by level. It is the quickest way to watch what your mode is doing without a debugger.

Include `<operator_sdk/log.h>` and call `op::log`:

```cpp
#include <operator_sdk/log.h>

op::log(op::LogLevel::Info, "mode ready");             // a message
op::log(op::LogLevel::Warn, "notes dropped", count);   // "notes dropped: 3"
op::log(op::LogLevel::Info, "rate", rate_x100, 2);     // "rate: 12.50"
```

The level is one of `op::LogLevel::Debug`, `Info`, `Warn`, or `Error`, and the Log Monitor lets the user filter on it. `op::log` formats the value for you and null-checks the API, so you never touch a buffer or `printf`. The last form is fixed-point: the value carries `decimals` fractional digits, the same convention as a Numeric param's `decimal_places` (so `1250` with `2` shows as `12.50`). There is no `double` or `sprintf` on the device, and none is needed here.

`op::log` is a thin wrapper over the raw ABI entry @ref OperatorApi::log "log", which takes a plain string. Prefer `op::log`: it is the same call with the formatting and the null check handled.

@warning Log only from `init`, `destroy`, or other rare event paths, never from the `process()` hot loop. A log call per MIDI message floods the monitor and eats into your real-time headroom.

## Common build failures

**`FAIL: my_mode.elf references N prohibited undefined symbol(s)`** means the SDK's freestanding check caught a C library, `double`, or 64-bit divide symbol. Read the categorised list in the error output, then grep your sources for the category's trigger:

| Category reported | Likely cause | Fix |
|-------------------|--------------|-----|
| `double-precision soft-float` | You used `double` somewhere | `grep -n "double\\|[0-9]\\.[0-9]" src/`, then replace with `float` or integer fixed-point |
| `libc math` | `#include <cmath>` with a transcendental | Replace `sin`, `log2`, `pow`, `lround` etc. with a precomputed lookup table (see `samples/custom-scale-tuner/scala.h`) |
| `libc string/number parsing` | `#include <cstring>` or `<cstdlib>` | Hand-roll the helper you need. `atoi`, `strcmp`, `strchr` are trivial, 10 to 15 LOC each |
| `libgcc 64-bit integer helpers` | `uint64_t / uint64_t` or similar | Redesign to 32-bit math (Cortex-M33 has hardware 32-bit `UDIV` only) |

See @ref freestanding-constraints "The Mode Mindset" for the full contract.

**`warning: thumb-1 mode PLT generation not currently supported`** means the build configured `-fPIC` instead of `-fPIE`. Use the SDK toolchain file. It sets `-fPIE -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative`, which avoids the PLT path entirely. The warning typically coincides with a runtime crash when the mode accesses a global through the GOT.

**`Missing required symbol 'mode_init'. Did you forget extern "C"?`** means the C++ compiler mangled the symbol. Wrap the lifecycle functions (and parameter tables) in `extern "C" { ... }`. This is the most common build failure, and the build calls it out explicitly when it sees the mangled form.

**`Parameter table size mismatch`** means `kParamCount` does not agree with the actual number of `ParamSpec` entries found in the ELF. Usually caused by forgetting to update one or the other after editing the tables. Forward-declare `extern "C" const ParamSpec kParams[]; extern "C" const uint8_t kParamCount;` at file scope before the definitions so `--gc-sections` keeps them linked.

**Devcontainer artifacts differ from local artifacts** usually means the devcontainer has an outdated toolchain compared to your host, or vice versa. Both paths pin `arm-none-eabi-gcc 13.2.Rel1`. If you see a mismatch, check `arm-none-eabi-gcc --version` inside the container and against the version your host install reports. Rebuild the container from scratch (`docker build --no-cache`) if the host has drifted.
