/* Flat layout for the Unicorn instruction-count bench (not for hardware). */
MEMORY {
    CODE (rx) : ORIGIN = 0x10000000, LENGTH = 1M
    RAM (rw)  : ORIGIN = 0x20000000, LENGTH = 512K
}
SECTIONS {
    .text : { KEEP(*(.text.entry)) *(.text*) *(.rodata*) } > CODE
    .data : { *(.data*) } > RAM
    .bss : { *(.bss*) *(COMMON) } > RAM
}
