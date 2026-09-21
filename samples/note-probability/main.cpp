// Note Probability sample mode.
//
// A gate on the notes passing through. Each note-on is rolled against the
// Probability param, and the ones that lose the roll are dropped.

#include <operator_sdk.h>
#include <operator_sdk/params.h>

#include <cstdint>

// --- Parameter ---------------------------------------------------------------

inline constexpr op::params::Numeric Probability {
    .name     = "Probability",
    .min      = 0,
    .max      = 100,
    .default_ = 100,
};

OP_MODE_PARAMS(Probability);

namespace {

// xorshift32, which is fast and repeatable and asks nothing of the hot path. A
// dropped note is a musical decision, so this is as much randomness as it needs.
uint32_t g_rng = 0x13579BDFu;

uint32_t xorshift32() {
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x;
    return x;
}

// Folding the top byte down spreads the draw evenly over 0 to 100, which is the
// range Probability is read against.
uint8_t roll() {
    return (xorshift32() >> 24) * 101u / 256u;
}

}  // namespace

// --- Lifecycle ---------------------------------------------------------------

void init() {
    // Seed from the current tick so successive power-ons diverge. xorshift needs
    // a non-zero seed, and the one above stands until the tick has moved.
    const uint32_t tick = op::api ? op::api->get_tick() : 0;
    if (tick != 0) g_rng = tick ^ 0xA5A5A5A5u;
}

void process(OpMidiMessage* messages, uint8_t count, uint32_t /*tick_us*/) {
    if (!op::api) return;

    const int32_t probability = param<Probability>();
    if (probability >= 100) return;  // every note plays, so there is nothing to roll

    for (uint8_t i = 0; i < count; ++i) {
        OpMidiMessage& message = messages[i];

        // Only note-ons are gated. Everything else carries on, which is what keeps
        // a note that did play from being left sounding when its release arrives.
        // A note-on at velocity 0 is a release, and it carries on too.
        if ((message.status & 0xF0u) != 0x90u) continue;
        if (message.data2 == 0) continue;

        const bool plays = probability > 0 && roll() < probability;
        if (!plays) {
            // Clearing the status consumes the note that lost its roll.
            message.status = 0;
        }
    }
}

void destroy() { }

OP_MODE_REGISTER(init, process, destroy);
