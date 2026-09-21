"""Tests for check_opm_freestanding.py.

Exercises the undefined-symbol gate three ways: through its CLI, by importing its
pure functions and by feeding it synthetic and compiled ELFs. The synthetic ELFs are
built in memory, so most of the suite runs anywhere. The one test that compiles a
real mode skips when the toolchain is absent.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

from conftest import HELLO_SOURCE, compile_mode_elf, requires_arm_toolchain

TOOL = Path(__file__).resolve().parent.parent / "check_opm_freestanding.py"


def run(args, check=False):
    """Run the tool, capturing stdout and stderr as text."""
    return subprocess.run(
        [sys.executable, str(TOOL), *args],
        capture_output=True,
        text=True,
        check=check,
    )


# ---------------------------------------------------------------------------
# CLI smoke tests
# ---------------------------------------------------------------------------
def test_tool_exists_and_executable():
    """Sanity: the script file lives at the expected path."""
    assert TOOL.exists(), f"check_opm_freestanding.py not found at {TOOL}"


def test_help_succeeds():
    result = run(["--help"])
    assert result.returncode == 0
    assert "ELF" in result.stdout or "elf" in result.stdout


def test_missing_elf_argument():
    """argparse exits 2 when a required argument is missing."""
    result = run([])
    assert result.returncode == 2
    # argparse writes "the following arguments are required" to stderr.
    assert "--elf" in result.stderr or "required" in result.stderr.lower()


def test_nonexistent_elf():
    result = run(["--elf", "/nonexistent/path.elf"])
    assert result.returncode == 2
    assert "not found" in result.stderr.lower()


# ---------------------------------------------------------------------------
# Compiled-ELF integration
# ---------------------------------------------------------------------------
@requires_arm_toolchain
def test_clean_mode_elf_passes(tmp_path):
    """A real mode compiled freestanding passes the gate."""
    elf = compile_mode_elf(tmp_path, HELLO_SOURCE)
    result = run(["--elf", str(elf)])
    assert result.returncode == 0, result.stderr


# ---------------------------------------------------------------------------
# Unit tests against the module's pure functions.
#
# The module imports with no side effect beyond its pyelftools check, so categorize()
# and the allow-list constants are exercised directly, with no toolchain needed.
# ---------------------------------------------------------------------------
@pytest.fixture(scope="module")
def mod():
    sys.path.insert(0, str(TOOL.parent))
    try:
        import check_opm_freestanding as m  # type: ignore
    finally:
        # Leave sys.path alone to avoid surprising other tests.
        pass
    return m


def test_categorize_covers_aeabi_double(mod):
    assert mod.categorize("__aeabi_ddiv").startswith("double")
    assert mod.categorize("__aeabi_dmul").startswith("double")
    assert mod.categorize("__aeabi_i2d").startswith("double")
    assert mod.categorize("__aeabi_d2f").startswith("double")


def test_categorize_covers_libc_math(mod):
    # expf, logf and powf are not here: the freestanding math shim provides them and
    # they live on the allow-list.
    # The base-2 helpers (exp2f/log2f), the double versions (exp), and the
    # rest of libc math still categorize as a math-reject.
    for sym in ("log2", "log2f", "exp2f", "exp", "sinf", "cosf", "lround", "floorf"):
        assert mod.categorize(sym).startswith("libc math"), sym


def test_single_precision_transcendentals_on_allow_list(mod):
    """expf/logf/powf are shim-provided (freestanding_math.c), not rejected."""
    for sym in ("expf", "logf", "powf"):
        assert sym in mod.SHIM_SYMBOLS, sym
        assert sym not in mod._LIBC_MATH, sym


def test_double_and_base2_math_still_rejected(mod):
    """The double exp/log/pow and exp2f/log2f helpers stay rejected."""
    for sym in ("exp", "log", "pow", "exp2", "exp2f", "log2", "log2f"):
        assert sym in mod._LIBC_MATH, sym
        assert sym not in mod.SHIM_SYMBOLS, sym


def test_categorize_covers_libc_string(mod):
    for sym in ("atoi", "atof", "strchr", "strlen", "strcmp", "sprintf", "printf"):
        assert mod.categorize(sym).startswith("libc string"), sym


def test_categorize_covers_libc_io(mod):
    for sym in ("fopen", "fclose", "fread", "fwrite", "puts"):
        assert mod.categorize(sym).startswith("libc I/O"), sym


def test_categorize_covers_libgcc_64bit(mod):
    for sym in ("__aeabi_ldivmod", "__moddi3", "__divdi3"):
        assert mod.categorize(sym).startswith("libgcc"), sym


def test_categorize_unknown(mod):
    assert mod.categorize("totally_random_symbol").startswith("unknown")


def test_shim_symbols_on_allow_list(mod):
    """Compiler-emitted intrinsics + single-precision math resolved by the
    shim must pass through (the shim adds expf/logf/powf)."""
    for sym in ("memcpy", "memset", "memmove", "__aeabi_uldivmod",
                "expf", "logf", "powf"):
        assert sym in mod.SHIM_SYMBOLS, sym


def test_api_prefix_definitions(mod):
    """OperatorApi runtime-resolved symbols identify by prefix."""
    assert any(p == "api_" for p in mod.API_PREFIXES)
    assert any(p == "op_api_" for p in mod.API_PREFIXES)


# ---------------------------------------------------------------------------
# Synthetic ELF tests build a minimal ELF in memory and check what the gate makes of
# it. Writing the bytes directly, rather than shelling out to the toolchain, keeps
# these runnable on any Python host with pyelftools.
# ---------------------------------------------------------------------------
def _make_elf_with_undef_symbols(tmp_path, symbol_names):
    """Write a tiny 32-bit ARM ELF whose only undefined symbols are given.

    pyelftools reads but does not write, so the bytes are packed by hand, which is
    simpler than taking on an ELF writer. The ELF carries a header, the two string
    tables and a symbol table holding one undefined entry per name.
    """
    import struct

    # --- String tables ---
    shstrtab_names = [b"", b".shstrtab", b".strtab", b".symtab"]
    shstrtab = b"\x00".join(shstrtab_names) + b"\x00"
    shstrtab_offsets = {}
    off = 0
    for n in shstrtab_names:
        shstrtab_offsets[n] = off
        off += len(n) + 1

    strtab = b"\x00"
    strtab_offsets = {}
    for name in symbol_names:
        strtab_offsets[name] = len(strtab)
        strtab += name.encode("ascii") + b"\x00"

    # --- Symbol table entries (Elf32_Sym: 16 bytes each) ---
    # Layout: st_name(4) st_value(4) st_size(4) st_info(1) st_other(1) st_shndx(2)
    SYM_FMT = "<IIIBBH"
    # First entry is STN_UNDEF (all zeros).
    symtab = struct.pack(SYM_FMT, 0, 0, 0, 0, 0, 0)
    for name in symbol_names:
        # st_info: STB_GLOBAL << 4 | STT_NOTYPE  == 0x10
        symtab += struct.pack(
            SYM_FMT,
            strtab_offsets[name],  # st_name
            0,                      # st_value
            0,                      # st_size
            0x10,                   # st_info (global, notype)
            0,                      # st_other
            0,                      # st_shndx (SHN_UNDEF)
        )
    symtab_num_entries = 1 + len(symbol_names)

    # --- Section headers (Elf32_Shdr: 40 bytes each) ---
    # [0] SHN_UNDEF (all zeros)
    # [1] .shstrtab  (SHT_STRTAB)
    # [2] .strtab    (SHT_STRTAB)
    # [3] .symtab    (SHT_SYMTAB, sh_link -> .strtab, sh_info -> first non-local)
    EHDR_SIZE = 52
    SHDR_SIZE = 40

    # Layout data after EHDR in order: shstrtab, strtab, symtab, then shdr table.
    shstrtab_off = EHDR_SIZE
    strtab_off = shstrtab_off + len(shstrtab)
    symtab_off = strtab_off + len(strtab)
    shdr_off = symtab_off + len(symtab)

    def shdr(name, type_, offset, size, link=0, info=0, entsize=0):
        # sh_name sh_type sh_flags sh_addr sh_offset sh_size sh_link sh_info sh_addralign sh_entsize
        return struct.pack(
            "<IIIIIIIIII",
            shstrtab_offsets[name],
            type_,
            0,  # flags
            0,  # addr
            offset,
            size,
            link,
            info,
            1,  # addralign
            entsize,
        )

    shdrs = b""
    shdrs += struct.pack("<IIIIIIIIII", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)  # [0] NULL
    shdrs += shdr(b".shstrtab", 3, shstrtab_off, len(shstrtab))       # [1]
    shdrs += shdr(b".strtab", 3, strtab_off, len(strtab))             # [2]
    shdrs += shdr(
        b".symtab", 2, symtab_off, len(symtab),
        link=2,     # sh_link -> .strtab index
        info=1,     # sh_info -> first non-local sym index (just > STN_UNDEF)
        entsize=16,
    )               # [3]

    # --- ELF header ---
    # e_ident: EI_MAG, EI_CLASS(32), EI_DATA(LE), EI_VERSION(1), EI_OSABI(SYSV), pad
    e_ident = b"\x7fELF" + b"\x01\x01\x01\x00" + b"\x00" * 8
    # e_type=REL(1), e_machine=ARM(0x28), e_version=1
    # e_entry=0, e_phoff=0, e_shoff=shdr_off, e_flags=0, e_ehsize=52
    # e_phentsize=0, e_phnum=0, e_shentsize=40, e_shnum=4, e_shstrndx=1
    ehdr = e_ident + struct.pack(
        "<HHIIIIIHHHHHH",
        1,           # e_type = ET_REL
        0x28,        # e_machine = EM_ARM
        1,           # e_version
        0,           # e_entry
        0,           # e_phoff
        shdr_off,    # e_shoff
        0,           # e_flags
        EHDR_SIZE,   # e_ehsize
        0, 0,        # e_phentsize, e_phnum
        SHDR_SIZE,   # e_shentsize
        4,           # e_shnum
        1,           # e_shstrndx (index of .shstrtab)
    )

    blob = ehdr + shstrtab + strtab + symtab + shdrs

    elf_path = tmp_path / "synthetic.elf"
    elf_path.write_bytes(blob)
    return elf_path


def test_synthetic_elf_with_atof_rejected(tmp_path):
    """Dirty ELF: a single undefined atof reference must be rejected."""
    elf = _make_elf_with_undef_symbols(tmp_path, ["atof"])
    result = run(["--elf", str(elf)])
    assert result.returncode == 1, result.stderr
    assert "atof" in result.stderr
    assert "libc string" in result.stderr


def test_synthetic_elf_with_aeabi_ddiv_rejected(tmp_path):
    """Dirty ELF: __aeabi_ddiv (double soft-float) must be rejected."""
    elf = _make_elf_with_undef_symbols(tmp_path, ["__aeabi_ddiv"])
    result = run(["--elf", str(elf)])
    assert result.returncode == 1, result.stderr
    assert "__aeabi_ddiv" in result.stderr
    assert "double" in result.stderr


def test_synthetic_elf_with_only_shim_symbols_passes(tmp_path):
    """Clean ELF: undefined memcpy / __aeabi_uldivmod are resolved by shim."""
    elf = _make_elf_with_undef_symbols(
        tmp_path, ["memcpy", "memset", "memmove", "__aeabi_uldivmod"])
    result = run(["--elf", str(elf)])
    assert result.returncode == 0, result.stderr


def test_synthetic_elf_with_api_prefix_symbols_passes(tmp_path):
    """Clean ELF: api_* and op_api_* are runtime-resolved by the loader."""
    elf = _make_elf_with_undef_symbols(
        tmp_path, ["api_send_midi", "op_api_get_beat_position"])
    result = run(["--elf", str(elf)])
    assert result.returncode == 0, result.stderr


def test_synthetic_elf_with_mixed_symbols_rejected(tmp_path):
    """Mixed bag: shim + bad symbol -> still rejected, bad one reported."""
    elf = _make_elf_with_undef_symbols(
        tmp_path, ["memcpy", "log2", "api_send_midi"])
    result = run(["--elf", str(elf)])
    assert result.returncode == 1, result.stderr
    assert "log2" in result.stderr
    # Shim and api symbols should not appear in the rejection list.
    assert "memcpy" not in result.stderr
    assert "api_send_midi" not in result.stderr
