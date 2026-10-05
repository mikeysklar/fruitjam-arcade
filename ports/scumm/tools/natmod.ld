/* fruitjam-scumm: partial link (ld -r) of the whole module into a single
 * .text section before mpy_ld sees it. mpy_ld only accepts text, rodata
 * and bss, refuses initialised .data and PC-relative references to bss,
 * and knows nothing about C++ constructors. Native module code is loaded
 * into RAM, so writable data can simply live inside .text. bss becomes
 * zero bytes in the image, and the constructor list is bracketed by two
 * symbols that mpy_init walks. */
SECTIONS
{
	.text : {
		*(.text .text.*)
		. = ALIGN(8);
		*(.rodata .rodata.*)
		*(.data.rel.ro .data.rel.ro.*)
		. = ALIGN(4);
		fj_init_array_start = .;
		KEEP(*(.init_array .init_array.*))
		fj_init_array_end = .;
		. = ALIGN(8);
		*(.data .data.*)
		. = ALIGN(8);
		*(.bss .bss.* COMMON)
		. = ALIGN(8);
	}
	/DISCARD/ : {
		*(.ARM.exidx .ARM.exidx.*)
		*(.ARM.extab .ARM.extab.*)
		*(.fini_array .fini_array.*)
		*(.comment)
	}
}
