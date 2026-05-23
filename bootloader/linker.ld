OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY {
    RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 24K
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

    /* stack lives at top of bootloader region, grows down */
    _stack_top = 0x00006000;
}
