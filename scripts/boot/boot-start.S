/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Pre-kernel loader assembly entry at 0x80000000.
 *
 * Sets up a temporary stack at 0x83f00000, calls linux_boot_main() to
 * initialize PMGR clocks and 720p HDMI scanout, flushes caches, and jumps
 * to zImage at 0x80008000 with r0=0, r1=0xffffffff, r2=DTB_PHYS.
 */

	.syntax unified
	.arch armv7-a
	.arm

	.section .text.start, "ax"
	.global _start
_start:
	cpsid	if

	/* Enable I-cache while MMU and D-cache remain disabled */
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #(1 << 0)
	bic	r0, r0, #(1 << 2)
	orr	r0, r0, #(1 << 12)
	mcr	p15, 0, r0, c1, c0, 0
	isb

	ldr	sp, =0x83f00000
	bl	linux_boot_main
1:	b	1b

	/*
	 * void jump_to_linux(uint32_t zimage_phys, uint32_t dtb_phys)
	 */
	.global jump_to_linux
jump_to_linux:
	mov	r4, r0
	mov	r5, r1

	cpsid	if

	/* Disable I-cache, D-cache, MMU, and branch predictor */
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #(1 << 0)
	bic	r0, r0, #(1 << 2)
	bic	r0, r0, #(1 << 11)
	bic	r0, r0, #(1 << 12)
	mcr	p15, 0, r0, c1, c0, 0
	isb

	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0
	mcr	p15, 0, r0, c7, c5, 6
	mcr	p15, 0, r0, c8, c7, 0
	dsb	sy
	isb

	mov	r0, #0
	mvn	r1, #0
	mov	r2, r5
	bx	r4
