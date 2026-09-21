#!/usr/bin/env python3
"""Lock the ABI shape of __aeabi_uldivmod in a packed mode.

The compiler calls __aeabi_uldivmod at every 64-bit divide, and its ABI returns
in registers: r0:r1 hold num and r2:r3 hold den on entry, and quot comes back in
r0:r1 with rem in r2:r3. A C function that returns a 16-byte struct lowers to a
different ABI, where the caller allocates the output and passes its address in r0,
so a shim written that way writes its result through whatever r0 held and faults.

This gate disassembles __aeabi_uldivmod with arm-none-eabi-objdump and rejects the
struct-return shape, whose epilogue carries either of two Thumb-2 encodings that a
register-return shim never emits:

    e9c6 a800   strd sl, r8, [r6]      writes quot through r6
    e9c6 2102   strd r2, r1, [r6, #8]  writes rem through r6

r6 there is the caller-passed output pointer, spilled and reloaded. A register-return
shim writes straight into r0:r1 and r2:r3 and cannot contain these.

A missing symbol is fine: --gc-sections drops __aeabi_uldivmod from any mode that
emits no 64-bit divide, and a mode without the symbol cannot exercise the shape, so
the gate passes when the symbol is absent.

Runs as a POST_BUILD step inside op_add_mode, after check_no_text_relocs.py and
before pack_opm.py, so a mode with the broken shape never packs.

Usage (invoked from CMake POST_BUILD):
  python3 check_uldivmod_abi.py --elf path/to/mode.elf

Exit codes:
  0  clean, or the symbol is absent
  1  the struct-return byte sequence was found in the symbol body
  2  usage error (missing arguments, missing ELF, objdump failure)
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

# The two byte sequences the struct-return shape emits: the STRDs in its epilogue
# that write the 16-byte result through r6, the spilled caller-passed output pointer.
# A register-return shim never writes through such a pointer, so it cannot carry
# these.
PROHIBITED_BYTES = (
    "e9c6 a800",   # strd sl, r8, [r6]
    "e9c6 2102",   # strd r2, r1, [r6, #8]
)

OBJDUMP = "arm-none-eabi-objdump"
SYMBOL = "__aeabi_uldivmod"


def disassemble_function(elf: Path, symbol: str) -> str:
    """Disassemble `elf` and slice out the body of `symbol`.

    Returns the text body (between '<symbol>:' and the next function
    header or EOF). Empty string if the symbol is absent.
    Exits with code 2 if objdump itself fails.
    """
    if shutil.which(OBJDUMP) is None:
        print(
            f"ERROR: {OBJDUMP} not on PATH. "
            "Install arm-none-eabi-gnu-toolchain or activate the SDK env.",
            file=sys.stderr,
        )
        sys.exit(2)
    out = subprocess.run(
        [OBJDUMP, "-d", str(elf)],
        capture_output=True, text=True, check=False,
    )
    if out.returncode != 0:
        print(
            f"ERROR: {OBJDUMP} -d {elf} failed (exit {out.returncode}):\n"
            f"{out.stderr}",
            file=sys.stderr,
        )
        sys.exit(2)

    in_function = False
    body: list[str] = []
    target = f"<{symbol}>:"
    for line in out.stdout.splitlines():
        if line.rstrip().endswith(target):
            in_function = True
            continue
        if in_function:
            # A function header line looks like: "00003604 <name>:".
            # Stop at the next header.
            stripped = line.strip()
            if stripped.endswith(":") and "<" in stripped and ">:" in stripped:
                break
            body.append(line)
    return "\n".join(body)


def main(argv: "list[str] | None" = None) -> int:
    parser = argparse.ArgumentParser(
        prog="check_uldivmod_abi.py",
        description=(
            f"Reject .opm ELFs whose {SYMBOL} body contains the broken-shim "
            "struct-return byte sequence (regression gate). Run as a "
            "POST_BUILD step by op_add_mode()."
        ),
    )
    parser.add_argument(
        "--elf",
        required=True,
        type=Path,
        help="Path to the ELF to inspect",
    )
    args = parser.parse_args(argv)

    if not args.elf.exists():
        print(f"ERROR: ELF not found: {args.elf}", file=sys.stderr)
        return 2

    body = disassemble_function(args.elf, SYMBOL)
    if not body.strip():
        # Symbol absence is legitimate: --gc-sections drops the shim's
        # __aeabi_uldivmod function from any mode whose code never emits
        # a 64-bit divide. A mode without the symbol cannot exercise the
        # broken-ABI bug, so this gate has nothing to check.
        return 0

    for needle in PROHIBITED_BYTES:
        if needle in body:
            print(
                f"FAIL: {args.elf} {SYMBOL} contains broken-ABI byte "
                f"sequence '{needle}'.",
                file=sys.stderr,
            )
            print(
                "      This is the struct-return shape. The compiler emits the\n"
                "      register-return ABI, and a struct-return shim writes 16\n"
                "      bytes through whatever r0 held at the call, which faults.",
                file=sys.stderr,
            )
            print(
                "\nHint: __aeabi_uldivmod is the shim in src/freestanding_intrinsics.c. "
                "Keep it a register-return trampoline, not a struct-returning C function.",
                file=sys.stderr,
            )
            print("\nDisassembly:\n" + body, file=sys.stderr)
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
