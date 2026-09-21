"""Shared fixture builders for the tool tests.

The tests compile their own mode ELFs with arm-none-eabi-g++ and the real mode
flags, so they need only this source tree and the toolchain. A test that needs the
toolchain skips when it is not on PATH.
"""
from __future__ import annotations

import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

# The linker script lives in the SDK next to this test tree.
_SDK_ROOT = Path(__file__).resolve().parents[2]
LINKER_SCRIPT = _SDK_ROOT / "linker" / "operator-mode.ld"
SDK_INCLUDE = _SDK_ROOT / "include"

ARM_GXX = shutil.which("arm-none-eabi-g++")

# Skip a whole module that needs the toolchain with this marker.
requires_arm_toolchain = pytest.mark.skipif(
    ARM_GXX is None,
    reason="arm-none-eabi-g++ not on PATH; this test compiles a mode fixture "
           "(see reference-build.md for the toolchain).",
)

# The flags a mode is built with, so a fixture ELF has the same shape a real mode
# has.
MODE_CXX_FLAGS = [
    "-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft",
    "-std=c++20",
    "-fno-exceptions", "-fno-rtti",
    "-ffreestanding", "-ffunction-sections", "-fdata-sections",
    "-Os",
    "-fPIE", "-msingle-pic-base",
    "-mpic-register=r9", "-mno-pic-data-is-text-relative",
]

MODE_LD_FLAGS = [
    f"-T{LINKER_SCRIPT}",
    "-Wl,--gc-sections",
    "-nostdlib",
    "-nostartfiles",
    "-pie",
]

# The types and forward declarations every fixture shares. The forward `extern "C"
# const` declarations give the definitions below external linkage, which a file-scope
# `const` does not get on its own even inside extern "C", so --gc-sections keeps the
# param tables the packer reads.
PROLOGUE = r"""
#include <cstdint>

struct OperatorApi;
struct OpMidiMessage {
    uint8_t status; uint8_t data1; uint8_t data2; uint8_t port; uint8_t length;
};

struct __attribute__((packed)) ParamSpec {
    uint16_t name_offset;
    uint16_t options_offset;
    uint8_t  kind;
    uint8_t  is_per_instance;
    uint8_t  decimal_places;
    uint8_t  option_count;
    int32_t  min_value;
    int32_t  max_value;
    int32_t  default_value;
    int32_t  step_value;
    uint8_t  value_slot_count;
    uint8_t  _reserved[7];
};
static_assert(sizeof(ParamSpec) == 32, "ParamSpec must be 32 bytes");

extern "C" const ParamSpec kParams[1];
extern "C" const uint8_t   kParamCount;
extern "C" const char      kParamStrings[];
"""

# One Bool param and the three lifecycle exports, with no state, so there is nothing
# to relocate.
HELLO_SOURCE = PROLOGUE + r"""
extern "C" {

__attribute__((used)) const ParamSpec kParams[1] = {
    { 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, {0, 0, 0, 0} }
};
__attribute__((used)) const uint8_t kParamCount = 1;
__attribute__((used)) const char    kParamStrings[] = "enable\0";

void mode_init(const OperatorApi*) {}
void mode_process(OpMidiMessage*, uint8_t, uint32_t) {}
void mode_destroy() {}

}  // extern "C"
"""

# A mode with real global state the code reads and writes, which the mode flags turn
# into the R_ARM_RELATIVE entries the packer extracts, the same as a stateful sample
# mode.
STATEFUL_SOURCE = PROLOGUE + r"""
extern "C" {

__attribute__((used)) const ParamSpec kParams[1] = {
    { 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, {0, 0, 0, 0} }
};
__attribute__((used)) const uint8_t kParamCount = 1;
__attribute__((used)) const char    kParamStrings[] = "enable\0";

static uint32_t g_counter = 1;
static uint32_t g_history[8] = {0};
static uint32_t g_scratch[8] = {0};

void mode_init(const OperatorApi*) { g_counter = 0; }
void mode_process(OpMidiMessage*, uint8_t count, uint32_t tick) {
    g_counter += count;
    g_history[g_counter & 7] = g_counter;
    g_scratch[tick & 7] += g_history[tick & 7];
}
void mode_destroy() { g_scratch[0] = g_counter; }

}  // extern "C"
"""


# The packed OpmHeader, matching pack_opm.py's HEADER_FMT, so a header packed there
# decodes here. Seven groups, 148 bytes.
HEADER_FMT = (
    "<"
    "I"   "I"          # magic, header_size
    "H"   "B"   "B"    # sdk_version, mode_type, flags
    "H"   "B"   "B"    # config_schema_ver, param_count, on_select_count
    "I"   "I"          # text_offset, text_size
    "I"   "I"          # rodata_offset, rodata_size
    "I"   "I"          # got_offset, got_size
    "I"   "I"          # data_offset, data_size
    "I"                # bss_size
    "I"   "I"          # reloc_offset, reloc_count
    "I"   "I"   "I"    # init_offset, process_offset, destroy_offset
    "I"                # migrate_offset
    "I"   "I"          # ui_render_offset, ui_gesture_offset
    "I"   "I"          # on_sysex_offset, on_select_table_offset
    "I"   "I"          # param_table_offset, param_table_size
    "I"   "I"          # param_strings_offset, param_strings_size
    "32s"              # name
    "8x"               # _reserved_tail[8]
)
assert struct.calcsize(HEADER_FMT) == 148

HEADER_FIELDS = (
    "magic", "header_size",
    "sdk_version", "mode_type", "flags",
    "config_schema_ver", "param_count", "on_select_count",
    "text_offset", "text_size",
    "rodata_offset", "rodata_size",
    "got_offset", "got_size",
    "data_offset", "data_size",
    "bss_size",
    "reloc_offset", "reloc_count",
    "init_offset", "process_offset", "destroy_offset",
    "migrate_offset",
    "ui_render_offset", "ui_gesture_offset",
    "on_sysex_offset", "on_select_table_offset",
    "param_table_offset", "param_table_size",
    "param_strings_offset", "param_strings_size",
    "name",
)


def decode_header(opm_path: Path) -> dict:
    """Decode a packed .opm's header into a field-name -> value dict."""
    data = opm_path.read_bytes()
    values = struct.unpack_from(HEADER_FMT, data, 0)
    return dict(zip(HEADER_FIELDS, values))


def compile_mode_elf(
    tmp_path: Path,
    source: str,
    elf_name: str = "mode.elf",
    gc_sections: bool = True,
    extra_cxx_flags: list[str] | None = None,
) -> Path:
    """Write `source` to tmp_path and link it into a mode ELF.

    Set `gc_sections=False` to keep the linker from pruning unreferenced sections,
    which the missing-extern-C test relies on so the mangled symbols survive for the
    packer to see. Pass `extra_cxx_flags` (for example `-I` for the SDK headers) when
    a fixture is built against the real macros rather than the local prologue.
    """
    src = tmp_path / "mode.cpp"
    src.write_text(source)
    elf = tmp_path / elf_name
    ld_flags = list(MODE_LD_FLAGS)
    if not gc_sections:
        ld_flags = [f for f in ld_flags if f != "-Wl,--gc-sections"]
    cmd = [ARM_GXX, *MODE_CXX_FLAGS, *(extra_cxx_flags or []),
           *ld_flags, str(src), "-o", str(elf)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        pytest.fail(
            "arm-none-eabi-g++ failed to build the fixture:\n"
            f"  cmd: {' '.join(cmd)}\n"
            f"  stdout: {result.stdout}\n"
            f"  stderr: {result.stderr}"
        )
    return elf


def run_pack_opm(*args: str) -> subprocess.CompletedProcess:
    """Run pack_opm.py, capturing stdout and stderr."""
    pack_opm = _SDK_ROOT / "tools" / "pack_opm.py"
    return subprocess.run(
        [sys.executable, str(pack_opm), *args],
        capture_output=True, text=True,
    )
