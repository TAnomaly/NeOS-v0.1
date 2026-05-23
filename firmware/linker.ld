OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY {
    /* App starts at 0x6000, leaving 0x0000-0x5FFF (24K) for NeOS. */
    RAM (rwx) : ORIGIN = 0x00006000, LENGTH = 8K
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

    _stack_top = ORIGIN(RAM) + LENGTH(RAM);   /* 0x8000 */
}
