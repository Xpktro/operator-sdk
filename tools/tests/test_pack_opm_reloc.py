"""Tests for pack_opm.py's R_ARM_RELATIVE extraction and reloc-table packing.

Compiles a stateful mode fixture, whose global state gives the ELF the relocations
the packer reads, then mutates the ELF to drive each rejection path: a wrong reloc
type, an R_ARM_NONE that is dropped, a misaligned offset, and an out-of-bounds
offset.
"""
from __future__ import annotations

import struct
from pathlib import Path

from conftest import (
    STATEFUL_SOURCE,
    compile_mode_elf,
    decode_header as _decode_header,
    requires_arm_toolchain,
    run_pack_opm as _run_pack_opm,
)

pytestmark = requires_arm_toolchain


def _build_fixture(tmp_path: Path) -> Path:
    """Compile the stateful fixture into tmp_path. Each test gets its own so a
    mutation does not leak across tests."""
    return compile_mode_elf(tmp_path, STATEFUL_SOURCE, "fixture.elf")


# ARM REL entry layout: <I r_offset  <I r_info>.
# r_info packs (sym << 8) | type_low_byte for 32-bit ELF.
def _rel_dyn_entry_offset(elf_path: Path, entry_index: int) -> int:
    """Return the file offset of .rel.dyn's entry #entry_index."""
    from elftools.elf.elffile import ELFFile

    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        section = elf.get_section_by_name(".rel.dyn")
        assert section is not None, f"fixture ELF {elf_path} has no .rel.dyn"
        sh_off = section["sh_offset"]
        ent = section["sh_entsize"]
        n = section["sh_size"] // ent
        assert 0 <= entry_index < n, (
            f"entry_index {entry_index} out of range [0, {n})"
        )
        return sh_off + entry_index * ent


def _rewrite_rel_entry(
    elf_path: Path,
    entry_index: int,
    new_r_offset: "int | None" = None,
    new_r_type: "int | None" = None,
) -> None:
    entry_off = _rel_dyn_entry_offset(elf_path, entry_index)
    raw = bytearray(elf_path.read_bytes())
    cur_r_offset, cur_r_info = struct.unpack_from("<II", raw, entry_off)
    r_offset = cur_r_offset if new_r_offset is None else new_r_offset
    if new_r_type is not None:
        sym = cur_r_info >> 8
        r_info = (sym << 8) | (new_r_type & 0xFF)
    else:
        r_info = cur_r_info
    struct.pack_into("<II", raw, entry_off, r_offset, r_info)
    elf_path.write_bytes(bytes(raw))


# ---------------------------------------------------------------------------
# Positive / happy path
# ---------------------------------------------------------------------------
def test_extracts_r_arm_relative_from_fixture(tmp_path):
    """The relocations the fixture carries are extracted, sorted, and recorded at
    the file tail."""
    elf = _build_fixture(tmp_path)
    opm = tmp_path / "out.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "output", "--name", "sample",
    )
    assert r.returncode == 0, r.stderr
    hdr = _decode_header(opm)
    assert hdr["reloc_count"] >= 1, (
        f"fixture should carry >=1 R_ARM_RELATIVE; got {hdr['reloc_count']}"
    )
    # Pull the reloc table from the file tail and validate each entry.
    file_bytes = opm.read_bytes()
    reloc_bytes = file_bytes[
        hdr["reloc_offset"] : hdr["reloc_offset"] + hdr["reloc_count"] * 4
    ]
    assert len(reloc_bytes) == hdr["reloc_count"] * 4
    # The packer bounds each offset by the whole allocatable region, since a
    # reference through the GOT lands past .rodata.
    ram_size = (hdr["text_size"] + hdr["rodata_size"] + hdr["got_size"]
                + hdr["data_size"] + hdr["bss_size"])
    prev = -1
    for i in range(hdr["reloc_count"]):
        (off,) = struct.unpack_from("<I", reloc_bytes, i * 4)
        assert off & 3 == 0, f"offset 0x{off:x} not 4-byte aligned"
        assert 0 <= off < ram_size, (
            f"offset 0x{off:x} outside [0, {ram_size:#x})"
        )
        assert off >= prev, f"reloc table not sorted at index {i}"
        prev = off


def test_silently_accepts_r_arm_none(tmp_path):
    """An entry turned into R_ARM_NONE is dropped, so the pack succeeds with one
    reloc fewer."""
    elf = _build_fixture(tmp_path)
    opm_base = tmp_path / "base.opm"
    assert _run_pack_opm(
        "--input", str(elf), "--output", str(opm_base),
        "--type", "output", "--name", "base",
    ).returncode == 0
    baseline_reloc_count = _decode_header(opm_base)["reloc_count"]
    assert baseline_reloc_count >= 1, "fixture needs at least one reloc"

    # Convert entry 0 to R_ARM_NONE (type 0).
    _rewrite_rel_entry(elf, entry_index=0, new_r_type=0)
    opm = tmp_path / "mutated.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "output", "--name", "mutated",
    )
    assert r.returncode == 0, r.stderr
    mutated_reloc_count = _decode_header(opm)["reloc_count"]
    # R_ARM_NONE is silently dropped, so the emitted count is baseline - 1.
    assert mutated_reloc_count == baseline_reloc_count - 1, (
        f"R_ARM_NONE should be silently dropped; expected "
        f"{baseline_reloc_count - 1}, got {mutated_reloc_count}"
    )


def test_reloc_offset_is_four_byte_aligned(tmp_path):
    elf = _build_fixture(tmp_path)
    opm = tmp_path / "aligned.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "output", "--name", "aligned",
    )
    assert r.returncode == 0, r.stderr
    hdr = _decode_header(opm)
    assert hdr["reloc_offset"] & 3 == 0, (
        f"reloc_offset {hdr['reloc_offset']} must be 4-byte aligned"
    )


def test_reloc_table_after_data_section(tmp_path):
    """When reloc_count > 0, reloc_offset lies at/after data end
    (with 0..3 bytes of alignment padding permitted)."""
    elf = _build_fixture(tmp_path)
    opm = tmp_path / "after_data.opm"
    r = _run_pack_opm(
        "--input", str(elf), "--output", str(opm),
        "--type", "output", "--name", "after_data",
    )
    assert r.returncode == 0, r.stderr
    hdr = _decode_header(opm)
    if hdr["reloc_count"] > 0:
        data_end = hdr["data_offset"] + hdr["data_size"]
        assert hdr["reloc_offset"] >= data_end, (
            f"reloc_offset {hdr['reloc_offset']} must come at/after "
            f"data end {data_end}"
        )
        assert hdr["reloc_offset"] - data_end < 4, (
            f"reloc alignment padding must be 0..3 bytes, got "
            f"{hdr['reloc_offset'] - data_end}"
        )


# ---------------------------------------------------------------------------
# Negative: reject non-R_ARM_RELATIVE / misaligned / out-of-bounds
# ---------------------------------------------------------------------------
# Each case locates .rel.dyn through pyelftools, rewrites entry 0 in the raw bytes,
# and runs the packer to read back the refusal.
R_ARM_ABS32 = 2  # any type that isn't R_ARM_RELATIVE (23) or R_ARM_NONE (0)


def test_rejects_abs32_reloc(tmp_path):
    """An entry turned into R_ARM_ABS32 is refused."""
    elf = _build_fixture(tmp_path)
    # Sanity: baseline pack succeeds.
    base = _run_pack_opm(
        "--input", str(elf), "--output", str(tmp_path / "base.opm"),
        "--type", "output", "--name", "base",
    )
    assert base.returncode == 0, base.stderr

    # Mutate entry 0 of .rel.dyn to ABS32 (type 2).
    _rewrite_rel_entry(elf, entry_index=0, new_r_type=R_ARM_ABS32)

    r = _run_pack_opm(
        "--input", str(elf), "--output", str(tmp_path / "bad.opm"),
        "--type", "output", "--name", "bad",
    )
    assert r.returncode == 1, r.stderr
    assert "unsupported relocation type" in r.stderr, r.stderr
    assert "operator-mode-toolchain.cmake" in r.stderr, r.stderr


def test_misaligned_offset_rejected(tmp_path):
    """An r_offset that is not 4-aligned is refused, and the error says so."""
    elf = _build_fixture(tmp_path)
    # Sanity first.
    assert _run_pack_opm(
        "--input", str(elf), "--output", str(tmp_path / "base.opm"),
        "--type", "output", "--name", "base",
    ).returncode == 0

    # Patch entry 0 with r_offset = 5 (not 4-aligned). Type stays RELATIVE.
    _rewrite_rel_entry(elf, entry_index=0, new_r_offset=5)

    r = _run_pack_opm(
        "--input", str(elf), "--output", str(tmp_path / "bad.opm"),
        "--type", "output", "--name", "bad",
    )
    assert r.returncode == 1, r.stderr
    assert "misaligned" in r.stderr.lower(), r.stderr
    assert "0x5" in r.stderr, r.stderr


def test_out_of_bounds_offset_rejected(tmp_path):
    """An r_offset past the allocatable region is refused, and the error says so."""
    elf = _build_fixture(tmp_path)
    # Read the baseline to learn the allocatable region size.
    opm_base = tmp_path / "base.opm"
    base = _run_pack_opm(
        "--input", str(elf), "--output", str(opm_base),
        "--type", "output", "--name", "base",
    )
    assert base.returncode == 0, base.stderr
    hdr = _decode_header(opm_base)
    ram_size = (hdr["text_size"] + hdr["rodata_size"] + hdr["got_size"]
                + hdr["data_size"] + hdr["bss_size"])
    # An offset past the region, still 4-aligned so the alignment check does not
    # shadow the bounds check.
    oob = (ram_size + 0x1000) & ~3

    _rewrite_rel_entry(elf, entry_index=0, new_r_offset=oob)

    r = _run_pack_opm(
        "--input", str(elf), "--output", str(tmp_path / "bad.opm"),
        "--type", "output", "--name", "bad",
    )
    assert r.returncode == 1, r.stderr
    assert "outside" in r.stderr.lower(), r.stderr
