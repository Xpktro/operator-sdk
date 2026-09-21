# Getting Started {#getting-started}

Install dependencies and get to work right away. It assumes you can use a terminal and have written a little C++. You do not need any embedded or DSP background.

## What you need

The mode toolchain is self-contained. You need three things:

- `arm-none-eabi-gcc` 13.2 or newer (the C++20 cross-compiler for the device).
- CMake 3.28 or newer.
- Python 3.11 or newer (it runs the packager).

Install them for your platform:

- **macOS:** `brew install --cask gcc-arm-embedded` then `brew install cmake python`
- **Debian / Ubuntu:** `sudo apt-get install gcc-arm-none-eabi cmake python3 python3-pip`
- **Windows:** install the [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads), [CMake](https://cmake.org/download/), and [Python](https://www.python.org/downloads/), then run the steps below from a Bash shell (Git Bash or WSL).

## Get the SDK

```bash
git clone https://github.com/xpktro/operator-sdk.git
cd operator-sdk
pip install -r tools/requirements.txt
```

## Build your first mode

The fastest start is to copy the declarative template. It is a complete, valid mode with a single parameter, and it builds unchanged.

```bash
cp -R template-declarative my-first-mode && cd my-first-mode
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../cmake/operator-mode-toolchain.cmake
cmake --build build
```

When the build finishes, the file you care about is `build/my_mode.opm`, the packaged mode the device loads. Two build byproducts sit next to it:

```
build/my_mode.opm     # the packaged mode — load this on the device
build/my_mode         # the compiled ELF (an intermediate the packager reads)
build/my_mode.map     # a linker map, handy only for checking your size budget
```

That is the whole build. `op_add_mode()` applied the right compiler flags and packaged the result for you, all as part of `cmake --build`. You never touch the ELF or the `.map`, unless one day you want to see what is filling your mode's size budget.

## Put it on the device

Copy the built mode onto real hardware:

1. Boot the device into USB drive mode so your computer mounts it as a drive.
2. Copy `build/my_mode.opm` into `/global/` (for a global mode) or `/output/` (for an output mode). The type is baked into the file at build time, and the device refuses a mode dropped into the wrong folder.
3. Open the Modes picker on the device and load it.

That is the round trip. Edit, build, copy the `.opm` across, and load it.

## Where to go next

Now that you can build and run a mode, the rest of the docs are organized by what you are trying to do:

- **Understand the device.** [How Operator Works](how-operator-works.md) explains the two cores, the MIDI signal chain, and the difference between global and output modes. [Mode Lifecycle](mode-lifecycle.md) covers when each of your hooks runs.
- **Learn the constraints.** [The Mode Mindset](the-mode-mindset.md) is the one concept page to read before writing anything nontrivial. It explains why there is no `double` and no C standard library, and how to work within that.
- **Process MIDI.** [Processing MIDI](processing-midi.md) is the core how-to: reading incoming messages and generating your own.
- **Do a specific thing.** [Parameters](parameters.md), [Custom UI](custom-ui.md), [Timing and Clock](timing-and-clock.md), [Storage and Files](storage-and-files.md), [Debugging](debugging.md), and [Testing](testing.md) are task-focused guides.
- **Look something up.** [ABI Reference](reference-abi.md), [.opm Format](reference-opm-format.md), and [Build Reference](reference-build.md) are the reference tables.

A good next move is to open the sample closest to what you want to build (the samples live under `../samples/`) and read it alongside [The Mode Mindset](the-mode-mindset.md).
