OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY {
    /* Bootloader region expanded to 28 KB + 64 bytes (was 24 KB) to fit
     * the GUI and edit subsystems. The extra 64 bytes absorb the IRQ
     * vector padding added in start.S (Stage 1 scheduler). cc workspace
     * and app slot now share the top 4 KB (0x7000-0x7FFF). */
    RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 28K + 64
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
    _stack_top = 0x00007000;
}
