/* Operator mode linker script.
 *
 * Emits a position-independent ELF-32 ARM executable whose allocatable
 * sections start at VMA = 0. The firmware loader allocates RAM at runtime
 * and relocates the GOT to that address.
 *
 * Section order matches the loader's expectation:
 *   .text  .rodata  .got  .data  .bss
 *
 * Each of those five starts on an 8-byte boundary.
 *
 * The mode_* export functions are KEEP()'d so --gc-sections cannot drop them.
 * pack_opm.py looks them up by symbol name. If the linker dropped them,
 * packaging would fail with "missing required symbol".
 */

ENTRY(mode_init)

SECTIONS {
    . = 0;

    .text : ALIGN(8) {
        KEEP(*(.text.mode_init))
        KEEP(*(.text.mode_process))
        KEEP(*(.text.mode_on_sysex))
        KEEP(*(.text.mode_destroy))
        KEEP(*(.text.mode_migrate_config))
        KEEP(*(.text.mode_ui_render))
        KEEP(*(.text.mode_ui_gesture))
        *(.text .text.*)
    }

    .rodata : ALIGN(8) {
        /* KEEP() the per-symbol rodata sections that -fdata-sections emits for
         * the three well-known tables pack_opm.py reads by name. These are
         * exported with external linkage (OP_MODE_PARAMS emits them extern
         * "C"). KEEP() is a belt-and-suspenders safeguard against accidental
         * --gc-sections pruning of the param-table region. */
        KEEP(*(.rodata.kParams*))
        KEEP(*(.rodata.kParamCount*))
        KEEP(*(.rodata.kParamStrings*))
        KEEP(*(.rodata.kOnSelectCount*))
        /* The Action callback table holds function pointers, so the compiler
         * emits it into .data.rel.ro. Merging it into .rodata here keeps the
         * allocatable sections VMA-contiguous from 0, and the R_ARM_RELATIVE
         * walk resolves its pointers to ram + VMA at load. */
        KEEP(*(.data.rel.ro.kOnSelectCallbacks*))
        KEEP(*(.data.rel.ro.local.kOnSelectCallbacks*))
        KEEP(*(.rodata.kOnSelectCallbacks*))
        *(.rodata .rodata.*)
        *(.data.rel.ro .data.rel.ro.* .data.rel.ro.local*)
    }

    /* GOT[0] is reserved as a per-instance cell the loader uses at load time to
     * associate a running mode with its own load instance. It must not be used
     * for anything else. The LONG(0) emits 4 bytes of zero filler that the
     * loader fills in when the mode is loaded. The linker assigns the filler
     * GOT offset 0, shifts every subsequent .got entry by +4, and re-emits
     * every R_ARM_RELATIVE so its offset reflects the shift, which is standard
     * ELF dynamic-linker behavior. pack_opm.py reads .got by section name (not
     * offset), so the .opm file format is unchanged. */
    .got : ALIGN(8) {
        LONG(0);
        *(.got .got.*)
    }

    .data : ALIGN(8) { *(.data .data.*) }

    .bss  : ALIGN(8) { *(.bss .bss.*) *(COMMON) }

    /* .rel.dyn is explicitly placed after .bss so the allocatable sections
     * (.text/.rodata/.got/.data/.bss) stay VMA-contiguous from 0. Without it,
     * ld's default orphan placement can insert .rel.dyn between .rodata and
     * .got, creating a VMA gap that breaks the single-load-bias relocation the
     * firmware loader relies on. pack_opm.py reads .rel.dyn by section name, so
     * placement does not affect extraction. */
    .rel.dyn : ALIGN(4) { *(.rel.dyn) }

    /DISCARD/ : {
        *(.plt) *(.interp) *(.dynamic) *(.dynsym) *(.dynstr)
        *(.hash) *(.gnu.hash) *(.note*) *(.comment)
        *(.ARM.exidx) *(.ARM.extab) *(.ARM.attributes)
    }
}
