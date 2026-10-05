/* Everything in one RAM region, as a native module is. */
ENTRY(armtest_main)
SECTIONS
{
	. = 0x20000000;
	.text : { *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	.data : {
		*(.data.rel.ro .data.rel.ro.*) *(.got .got.*) *(.data .data.*)
		. = ALIGN(4);
		fj_init_array_start = .;
		KEEP(*(.init_array .init_array.*))
		fj_init_array_end = .;
	}
	.bss (NOLOAD) : { __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(8); __bss_end = .; }
	/DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.fini_array*) }
}
