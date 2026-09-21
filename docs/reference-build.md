# Build Reference {#reference-build}

How to wire up a mode's build: the `CMakeLists.txt` you write, and the options `op_add_mode()` gives you. For installing the toolchain, see [Getting Started](getting-started.md).

@note The SDK compiles modes in a freestanding, single-precision, integer-first environment (see @ref freestanding-constraints "The Mode Mindset"). The most common build failure is accidentally using `double` or a C library function. The SDK checks for this at build time and rejects the mode with a clear, categorized error.

## Your mode's CMakeLists.txt

A mode is an ordinary CMake project that pulls in the SDK helper and calls `op_add_mode()`. The declarative and custom-UI templates ship a working `CMakeLists.txt` you can copy, and the whole thing is just four lines:

```cmake
cmake_minimum_required(VERSION 3.28)
project(my_mode LANGUAGES CXX)

# Point this at cmake/operator-mode.cmake in your SDK checkout.
include(/path/to/sdk/cmake/operator-mode.cmake)

op_add_mode(my_mode TYPE global SOURCES main.cpp)
```

The templates use a relative `include(${CMAKE_CURRENT_LIST_DIR}/../cmake/operator-mode.cmake)` because they live inside the SDK tree. A mode kept elsewhere just points `include()` at the same file in your SDK checkout.

Configure with the SDK toolchain file, then build:

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/sdk/cmake/operator-mode-toolchain.cmake
cmake --build build
```

You always need both pieces. The toolchain file supplies the compiler and its flags (including `-fno-exceptions -fno-rtti`), and `op_add_mode()` supplies the link and packaging steps. After the build, `build/my_mode.opm` is ready for the device. The compiled ELF sits next to it (the packager reads it), along with a linker `.map` you can consult if you ever need to see what fills your mode's size budget.

## `op_add_mode()` options

`op_add_mode()` is declared in `cmake/operator-mode.cmake`.

```cmake
op_add_mode(<name>
            TYPE   global | output
            [FLAGS <name>...]
            SOURCES <src1> [<src2> ...])
```

**Required:**

- `name` is the target name. It becomes the `.opm` filename and is written into the mode's header verbatim, so keep it short (<= 31 bytes, no `/`, backslash, or `.` characters).
- `TYPE` is `global` (drops into `/global/` on the device) or `output` (drops into `/output/`). The type is baked into the file, and the device refuses a mode loaded from the wrong folder.
- `SOURCES` is one or more C++ sources. Multi-file modes are supported and link into a single `.opm`.

**Optional:**

- `FLAGS` declares the extra capabilities a mode needs at runtime. An unknown name fails the build.

  | Flag | Capability |
  | ---- | ---------- |
  | `FULLSCREEN_UI` | The mode draws the whole screen, with no status bar over it. |
  | `GENERATES_CLOCK` | The mode can drive the device clock. |
  | `HANDLES_SYSEX` | The mode actively inspects, consumes, or generates system-exclusive messages. It may call `send_sysex`, and inbound SysEx reaches its `on_sysex` handler. |

  They compose, so `FLAGS FULLSCREEN_UI GENERATES_CLOCK` is a clock-generating mode that owns the screen. The bits they set are in @ref flag-bits.

## The reproducible devcontainer

If you would rather not install the toolchain on your host, a Debian image with every tool pinned ships under `.devcontainer/`. Open the SDK directory in VS Code and the Dev Containers extension offers to reopen inside it, or build it directly:

```bash
docker build -t operator-sdk .devcontainer
docker run --rm -it -v "$PWD:/workspaces/operator-sdk" operator-sdk bash
```

The pinned versions match the manual install, so artifacts built either way are identical.

## See also

- Build-failure diagnostics in [Debugging](debugging.md).
