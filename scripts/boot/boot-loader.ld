/* SPDX-License-Identifier: GPL-2.0-only */
ENTRY(_start)

SECTIONS
{
	. = 0x80000000;

	.text : {
		KEEP(*(.text.start))
		*(.text .text.*)
		*(.rodata .rodata.*)
		*(.data .data.*)
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(4);
	}

	.ARM.attributes 0 : { *(.ARM.attributes) }

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}
}
