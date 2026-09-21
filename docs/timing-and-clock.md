# Timing and Clock {#timing-and-clock}

Modes that play or sequence time-stamped material (SMF playback, LFOs, smooth visuals) need a clock position. The firmware exposes that position as an **integer 24 PPQ counter**, @ref OperatorApi::get_pulse_count "get_pulse_count". This section explains why a naive read of that counter chunks, and how the @ref op::sdk::ClockInterpolator "ClockInterpolator" helper smooths it.

## Does your mode need the clock?

Most modes don't. A transform or a filter reacts to the messages that arrive, not to time, so it never touches the clock at all. You only need the clock when your mode **sequences or plays time-stamped material**: an arpeggiator, a step sequencer, an LFO, an SMF player. If that is not your mode, you can skip the rest of this page.

The rest covers the three things a clock-aware mode does: read a smooth position, react to the transport, and generate clock itself.

## The problem

`get_pulse_count()` advances 24 times per quarter-note. At 120 BPM that is only ~48 increments per second, one step every ~21 ms. A mode that derives its position as `get_pulse_count() / 24.0f` gets a value that jumps in 24-PPQ steps. Any mode playing or sequencing at a resolution **finer than 24 PPQ** (an SMF file's `division` is typically 96, 192, or 480 ticks/quarter) bunches its events onto those pulse edges. The audible result is a ~48 Hz "chunky" or "swingy" feel: events that should be evenly spread instead arrive in clumps locked to the pulse grid.

## A solution: the Clock Interpolator

`op::sdk::ClockInterpolator` (header-only, in `<operator_sdk/timing.h>`) turns the integer counter into a smooth fractional position. It is **opt-in**. `timing.h` is deliberately NOT pulled in by `operator_sdk.h`, so you must `#include <operator_sdk/timing.h>` explicitly.

Internally it measures the recent inter-pulse interval (a median of the last three intervals, in @ref OperatorApi::get_tick "get_tick" microseconds) and reports a fraction of the way toward the next pulse. The fraction is **hard re-synced at every real pulse edge**, a derived prediction between edges, snapped back to truth whenever the counter actually advances.

The interpolator is freestanding-safe by construction: no `double`, no libc, no 64-bit divide (see @ref freestanding-constraints "The Mode Mindset", because `timing.h` is `#include`d into your mode, it _is_ mode code for the build check).

### Usage pattern

Hold a `ClockInterpolator` as a **per-instance member** of your mode's own state, never as `static` or shared file-scope state, which would violate per-instance isolation (each loaded mode instance must have its own interpolation history). Call `now()` exactly **once per `process()` tick**: that single call is what feeds the interpolator's interval history.

```cpp
#include <operator_sdk/timing.h>

struct MyModeState {
    op::sdk::ClockInterpolator clock;   // per-instance, NOT static
    // ... other per-instance fields ...
};

void process_tick(MyModeState& st) {
    // Call now() ONCE per process(). It reads op::api itself.
    const op::sdk::ClockInterpolator::Position pos = st.clock.now();

    // pos.pulse is the exact raw get_pulse_count() value (24 PPQ pulses).
    // pos.frac is a [0,1) fraction toward the next pulse.
    // Units are 24 PPQ pulses, divide by 24 for beats, or by your own
    // modulus for bars / steps.
    const float beats = (static_cast<float>(pos.pulse) + pos.frac) / 24.0f;
    // ... use `beats` as a continuous, smooth position ...
}
```

`now()` returns a `Position`:

```cpp
struct Position {
    uint32_t pulse;   // exact raw get_pulse_count() value (24 PPQ pulses)
    float    frac;    // fractional progress in [0, 1) toward the next pulse
    Status   status;  // health of this sample (see below)
};
```

The `Status` field has four values:

| Status        | Meaning                                                                                                                                                              |
| ------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Live`        | Clock active, and the fraction is sweeping normally within a pulse.                                                                                                  |
| `HeldOverdue` | Clock active but the next pulse is late, so the fraction is clamped just shy of unity and held until the pulse arrives.                                              |
| `HeldStale`   | Clock was active but no pulse has arrived for ~4x the expected period, so the position is frozen at last-known (a stalled clock).                                    |
| `NoClock`     | @ref OperatorApi::get_clock_state "get_clock_state" reports no clock (clock idle, or Clock Source = Mode with no clock-gen mode loaded), and the position is frozen. |

The interpolator **never resets to zero** on clock loss: a stalled or absent clock freezes the last-known position rather than snapping back.

`pulse_period_us()` reports the smoothed microseconds between consecutive 24 PPQ pulse edges (median of the last three intervals), or `0` until three intervals have been observed. One quarter note spans 24 pulses, so `pulse_period_us() * 24` is microseconds-per-quarter-note and `60'000'000 / (pulse_period_us() * 24)` is BPM, useful when a mode needs the _tempo_ of the external clock and not just the position, such as scaling an elapsed-time and total-time readout to whatever tempo is arriving. It reflects whatever `now()` last sampled, so the once-per-tick `now()` contract keeps it current.

### Application-level deltas

The interpolator is purely free-running. It has no `reset()` and no anchor. Deltas such as "pulses since the user pressed PLAY" are the **consuming mode's responsibility**: snapshot `now().pulse` into one of your own mode variables at the anchor point and do integer subtraction against later samples.

`get_pulse_count()` restarts at zero once it reaches 4,294,965,600, which is about 41 days of continuous clock at 24 PPQ. The restart value is a multiple of 7200, so `% 24` and `% 96` step math stays musically aligned straight through it. A plain subtraction taken across the restart still returns a meaningless delta, so a mode that emits per pulse needs to bound what it does with one (see "Generating clock" below).

### Caveats

Be clear about what interpolation does and does not give you. A 24 PPQ clock fundamentally reports position only **24 times per quarter-note**. The interpolator yields a **smooth + predicted** position. It is _not_ true sub-pulse cycle-accuracy, which simply is not recoverable from a 24 PPQ source. Sustained external-clock jitter still produces mild between-pulse wobble as the derived period chases the real one. This is a feature, not a defect.

Corollary: modes that only need a **coarse integer step index** (an arpeggiator firing on grid edges, a step sequencer advancing one cell per pulse) do **not** need the interpolator at all. Read `get_pulse_count()` directly and take an integer modulus. Interpolation only helps modes that consume a _continuous fractional_ position.

## Reading incoming clock and transport

When the device follows a clock, you consume it through the position API. @ref OperatorApi::get_pulse_count "get_pulse_count" is the 24 PPQ position, and @ref OperatorApi::get_clock_state "get_clock_state" tells you whether a clock is running and where it comes from.

@note Watching for `0xF8` bytes in your batch is fragile (the monitor even hides them), so prefer the counter.

Transport re-anchors the beat grid. In the MIDI standard there is no true pause: both Start and Stop reset the beat position, and Continue is ignored. Do not assume the counter is zero at the downbeat. If your mode tracks something like "steps since the user pressed play," snapshot @ref OperatorApi::get_pulse_count "get_pulse_count" at your own anchor point and subtract it from later samples.

## Generating clock

A clock-generating mode drives the device's tempo. Build it as a global mode with `FLAGS GENERATES_CLOCK` in `op_add_mode()` (see @ref reference-build). It becomes eligible when the user sets Clock Source to Mode, and only one clock-generating mode can be loaded at a time.

### Clock generation and process()

@ref OperatorApi::set_clock_period_us "set_clock_period_us" takes the number of microseconds between 24 PPQ pulses and arms a device interrupt that advances the 24 PPQ pulse counter that @ref OperatorApi::get_pulse_count "get_pulse_count" reads. It does not call your `process()`, and it does not emit any MIDI. Sending the `0xF8` clock messages is your mode's own job.

The model has two halves: the interrupt produces pulses at exactly the period you programmed, and your `process()`, called on every pass of the real-time loop (@ref core-1-cycle), polls the counter and does the work for however many pulses have elapsed since its previous call. The two halves run independently, and they do not need to line up.

### Drive the work off the pulse delta

Keep the last pulse count you acted on in one of your own per-instance variables, and work from `pulse_now - last_seen_pulse`. Do not drive musical work off the `process()` call cadence, and do not accumulate @ref OperatorApi::get_tick "get_tick" microseconds into a tempo of your own.

The accuracy lives in the interrupt. Jitter in when `process()` runs changes only when a pulse gets noticed, never how many pulses elapsed, so a late pass borrows nothing from the next one.

Almost every call sees a delta of 0 or 1. The loop runs far faster than the pulse period, so a pulse lands on one pass and the next few passes find nothing new to act on. A delta above 1 means a pass ran late and pulses queued up behind it, and the mode emits that backlog to catch up.

Always cap the backlog. The counter restarts at zero roughly every 41 days of continuous clock (see "Application-level deltas" above), and a subtraction taken across that restart does not return the small true delta. It returns a large meaningless one, which an uncapped emit loop turns into a flood of clock messages on every output. The cap is the only thing standing between that restart and the flood, and it costs a single comparison. Cap at 24, one quarter note, which is the largest backlog worth emitting.

### Effective clock generator checks

Two separate checks decide whether a mode's clock has any effect. A mode that misses either one goes silently ineffective rather than getting an error back, so it is worth knowing both.

- **Arming the generator.** `set_clock_period_us` takes effect only when the calling mode's `.opm` carries the clock flag and the user has set Clock Source to Mode. The period is either 0, an explicit disable, or a value from 100 to 1,000,000,000 microseconds. A period outside that range is rejected.
- **Emitting the clock messages.** `0xF8` output is gated on its own. Clock messages from a mode without the clock flag are dropped, and clock messages are suppressed whenever Clock Source is not Mode. That second rule is deliberate. It keeps a clock-generating mode from doubling the clock stream the device already passes through when the user has picked an external source. A clock-generating mode therefore goes quiet on its clock output, though not on its notes, while an external clock source is selected. That is by design.

If a clock is not coming out of the device, check the flag and the Clock Source setting before looking anywhere else, then check that the output is set to pass clock.

### destroy has to disarm the generator

Call `set_clock_period_us(0)` in `destroy`. The interrupt belongs to the device and outlives your slot, so a mode that leaves it armed leaves the device producing pulses after the mode unloads.

### One counter, either source

@ref OperatorApi::get_pulse_count "get_pulse_count" is a single counter. The interrupt advances it when Clock Source is Mode, and an incoming external clock advances it otherwise. Consumers read the same counter either way, which is why a clock-following mode behaves identically whether the tempo comes from inside the device or from outside it. A clock-generating mode polls that same counter, so what it emits from and what every other mode follows are one and the same.

### Stay inside the budget

`process()` is called on every pass of the real-time loop, and the time budget applies to a clock-generating mode exactly as it does to any other. Do close to nothing in the common case. Read the delta, and if it is zero, return. A mode that repeatedly overruns its per-call time budget is disabled at runtime.
