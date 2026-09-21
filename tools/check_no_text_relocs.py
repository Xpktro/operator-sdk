#!/usr/bin/env python3
"""Reject a mode whose shared code carries a relocation into its own data.

Reject any mode whose .rel.dyn carries an R_ARM_RELATIVE offset inside .text or
.rodata that targets per-instance memory (.got, .data or .bss). Every reference
into per-instance data routes through the GOT, so a direct pointer to it baked into
shared .text or .rodata leaves copies of a mode that share its code pointed at the
first copy's data.

A reloc inside .text or .rodata whose target is itself shared (.text or .rodata) is
legal. The Action callback table lives in .rodata and holds pointers into the mode's
shared .text, so every copy resolves the same shared functions.

Without the compile flags below, the compiler puts literal pool entries inside .text
that hold direct .data addresses. After the loader applies its single load bias, one
copy of a mode reads another copy's data and corrupts it.

    -fPIE -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative

op_add_mode applies those flags, so a mode never asks for them. This gate is what
catches them going missing, whether from an edited build file or a change of
toolchain. The build then fails with a message naming the flags to restore, in place
of a mode that loads twice and lets one copy corrupt the other.

Runs as a POST_BUILD step inside op_add_mode, between the freestanding-symbol gate and
the packer, so a mode that trips it never packs.

Usage (invoked from CMake POST_BUILD):
  python3 check_no_text_relocs.py --elf path/to/mode.elf

Exit codes:
  0  clean (no text- or rodata-resident R_ARM_RELATIVE)
  1  violation (relocs in .text or .rodata, details on stderr)
  2  usage error (missing or invalid arguments, missing ELF)
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from elftools.elf.elffile import ELFFile
    from elftools.elf.relocation import RelocationSection
except ImportError:  # pragma: no cover - surfaced only in misconfigured envs
    print(
        "ERROR: pyelftools not installed. Install with: "
        "pip install -r tools/requirements.txt",
        file=sys.stderr,
    )
    sys.exit(2)


# R_ARM_RELATIVE relocation type code (ARM EABI, r_info_type field), the same value
# pack_opm.py carries.
R_ARM_RELATIVE = 23


def section_range(elf, name: str) -> "tuple[int, int]":
    """Return (sh_addr, sh_addr + sh_size) for `name`, or (0, 0) if missing."""
    s = elf.get_section_by_name(name)
    if s is None:
        return (0, 0)
    return (s["sh_addr"], s["sh_addr"] + s["sh_size"])


def _read_addend(elf, offset: int) -> "int | None":
    """Return the 32-bit little-endian word stored at link VMA `offset`.

    R_ARM_RELATIVE stores the target VMA as the addend at the reloc site, so the
    word already sitting there names what the pointer resolves to. Returns None
    when no allocatable section covers the offset.
    """
    for sec in elf.iter_sections():
        if not (sec["sh_flags"] & 0x2):  # SHF_ALLOC
            continue
        lo = sec["sh_addr"]
        hi = lo + sec["sh_size"]
        if lo <= offset < hi and sec["sh_type"] != "SHT_NOBITS":
            data = sec.data()
            pos = offset - lo
            if pos + 4 <= len(data):
                return int.from_bytes(data[pos:pos + 4], "little")
    return None


def check_elf(elf_path: Path) -> "list[tuple[int, str, str]]":
    """Return [(offset, section_name, reason), ...] for every R_ARM_RELATIVE
    inside .text or .rodata whose target is per-instance memory. Empty = clean.

    Exits the process with code 2 if the ELF path is unusable.
    """
    if not elf_path.exists():
        print(f"ERROR: ELF not found: {elf_path}", file=sys.stderr)
        sys.exit(2)

    bad: "list[tuple[int, str, str]]" = []
    with elf_path.open("rb") as f:
        elf = ELFFile(f)
        text_lo, text_hi = section_range(elf, ".text")
        rod_lo, rod_hi = section_range(elf, ".rodata")
        got_lo, got_hi = section_range(elf, ".got")
        data_lo, data_hi = section_range(elf, ".data")
        bss_lo, bss_hi = section_range(elf, ".bss")
        for sec in elf.iter_sections():
            if not isinstance(sec, RelocationSection):
                continue
            for rel in sec.iter_relocations():
                if rel["r_info_type"] != R_ARM_RELATIVE:
                    continue
                off = rel["r_offset"]
                if text_lo <= off < text_hi:
                    where = ".text"
                elif rod_lo <= off < rod_hi:
                    where = ".rodata"
                else:
                    continue  # .got / .data relocs are always legal

                # A reloc into shared code or rodata is legal only when its target
                # is also shared. Mask the Thumb bit so a function pointer resolves
                # to its section.
                target = _read_addend(elf, off)
                if target is None:
                    bad.append((off, where, "unreadable addend"))
                    continue
                target &= ~1
                if got_lo <= target < got_hi:
                    bad.append((off, where, "-> .got"))
                elif data_lo <= target < data_hi:
                    bad.append((off, where, "-> .data"))
                elif bss_lo <= target < bss_hi:
                    bad.append((off, where, "-> .bss"))
    return bad


def main(argv: "list[str] | None" = None) -> int:
    parser = argparse.ArgumentParser(
        prog="check_no_text_relocs.py",
        description=(
            "Reject .opm ELFs whose .rel.dyn contains R_ARM_RELATIVE entries "
            "inside .text or .rodata (per-instance memory invariant). Run "
            "as a POST_BUILD step by op_add_mode() between check_opm_freestanding "
            "and pack_opm."
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
        f"FAIL: {args.elf} has {len(bad)} R_ARM_RELATIVE entries inside "
        f".text or .rodata that target per-instance memory. A .data or .bss "
        f"reference was emitted as a text-resident literal pool entry.",
        file=sys.stderr,
    )
    print(
        "Verify -fPIE -msingle-pic-base -mpic-register=r9 -mno-pic-data-is-text-relative are present in the compile command.",
        file=sys.stderr,
    )
    for off, sec, reason in sorted(bad):
        print(f"  reloc 0x{off:08x} in {sec} {reason}", file=sys.stderr)
    print(
        "\nHint: see docs/the-mode-mindset.md#per-instance-memory for the "
        "contract. The four flags above are applied through "
        "target_compile_options() inside op_add_mode.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
