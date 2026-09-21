# Delay

An output mode that trails each note with a decaying echo. It repeats the note a
few times, each repeat quieter than the last and optionally shifted in pitch, and
works on notes only, so pitch bend, CC, aftertouch, program change, clock, and
other messages pass straight through.

The note plays as usual, then trails with up to Repeats echoes, spaced by the
delay time and fading out by the Feedback amount. The echoes start on release, so
a repeat never overlaps a still-held note and same-pitch echoes never collide.
Echoes are timed relative to the note and never snapped to the clock grid, so
their spacing stays even if the tempo drifts slightly. Delay time can follow
the clock (a musical division like 1/8) or run free in milliseconds. In Sync mode
the echoes wait until the clock is running, and starting or stopping the transport
stops any echoes in progress so nothing hangs. Toggling Bypass while notes are
held never strands a note.

## Controls

| Control   | Range | Default | What it does |
| --------- | ----- | ------- | ------------ |
| Sync      | On, Off | On | On times the echoes to the clock, Off uses a free-running time in milliseconds. This also chooses which timing control is shown. |
| Rate      | 2, 1, 1/2, 1/4, 1/8, 1/16, 1/32 | 1/8 | The musical division between echoes (shown when Sync is On). |
| Rhythm    | Straight, Dotted, Triplet | Straight | A groove feel applied to the division, where Dotted lengthens it and Triplet shortens it (shown when Sync is On). |
| Time      | 1 to 2000 ms | 250 | The echo spacing in milliseconds (shown when Sync is Off). |
| Repeats   | 1 to 16 | 3 | How many echoes follow the note. |
| Feedback  | 0 to 100 | 70 | How much quieter each echo is than the one before, as a percentage. At 0 the echoes fade out immediately, leaving only the dry note. |
| Transpose | -24 to +24 | 0 | A semitone shift added to each successive echo, so the trail can walk up or down. An echo that would leave the MIDI range is dropped. |
| Channel   | Omni, 1..16 | Omni | Which MIDI channel the delay affects. Omni delays every channel. A specific channel delays only that one and passes the rest through untouched. |
| Bypass    | On, Off | Off | On cuts the delay, stopping any sounding echoes and passing the input through dry until it is turned off. |

## Build and flash

From the SDK root:

```bash
cd samples/delay
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../cmake/operator-mode-toolchain.cmake
cmake --build build
```

This produces `build/delay.opm`. Copy it into `/output/` on the Operator's USB
drive, then reboot and load it on any output from the mode browser.
