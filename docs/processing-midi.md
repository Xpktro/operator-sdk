# Processing MIDI {#processing-midi}

This is where a mode does its real work: reacting to the MIDI that arrives and deciding what leaves. Everything else in the SDK supports this one function.

## Your entry point: process()

The firmware calls your process function on every pass of the real-time loop, whether or not any MIDI arrived. Each call hands you the batch of messages that landed on that pass, which is very often empty (see @ref core-1-cycle):

```cpp
void process(OpMidiMessage* msgs, uint8_t count, uint32_t tick) {
    if (!op::api) return;   // always guard: the API handle is null before load
    for (uint8_t i = 0; i < count; ++i) {
        // inspect and act on msgs[i]
    }
}
```

- `msgs` is a **writable** array of `count` messages. You edit it in place.
- `count` is how many messages arrived on this pass. `count == 0` is normal and common, because the loop runs far faster than MIDI does. A mode that only reacts to messages returns straight away in that case, and a mode that generates does its work regardless of `count`.
- `tick` is a microsecond timestamp for this pass, the same value @ref OperatorApi::get_tick "get_tick" returns.
- What is in `msgs` depends on the mode type. A **global** mode sees every input that arrived on this pass. An **output** mode sees only the messages routed to its output. See @ref how-operator-works.

Each message is an @ref OpMidiMessage, five bytes: `status`, `data1`, `data2`, `port`, and `length`. `port` names the input the message arrived on, `0..3` for the four physical inputs and `4` for USB, whichever kind of mode is reading it. A message another mode generated arrived on no input, so its `port` is `0xFF`.

## Reading what arrived

The status byte packs the message type in its high nibble and the channel in its low nibble:

```cpp
const uint8_t type    = msgs[i].status & 0xF0;   // message type
const uint8_t channel = msgs[i].status & 0x0F;   // 0..15 (channel 1..16)
```

The common channel-voice types:

| `status & 0xF0` | Message | `data1` | `data2` |
|-----------------|---------|---------|---------|
| `0x80` | Note Off | note | velocity |
| `0x90` | Note On | note | velocity |
| `0xA0` | Poly Aftertouch | note | pressure |
| `0xB0` | Control Change | controller | value |
| `0xC0` | Program Change | program | (unused) |
| `0xD0` | Channel Aftertouch | pressure | (unused) |
| `0xE0` | Pitch Bend | LSB | MSB |

@note A Note On (`0x90`) with `data2 == 0` (velocity 0) is really a Note Off. If your mode reacts to note-ons, check for it, or you will treat key releases as key presses.

System real-time messages such as `0xF8` (clock), `0xFA` (start), and `0xFC` (stop) have no channel. For clock-driven timing, prefer @ref OperatorApi::get_pulse_count "get_pulse_count" over watching for `0xF8` yourself. See @ref timing-and-clock.

## Acting on the batch

### Let a message through

The default. If you do nothing to `msgs[i]`, it passes through unchanged. A mode that only reacts to some messages simply leaves the rest alone.

### Change a message

Edit the fields in place. This transposes every note up a perfect fifth and leaves everything else untouched:

```cpp
for (uint8_t i = 0; i < count; ++i) {
    const uint8_t type = msgs[i].status & 0xF0;
    if (type == 0x90 || type == 0x80) {   // note-on or note-off
        if (msgs[i].data1 <= 120) {       // only shift notes that stay in range
            msgs[i].data1 += 7;           // up a perfect fifth
        }
    }
}
```

The declarative template (`template-declarative/main.cpp`) is a full transpose mode built exactly this way.

### Remove a message

Clear the message's `status`. A message with a status of 0 is out of the batch, so nothing further sends it:

```cpp
for (uint8_t i = 0; i < count; ++i) {
    if ((msgs[i].status & 0xF0) == 0xB0) {   // a Control Change
        msgs[i].status = 0;                  // dropped
    }
}
```

A probability filter is built on exactly this: drop a random share of the incoming notes and pass the rest through untouched.

### Emit MIDI messages

Three calls send a 1 to 3 byte message, and each one behaves differently depending on which kind of mode calls it. Where each inserts the message into the chain is in @ref how-operator-works.

**@ref OperatorApi::send_midi "send_midi"** sends to one output, where the modes loaded after yours can act on the message.

```cpp
bool sent = op::api->send_midi(out, status, data1, data2);
```

- **Global mode:** `out` picks the destination. `0..7` are the eight physical outputs, `8` is USB.
- **Output mode:** `out` is ignored and the message goes to the output this instance is bound to, so pass `0`.

Returns false when the output cannot take it, which includes an output not set to pass that kind of message.

**@ref OperatorApi::send_midi_from "send_midi_from"** sends a message caused by one you were handed, so it travels wherever that one travels.

```cpp
bool sent = op::api->send_midi_from(msgs[i].port, status, data1, data2);
```

- **Global mode:** the first argument is an **input** index, normally the `port` of the message that caused this one.
- **Output mode:** refused, returning false. An output mode runs once routing has already decided where everything goes, so there is no route left to follow.

Returns false for an input out of range, or when there is no room for the message this pass.

**@ref OperatorApi::send_midi_direct "send_midi_direct"** sends to the output without the modes loaded after yours seeing it. It suits a timing reference or a tap reporting what an output is doing.

```cpp
bool sent = op::api->send_midi_direct(out, status, data1, data2);
```

- **Global mode:** `out` picks the destination, numbered as in `send_midi`.
- **Output mode:** `out` is ignored and the bound output is used, as in `send_midi`. The modes after yours on that output are skipped.

Returns false on the same terms as `send_midi`, the message-type filter included.

@note `out` numbers outputs and `port` numbers inputs, and the two do not line up. Input 2 and output 2 are different sockets.

System-exclusive messages go out through @ref OperatorApi::send_sysex "send_sysex", covered below. @ref OperatorApi::output_active "output_active" reports whether an output can emit right now, which is useful before a round-robin.

A generator ignores `msgs` and `count` and emits from elapsed time, since `process()` is called on every pass. A clock generator emits one `0xF8` per elapsed pulse:

```cpp
void process(OpMidiMessage* /*msgs*/, uint8_t /*count*/, uint32_t /*tick*/) {
    if (!op::api) return;
    const uint32_t pulse_now = op::api->get_pulse_count();
    uint32_t delta = pulse_now - last_seen_pulse_;          // pulses owed since the last call
    if (delta > 24) delta = 24;                             // cap the catch-up at one quarter note
    for (uint32_t p = 0; p < delta; ++p) {
        op::api->send_midi_direct(/*out=*/0, 0xF8, 0, 0);   // MIDI clock
    }
    last_seen_pulse_ = pulse_now;
}
```

The full clock-generation contract, including the two gates that decide whether your clock reaches an output at all, is in @ref timing-and-clock.

## Working with SysEx {#receiving-sysex}

A mode can receive inbound system-exclusive (SysEx) messages and send its own, so it can read a bulk dump off the wire or answer a query it issued. Both directions need the same declaration, and the subsections below take them in turn.

Delivery takes two steps. The mode carries the `HANDLES_SYSEX` flag in its build declaration, and its handler goes to `OP_MODE_REGISTER` third, beside `process`:

```cpp
bool on_sysex(uint8_t port, const uint8_t* data,
              uint16_t len, uint8_t flags) {
    // inspect data[0..len), react, and answer whether the mode takes the message
    return false;
}

OP_MODE_REGISTER(init, process, on_sysex, destroy);
```

`HANDLES_SYSEX` belongs on a mode that inspects, consumes, or generates system-exclusive messages. SysEx reaches the routed outputs on its own, so a mode with no interest in it carries no flag and exports no handler.

### Arriving in fragments

A SysEx message arrives in one or more calls:

- A small message arrives as a single call carrying both boundary flags.
- A large dump arrives as several fragments. The first carries @ref kSysExStart "kSysExStart", the last carries @ref kSysExEnd "kSysExEnd", and the ones between carry neither.

The `flags` byte marks the boundaries, which can be read with the two bits:

```cpp
const bool first = flags & kSysExStart;   // opening fragment of a message
const bool last  = flags & kSysExEnd;     // closing fragment (both set = whole message in one call)
```

The handler's `port` is the same input index a message in `msgs` carries.

@note A mode handling a dump gathers its fragments in its own instance state, sized to the largest message it expects, appending each call's bytes until the fragment flagged @ref kSysExEnd "kSysExEnd". Two senders can be dumping at once and their fragments interleave, so state keyed by `port` keeps each dump whole.

### Passing and consuming

The handler's return value states how each SysEx message is ultimately handled. Returning `false` lets it continue to the routed outputs, where the device emits it. Returning `true` stops it from being propagated. Nothing is sent on, and no later mode in the chain sees the rest of it.

The choice is made on the fragment flagged @ref kSysExStart "kSysExStart" and held through the one flagged @ref kSysExEnd "kSysExEnd", so a message goes whole whichever way it goes. Later fragments of a message a mode took reach that mode alone.

Fragments of a message nobody took keep reaching every handler, so a mode rewriting a long message tracks in its own state whether the message in flight is one it took, keyed by `port` when two inputs can be mid-message at once.

### Sending a message

@ref OperatorApi::send_sysex "send_sysex" is used for generating, either a reply the mode composes or a modified copy of a message it took. A transform reads what it was handed, takes it, and sends its own version.

Where that copy lands follows the rule @ref OperatorApi::send_midi "send_midi" already uses. An output mode is bound to one output and its copy goes there whatever index the call names, so it passes `0`. A global mode names the output it wants. System-exclusive is one of the message types an output can be set to pass, so an output set to reject it takes nothing and the call answers `-1`.

```cpp
// An identity request is short enough to arrive in one call, and the reply is
// short enough to leave in one, so neither side needs reassembly.
bool on_sysex(uint8_t /*port*/, const uint8_t* data, uint16_t len, uint8_t flags) {
    if (!op::api) return false;
    if ((flags & kSysExStart) == 0) return false;   // decide on the opening fragment

    const bool is_identity_request =
        len >= 4 && data[0] == 0x7E && data[2] == 0x06 && data[3] == 0x01;
    if (!is_identity_request) return false;         // someone else's message, pass it on

    static const uint8_t reply[] = {0x7E, 0x00, 0x06, 0x02, 0x7D, 0x01};
    if (op::api->send_sysex(/*out=*/0, reply, sizeof(reply),
                            kSysExStart | kSysExEnd) < 0) {
        return false;                               // nothing queued, so the request passes
    }
    return true;                                    // answered, so the request stops here
}
```

@note Fragments arrive without the opening `F0` and the closing `F7`, and `send_sysex` takes the bytes in that same form.

A mode that takes a message owns delivering it. Returning `true` after a refused send drops the original along with the copy, so the message reaches the wire in neither form. A refusal on the opening call has queued nothing, so returning `false` there sends the original on untouched.

Every negative answer says something different, and they call for different things:

| Answer | What it means |
|--------|---------------|
| @ref kOutputBusy "kOutputBusy" | the output could not take the fragment this pass and the message is still open, so the same fragment goes out again on a later `process()` |
| `-1` | the message is over, so a new one opens before anything else is sent |
| @ref kOutputTooLarge "kOutputTooLarge" | the fragment can never fit, however long the wait, so the message needs splitting into calls of at most 253 bytes |

`-1` is the generic failure every entry in the function table answers with. @ref kOutputBusy "kOutputBusy" and @ref kOutputTooLarge "kOutputTooLarge" carry their own values so they can be told apart from it, and all three fall through a plain `n < 0` check.

Treating every negative answer as "try again" is the loop that spins, and the write that eventually lands would put payload bytes on the wire with no `F0` in front of them.

A length of zero only means something on a call that closes the message, so a zero length without @ref kSysExEnd "kSysExEnd" is rejected.

### While a message is open

An output sends one SysEx message at a time. If something else is already sending one there, `send_sysex` answers @ref kOutputBusy "kOutputBusy" instead of mixing the two together. Notes routed to that output wait their turn and go out after the closing `F7`. Starting a second message closes the first. MIDI clock, start and stop are the exception and travel inside the message, which is where a receiver expects to find them.

Whatever happens, every message reaching the output starts with `F0` and ends with `F7`, so a receiver is never left waiting for an ending that never comes. A mode unloaded part-way through a message has that message closed off automatically. The one thing a receiver can notice is a message that arrived shorter than it was sent.

**A message ends after a second of silence.** A bulk dump that keeps writing holds its output for as long as it needs, and a 100 KB message is perfectly normal. What ends one early is a full second in which none of it reached the output, which no real sender produces and a stalled retry loop does.

**A message passing through can be cut by the user.** This one belongs to no mode. Turning an output off, unassigning an input from it, or clearing its SysEx filter bit while a message is in flight ends that message on that output with one `F7`, and no later part of it reaches that output even if the setting is turned straight back on. Fragments keep arriving at the handler throughout, so a handler gathering a dump sees the whole thing whatever the outputs are doing. The message that follows opens normally.

## Staying real-time

`process()` runs on the real-time core on every pass of its loop, for every loaded mode, which makes it the hot path. Keep it quick and predictable, with no blocking, no waiting, and no allocation. If you need lookup tables or per-note state, size them at compile time. A mode that repeatedly overruns its per-call time budget is disabled at runtime, so a generator should do close to nothing in the common case. See @ref the-mode-mindset.
