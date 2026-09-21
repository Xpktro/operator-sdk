// Declarative mode template, a note transposer.
//
// Copy this directory to start a mode that carries params and draws no screen of
// its own. The device builds the params page from what the mode declares.

#include <operator_sdk.h>
#include <operator_sdk/log.h>
#include <operator_sdk/params.h>

#include <cstdint>

// --- Parameters --------------------------------------------------------------
//
// The name on the left is the one the code reaches for. The .name is the one the
// params page shows.

inline constexpr op::params::Numeric Transpose {
    .name     = "Transpose",
    .min      = -12,
    .max      = 12,
    .default_ = 0,
};

inline constexpr op::params::Bool Enable {
    .name     = "Enable",
    .default_ = true,
};

inline constexpr op::params::Enum Channel {
    .name     = "Channel",
    .options  = {"All", "Ch 1", "Ch 2", "Ch 3", "Ch 4", "Ch 5", "Ch 6", "Ch 7", "Ch 8", "Ch 9", "Ch 10",
                 "Ch 11", "Ch 12", "Ch 13", "Ch 14", "Ch 15", "Ch 16"},
    .default_ = 0,
};

// An Action is a button on the params page that runs one of the mode's
// functions. It is declared here and defined below, past param_slot_info.
void reset_transpose();

inline constexpr op::params::Action Reset {
    .name      = "Reset Transpose",
    .on_select = &reset_transpose,
};

OP_MODE_PARAMS(Transpose, Enable, Channel, Reset);

void reset_transpose() {
    if (!op::api) return;
    op::api->set_param_value(param_slot_info<Transpose>().slot, 0);
}

// --- Lifecycle ---------------------------------------------------------------

void init() {
    // A line in the Log Monitor. Logging belongs in the lifecycle and in rare
    // events, and stays out of process(), which sees every message.
    op::log(op::LogLevel::Info, "note transposer ready");
}

void process(OpMidiMessage* messages, uint8_t count, uint32_t /*tick_us*/) {
    if (!op::api || !param<Enable>()) return;

    const int32_t shift          = param<Transpose>();
    const int32_t channel_filter = param<Channel>();  // 0 takes every channel, 1..16 pick one

    for (uint8_t i = 0; i < count; ++i) {
        OpMidiMessage& message = messages[i];
        const uint8_t channel  = message.status & 0x0F;
        const uint8_t kind     = message.status & 0xF0;

        if (channel_filter != 0 && channel != channel_filter - 1) continue;

        if (kind == 0x90 || kind == 0x80) {  // note-on or note-off
            // A shift can carry the note past either end of the range, so it is
            // held inside.
            int32_t note = message.data1 + shift;
            if (note < 0) note = 0;
            if (note > 127) note = 127;
            message.data1 = note;
        }
    }
}

void destroy() { }

OP_MODE_REGISTER(init, process, destroy);
