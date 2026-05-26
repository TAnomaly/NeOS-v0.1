OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY {
    /* Bootloader claims full 32K BRAM. cc workspace + app slot temporarily
     * disabled (cc command broken until memory map cleanup). Stack at top. */
    RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 32K
}

SECTIONS {
    /DISCARD/ : { *(.note*) *(.comment*) *(.riscv.attributes) }

    .text : {
        KEEP(*(.text.init))
        *(.text*)
    } > RAM

    .rodata : { *(.rodata*) } > RAM
    .data   : { *(.data*)   } > RAM
    .bss    : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _bss_end = .;
    } > RAM

    /* stack lives at top of BRAM, grows down */
    _stack_top = 0x00008000;
}
