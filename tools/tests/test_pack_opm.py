"""Tests for pack_opm.py.

Packs fixtures compiled with arm-none-eabi-g++ and reads the header back. The
fixtures are built here, so the tests need only the source and the toolchain, and
skip cleanly when the toolchain is absent.

The layout-model cases at the end call the packer's layout function directly, so they
state the arithmetic without a build in the way.
"""
from __future__ import annotations

import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

from conftest import (
    HELLO_SOURCE as _HELLO_MODE_SOURCE,
    LINKER_SCRIPT as _LINKER_SCRIPT,
    PROLOGUE as _COMMON_PROLOGUE,
    SDK_INCLUDE as _SDK_INCLUDE,
    compile_mode_elf as _compile_elf,
    decode_header as _decode_header,
    requires_arm_toolchain,
    run_pack_opm as _run_pack_opm,
)

pytestmark = requires_arm_toolchain

# The packer sits one directory up from this test tree. The layout-model cases import
# it and call its functions directly.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import pack_opm as _pack_opm  # noqa: E402

# Modes built against the real SDK macros, so the packer sees exactly the symbols
# OP_MODE_REGISTER emits. The three-arg form registers no handler; the four-arg
# form passes one third, beside process.
_REGISTER_3ARG_SOURCE = r"""
#include <operator_sdk.h>
#include <operator_sdk/params.h>

static void my_init() {}
static void my_process(OpMidiMessage*, uint8_t, uint32_t) {}
static void my_destroy() {}

OP_MODE_NO_PARAMS();
OP_MODE_REGISTER(my_init, my_process, my_destroy);
"""

_REGISTER_4ARG_SOURCE = r"""
#include <operator_sdk.h>
#include <operator_sdk/params.h>

static void my_init() {}
static void my_process(OpMidiMessage*, uint8_t, uint32_t) {}
static void my_destroy() {}
static bool my_on_sysex(uint8_t, const uint8_t*, uint16_t, uint8_t) { return false; }

OP_MODE_NO_PARAMS();
OP_MODE_REGISTER(my_init, my_process, my_on_sysex, my_destroy);
"""

# Declarative modes that differ only in whether they declare Action rows, so the
# packed on_select_count can be read back against a known expected length.
_NO_ACTION_PARAMS_SOURCE = r"""
#include <operator_sdk.h>
#include <operator_sdk/params.h>

inline constexpr op::params::Bool Enable {.name = "Enable", .default_ = true};

OP_MODE_PARAMS(Enable);

static void my_init() {}
static void my_process(OpMidiMessage*, uint8_t, uint32_t) {}
static void my_destroy() {}

OP_MODE_REGISTER(my_init, my_process, my_destroy);
"""

_TWO_ACTION_PARAMS_SOURCE = r"""
#include <operator_sdk.h>
#include <operator_sdk/params.h>

void run_first();
void run_second();

inline constexpr op::params::Action First  {.name = "First",  .on_select = &run_first};
inline constexpr op::params::Action Second {.name = "Second", .on_select = &run_second};

OP_MODE_PARAMS(First, Second);

void run_first() {}
void run_second() {}

static void my_init() {}
static void my_process(OpMidiMessage*, uint8_t, uint32_t) {}
static void my_destroy() {}

OP_MODE_REGISTER(my_init, my_process, my_destroy);
"""

_SDK_INCLUDE_FLAG = [f"-I{_SDK_INCLUDE}"]

# The mode helper and the toolchain file, reached from the linker script's own
# directory so this file names no path of its own.
_SDK_ROOT = _LINKER_SCRIPT.parents[1]
_MODE_HELPER = _SDK_ROOT / "cmake" / "operator-mode.cmake"
_TOOLCHAIN_FILE = _SDK_ROOT / "cmake" / "operator-mode-toolchain.cmake"

# A configure is what carries the helper's own refusal, so the case that drives one
# needs cmake as well as the toolchain the module marker already covers.
requires_cmake = pytest.mark.skipif(
    shutil.which("cmake") is None,
    reason="cmake not on PATH; this test configures a mode project "
           "(see reference-build.md for the toolchain).",
)


# ---------------------------------------------------------------------------
# Fixture source templates specific to this file's cases
# ---------------------------------------------------------------------------
_MISSING_INIT_SOURCE = _COMMON_PROLOGUE + r"""
extern "C" {

__attribute__((used)) const ParamSpec kParams[1] = {
    { 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, {0, 0, 0, 0} }
};
__attribute__((used)) const uint8_t kParamCount = 1;
__attribute__((used)) const char    kParamStrings[] = "enable\0";

// mode_init intentionally omitted
void mode_process(OpMidiMessage*, uint8_t, uint32_t) {}
void mode_destroy() {}

}  // extern "C"
"""

# Without extern "C", C++ mangles every function name.
_MANGLED_INIT_SOURCE = _COMMON_PROLOGUE + r"""
extern "C" {
__attribute__((used)) const ParamSpec kParams[1] = {
    { 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, {0, 0, 0, 0} }
};
__attribute__((used)) const uint8_t kParamCount = 1;
__attribute__((used)) const char    kParamStrings[] = "enable\0";
}  // extern "C"

// These three forget extern "C", so the compiler emits mangled symbols.
__attribute__((used)) void mode_init(const OperatorApi*) {}
__attribute__((used)) void mode_process(OpMidiMessage*, uint8_t, uint32_t) {}
__attribute__((used)) void mode_destroy() {}
"""

_UI_EXPORTS_SOURCE = _COMMON_PROLOGUE + r"""
extern "C" {

__attribute__((used)) const ParamSpec kParams[1] = {
    { 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, {0, 0, 0, 0} }
};
__attribute__((used)) const uint8_t kParamCount = 1;
__attribute__((used)) const char    kParamStrings[] = "enable\0";

void mode_init(const OperatorApi*) {}
void mode_process(OpMidiMessage*, uint8_t, uint32_t) {}
void mode_destroy() {}

// The custom-UI exports, whose presence makes pack_opm.py fill in
// ui_render_offset and ui_gesture_offset.
void mode_ui_render() {}
void mode_ui_gesture(uint8_t, uint8_t, int16_t) {}

}  // extern "C"
"""

# A zero-param mode, the shape OP_MODE_NO_PARAMS emits: kParamCount at 0 beside a
# dummy kParams[1] and kParamStrings[1] that exist only for the linker to resolve.
_NO_PARAMS_SOURCE = _COMMON_PROLOGUE + r"""
extern "C" {

__attribute__((used)) const ParamSpec kParams[1] = {};
__attribute__((used)) const uint8_t   kParamCount = 0;
__attribute__((used)) const char      kParamStrings[1] = "";

void mode_init(const OperatorApi*) {}
void mode_process(OpMidiMessage*, uint8_t, uint32_t) {}
void mode_destroy() {}

}  // extern "C"
"""


def _write_clock_mode_project(tmp_path: Path, directory: str, mode_type: str) -> Path:
    """Write a mode project declaring `mode_type` together with the clock flag.

    The helper refuses before it reaches add_executable, so the source only has to
    exist. The control case configures all the way through and compiles nothing.
    """
    root = tmp_path / directory
    root.mkdir()
    (root / "main.cpp").write_text('extern "C" void mode_init() {}\n')
    (root / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.28)\n"
        "project(scratch_clock LANGUAGES CXX)\n"
        f"include({_MODE_HELPER.as_posix()})\n"
        f"op_add_mode(scratch_clock TYPE {mode_type} FLAGS GENERATES_CLOCK "
        "SOURCES main.cpp)\n"
    )
    return root


def _configure(root: Path) -> subprocess.CompletedProcess:
    """Configure a mode project under the SDK toolchain, capturing both streams."""
    return subprocess.run(
        ["cmake", "-S", str(root), "-B", str(root / "build"),
         f"-DCMAKE_TOOLCHAIN_FILE={_TOOLCHAIN_FILE}"],
        capture_output=True, text=True,
    )


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
def test_happy_path_global(tmp_path):
    elf = _compile_elf(tmp_path, _HELLO_MODE_SOURCE)
    opm = tmp_path / "hello_mode.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "global", "--name", "hello_mode",
    )
    assert r.returncode == 0, r.stderr
    hdr = _decode_header(opm)
    assert hdr["magic"] == 0x4F504D31
    assert hdr["header_size"] == 148
    assert hdr["sdk_version"] == 1
    assert hdr["mode_type"] == 0           # global
    assert hdr["param_count"] == 1
    assert hdr["name"].rstrip(b"\0") == b"hello_mode"
    # First 4 bytes of the file must decode as the magic, too.
    first4 = opm.read_bytes()[:4]
    assert struct.unpack("<I", first4)[0] == 0x4F504D31


def test_happy_path_output(tmp_path):
    elf = _compile_elf(tmp_path, _HELLO_MODE_SOURCE)
    opm = tmp_path / "hello_mode.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "output", "--name", "hello_mode",
    )
    assert r.returncode == 0, r.stderr
    hdr = _decode_header(opm)
    assert hdr["mode_type"] == 1           # output
    assert hdr["magic"] == 0x4F504D31


def test_missing_symbol_mode_init(tmp_path):
    elf = _compile_elf(tmp_path, _MISSING_INIT_SOURCE)
    opm = tmp_path / "broken.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "global", "--name", "broken",
    )
    assert r.returncode == 1
    assert "mode_init" in r.stderr
    assert "extern" in r.stderr.lower()


def test_missing_extern_c(tmp_path):
    # Compile without --gc-sections so the mangled mode_* symbols stay in .symtab.
    # With --gc-sections the linker prunes them, and the packer can still exit 1 but
    # cannot see the mangled form to point at extern "C". This checks the targeted
    # message, which needs the mangled symbol present.
    elf = _compile_elf(tmp_path, _MANGLED_INIT_SOURCE, gc_sections=False)
    opm = tmp_path / "mangled.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "global", "--name", "mangled",
    )
    assert r.returncode == 1
    # Targeted suggestion when an export is name-mangled instead of extern "C".
    assert "Did you forget extern" in r.stderr
    assert "mode_init" in r.stderr


def test_ui_offsets_zero_when_absent(tmp_path):
    elf = _compile_elf(tmp_path, _HELLO_MODE_SOURCE)
    opm = tmp_path / "hello_mode.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "global", "--name", "hello_mode",
    )
    assert r.returncode == 0, r.stderr
    hdr = _decode_header(opm)
    assert hdr["ui_render_offset"] == 0
    assert hdr["ui_gesture_offset"] == 0
    assert hdr["migrate_offset"] == 0


def test_ui_offsets_nonzero_when_present(tmp_path):
    elf = _compile_elf(tmp_path, _UI_EXPORTS_SOURCE)
    opm = tmp_path / "with_ui.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "output", "--name", "with_ui",
    )
    assert r.returncode == 0, r.stderr
    hdr = _decode_header(opm)
    assert hdr["ui_render_offset"] != 0
    assert hdr["ui_gesture_offset"] != 0
    # Both must reside inside [text_offset, text_offset + text_size).
    assert hdr["text_offset"] <= hdr["ui_render_offset"] < hdr["text_offset"] + hdr["text_size"]
    assert hdr["text_offset"] <= hdr["ui_gesture_offset"] < hdr["text_offset"] + hdr["text_size"]


def test_register_three_arg_emits_no_on_sysex(tmp_path):
    """The three-arg OP_MODE_REGISTER emits no handler, so on_sysex_offset is 0."""
    elf = _compile_elf(tmp_path, _REGISTER_3ARG_SOURCE,
                       extra_cxx_flags=_SDK_INCLUDE_FLAG)
    opm = tmp_path / "three_arg.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "global", "--name", "three_arg",
    )
    assert r.returncode == 0, r.stderr
    assert _decode_header(opm)["on_sysex_offset"] == 0


def test_register_four_arg_emits_on_sysex(tmp_path):
    """The four-arg OP_MODE_REGISTER emits the handler, so on_sysex_offset lands in .text."""
    elf = _compile_elf(tmp_path, _REGISTER_4ARG_SOURCE,
                       extra_cxx_flags=_SDK_INCLUDE_FLAG)
    opm = tmp_path / "with_sysex.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "global", "--name", "with_sysex",
        "--flags", "0x01",
    )
    assert r.returncode == 0, r.stderr
    hdr = _decode_header(opm)
    assert hdr["on_sysex_offset"] != 0
    assert hdr["text_offset"] <= hdr["on_sysex_offset"] < hdr["text_offset"] + hdr["text_size"]


def test_on_select_count_is_zero_without_action_params(tmp_path):
    """A mode declaring no Action row packs an on_select_count of 0."""
    elf = _compile_elf(tmp_path, _NO_ACTION_PARAMS_SOURCE,
                       extra_cxx_flags=_SDK_INCLUDE_FLAG)
    opm = tmp_path / "no_action.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "global", "--name", "no_action",
    )
    assert r.returncode == 0, r.stderr
    hdr = _decode_header(opm)
    assert hdr["on_select_count"] == 0
    assert hdr["on_select_table_offset"] == 0


def test_on_select_count_matches_declared_action_params(tmp_path):
    """The packed on_select_count equals the number of Action rows declared."""
    elf = _compile_elf(tmp_path, _TWO_ACTION_PARAMS_SOURCE,
                       extra_cxx_flags=_SDK_INCLUDE_FLAG)
    opm = tmp_path / "two_actions.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "global", "--name", "two_actions",
    )
    assert r.returncode == 0, r.stderr
    hdr = _decode_header(opm)
    assert hdr["on_select_count"] == 2
    assert hdr["on_select_table_offset"] != 0


def test_handles_sysex_without_handler_is_rejected(tmp_path):
    """A HANDLES_SYSEX mode with no registered handler fails the pack with an actionable message."""
    elf = _compile_elf(tmp_path, _REGISTER_3ARG_SOURCE,
                       extra_cxx_flags=_SDK_INCLUDE_FLAG)
    opm = tmp_path / "sysex_no_handler.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "global", "--name", "sysex_no_handler",
        "--flags", "0x01",
    )
    assert r.returncode != 0
    assert "HANDLES_SYSEX" in r.stderr
    assert "OP_MODE_REGISTER" in r.stderr


def test_zero_param_mode_packs_with_zeroed_param_fields(tmp_path):
    """A zero-param mode packs with all four param fields at 0.

    OP_MODE_NO_PARAMS emits a dummy kParams[1] and kParamStrings[1] so the linker can
    resolve the required exports, but there is no real table. The packer treats the
    dummy as a placeholder and advertises no param table, which is what the OpmHeader
    contract wants when param_count is 0: all four param fields zero, and the loader
    reads the table only when param_table_offset is non-zero.
    """
    elf = _compile_elf(tmp_path, _NO_PARAMS_SOURCE)
    opm = tmp_path / "no_params_mode.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "global", "--name", "no_params_mode",
    )
    assert r.returncode == 0, r.stderr
    hdr = _decode_header(opm)
    # The whole point: zero params -> zeroed param header group.
    assert hdr["param_count"] == 0
    assert hdr["param_table_offset"] == 0
    assert hdr["param_table_size"] == 0
    assert hdr["param_strings_offset"] == 0
    assert hdr["param_strings_size"] == 0
    # The rest of the header must still be well-formed.
    assert hdr["magic"] == 0x4F504D31
    assert hdr["header_size"] == 148
    assert hdr["mode_type"] == 0           # global
    assert hdr["text_size"] > 0
    assert hdr["name"].rstrip(b"\0") == b"no_params_mode"


def test_flags_propagate(tmp_path):
    """Sanity check: --flags round-trips through OpmHeader.flags."""
    elf = _compile_elf(tmp_path, _HELLO_MODE_SOURCE)
    opm = tmp_path / "hello_mode.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "global", "--name", "hello_mode",
        "--flags", "0x06",
    )
    assert r.returncode == 0, r.stderr
    hdr = _decode_header(opm)
    assert hdr["flags"] == 0x06


# ---------------------------------------------------------------------------
# rodata + reloc header-field + group-alignment tests
# ---------------------------------------------------------------------------
def test_header_has_rodata_fields(tmp_path):
    """The packer records the .rodata offset and size."""
    elf = _compile_elf(tmp_path, _HELLO_MODE_SOURCE)
    opm = tmp_path / "hello_mode.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "global", "--name", "hello_mode",
    )
    assert r.returncode == 0, r.stderr
    hdr = _decode_header(opm)
    # kParams + kParamCount + kParamStrings live in .rodata, so it has bytes.
    assert hdr["rodata_size"] > 0, (
        f"rodata_size expected > 0 but was {hdr['rodata_size']}"
    )
    # Packer places .rodata at/after .text in the file layout.
    assert hdr["rodata_offset"] >= hdr["text_offset"] + hdr["text_size"], (
        f"rodata_offset {hdr['rodata_offset']} must come at/after "
        f"text_offset+text_size ({hdr['text_offset']}+{hdr['text_size']})"
    )


def test_header_has_reloc_fields(tmp_path):
    """The packer records the R_ARM_RELATIVE offsets."""
    elf = _compile_elf(tmp_path, _HELLO_MODE_SOURCE)
    opm = tmp_path / "hello_mode.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "global", "--name", "hello_mode",
    )
    assert r.returncode == 0, r.stderr
    hdr = _decode_header(opm)
    if hdr["reloc_count"] == 0:
        # Sentinel convention when no relocations: reloc_offset == 0.
        assert hdr["reloc_offset"] == 0, (
            f"reloc_offset must be 0 when reloc_count==0, "
            f"was {hdr['reloc_offset']}"
        )
    else:
        # 4-byte aligned.
        assert hdr["reloc_offset"] & 3 == 0, (
            f"reloc_offset {hdr['reloc_offset']} must be 4-byte aligned"
        )
        # Lives at/after data section.
        assert hdr["reloc_offset"] >= hdr["data_offset"] + hdr["data_size"], (
            f"reloc_offset {hdr['reloc_offset']} must come at/after "
            f"data end ({hdr['data_offset']}+{hdr['data_size']})"
        )
        # Every decoded offset is 4-byte aligned and in bounds.
        file_bytes = opm.read_bytes()
        reloc_blob = file_bytes[
            hdr["reloc_offset"] : hdr["reloc_offset"] + hdr["reloc_count"] * 4
        ]
        assert len(reloc_blob) == hdr["reloc_count"] * 4
        # The packer bounds each offset by the whole allocatable region, since a
        # reference through the GOT lands past .rodata.
        ram_size = (hdr["text_size"] + hdr["rodata_size"] + hdr["got_size"]
                    + hdr["data_size"] + hdr["bss_size"])
        for i in range(hdr["reloc_count"]):
            (off,) = struct.unpack_from("<I", reloc_blob, i * 4)
            assert off & 3 == 0, f"entry {i} offset 0x{off:x} not 4-aligned"
            assert 0 <= off < ram_size, (
                f"entry {i} offset 0x{off:x} outside [0, ram_size={ram_size:#x})"
            )


def test_header_groups_align_to_expected_byte_offsets(tmp_path):
    """HEADER_FMT encodes every group's first field at the declared byte."""
    elf = _compile_elf(tmp_path, _HELLO_MODE_SOURCE)
    opm = tmp_path / "hello_mode.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "global", "--name", "hello_mode",
    )
    assert r.returncode == 0, r.stderr
    hdr = _decode_header(opm)
    # Read the raw header bytes straight from the file.
    raw = opm.read_bytes()[:148]
    assert len(raw) == 148, f"header shorter than 148 bytes: {len(raw)}"
    # Group 1 starts at 0: magic
    assert struct.unpack_from("<I", raw, 0)[0] == hdr["magic"]
    # Group 2 starts at 16: text_offset
    assert struct.unpack_from("<I", raw, 16)[0] == hdr["text_offset"]
    # Group 3 starts at 52: reloc_offset
    assert struct.unpack_from("<I", raw, 52)[0] == hdr["reloc_offset"]
    # Group 4 starts at 60: init_offset
    assert struct.unpack_from("<I", raw, 60)[0] == hdr["init_offset"]
    # The two extended entry points close Group 4, at 84 and 88.
    assert struct.unpack_from("<I", raw, 84)[0] == hdr["on_sysex_offset"]
    assert struct.unpack_from("<I", raw, 88)[0] == hdr["on_select_table_offset"]
    # Group 5 starts at 92: param_table_offset
    assert struct.unpack_from("<I", raw, 92)[0] == hdr["param_table_offset"]
    # Group 6 starts at 108: name (32 bytes)
    assert struct.unpack_from("<32s", raw, 108)[0] == hdr["name"]
    # Group 7 starts at 140: reserved tail, must be zero-filled (8 bytes)
    assert raw[140:148] == b"\x00" * 8


# ---------------------------------------------------------------------------
# The clock-generating rule
#
# The three pack cases separate the combination from the type and from the flag, and
# the fourth drives the configure the helper refuses at.
# ---------------------------------------------------------------------------
def test_an_output_mode_carrying_the_clock_flag_is_refused(tmp_path):
    """The combination never becomes a .opm, and the refusal names the rule."""
    elf = _compile_elf(tmp_path, _HELLO_MODE_SOURCE)
    opm = tmp_path / "bound_clock.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "output", "--name", "bound_clock",
        "--flags", "0x04",
    )
    assert r.returncode != 0
    assert "GENERATES_CLOCK" in r.stderr
    assert "global" in r.stderr
    assert not opm.exists(), "a refused mode must leave no .opm behind"


def test_a_global_mode_carrying_the_clock_flag_packs(tmp_path):
    """The same flag on a global mode packs, so the refusal is on the combination."""
    elf = _compile_elf(tmp_path, _HELLO_MODE_SOURCE)
    opm = tmp_path / "whole_device_clock.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "global", "--name", "whole_device_clock",
        "--flags", "0x04",
    )
    assert r.returncode == 0, r.stderr
    hdr = _decode_header(opm)
    assert hdr["mode_type"] == 0           # global
    assert hdr["flags"] == 0x04


def test_an_output_mode_carrying_a_flag_other_than_clock_packs(tmp_path):
    """The same type with another flag packs, so the refusal is not on the type."""
    elf = _compile_elf(tmp_path, _HELLO_MODE_SOURCE)
    opm = tmp_path / "bound_fullscreen.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "output", "--name", "bound_fullscreen",
        "--flags", "0x02",
    )
    assert r.returncode == 0, r.stderr
    hdr = _decode_header(opm)
    assert hdr["mode_type"] == 1           # output
    assert hdr["flags"] == 0x02


@requires_cmake
def test_the_configure_refuses_the_clock_flag_on_an_output_mode_but_not_a_global_one(
        tmp_path):
    """The helper stops the build where the author has their own source in front of them."""
    refused = _configure(_write_clock_mode_project(tmp_path, "bound", "output"))
    assert refused.returncode != 0
    assert "GENERATES_CLOCK" in refused.stderr
    assert "global" in refused.stderr

    allowed = _configure(_write_clock_mode_project(tmp_path, "whole_device", "global"))
    assert allowed.returncode == 0, allowed.stderr


# ---------------------------------------------------------------------------
# The layout model
#
# A plain dict stands in for a pyelftools section, so the arithmetic is stated
# without a build in the way.
# ---------------------------------------------------------------------------
def test_loader_layout_starts_each_section_at_the_rounded_end_of_the_last():
    layout = _pack_opm.loader_layout({
        ".text": 0x4074, ".rodata": 0x0103, ".got": 0x0088,
        ".data": 0x0004, ".bss": 0x8470,
    })
    assert layout[".text"] == 0x0000
    assert layout[".rodata"] == 0x4078
    assert layout[".got"] == 0x4180
    assert layout[".data"] == 0x4208
    assert layout[".bss"] == 0x4210
    assert layout["_end"] == 0x4210 + 0x8470


def test_loader_layout_counts_an_absent_section_as_zero_size():
    """Two shipped samples have no .data, and the loader reads that as a zero size."""
    layout = _pack_opm.loader_layout(
        {".text": 64, ".rodata": 16, ".got": 8, ".bss": 12})
    assert layout[".data"] == 88
    assert layout[".bss"] == 88, ".data contributes nothing, so .bss starts on .got's end"
    assert layout["_end"] == 104
