#!/usr/bin/env python3
"""Reject a mode that references a symbol the device does not provide.

Runs as a POST_BUILD step inside op_add_mode.
If any undefined symbol in the ELF falls outside the allow-list (compiler-
intrinsic shim + OperatorApi runtime-resolved GOT entries), the script exits
1 and prints the offending symbols with a human-readable category.

Rationale: the Operator SDK's ABI contract is that .opm files are linked
with `-nostdlib -nostartfiles -pie` for Cortex-M33. Any surviving undefined
symbol is, at best, a runtime symbol the firmware does not provide (instant
crash on load) or, at worst, evidence that libgcc / libc leaked into a mode
at compile time. Such leaks are invisible to host-native ctest. This gate
makes regressions a build-time failure with a clear pointer at the author's
mistake.

Allow-list:
  - SHIM_SYMBOLS   the intrinsics the freestanding shim provides. They appear
                     undefined in each translation unit's ELF, since the shim is a
                     separate unit the linker resolves at the final link.
  - API_PREFIXES   names starting api_ or op_api_, the OperatorApi entries the
                     device resolves at mode init through the GOT, undefined in the
                     ELF by design.

Everything else is a prohibited symbol. The script categorises each
prohibited symbol to help authors diagnose the root cause quickly (e.g.
a stray `double` produces `__aeabi_ddiv`, categorised as "double-
precision soft-float").

Usage (invoked from CMake POST_BUILD):
  python3 check_opm_freestanding.py --elf path/to/mode.elf

Exit codes:
  0  clean (all undefined symbols are in the allow-list)
  1  prohibited symbols found, details on stderr
  2  usage error (missing or invalid arguments, missing ELF)
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from elftools.elf.elffile import ELFFile
    from elftools.elf.sections import SymbolTableSection
except ImportError:  # pragma: no cover - surfaced only in misconfigured envs
    print(
        "ERROR: pyelftools not installed. Install with: "
        "pip install -r tools/requirements.txt",
        file=sys.stderr,
    )
    sys.exit(2)


# ---------------------------------------------------------------------------
# Allow-list
# ---------------------------------------------------------------------------
# The intrinsics the freestanding shim provides. Referencing them is fine, since the
# shim is linked into every .opm.
SHIM_SYMBOLS = frozenset({
    "memmove",
    "memcpy",
    "memset",
    "__aeabi_uldivmod",
    # The single-precision transcendentals the freestanding math shim provides,
    # also linked into every .opm. Only these f-variants are shim-provided: the
    # double exp/log/pow and the exp2f/log2f helpers, which stay internal to the
    # shim, are rejected in _LIBC_MATH below.
    "expf",
    "logf",
    "powf",
})

# Prefixes for the OperatorApi symbols the device resolves through the GOT at mode
# init. They appear undefined in the ELF by design. A new convention adds its prefix
# here.
API_PREFIXES = ("api_", "op_api_")


# ---------------------------------------------------------------------------
# Prohibited symbol categories (for friendlier error reporting).
# The check is that an undefined symbol outside the allow-list fails. A category
# only helps the author find the rule they broke.
# ---------------------------------------------------------------------------
_DOUBLE_AEABI_EXTRAS = {
    "__aeabi_i2d", "__aeabi_ui2d", "__aeabi_l2d", "__aeabi_ul2d",
    "__aeabi_d2f", "__aeabi_f2d",
    "__aeabi_d2iz", "__aeabi_d2uiz", "__aeabi_d2lz", "__aeabi_d2ulz",
    "__aeabi_dcmpeq", "__aeabi_dcmplt", "__aeabi_dcmple",
    "__aeabi_dcmpgt", "__aeabi_dcmpge", "__aeabi_dcmpun",
}

_LIBC_MATH = {
    # Trig
    "sin", "sinf", "cos", "cosf", "tan", "tanf",
    "asin", "asinf", "acos", "acosf", "atan", "atanf", "atan2", "atan2f",
    # Logs and exponentials. expf, logf and powf are not here: they are shim-provided
    # and live on SHIM_SYMBOLS. The double versions, the base-2 helpers, log10 and
    # sqrt stay rejected.
    "log", "log2", "log2f", "log10", "log10f",
    "exp", "exp2", "exp2f",
    "pow", "sqrt", "sqrtf",
    # Rounding
    "lround", "lroundf", "round", "roundf",
    "floor", "floorf", "ceil", "ceilf",
    "trunc", "truncf", "nearbyint", "nearbyintf",
    "rint", "rintf",
    "fmod", "fmodf", "fabs", "fabsf",
}

_LIBC_STRING = {
    "atoi", "atol", "atoll", "atof",
    "strtol", "strtoul", "strtof", "strtod",
    "strchr", "strrchr", "strcmp", "strncmp", "strstr",
    "strlen", "strnlen", "strncpy", "strcpy",
    "strcat", "strncat", "strdup",
    "sprintf", "snprintf", "vsprintf", "vsnprintf",
    "sscanf", "printf", "fprintf", "vprintf",
    "memcmp",  # the shim allow-list carries memset/memcpy/memmove, not memcmp
}

_LIBC_IO = {
    "fopen", "fclose", "fread", "fwrite",
    "fputs", "fgets", "fputc", "fgetc",
    "puts", "putchar", "getchar",
    "fflush", "fseek", "ftell",
}

_LIBGCC_64BIT = {
    "__aeabi_ldivmod",    # signed 64-bit divmod
    "__aeabi_llsl", "__aeabi_llsr", "__aeabi_lasr",
    "__moddi3", "__divdi3", "__udivdi3", "__umoddi3",
    "__muldi3",
}

_LIBC_ALLOC = {
    "malloc", "free", "calloc", "realloc",
    "_sbrk", "_malloc_r", "_free_r",
}


def _is_double_aeabi(sym: str) -> bool:
    return sym.startswith("__aeabi_d") or sym in _DOUBLE_AEABI_EXTRAS


# Category table of a label and a predicate. The first matching category wins, so the
# most specific patterns come first.
CATEGORIES = (
    ("double-precision soft-float", _is_double_aeabi),
    ("libgcc 64-bit integer helpers", lambda s: s in _LIBGCC_64BIT),
    ("libc math", lambda s: s in _LIBC_MATH),
    ("libc string/number parsing", lambda s: s in _LIBC_STRING),
    ("libc I/O", lambda s: s in _LIBC_IO),
    ("libc allocation", lambda s: s in _LIBC_ALLOC),
)


def categorize(symbol: str) -> str:
    """Return a human-readable category for a prohibited undefined symbol."""
    for label, match in CATEGORIES:
        if match(symbol):
            return label
    return "unknown (unresolved external)"


# ---------------------------------------------------------------------------
# Core: ELF inspection
# ---------------------------------------------------------------------------
def _is_undefined(sym) -> bool:
    """True if the given pyelftools Symbol is undefined in its containing TU.

    pyelftools exposes `st_shndx` as either a string (e.g. 'SHN_UNDEF') or
    an integer depending on whether the value is a reserved index. Treat
    the zero index and the SHN_UNDEF tag identically.
    """
    shndx = sym.entry.get("st_shndx")
    return shndx == "SHN_UNDEF" or shndx == 0


def check_elf(elf_path: Path) -> list[tuple[str, str]]:
    """Return [(symbol, category), ...] for every prohibited undefined symbol.

    Exits the process with code 2 if the ELF path is unusable.
    """
    if not elf_path.exists():
        print(f"ERROR: ELF not found: {elf_path}", file=sys.stderr)
        sys.exit(2)

    prohibited: list[tuple[str, str]] = []
    seen: set[str] = set()

    with elf_path.open("rb") as f:
        elf = ELFFile(f)
        for section in elf.iter_sections():
            if not isinstance(section, SymbolTableSection):
                continue
            for sym in section.iter_symbols():
                if not _is_undefined(sym):
                    continue
                name = sym.name
                if not name or name in seen:
                    continue
                seen.add(name)
                if name in SHIM_SYMBOLS:
                    continue
                if any(name.startswith(p) for p in API_PREFIXES):
                    continue
                prohibited.append((name, categorize(name)))
    return prohibited


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def main(argv: "list[str] | None" = None) -> int:
    parser = argparse.ArgumentParser(
        prog="check_opm_freestanding.py",
        description=(
            "Reject .opm ELFs that reference prohibited undefined symbols "
            "(libc, double-precision soft-float, libgcc 64-bit helpers). "
            "Run as a POST_BUILD step by op_add_mode()."
        ),
    )
    parser.add_argument(
        "--elf",
        required=True,
        type=Path,
        help="Path to the ELF to inspect",
    )
    args = parser.parse_args(argv)

    bad = check_elf(args.elf)
    if not bad:
        return 0

    print(
        f"FAIL: {args.elf} references {len(bad)} prohibited undefined "
        f"symbol(s):",
        file=sys.stderr,
    )
    for name, cat in sorted(bad):
        print(f"  {name}  [{cat}]", file=sys.stderr)
    print(
        "\nHint: see docs/the-mode-mindset.md#freestanding-constraints",
        file=sys.stderr,
    )
    print(
        "      The Operator SDK compiles modes as freestanding, "
        "single-precision, integer-first. Any libc / double / 64-bit "
        "divide symbol is a mistake in the mode source, not a missing "
        "toolchain feature.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
