// Delay sample mode.
//
// Output-only, per-instance delay. The mode takes ownership of each note it
// processes, re-emits the dry note itself, and schedules the decayed echoes that
// follow it.
//
// Echoes fire on release. MIDI 1.0 identifies a note by (channel, note number)
// alone, so two sounding notes of the same pitch on one channel share an identity
// and a note-off between them is ambiguous. Holding the echo train until the note
// is released keeps one pitch to one open note, and the whole engine is built on
// that invariant.
//
// Scheduling is note-relative on the us tick, so an echo lands `delta` after the
// dry note's own arrival. Sync derives delta from the tempo measure in
// op::sdk::ClockInterpolator (24 PPQ), and free-run takes it from Time.

#include <operator_sdk.h>
#include <operator_sdk/params.h>
#include <operator_sdk/timing.h>  // ClockInterpolator, the smoothed us-per-pulse

#include <cstdint>

// --- Parameters (all per-instance, declarative-only) ------------------------
//
// Sync is declared first so its object reference is a constant expression the
// dependent params can capture in `.visible_when = {.watch = Sync, ...}`
// (the SDK's watcher-before-dependents rule).

inline constexpr op::params::Bool Sync {.name = "Sync", .default_ = true, .is_per_instance = 1};

inline constexpr op::params::Enum Rate {
    .name            = "Rate",
    .options         = {"2", "1", "1/2", "1/4", "1/8", "1/16", "1/32"},
    .default_        = 4, // "1/8"
    .is_per_instance = 1,
    .visible_when    = {.watch = Sync, .op = op::params::Op::Eq, .value = 1},
};

inline constexpr op::params::Enum Rhythm {
    .name            = "Rhythm",
    .options         = {"Straight", "Dotted", "Triplet",},
    .default_        = 0, // Straight
    .is_per_instance = 1,
    .visible_when    = {.watch = Sync, .op = op::params::Op::Eq, .value = 1},
};

inline constexpr op::params::Numeric Time {
    .name            = "Time",
    .min             = 1,
    .max             = 2000,
    .default_        = 250,
    .step            = 1,
    .is_per_instance = 1,
    .visible_when    = {.watch = Sync, .op = op::params::Op::Eq, .value = 0},
};

inline constexpr op::params::Numeric Repeats {
    .name = "Repeats", .min = 1, .max = 16, .default_ = 3, .is_per_instance = 1};

inline constexpr op::params::Numeric Feedback {
    .name = "Feedback", .min = 0, .max = 100, .default_ = 70, .is_per_instance = 1};

inline constexpr op::params::Numeric Transpose {
    .name = "Transpose", .min = -24, .max = 24, .default_ = 0, .is_per_instance = 1};

// Omni (0) owns notes on every channel, and option N (1..16) owns only notes on
// MIDI channel N-1.
inline constexpr op::params::Enum Channel {
    .name            = "Channel",
    .options         = {"Omni", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13",
                        "14", "15", "16",},
    .default_        = 0, // Omni
    .is_per_instance = 1,
};

// Engaging Bypass flushes every alive voice once, then the mode passes its input
// through dry until Bypass is cleared.
inline constexpr op::params::Bool Bypass {.name = "Bypass", .default_ = false, .is_per_instance = 1};

OP_MODE_PARAMS(Sync, Rate, Rhythm, Time, Repeats, Feedback, Transpose, Channel, Bypass);

// --- Pulses-per-bounce table (24 PPQ, integer-only) -------------------------
//
// Flat constexpr uint32_t table indexed by feel * 7 + rate_index, where feel is
// 0 = Straight, 1 = Dotted, 2 = Triplet and rate_index is 0 = 2, 1 = 1, 2 = 1/2,
// 3 = 1/4, 4 = 1/8, 5 = 1/16, 6 = 1/32.
//
// The dotted and triplet cells are the plain pulse count scaled and rounded, so
// the derivation for each one is spelled out below and pinned by the doctest
// "sync dotted and triplet pulses". Every cell is clamped to >= 1 so a bounce
// always advances.
//
//   index | feel     | rate | value | math
//   ------+----------+------+-------+--------------------------------
//      0  | straight | 2    |  192  |
//      1  | straight | 1    |   96  |
//      2  | straight | 1/2  |   48  |
//      3  | straight | 1/4  |   24  |
//      4  | straight | 1/8  |   12  |
//      5  | straight | 1/16 |    6  |
//      6  | straight | 1/32 |    3  |
//      7  | dotted   | 2    |  288  | (192*3+1)/2 = 577/2 = 288
//      8  | dotted   | 1    |  144  | ( 96*3+1)/2 = 289/2 = 144
//      9  | dotted   | 1/2  |   72  | ( 48*3+1)/2 = 145/2 =  72
//     10  | dotted   | 1/4  |   36  | ( 24*3+1)/2 =  73/2 =  36
//     11  | dotted   | 1/8  |   18  | ( 12*3+1)/2 =  37/2 =  18
//     12  | dotted   | 1/16 |    9  | (  6*3+1)/2 =  19/2 =   9
//     13  | dotted   | 1/32 |    5  | (  3*3+1)/2 =  10/2 =   5
//     14  | triplet  | 2    |  128  | (192*2+1)/3 = 385/3 = 128
//     15  | triplet  | 1    |   64  | ( 96*2+1)/3 = 193/3 =  64
//     16  | triplet  | 1/2  |   32  | ( 48*2+1)/3 =  97/3 =  32
//     17  | triplet  | 1/4  |   16  | ( 24*2+1)/3 =  49/3 =  16
//     18  | triplet  | 1/8  |    8  | ( 12*2+1)/3 =  25/3 =   8
//     19  | triplet  | 1/16 |    4  | (  6*2+1)/3 =  13/3 =   4
//     20  | triplet  | 1/32 |    2  | (  3*2+1)/3 =   7/3 =   2

namespace {

constexpr uint32_t kPulsesPerBounce[21] = {
    // Straight (feel 0), so 2, 1, 1/2, 1/4, 1/8, 1/16, 1/32
    192u,
    96u,
    48u,
    24u,
    12u,
    6u,
    3u,
    // Dotted x3/2 round-half-up (feel 1)
    288u,
    144u,
    72u,
    36u,
    18u,
    9u,
    5u,
    // Triplet x2/3 round-half-up (feel 2)
    128u,
    64u,
    32u,
    16u,
    8u,
    4u,
    2u,
};

// Look up pulses-per-bounce for a (rate_index, feel) combination. rate_index is
// clamped into [0,6] and feel into [0,2], and the result is clamped to >= 1.
inline uint32_t pulses_per_bounce(int32_t rate_index, int32_t feel) {
    if (rate_index < 0) rate_index = 0;
    if (rate_index > 6) rate_index = 6;
    if (feel < 0) feel = 0;
    if (feel > 2) feel = 2;
    const uint32_t index  = static_cast<uint32_t>(feel) * 7u + static_cast<uint32_t>(rate_index);
    const uint32_t pulses = kPulsesPerBounce[index];
    return pulses < 1u ? 1u : pulses;
}

// --- Release-triggered echo engine ------------------------------------------
//
// One Voice per (pitch, channel). A Voice is Held between its note-on and
// note-off, then flips to Echoing on the note-off, emits the decayed repeat
// train, and dies. All state is fixed-size.

constexpr uint8_t kPoolSize = 32;  // simultaneous source notes

enum class VoiceState : uint8_t {
    Held    = 0,  // dry note sounding, echoes not yet scheduled
    Echoing = 1,  // released, emitting the decayed repeat train
};

struct Voice {
    bool alive               = false;
    VoiceState state         = VoiceState::Held;
    uint8_t pitch            = 0;      // source pitch (echo pitch = pitch + echo_index*transpose)
    uint8_t channel          = 0;      // source channel (0..15), preserved on every echo
    int8_t transpose         = 0;      // per-voice transpose offset cached at note-on
    uint8_t source_velocity  = 0;      // source note-on velocity (decay base)
    uint8_t current_velocity = 0;      // velocity of the next echo to emit
    uint8_t echo_index       = 0;      // echo being emitted (1..repeats), 0 while Held
    uint32_t on_tick         = 0;      // us tick the dry note-on fired on, the schedule anchor
    uint32_t delta_us        = 0;      // delay interval (us) frozen at release
    uint32_t echo_gate       = 0;      // min(source_gate, delta) in us, set at release
    uint32_t next_fire       = 0;      // us tick of the next scheduled echo on/off event
    bool on_phase            = true;   // the next echo event is a note-on (true) or note-off
    bool echo_open           = false;  // an echo note-on is open, awaiting its off
    uint8_t open_pitch       = 0;      // pitch of the currently-open echo (when echo_open)
    uint32_t sequence        = 0;      // insertion order, used to steal the oldest voice
};

Voice g_pool[kPoolSize] = {};
uint32_t g_sequence     = 0;      // monotonic counter for oldest-steal ordering
bool g_bypassed         = false;  // true while Bypass is engaged (flush-once edge tracking)

// Smoothed tempo measure for Sync mode. Call g_clock.now() exactly once per
// process() tick and read the per-pulse period via g_clock.pulse_period_us(),
// which returns 0 until it has seen 3 clean pulse intervals. It tracks tempo
// alone, so it stays valid across a Rate or Rhythm change.
op::sdk::ClockInterpolator g_clock;

// Decay velocity by Feedback percent, so next = floor(velocity * feedback / 100)
// clamped to [0,127]. A return of 0 stops the trail, since a velocity-0 note-on
// reads as a note-off.
inline uint8_t next_velocity(uint8_t velocity, int32_t feedback_percent) {
    const int32_t decayed = (static_cast<int32_t>(velocity) * feedback_percent) / 100;
    if (decayed < 1) return 0;
    if (decayed > 127) return 127;
    return static_cast<uint8_t>(decayed);
}

// MIDI status helpers. Channel-voice status = (type | channel).
inline bool is_note_on(uint8_t status, uint8_t data2) {
    return (status & 0xF0u) == 0x90u && data2 != 0u;
}
inline bool is_note_off(uint8_t status, uint8_t data2) {
    return (status & 0xF0u) == 0x80u || ((status & 0xF0u) == 0x90u && data2 == 0u);
}

// Echo pitch for echo_index is source + echo_index*transpose. A false return means
// the pitch left the MIDI range, which ends the trail: the offset grows
// monotonically with echo_index, so once it is out it stays out.
inline bool echo_pitch_at(const Voice& voice, uint8_t echo_index, uint8_t& out_pitch) {
    const int32_t shifted = static_cast<int32_t>(voice.pitch)
        + static_cast<int32_t>(echo_index) * static_cast<int32_t>(voice.transpose);
    if (shifted < 0 || shifted > 127) return false;
    out_pitch = static_cast<uint8_t>(shifted);
    return true;
}

// Wrap-safe "has `now` reached `target`?" on a 32-bit pulse counter or us tick.
// The signed delta stays correct across the 32-bit wrap, which the us tick reaches
// after about 71 minutes.
inline bool time_reached(uint32_t now, uint32_t target) {
    return (int32_t)(now - target) >= 0;
}

// Unused output argument, since an output mode reaches the output it was loaded
// on.
constexpr uint8_t kAnyOutput = 0;

// Emit one channel-voice note event on the output this mode is loaded on.
inline void emit_note(const Voice& voice, bool on, uint8_t pitch, uint8_t velocity) {
    if (!op::api) return;
    op::api->send_midi(kAnyOutput, static_cast<uint8_t>((on ? 0x90u : 0x80u) | (voice.channel & 0x0Fu)),
                       pitch, on ? velocity : 0u);
}

// Open output pitch `pitch` for `voice`, keeping at most one note open per
// (channel, pitch). Transpose can send two voices' outputs to the same
// pitch, say a Transpose of -12 taking source-S's 2nd echo and source-(S-12)'s
// 1st echo both to S-24, so a collision is resolved by dry-note priority: an
// echo yields to a held dry note and is suppressed, and in every other case the
// colliding note is closed before `pitch` is opened. Returns true when the
// note-on was emitted. A held dry note occupies its source pitch (state == Held)
// and an echo occupies open_pitch. The scan is O(kPoolSize).
bool try_open(Voice& voice, uint8_t pitch, uint8_t velocity, bool is_dry) {
    for (uint8_t i = 0; i < kPoolSize; ++i) {
        Voice& other = g_pool[i];
        if (&other == &voice || !other.alive) continue;
        if (other.channel != voice.channel) continue;
        const bool other_dry_here  = (other.state == VoiceState::Held && other.pitch == pitch);
        const bool other_echo_here = (other.echo_open && other.open_pitch == pitch);
        if (!other_dry_here && !other_echo_here) continue;
        if (!is_dry && other_dry_here) return false;  // dry priority suppresses the echo
        emit_note(other, /*on=*/false, pitch, 0u);    // steal, closing the holder first
        if (other_dry_here)
            other.alive = false;
        else
            other.echo_open = false;
        break;  // the invariant leaves at most one holder
    }
    emit_note(voice, /*on=*/true, pitch, velocity);
    return true;
}

// Close whatever this voice has sounding, either the held dry note or the single
// open echo, and mark it dead. Used on same-pitch retrigger, pool-steal, the
// transport Start and Stop flush, and destroy().
void release_voice(Voice& voice) {
    if (!voice.alive) return;
    if (voice.state == VoiceState::Held) {
        emit_note(voice, /*on=*/false, voice.pitch, 0u);  // close the held dry note
    } else if (voice.echo_open) {
        emit_note(voice, /*on=*/false, voice.open_pitch,
                  0u);  // close the one open echo
        voice.echo_open = false;
    }
    voice.alive = false;
}

// Close every alive voice. Called from the transport Start/Stop branch and from
// destroy().
void flush_all_voices() {
    for (uint8_t i = 0; i < kPoolSize; ++i)
        if (g_pool[i].alive) release_voice(g_pool[i]);
}

// Find the alive voice for (pitch, channel), or nullptr.
Voice* find_voice(uint8_t pitch, uint8_t channel) {
    for (uint8_t i = 0; i < kPoolSize; ++i) {
        Voice& voice = g_pool[i];
        if (voice.alive && voice.pitch == pitch && voice.channel == channel) return &voice;
    }
    return nullptr;
}

// Acquire a slot for a fresh note-on, taking a free slot if there is one and
// otherwise stealing the oldest alive voice, which is released before its slot is
// handed back. The caller emits only after the slot is secured.
Voice* acquire_slot() {
    for (uint8_t i = 0; i < kPoolSize; ++i)
        if (!g_pool[i].alive) return &g_pool[i];
    uint8_t oldest_index     = 0;
    uint32_t oldest_sequence = g_pool[0].sequence;
    for (uint8_t i = 1; i < kPoolSize; ++i) {
        if (static_cast<int32_t>(g_pool[i].sequence - oldest_sequence) < 0) {
            oldest_sequence = g_pool[i].sequence;
            oldest_index    = i;
        }
    }
    release_voice(g_pool[oldest_index]);  // note-off first, then reuse
    return &g_pool[oldest_index];
}

// A fresh note-on emits the dry note at full velocity and holds, since the echoes
// wait for the release. A same-pitch voice already alive is a re-press, so its old
// trail is released first.
void note_on(uint8_t pitch, uint8_t channel, uint8_t velocity, uint32_t now, int32_t transpose) {
    if (Voice* previous = find_voice(pitch, channel)) release_voice(*previous);
    Voice* voice           = acquire_slot();
    *voice                 = Voice {};
    voice->alive           = true;
    voice->state           = VoiceState::Held;
    voice->pitch           = pitch;
    voice->channel         = channel;
    voice->transpose       = static_cast<int8_t>(transpose);
    voice->source_velocity = velocity;
    voice->on_tick         = now;
    voice->sequence        = g_sequence++;
    try_open(*voice, pitch, velocity, /*is_dry=*/true);
}

// The matching note-off emits the dry note-off and arms the echo train, freezing
// delta_us so the spacing holds even if the tempo estimate drifts mid-train.
//
// A note-off with no matching held voice, already released or stolen, is not owned
// and passes through. That is what closes a note which started outside the mode's
// ownership, such as one held across a Bypass toggle. Returns true when the mode
// owned the note and emitted its dry note-off, which is when the caller consumes
// the message.
bool note_off(
    uint8_t pitch, uint8_t channel, uint32_t now, uint32_t delta, int32_t repeats, int32_t feedback) {
    Voice* voice = find_voice(pitch, channel);
    if (voice == nullptr || voice->state != VoiceState::Held) return false;

    emit_note(*voice, /*on=*/false, pitch, 0u);  // dry note-off

    const uint32_t gate     = now - voice->on_tick;  // wrap-safe span (us)
    voice->delta_us         = delta;
    voice->echo_gate        = (gate < delta) ? gate : delta;  // min(gate, delta)
    const uint32_t natural  = voice->on_tick + delta;
    voice->next_fire        = time_reached(now, natural) ? now : natural;  // max(on+delta, release)
    voice->current_velocity = next_velocity(voice->source_velocity, feedback);
    voice->echo_index       = 1;
    voice->on_phase         = true;
    voice->echo_open        = false;

    // The trail is empty when Repeats < 1, when the first echo already floors to
    // silence, or when delta is still unknown (delta == 0), which in Sync mode
    // means the clock has yet to establish a tempo. In each case only the dry note
    // sounds.
    if (repeats < 1 || delta == 0u || voice->current_velocity < 1) {
        voice->alive = false;
        return true;
    }
    voice->state = VoiceState::Echoing;
    return true;
}

// Emit every echo event of every echoing voice due at `now`. A voice is drained in
// a while-loop so a coincident off and on (back-to-back, echo_gate == delta) both
// fire this tick, off first. Each voice keeps its own delta_us, frozen at release.
void fire_due_echoes(uint32_t now, int32_t repeats, int32_t feedback) {
    for (uint8_t i = 0; i < kPoolSize; ++i) {
        Voice& voice = g_pool[i];
        while (voice.alive && voice.state == VoiceState::Echoing && time_reached(now, voice.next_fire)) {
            if (voice.on_phase) {
                uint8_t pitch;
                if (voice.current_velocity >= 1 && echo_pitch_at(voice, voice.echo_index, pitch)) {
                    // When a held dry note owns this pitch, try_open yields to it and
                    // returns false. The echo stays silent and the train carries on,
                    // and the off-phase sees echo_open == false and emits nothing.
                    if (try_open(voice, pitch, voice.current_velocity, /*is_dry=*/false)) {
                        voice.echo_open  = true;
                        voice.open_pitch = pitch;
                    }
                    voice.on_phase = false;
                    voice.next_fire += voice.echo_gate;  // this echo's note-off time
                } else {
                    voice.alive = false;  // velocity floored or out of range, so
                                          // the trail ends
                }
            } else {
                if (voice.echo_open) {
                    emit_note(voice, /*on=*/false, voice.open_pitch, 0u);
                    voice.echo_open = false;
                }
                voice.current_velocity = next_velocity(voice.current_velocity, feedback);
                if (static_cast<int32_t>(voice.echo_index) >= repeats || voice.current_velocity < 1) {
                    voice.alive = false;  // all repeats emitted
                } else {
                    ++voice.echo_index;
                    voice.on_phase = true;
                    voice.next_fire += (voice.delta_us - voice.echo_gate);  // next echo on
                                                                            // = this off +
                                                                            // (delta-gate)
                }
            }
        }
    }
}

// --- Lifecycle (ABI entry points) -------------------------------------------

struct TimeBase {
    uint32_t now;
    uint32_t delta;
};

TimeBase current_time_base(uint32_t tick_us) {
    if (param<Sync>() != 0) {
        const uint32_t delta_pulses = pulses_per_bounce(param<Rate>(), param<Rhythm>());
        const uint32_t period_us    = g_clock.pulse_period_us();  // 0 until the clock is ready
        // delta_pulses <= 288 and period_us stays small for any musical tempo, so
        // the product fits uint32.
        return TimeBase {tick_us, delta_pulses * period_us};
    }
    int32_t time_ms = param<Time>();
    if (time_ms < 1) time_ms = 1;
    return TimeBase {tick_us, static_cast<uint32_t>(time_ms) * 1000u};
}

}  // namespace

void init() {
    for (uint8_t i = 0; i < kPoolSize; ++i) g_pool[i] = Voice {};
    g_sequence = 0;
    g_bypassed = false;
    g_clock    = op::sdk::ClockInterpolator {};  // fresh smoothing ring on (re)load
}

void process(OpMidiMessage* messages, uint8_t count, uint32_t tick_us) {
    if (!op::api) return;

    // Sample the clock once per tick so pulse_period_us() reflects the current
    // tempo.
    g_clock.now();

    if (param<Bypass>() != 0) {
        if (!g_bypassed) {
            flush_all_voices();  // cut the held dry notes and echo tails on the edge
            g_bypassed = true;
        }
        return;  // input passes through dry
    }
    g_bypassed = false;

    const TimeBase time_base     = current_time_base(tick_us);
    const int32_t repeats        = param<Repeats>();
    const int32_t feedback       = param<Feedback>();
    const int32_t transpose      = param<Transpose>();
    const int32_t channel_filter = param<Channel>();

    // (A) Observe transport. Start (0xFA) and Stop (0xFC) both flush every voice,
    //     and Continue (0xFB) is ignored. Realtime bytes are not notes, so they
    //     pass through dry.
    for (uint8_t i = 0; messages != nullptr && i < count; ++i) {
        const uint8_t status = messages[i].status;
        if (status == 0xFAu || status == 0xFCu) {
            flush_all_voices();
            break;
        }
    }

    // (B) Own each incoming note by zeroing its status and re-emitting it here. The
    //     dry note-off is emitted ahead of any coincident echo1 fired in (C), which
    //     keeps the same-pitch stream strictly alternating.
    for (uint8_t i = 0; messages != nullptr && i < count; ++i) {
        OpMidiMessage& message = messages[i];
        const uint8_t channel  = message.status & 0x0Fu;
        if (is_note_on(message.status, message.data2)) {
            if (channel_filter == 0 || (channel_filter - 1) == channel) {
                note_on(message.data1, channel, message.data2, time_base.now, transpose);
                message.status = 0;  // the mode owns it
            }
        } else if (is_note_off(message.status, message.data2)) {
            if (note_off(message.data1, channel, time_base.now, time_base.delta, repeats, feedback)) {
                message.status = 0;  // the mode owned the note
            }
        }
    }

    // (C) Fire every echo event now due. This runs on an empty batch too, so an
    //     armed train advances on every tick.
    fire_due_echoes(time_base.now, repeats, feedback);
}

void destroy() {
    if (op::api) flush_all_voices();
    for (uint8_t i = 0; i < kPoolSize; ++i) g_pool[i] = Voice {};
    g_sequence = 0;
    g_bypassed = false;
}

OP_MODE_REGISTER(init, process, destroy);
