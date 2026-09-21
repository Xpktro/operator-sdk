# op_add_mode(name TYPE global|output [FLAGS <name>...] [OPM_NAME <basename>] SOURCES src1 [src2 ...])
#   TYPE    - required. Global or output mode, loaded from /global/ or /output/
#   FLAGS   - optional. Zero or more of the names below, in any order. What each
#             one asks the device for:
#             HANDLES_SYSEX    the mode deals in system-exclusive messages. It
#                              may call send_sysex, and inbound SysEx reaches its
#                              on_sysex handler.
#             FULLSCREEN_UI    the mode draws the whole screen, with no status bar
#             GENERATES_CLOCK  the mode can drive the device clock. Valid on
#                              TYPE global.
#             Example: FLAGS FULLSCREEN_UI GENERATES_CLOCK
#   OPM_NAME - optional. Overrides the .opm basename. Default is
#              the CMake target name. Use when two variants of the same
#              logical mode (e.g. user_scale_output + user_scale_global)
#              should share a single user-facing filename, with the
#              /global/ vs /output/ folder providing disambiguation.
#              The CMake target, OpmHeader.name, and OP_MODE_NAME compile
#              definition all stay equal to `name`. Only the on-disk
#              filename of the produced .opm changes.
#   SOURCES - one or more C++ sources compiled into the PIC .opm binary
# Example: op_add_mode(master_clock TYPE global FLAGS GENERATES_CLOCK SOURCES main.cpp)
#
# CMakeLists.txt using this helper is 3 content lines:
#   cmake_minimum_required(VERSION 3.28)
#   project(my_arp LANGUAGES CXX)
#   include($ENV{OPERATOR_SDK}/cmake/operator-mode.cmake)
#   op_add_mode(my_arp TYPE output SOURCES main.cpp)
#
# The helper:
#   1. Creates an executable from SOURCES
#   2. Applies linker/operator-mode.ld and PIE link options
#   3. Runs the pack-opm tool as a POST_BUILD step to emit <name>.opm
#      next to the ELF in the current binary dir.
#
# Must be used together with CMAKE_TOOLCHAIN_FILE=<sdk>/cmake/operator-mode-toolchain.cmake
# so the ARM flags from that file apply to compilation.
#
# `op_add_mode(...)` produces a loadable `<name>.opm` when invoked under the
# ARM toolchain (cmake/operator-mode-toolchain.cmake). Branching keys on
# CMAKE_CROSSCOMPILING (TRUE under the ARM toolchain). The non-cross branch is
# reserved for Operator's internal build tooling, which supplies its own wiring;
# that wiring is not part of the public SDK, so a native configure without it
# fails cleanly (see the dispatch below).

cmake_minimum_required(VERSION 3.28)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# op_add_mode attaches two C shim sources, so C has to be an enabled language for
# them to compile. CMake skips sources whose language is off without a diagnostic,
# and a mode's project() line commonly names CXX alone, so enabling C is the SDK's
# own job.
if(NOT CMAKE_C_COMPILER_LOADED)
    enable_language(C)
endif()

# ---- ARM path: pie-linked ELF + freestanding gate + pack-opm ------------
function(_op_add_mode_arm name type flags opm_name)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})

    # Auto-attach the freestanding intrinsics shim + single-precision math
    # shim. See src/freestanding_intrinsics.c (memcpy/memset/memmove/
    # __aeabi_uldivmod) and src/freestanding_math.c (expf/logf/powf) for
    # rationale. Every .opm links both transparently -- third-party
    # modes need no opt-in. Both files expose only allow-listed symbols, so the
    # check_opm_freestanding.py POST_BUILD gate passes.
    target_sources(${name} PRIVATE
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/freestanding_intrinsics.c
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/freestanding_math.c)

    # Public SDK include directory (umbrella header + abi/ mirrors).
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../include)

    # Inject build-time identifiers OP_MODE_REGISTER references for
    # __opm_header emission (no-op on ARM; the pack-opm tool owns header
    # bytes).
    string(TOUPPER ${type} _TYPE_UP)
    if(_TYPE_UP STREQUAL "GLOBAL")
        set(_type_value 0)
    else()
        set(_type_value 1)
    endif()
    target_compile_definitions(${name} PRIVATE
        "OP_MODE_NAME=\"${name}\""
        "OP_MODE_TYPE_VALUE=${_type_value}"
        "OP_MODE_FLAGS=${flags}"
        "OP_MODE_CONFIG_SCHEMA_VER=0")

    # Freestanding .opm compile flags.
    # The operator-mode.ld linker script uses KEEP(*(.rodata.kParams*)) etc.
    # to pin the packer's required ABI tables across --gc-sections. That KEEP
    # only matches when each symbol lives in its own .rodata.<symbol> input
    # section, which requires -fdata-sections. Without it, kParams lands in a
    # generic .rodata blob that --gc-sections drops unless some hot-path code
    # references the symbol. -ffunction-sections + -fdata-sections are also
    # the standard companion flags for --gc-sections on embedded targets.
    # -ffreestanding signals "no hosted C runtime" so libc-implicit
    # assumptions (hosted environment, libc guarantees) don't creep in.
    target_compile_options(${name} PRIVATE
        -ffunction-sections
        -fdata-sections
        -ffreestanding
        # Per-instance .data isolation: force every .data/.bss reference
        # through the GOT so .text carries zero data-pointing literals.
        # Without these, GOT redirection for sibling instances is
        # insufficient -- .text-resident literal pools would leak the
        # originator's .data address.
        # See docs/the-mode-mindset.md "per-instance state" section. The
        # check_no_text_relocs.py POST_BUILD gate below enforces this
        # invariant so a future flag drop fails the build instead of silently
        # corrupting sibling state.
        -fPIE
        -msingle-pic-base
        -mpic-register=r9
        -mno-pic-data-is-text-relative)

    target_link_options(${name} PRIVATE
        -T${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../linker/operator-mode.ld
        -Wl,--gc-sections
        -nostdlib
        -nostartfiles
        -Wl,-Map=${name}.map
        -pie
        # Silence binutils 2.39+ "LOAD segment with RWX permissions"
        # warning. Our single LOAD segment intentionally contains .text +
        # .rodata + .got + .bss + .rel.dyn so single-load-bias relocation can
        # apply one ram offset uniformly across all sections. The W^X concern
        # is a hosted-OS pattern (MMU-enforced page permissions) and does not
        # apply to operator's loader: it reads individual sections by name
        # from the .opm header and places them in pool RAM without honoring
        # segment-level attributes. The actual mode-isolation contract is
        # enforced by the no-text-relocs invariant gate
        # (check_no_text_relocs.py) plus the per-instance .data/.bss/.got slab
        # in the firmware mode loader.
        -Wl,--no-warn-rwx-segments)

    # Track the linker script as a link input so edits to it force a relink.
    # CMake treats a -T script as a flag, so the dependency has to be declared.
    set_target_properties(${name} PROPERTIES
        LINK_DEPENDS ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../linker/operator-mode.ld)

    # POST_BUILD step 1: reject any prohibited undefined symbol (libc, double-
    # precision soft-float, libgcc 64-bit helpers). Build-time gate that
    # prevents regression of the ARM cross-compile freestanding fixes. See
    # docs/the-mode-mindset.md#freestanding-constraints for the author-
    # facing contract; the enforcement script is the COMMAND just below.
    # A failure here aborts the POST_BUILD chain, so step 2 (packaging) is
    # only invoked on a clean ELF.
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/check_opm_freestanding.py
                --elf $<TARGET_FILE:${name}>
        COMMENT "Checking ${name} for prohibited undefined symbols (freestanding gate)"
        VERBATIM)

    # POST_BUILD step 1b: per-instance .data invariant -- assert zero
    # R_ARM_RELATIVE entries inside .text or .rodata. Runs after the
    # freestanding gate (step 1) and before pack_opm.py (step 2). A failure
    # here aborts the chain so a regressed build never ships a packed .opm.
    # See tools/check_no_text_relocs.py for the invariant rationale and
    # diagnostic. The gate exists because the four isolation compile flags
    # above are silent contracts -- a future toolchain or flag drop would leave
    # .text-resident .data/.bss literals that single-load-bias relocation
    # would point at the originator's runtime address, corrupting siblings.
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/check_no_text_relocs.py
                --elf $<TARGET_FILE:${name}>
        COMMENT "Checking ${name} for per-instance .data invariant (no R_ARM_RELATIVE in .text/.rodata)"
        VERBATIM)

    # POST_BUILD step 1c: assert __aeabi_uldivmod in the produced ELF uses
    # the EABI register-return ABI, not the broken C struct-return shape that
    # crashed midi_player playback. See tools/check_uldivmod_abi.py for
    # the regression-pattern detail and root-cause analysis. The gate
    # short-circuits to PASS when
    # __aeabi_uldivmod is absent (--gc-sections legitimately drops it from
    # modes that emit no 64-bit divide). A failure here aborts the chain
    # so a regressed build never ships a packed .opm.
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/check_uldivmod_abi.py
                --elf $<TARGET_FILE:${name}>
        COMMENT "Checking ${name} for __aeabi_uldivmod ABI shape"
        VERBATIM)

    # POST_BUILD step 2: pack the ELF into <opm_name>.opm via the packer.
    # The staged-artifact basename (opm_name) is decoupled from the CMake
    # target name (name); --name (OpmHeader.name) stays equal to the CMake
    # target name so manifest persistence + sibling-instance dedup keep their
    # current identity contract.
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/pack_opm.py
                --input  $<TARGET_FILE:${name}>
                --output ${CMAKE_CURRENT_BINARY_DIR}/${opm_name}.opm
                --type   ${type}
                --name   ${name}
                --flags  ${flags}
        COMMENT "Packaging ${opm_name}.opm (target=${name} TYPE=${type} FLAGS=${flags})"
        VERBATIM)
endfunction()

function(op_add_mode name)
    cmake_parse_arguments(OP "" "TYPE;OPM_NAME" "SOURCES;FLAGS" ${ARGN})

    # Validate TYPE
    if(NOT OP_TYPE MATCHES "^(global|output)$")
        message(FATAL_ERROR
            "op_add_mode(${name}): TYPE must be 'global' or 'output' (got '${OP_TYPE}')")
    endif()

    # Validate SOURCES
    if(NOT OP_SOURCES)
        message(FATAL_ERROR
            "op_add_mode(${name}): SOURCES is required and must be non-empty")
    endif()

    # FLAGS names compose, so a mode that draws its own screen and drives the clock
    # asks for both by name. An unknown name stops the configure here, where a raw
    # byte would have packed a bit the device has no meaning for.
    set(_flags 0)
    set(_generates_clock FALSE)
    foreach(_flag IN LISTS OP_FLAGS)
        if(_flag STREQUAL "HANDLES_SYSEX")
            set(_bit 1)
        elseif(_flag STREQUAL "FULLSCREEN_UI")
            set(_bit 2)
        elseif(_flag STREQUAL "GENERATES_CLOCK")
            set(_bit 4)
            set(_generates_clock TRUE)
        else()
            message(FATAL_ERROR
                "op_add_mode(${name}): unknown FLAGS value '${_flag}'. "
                "Valid names: HANDLES_SYSEX FULLSCREEN_UI GENERATES_CLOCK")
        endif()
        math(EXPR _flags "${_flags} | ${_bit}")
    endforeach()
    math(EXPR OP_FLAGS "${_flags}" OUTPUT_FORMAT HEXADECIMAL)

    if(_generates_clock AND OP_TYPE STREQUAL "output")
        message(FATAL_ERROR
            "op_add_mode(${name}): GENERATES_CLOCK is valid on TYPE global, and this "
            "mode declares TYPE output. See docs/timing-and-clock.md")
    endif()

    # OPM_NAME defaults to the CMake target name when omitted -- preserves
    # current behavior for every sample that doesn't opt in. When provided,
    # the staged artifact basename decouples from the target name. Used by
    # samples that share a "logical" mode name across global/output variants
    # so the staged filename matches the picker's intended display label
    # (the picker shows the file basename).
    if(NOT DEFINED OP_OPM_NAME OR "${OP_OPM_NAME}" STREQUAL "")
        set(OP_OPM_NAME "${name}")
    endif()

    # Branch on CMAKE_CROSSCOMPILING.
    # CMAKE_CROSSCOMPILING=TRUE when operator-mode-toolchain.cmake is active
    # (that file sets CMAKE_SYSTEM_NAME Generic). This STATUS message surfaces
    # the branch at configure time for debugging.
    if(CMAKE_CROSSCOMPILING)
        message(STATUS "op_add_mode(${name}) -> ARM path (.opm as ${OP_OPM_NAME}.opm)")
        _op_add_mode_arm(${name} ${OP_TYPE} ${OP_FLAGS} ${OP_OPM_NAME} SOURCES ${OP_SOURCES})
    else()
        # Native builds are reserved for Operator's internal tooling, which
        # injects _op_add_mode_internal. It is absent from the public SDK, so a
        # native configure fails cleanly here.
        if(COMMAND _op_add_mode_internal)
            _op_add_mode_internal(${name} ${OP_TYPE} ${OP_FLAGS} ${OP_OPM_NAME} SOURCES ${OP_SOURCES})
        else()
            message(FATAL_ERROR
                "op_add_mode(${name}): loadable Operator modes require the ARM toolchain "
                "(cmake/operator-mode-toolchain.cmake). Building modes without it is "
                "reserved for Operator's internal tooling and is not part of the public SDK.")
        endif()
    endif()
endfunction()
