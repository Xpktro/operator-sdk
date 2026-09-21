# Operator SDK

The Operator SDK lets you build MIDI processing and UI modes that the Operator device loads dynamically from its `/global/` and `/output/` folders. Modes are position-independent C++ binaries that call firmware services through a stable, append-only function-table ABI.

## Quickstart

**1. Install the ARM toolchain (`arm-none-eabi-gcc` 13.2+), CMake, and Python.** Nothing else is required — the mode toolchain is self-contained.

- **macOS:** `brew install --cask gcc-arm-embedded` then `brew install cmake python`
- **Debian / Ubuntu:** `sudo apt-get install gcc-arm-none-eabi cmake python3 python3-pip`
- **Windows:** install the [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads), [CMake](https://cmake.org/download/), and [Python](https://www.python.org/downloads/), then run the steps below from a Bash shell (Git Bash or WSL).

**2. Build your first mode:**

```bash
git clone https://github.com/xpktro/operator-sdk.git
cd operator-sdk
pip install -r tools/requirements.txt
cp -R template-declarative my-first-mode && cd my-first-mode
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../cmake/operator-mode-toolchain.cmake
cmake --build build
# build/my_mode.opm is ready — copy to device over USB drive mode into /global/
```

## What's included

- [Getting Started](docs/getting-started.md) walks you from an empty machine to a mode running on the device.
- [How Operator Works](docs/how-operator-works.md) and [Mode Lifecycle](docs/mode-lifecycle.md) are the concepts: the two cores, the MIDI signal chain, and how a mode becomes a live callback.
- [Processing MIDI](docs/processing-midi.md), [Parameters](docs/parameters.md), [Custom UI](docs/custom-ui.md), [Timing and Clock](docs/timing-and-clock.md), [Storage and Files](docs/storage-and-files.md), [The Mode Mindset](docs/the-mode-mindset.md), [Debugging](docs/debugging.md), and [Testing](docs/testing.md) are the task-focused how-to guides, from reading and generating MIDI to testing a mode on a computer.
- [ABI Reference](docs/reference-abi.md), [.opm Format](docs/reference-opm-format.md), and [Build Reference](docs/reference-build.md) are the lookup references.
- The rendered HTML API reference is online at https://xpktro.github.io/operator-sdk/, rebuilt from the main branch on every push. It is the indexed, searchable companion to the per-symbol `///` hover docs and the guides above, and [Building the API reference](#building-the-api-reference) covers generating it locally.

Per-symbol API docs live in the headers themselves as `///` comments (your editor
surfaces them on hover). The same per-symbol docs are also collected into the rendered
HTML API reference described above. [ABI Reference](docs/reference-abi.md) is the canonical reference
for the function table and binary layout.

## Building the API reference

The guides above plus the per-symbol `///` comments in the headers render into a
themed, searchable HTML site (Doxygen styled with the vendored doxygen-awesome-css).
Doxygen is the only extra dependency, and it is **not** needed to build modes.

1. **Install Doxygen** — `brew install doxygen` (macOS) or `apt install doxygen`
   (Debian / Ubuntu). Nothing else is fetched: the theme is vendored under
   `docs/doxygen/`.
2. **Generate** — from the SDK root, run:

   ```bash
   doxygen Doxyfile
   ```

3. **Open** `build/html/index.html`.

The site is regenerated from source on every run, so the output (`build/`) is not
committed.

## Samples

Thirteen reference `.opm` modes live under `samples/`, covering every SDK idiom.

**Global modes** (dropped into `/global/`, full I/O access):

- `master-clock`: BPM-driven 24 PPQ clock generator, the canonical declarative example.
- `generative-sequencer`: probabilistic note generator with note-range + length-range params.
- `note-probability`: minimal declarative demonstrator, one Numeric param that drops notes by probability.
- `user-scale-global`: fullscreen custom UI keyboard for per-semitone scale editing (all outputs).

**Output modes** (dropped into `/output/`, process messages per-output):

- `custom-scale-tuner`: Scala (`.scl`) retuner for the bound output, showcases `FilePicker`.
- `user-scale-output`: per-output variant of the User Scale custom UI.
- `quantizer`: snaps notes to a clock-divided grid, showcases windowed custom UI with large text.
- `arpeggiator`: declarative ARP with style / rate / octave / fill / chord params.
- `cc-lfo`: four LFO slots driving CC output, showcases nested declarative submenus.
- `euclidean-sequencer`: four euclidean tracks, showcases two-screen custom UI navigation.
- `midi-player`: `.mid` file playback with transport state driving the button row (custom UI).
- `delay`: trails each note with decaying echoes timed to the clock or in milliseconds. Its Sync switch picks which timing control is shown, which showcases params that appear and hide with another param (`op::params::Op`).
- `velocity-remap`: reshapes note velocities through a curve, showcases a windowed custom UI (`OP_MODE_REGISTER_UI`) that draws the curve beside declarative params.

Start a new mode by copying the closest sample; each sample directory compiles standalone through the same `op_add_mode()` helper external developers use.

## License

MIT. See [LICENSE](LICENSE).
