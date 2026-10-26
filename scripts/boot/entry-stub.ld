/* SPDX-License-Identifier: GPL-2.0-only */
ENTRY(_start)

SECTIONS
{
	. = 0x8fe00000;

	.text : {
		KEEP(*(.text.start))
		*(.text .text.*)
		*(.rodata .rodata.*)
		. = ALIGN(4);
	}

	/DISCARD/ : {
		*(.ARM.attributes)
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}
}
