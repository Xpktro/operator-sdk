#!/usr/bin/env python3
"""Pack a linked ELF into the .opm a mode ships as.

Invoked as a POST_BUILD step by op_add_mode().
Reads the linker output (.elf) produced by arm-none-eabi-g++ with the
SDK toolchain + operator-mode.ld linker script, collects section bytes,
resolves required/optional export symbols, synthesizes the OpmHeader, and
writes a .opm file whose first byte is header[0] and whose tail is the
concatenated allocatable section blob.

Notes:
  * The struct.pack format below matches the OpmHeader layout in
    include/operator_sdk/abi/mode_format.h, the ABI header a mode compiles against.
  * A required symbol has to be declared extern "C" in the mode. When one is
    absent but its C++-mangled form is present, the error asks whether the extern
    "C" was forgotten, so the author fixes the cause.
  * This is a correctness tool, not a security boundary. The runtime validates
    the header when it loads a mode, and that is the boundary.

Usage (invoked from CMake POST_BUILD):
  python3 pack_opm.py --input mode.elf --output mode.opm \\
                      --type {global|output} --name mode_name \\
                      [--flags 0x06] [--schema-version 1]
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

try:
    from elftools.elf.elffile import ELFFile
    from elftools.elf.sections import SymbolTableSection
    from elftools.elf.relocation import RelocationSection
except ImportError as e:  # pragma: no cover - surfaced at pack time, not import time
    sys.stderr.write(
        f"pack_opm: pyelftools is required but not installed ({e}). "
        "Install it via: pip install -r tools/requirements.txt\n"
    )
    raise

# ---------------------------------------------------------------------------
# The OpmHeader constants, matching the ABI header include/operator_sdk/abi/mode_format.h.
# ---------------------------------------------------------------------------
OPM_MAGIC = 0x4F504D31          # "OPM1" little-endian
HEADER_SIZE = 148
SDK_VERSION = 1

# ARM EABI relocation type codes used by the reloc-table pass.
R_ARM_RELATIVE = 23   # the only type .rel.dyn may carry
R_ARM_NONE     = 0    # silently dropped (no-op sentinel)

MODE_TYPE = {"global": 0, "output": 1}

# The OpmHeader.flags bit that marks a clock-generating mode.
GENERATES_CLOCK_FLAG = 0x04

# The exports the packer requires in the ELF, each declared `extern "C"` in the
# mode source.
REQUIRED_SYMBOLS = (
    "mode_init",
    "mode_process",
    "mode_destroy",
    "kParams",
    "kParamCount",
    "kParamStrings",
)

# Optional exports. Absence -> the corresponding OpmHeader offset is set to 0.
OPTIONAL_SYMBOLS = (
    "mode_migrate_config",
    "mode_ui_render",
    "mode_ui_gesture",
    "mode_on_sysex",
)

# The OpmHeader as a struct.pack format, little-endian and exactly 148 bytes
# across its seven groups.
HEADER_FMT = (
    "<"                # little-endian, no padding
    # --- Group 1: Identity (16 bytes) ---
    "I"   "I"          # magic, header_size
    "H"   "B"   "B"    # sdk_version, mode_type, flags
    "H"   "B"   "B"    # config_schema_ver, param_count, on_select_count
    # --- Group 2: Section Layout (36 bytes) ---
    "I"   "I"          # text_offset, text_size
    "I"   "I"          # rodata_offset, rodata_size
    "I"   "I"          # got_offset, got_size
    "I"   "I"          # data_offset, data_size
    "I"                # bss_size
    # --- Group 3: Relocations (8 bytes) ---
    "I"   "I"          # reloc_offset, reloc_count
    # --- Group 4: Entry Points (32 bytes) ---
    "I"   "I"   "I"    # init_offset, process_offset, destroy_offset
    "I"                # migrate_offset
    "I"   "I"          # ui_render_offset, ui_gesture_offset
    "I"   "I"          # on_sysex_offset, on_select_table_offset
    # --- Group 5: Parameter Tables (16 bytes) ---
    "I"   "I"          # param_table_offset, param_table_size
    "I"   "I"          # param_strings_offset, param_strings_size
    # --- Group 6: Name (32 bytes) ---
    "32s"              # name
    # --- Group 7: Reserved (8 bytes) ---
    "8x"               # _reserved_tail[8] (zero-filled)
)
assert struct.calcsize(HEADER_FMT) == HEADER_SIZE, (
    f"HEADER_FMT size mismatch: {struct.calcsize(HEADER_FMT)} != {HEADER_SIZE}"
)

# The sections the packer concatenates, in the order the linker script emits them.
# .bss carries no file bytes (it is zero-initialized at load), so only its size
# goes in the header.
BLOB_SECTION_ORDER = (".text", ".rodata", ".got", ".data")

# The boundary every allocatable section starts on, matching the linker script.
ALLOC_ALIGN = 8

# The allocatable sections, in the order the loader lays them out in RAM. These five
# are the whole of what the loader allocates, .bss included.
ALLOC_SECTIONS = (".text", ".rodata", ".got", ".data", ".bss")

# ParamSpec size, used only to cross-check the kParams symbol size. The struct is not
# decoded here.
PARAM_SPEC_SIZE = 32

# One Action callback table entry is a 32-bit function pointer on the target.
ON_SELECT_ENTRY_SIZE = 4


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _die(msg: str) -> "int":
    """Print an error to stderr and exit(1). Returns int for typing only."""
    sys.stderr.write(f"pack_opm: {msg}\n")
    sys.exit(1)


def _collect_alloc_sections(elf: ELFFile) -> dict:
    """Return {name -> section} for every allocatable section in the ELF."""
    # SHF_ALLOC = 0x2 marks a section that occupies memory at runtime.
    alloc = {}
    for section in elf.iter_sections():
        flags = section["sh_flags"]
        if flags & 0x2:
            alloc[section.name] = section
    return alloc


def _collect_symbols(elf: ELFFile) -> dict:
    """Return {name -> Symbol} from the ELF's .symtab. Empty when no .symtab.

    An undefined symbol (st_shndx == SHN_UNDEF) is skipped: the linker emits a
    `U mode_init` entry even when the source is missing the definition, and the packer
    treats a symbol as present only when it has a concrete definition.
    """
    symtab = elf.get_section_by_name(".symtab")
    if symtab is None or not isinstance(symtab, SymbolTableSection):
        return {}
    out: dict = {}
    for sym in symtab.iter_symbols():
        if not sym.name:
            continue
        # A symbol at SHN_UNDEF is declared but not defined here. Skip it, so the
        # required-symbol check fires for the missing definition.
        shndx = sym.entry.get("st_shndx")
        if shndx == "SHN_UNDEF" or shndx == 0:
            continue
        # First occurrence wins (usually the defining symbol).
        out.setdefault(sym.name, sym)
    return out


def _find_section_containing(sections: dict, vaddr: int, size: int) -> "tuple[str, object] | None":
    """Return (name, section) whose VMA range contains [vaddr, vaddr+size)."""
    for name, section in sections.items():
        start = section["sh_addr"]
        end = start + section["sh_size"]
        if vaddr >= start and (vaddr + size) <= end:
            return (name, section)
    return None


def _maybe_mangled(symbols: dict, name: str) -> "str | None":
    """If a C++-mangled form of `name` appears in `symbols`, return it.

    A mangled free function is `_Z<len><name><args>`, so the scan looks for
    any symbol carrying the Itanium ABI unmangled-name-with-length prefix, e.g.
    `_Z9mode_init...`. The result is a hint for the error message, and the match
    stays a prefix heuristic because the packer does no real demangling.
    """
    prefix = f"_Z{len(name)}{name}"
    for sym in symbols:
        if sym.startswith(prefix):
            return sym
    return None


# ---------------------------------------------------------------------------
# The layout model
#
# The loader rebuilds a mode's RAM layout from the section sizes in the header,
# rounding each size up to ALLOC_ALIGN and accumulating. The linker builds the same
# layout by starting each section at the next boundary that section's own alignment
# asks for, and the linker script pins all five to one value, which is the condition
# under which the two answers agree. Every caller reads the layout from here, so
# there is one answer.
# ---------------------------------------------------------------------------
def _align_up(value: int, align: int) -> int:
    return (value + align - 1) & ~(align - 1)


def loader_layout(sizes: "dict[str, int]") -> "dict[str, int]":
    """Return {section name -> the VMA the loader's size-rounding formula gives it}.

    A name absent from `sizes` counts as size 0, matching what the loader reads from a
    zero-size header field, which is what a mode carrying no .data presents. The extra
    `_end` key carries the accumulated total, the size of the whole allocatable region.
    """
    layout: "dict[str, int]" = {}
    cursor = 0
    for name in ALLOC_SECTIONS:
        layout[name] = cursor
        cursor += _align_up(sizes.get(name, 0), ALLOC_ALIGN)
    layout["_end"] = cursor
    return layout


# Collect the R_ARM_RELATIVE offsets so the runtime can relocate the mode's pointer
# literals when it loads. The helper accepts R_ARM_NONE, rejects any other type with
# an error naming the toolchain file, requires each offset to be 4-byte aligned, and
# requires each to fall inside [0, ram_size), the whole allocatable region. The region
# is that wide because every .data and .bss reference routes through the GOT, so the
# relocs live past .rodata.
def extract_r_arm_relative(elf, text_size: int, rodata_size: int,
                            got_size: int, data_size: int,
                            bss_size: int) -> "list[int]":
    # Both bounds below read the one layout model, so the check and the message it
    # prints cannot drift apart.
    layout = loader_layout({
        ".text": text_size, ".rodata": rodata_size, ".got": got_size,
        ".data": data_size, ".bss": bss_size,
    })

    # .rodata sits at the next boundary after .text, so the alignment gap between
    # them is not a content region and an offset there is rejected below.
    rodata_vma = layout[".rodata"]

    # The valid range is the whole allocatable region, which the model's running
    # total already carries. The offsets are link VMAs, so the bound has to include
    # the padding between sections as well as their bytes.
    ram_size = layout["_end"]

    offsets: "list[int]" = []
    for section in elf.iter_sections():
        if not isinstance(section, RelocationSection):
            continue
        for rel in section.iter_relocations():
            t = rel["r_info_type"]
            if t == R_ARM_NONE:
                continue
            if t != R_ARM_RELATIVE:
                _die(
                    f"unsupported relocation type {t} in {section.name}. "
                    "R_ARM_RELATIVE (23) is the only accepted type. Verify the "
                    "build uses operator-mode-toolchain.cmake."
                )
            off = rel["r_offset"]
            if off & 3:
                _die(
                    f"misaligned reloc offset 0x{off:x} in {section.name}. "
                    "Every pointer slot must be 4-byte aligned."
                )
            # Reject an offset in the .text-to-.rodata alignment gap, which the
            # linker never emits into and which is not a content region.
            if off >= text_size and off < rodata_vma:
                _die(
                    f"reloc offset 0x{off:x} in text->rodata alignment "
                    f"gap [0x{text_size:x}, 0x{rodata_vma:x}). The linker "
                    "emitted a reloc for a non-content region. Verify "
                    "operator-mode.ld did not change shape."
                )
            # A reloc can legitimately land in .got or .data as well as .text or
            # .rodata, so the bound is the full RAM region.
            if off >= ram_size:
                _die(
                    f"reloc offset 0x{off:x} outside "
                    f"[0, ram_size) = [0, 0x{ram_size:x}). "
                    "Every R_ARM_RELATIVE offset must land inside an allocatable section."
                )
            offsets.append(off)
    offsets.sort()
    return offsets


# ---------------------------------------------------------------------------
# Core: pack()
# ---------------------------------------------------------------------------
def pack(
    elf_path: str | Path,
    out_path: str | Path,
    mode_name: str,
    mode_type: str,
    flags: int = 0,
    schema_ver: int = 1,
) -> None:
    """Read `elf_path`, validate exports, write `.opm` file at `out_path`.

    Raises SystemExit(1) with a helpful message on any validation failure.
    """
    elf_path = Path(elf_path)
    out_path = Path(out_path)

    if mode_type not in MODE_TYPE:
        _die(f"--type must be one of {sorted(MODE_TYPE)}, got '{mode_type}'")
    if not (0 <= flags <= 0xFF):
        _die(f"--flags must fit in a uint8 (0..255), got {flags}")
    if not (0 <= schema_ver <= 0xFFFF):
        _die(f"--schema-version must fit in uint16, got {schema_ver}")
    if mode_type == "output" and (flags & GENERATES_CLOCK_FLAG):
        _die(
            "GENERATES_CLOCK is valid on --type global, and this mode is --type "
            "output. See docs/timing-and-clock.md"
        )

    encoded_name = mode_name.encode("utf-8")
    if len(encoded_name) > 31:
        _die(
            f"--name must fit in 31 bytes + NUL (got {len(encoded_name)} bytes: "
            f"'{mode_name}')"
        )
    name_field = encoded_name + b"\0" * (32 - len(encoded_name))

    with open(elf_path, "rb") as f:
        elf = ELFFile(f)

        alloc_sections = _collect_alloc_sections(elf)
        symbols = _collect_symbols(elf)

        # --- Validate required symbols ---------------------------------------
        missing = []
        for sym in REQUIRED_SYMBOLS:
            if sym in symbols:
                continue
            mangled = _maybe_mangled(symbols, sym)
            if mangled is not None:
                _die(
                    f"missing required symbol '{sym}'. The mangled form "
                    f"'{mangled}' is present instead. Did you forget extern \"C\"?"
                )
            missing.append(sym)
        if missing:
            _die(
                "missing required symbols: "
                + ", ".join(repr(m) for m in missing)
                + ". Every mode exports these inside an extern \"C\" block."
            )

        # --- Collect section bytes in linker-script order --------------------
        # .text, .rodata, .got, .data are copied verbatim. Every section is
        # accepted even when empty (pyelftools returns size 0 correctly).
        # .bss is size-only (its contents live in RAM, not the file).
        #
        # A section missing from the ELF counts as zero-size, which is what .got
        # is for a mode the linker gave no GOT entries. .text has to be there,
        # since mode_init and the rest live in it, and the missing-symbols check
        # above already enforces that.
        def _section_or_empty(name: str) -> "tuple[bytes, int]":
            """Return (bytes, vma). Missing section -> (b'', 0)."""
            section = alloc_sections.get(name)
            if section is None:
                return (b"", 0)
            # .bss and its kind are SHT_NOBITS, so data() hands back b''.
            return (section.data(), section["sh_addr"])

        text_bytes, text_vma = _section_or_empty(".text")
        rodata_bytes, rodata_vma = _section_or_empty(".rodata")
        got_bytes, got_vma = _section_or_empty(".got")
        data_bytes, data_vma = _section_or_empty(".data")

        if len(text_bytes) == 0:
            _die("ELF has no .text section, so there is no code to pack")

        # .bss: declared size, no bytes in the file.
        bss_section = alloc_sections.get(".bss")
        bss_size = bss_section["sh_size"] if bss_section is not None else 0

        # --- Compute file offsets --------------------------------------------
        # The packed .opm file layout is:
        #   [0 ..  148)                     OpmHeader
        #   [text_offset .. +text_size)     .text bytes
        #   [rodata_offset .. +rodata_size) .rodata bytes
        #   [got_offset .. +got_size)       .got bytes
        #   [data_offset .. +data_size)     .data bytes
        #   [reloc_pad (0..3 bytes)]        alignment to 4
        #   [reloc_offset .. +reloc_count*4] R_ARM_RELATIVE table
        text_offset = HEADER_SIZE
        rodata_offset_val = text_offset + len(text_bytes)
        got_offset = rodata_offset_val + len(rodata_bytes)
        data_offset = got_offset + len(got_bytes)

        text_size = len(text_bytes)
        rodata_size_val = len(rodata_bytes)
        got_size = len(got_bytes)
        data_size = len(data_bytes)

        # Extract the R_ARM_RELATIVE offsets the runtime relocates at load. The
        # helper needs got_size, data_size and bss_size to compute ram_size, the
        # bound each offset is checked against. That bound spans the whole
        # allocatable region because a .data or .bss reference routes through the
        # GOT, which puts nearly every reloc in .got.
        reloc_offsets = extract_r_arm_relative(
            elf, text_size, rodata_size_val,
            got_size, data_size, bss_size,
        )
        reloc_count = len(reloc_offsets)
        # Align reloc_offset up to 4 (the runtime reads it as a uint32 array).
        _unaligned_reloc_offset = data_offset + data_size
        reloc_offset_val = (_unaligned_reloc_offset + 3) & ~3
        reloc_pad = reloc_offset_val - _unaligned_reloc_offset
        assert (reloc_offset_val & 3) == 0, (
            "reloc_offset must be 4-byte aligned"
        )
        # Sentinel convention when no relocations: reloc_offset == 0.
        if reloc_count == 0:
            reloc_offset_val = 0
            reloc_pad = 0

        # --- Resolve symbol offsets ------------------------------------------
        # Function export offsets are expressed relative to .text VMA, then
        # translated to the packed-file layout where .text lives at text_offset.
        # The runtime reads init_offset / process_offset / destroy_offset as
        # absolute packed-file offsets that reside inside .text.
        def _text_relative_offset(sym_name: str) -> int:
            sym = symbols[sym_name]
            value = sym["st_value"]
            # A Thumb function address carries its low bit set to mark Thumb
            # state, so it is masked off for the offset to land on the section
            # bytes.
            if value & 1:
                value -= 1
            if value < text_vma or value >= text_vma + text_size:
                _die(
                    f"symbol '{sym_name}' at 0x{value:x} is outside .text "
                    f"[0x{text_vma:x}, 0x{text_vma + text_size:x})"
                )
            return text_offset + (value - text_vma)

        init_offset = _text_relative_offset("mode_init")
        process_offset = _text_relative_offset("mode_process")
        destroy_offset = _text_relative_offset("mode_destroy")

        migrate_offset = 0
        if "mode_migrate_config" in symbols:
            migrate_offset = _text_relative_offset("mode_migrate_config")

        ui_render_offset = 0
        if "mode_ui_render" in symbols:
            ui_render_offset = _text_relative_offset("mode_ui_render")

        ui_gesture_offset = 0
        if "mode_ui_gesture" in symbols:
            ui_gesture_offset = _text_relative_offset("mode_ui_gesture")

        # --- Param table -----------------------------------------------------
        kparams_sym = symbols["kParams"]
        kparams_addr = kparams_sym["st_value"]
        kparams_size = kparams_sym["st_size"]
        if kparams_size == 0:
            _die(
                "kParams has zero size. Declare at least one ParamSpec entry "
                "in kParams[], or set kParamCount = 0 and kParams[0] = {} for a "
                "mode with no params at all."
            )
        # kParams lives in .rodata. Locate it and convert to packed-file offset.
        host = _find_section_containing(alloc_sections, kparams_addr, kparams_size)
        if host is None:
            _die(
                f"kParams at 0x{kparams_addr:x} (+{kparams_size}) is not inside "
                "any allocatable section. Declaring it const puts it in .rodata."
            )
        host_name, host_section = host
        host_bytes, host_vma = _section_or_empty(host_name)
        host_offset_in_file = {
            ".text": text_offset,
            ".rodata": rodata_offset_val,
            ".got": got_offset,
            ".data": data_offset,
        }.get(host_name)
        if host_offset_in_file is None:
            _die(
                f"kParams lives in unexpected section '{host_name}', not "
                ".rodata. Declare kParams as const."
            )
        param_table_offset = host_offset_in_file + (kparams_addr - host_vma)
        param_table_size = kparams_size

        # param_count: read the initialized byte from kParamCount's storage.
        kpc_sym = symbols["kParamCount"]
        kpc_addr = kpc_sym["st_value"]
        kpc_size = kpc_sym["st_size"]
        if kpc_size != 1:
            _die(
                f"kParamCount must be a single uint8_t (size=1), got size={kpc_size}. "
                "Declare it as: const uint8_t kParamCount = <N>;"
            )
        kpc_host = _find_section_containing(alloc_sections, kpc_addr, kpc_size)
        if kpc_host is None:
            _die(f"kParamCount at 0x{kpc_addr:x} is not inside any allocatable section")
        kpc_host_name, _ = kpc_host
        kpc_host_bytes, kpc_host_vma = _section_or_empty(kpc_host_name)
        param_count = kpc_host_bytes[kpc_addr - kpc_host_vma]

        if param_count == 0:
            # Zero-param mode (OP_MODE_NO_PARAMS). The macro still emits a dummy
            # kParams[1] (32 bytes) + kParamStrings[1] so the linker can resolve
            # the required export symbols, but those are linker-only
            # placeholders, with no real parameter table. The OpmHeader ABI
            # contract (see the Parameter Tables group in
            # include/operator_sdk/abi/mode_format.h) mandates all four param-table
            # header fields be zero when param_count == 0, and the runtime reads
            # the table only when param_table_offset is non-zero. No param table is
            # advertised and the placeholder symbols are ignored, which overwrites
            # the param_table_offset and param_table_size computed above from the
            # placeholder kParams symbol.
            param_table_offset = 0
            param_table_size = 0
            param_strings_offset = 0
            param_strings_size = 0
        else:
            if param_count * PARAM_SPEC_SIZE != param_table_size:
                # kParams size should equal param_count * sizeof(ParamSpec).
                # A mismatch here almost always means the ParamSpec layout
                # drifted, which is worth failing loudly over.
                _die(
                    f"kParams size ({param_table_size}) != kParamCount "
                    f"({param_count}) * sizeof(ParamSpec) ({PARAM_SPEC_SIZE}). "
                    "Check that kParams[] has exactly kParamCount entries."
                )

            # Locate kParamStrings on its own. It does not necessarily sit right
            # after kParams, since the linker may place kParamCount and its padding
            # between them in .rodata, so the file records an explicit strings offset
            # and size and the runtime reads them straight.
            kps_sym = symbols["kParamStrings"]
            kps_addr = kps_sym["st_value"]
            kps_size = kps_sym["st_size"]
            kps_host = _find_section_containing(alloc_sections, kps_addr, kps_size)
            if kps_host is None:
                _die(f"kParamStrings at 0x{kps_addr:x} is not inside any "
                     "allocatable section")
            kps_host_name, _ = kps_host
            kps_offset_in_file = {
                ".text": text_offset,
                ".rodata": rodata_offset_val,
                ".got": got_offset,
                ".data": data_offset,
            }.get(kps_host_name)
            if kps_offset_in_file is None:
                _die(f"kParamStrings lives in unexpected section "
                     f"'{kps_host_name}', not .rodata. Declare kParamStrings "
                     "as const.")
            _, kps_host_vma = _section_or_empty(kps_host_name)
            param_strings_offset = kps_offset_in_file + (kps_addr - kps_host_vma)
            param_strings_size = kps_size

        # --- Action callback table ------------------------------------------
        # The DSL emits an Action's .on_select pointers into kOnSelectCallbacks
        # (in .rodata, relocated at load) and the real length into kOnSelectCount.
        # A mode with no Action rows still emits a one-entry placeholder table,
        # so the count byte is what gates the header field, 0 meaning offset 0.
        # The stored value is the table symbol's link address (VMA). The count
        # travels in the header too: it is the only bound the device can check
        # an Action spec's callback index against.
        on_select_table_offset = 0
        on_select_count = 0
        if "kOnSelectCount" in symbols and "kOnSelectCallbacks" in symbols:
            kosc_sym = symbols["kOnSelectCount"]
            kosc_host = _find_section_containing(
                alloc_sections, kosc_sym["st_value"], kosc_sym["st_size"])
            if kosc_host is not None:
                kosc_host_name, _ = kosc_host
                kosc_host_bytes, kosc_host_vma = _section_or_empty(kosc_host_name)
                on_select_count = kosc_host_bytes[kosc_sym["st_value"] - kosc_host_vma]
                if on_select_count > 0:
                    # The table's link VMA travels in the header unchanged: the
                    # loader lays every allocatable section out contiguously
                    # from the pool base in VMA order, so ram + VMA is the
                    # runtime address. Locating it first is what makes that
                    # true -- a mis-linked table that fell outside the
                    # allocatable region would otherwise be written as an
                    # offset the loader resolves into unrelated pool memory and
                    # calls through.
                    kost_addr = symbols["kOnSelectCallbacks"]["st_value"]
                    kost_size = on_select_count * ON_SELECT_ENTRY_SIZE
                    # .rodata alone, not every allocatable section. The device
                    # bounds the table against .rodata's extent, and the linker
                    # places sections such as .rel.dyn after .bss, outside the
                    # window where ram + VMA is the runtime address. A table
                    # landing in one of those would resolve into unrelated pool
                    # memory on the device and be called.
                    rodata_only = {
                        name: section
                        for name, section in alloc_sections.items()
                        if name == ".rodata"
                    }
                    kost_host = _find_section_containing(
                        rodata_only, kost_addr, kost_size)
                    if kost_host is None:
                        _die(
                            f"kOnSelectCallbacks at 0x{kost_addr:x} (+{kost_size}) "
                            "is not inside any allocatable section. The Action "
                            "callback table must live in .rodata."
                        )
                    on_select_table_offset = kost_addr

        # on_sysex_offset locates the mode_on_sysex handler in .text, the same
        # way ui_render_offset locates its own. 0 when the mode exports none.
        on_sysex_offset = 0
        if "mode_on_sysex" in symbols:
            on_sysex_offset = _text_relative_offset("mode_on_sysex")

        # A mode marked HANDLES_SYSEX must register a handler, which the device
        # resolves through this offset.
        HANDLES_SYSEX_FLAG = 0x01
        if (flags & HANDLES_SYSEX_FLAG) and on_sysex_offset == 0:
            _die("mode is marked HANDLES_SYSEX but registers no SysEx handler. "
                 "Pass one third to OP_MODE_REGISTER, or drop "
                 "HANDLES_SYSEX from the mode's FLAGS.")

        # Sentinel-convert for the header fields (internal rodata_offset_val
        # and reloc_offset_val may already be 0 if their sections are empty).
        header_rodata_offset = rodata_offset_val if rodata_size_val > 0 else 0
        header_reloc_offset = reloc_offset_val if reloc_count > 0 else 0

        # --- Build header (seven-group layout) -------------------------------
        header = struct.pack(
            HEADER_FMT,
            # Group 1: Identity
            OPM_MAGIC,
            HEADER_SIZE,
            SDK_VERSION,
            MODE_TYPE[mode_type],
            flags & 0xFF,
            schema_ver,
            param_count,
            on_select_count,
            # Group 2: Section Layout
            text_offset, text_size,
            header_rodata_offset, rodata_size_val,
            got_offset, got_size,
            data_offset, data_size,
            bss_size,
            # Group 3: Relocations
            header_reloc_offset, reloc_count,
            # Group 4: Entry Points
            init_offset, process_offset, destroy_offset,
            migrate_offset,
            ui_render_offset, ui_gesture_offset,
            on_sysex_offset, on_select_table_offset,
            # Group 5: Parameter Tables
            param_table_offset, param_table_size,
            param_strings_offset, param_strings_size,
            # Group 6: Name
            name_field,
        )
        if len(header) != HEADER_SIZE:
            _die(f"internal error: header is {len(header)} bytes (expected {HEADER_SIZE})")

        # --- Assemble and write the .opm -------------------------------------
        # Append R_ARM_RELATIVE uint32[] table at 4-byte-aligned tail.
        reloc_blob = struct.pack(f"<{reloc_count}I", *reloc_offsets)
        blob = b"".join((
            header,
            text_bytes, rodata_bytes, got_bytes, data_bytes,
            b"\x00" * reloc_pad,
            reloc_blob,
        ))

        out_path.parent.mkdir(parents=True, exist_ok=True)
        with open(out_path, "wb") as o:
            o.write(blob)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def _parse_flags(value: str) -> int:
    """Parse --flags that may be hex ('0x06'), decimal ('6'), or blank."""
    if value is None or value == "":
        return 0
    # int(value, 0) auto-detects 0x/0o/0b prefixes.
    return int(value, 0)


def main(argv: "list[str] | None" = None) -> int:
    parser = argparse.ArgumentParser(
        prog="pack_opm.py",
        description="Pack an ELF produced by the Operator SDK toolchain "
                    "into a firmware-loadable .opm binary.",
    )
    parser.add_argument("--input", required=True, help="Path to the linker output (.elf)")
    parser.add_argument("--output", required=True, help="Path to write the .opm")
    parser.add_argument("--type", required=True, choices=sorted(MODE_TYPE),
                        help="Mode type: global or output")
    parser.add_argument("--name", required=True,
                        help="Mode name, up to 31 bytes, stored NUL-terminated")
    parser.add_argument("--flags", default="0",
                        help="OpmHeader.flags byte, hex or decimal, default 0x00")
    parser.add_argument("--schema-version", default=1, type=int,
                        help="Config schema version (default 1)")
    args = parser.parse_args(argv)

    try:
        flags = _parse_flags(args.flags)
    except ValueError as exc:
        _die(f"could not parse --flags='{args.flags}': {exc}")

    pack(
        elf_path=args.input,
        out_path=args.output,
        mode_name=args.name,
        mode_type=args.type,
        flags=flags,
        schema_ver=args.schema_version,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
