/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Pre-kernel loader assembly entry at 0x80000000.
 *
 * Sets up a stack at 0x80007000 (below zImage at 0x80008000), calls
 * linux_boot_main() to initialize PMGR clocks and 720p HDMI scanout,
 * flushes L1 and L2 (leaving L2 disabled for the kernel), and jumps to zImage at 0x80008000 with
 * r0=0, r1=0xffffffff, r2=DTB_PHYS.
 */

	.arch	armv7-a
	.syntax	unified
	.arm

	.equ	PL310_BASE,	0x3e000000

	.section .text.start, "ax"
	.global	_start
	.global	jump_to_linux
_start:
	cpsid	if
	msr	cpsr_c, #0xd3

	ldr	sp, =0x80007000
	bl	linux_boot_main
1:	wfi
	b	1b

/*
 * void jump_to_linux(uint32_t zimage_phys, uint32_t dtb_phys)
 */
jump_to_linux:
	mov	r4, r0
	mov	r5, r1

	bl	v7_flush_dcache_all
	bl	l2x0_flush_all

	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #(1 << 0)
	bic	r0, r0, #(1 << 2)
	bic	r0, r0, #(1 << 12)
	bic	r0, r0, #(1 << 11)
	mcr	p15, 0, r0, c1, c0, 0
	isb

	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0
	mcr	p15, 0, r0, c7, c5, 6
	mcr	p15, 0, r0, c8, c7, 0
	dsb
	isb

	mov	r0, #0
	mvn	r1, #0
	mov	r2, r5
	bx	r4

v7_flush_dcache_all:
	push	{r4-r11, lr}
	dmb
	mrc	p15, 1, r0, c0, c0, 1
	ands	r3, r0, #0x7000000
	mov	r3, r3, lsr #23
	beq	.Lfinished
	mov	r10, #0
.Lloop_level:
	add	r2, r10, r10, lsr #1
	mov	r1, r0, lsr r2
	and	r1, r1, #7
	cmp	r1, #2
	blt	.Lskip_level
	mcr	p15, 2, r10, c0, c0, 0
	isb
	mrc	p15, 1, r1, c0, c0, 0
	and	r2, r1, #7
	add	r2, r2, #4
	movw	r4, #0x3ff
	ands	r4, r4, r1, lsr #3
	clz	r5, r4
	movw	r7, #0x7fff
	ands	r7, r7, r1, lsr #13
.Lloop_way:
	mov	r9, r7
.Lloop_set:
	orr	r11, r10, r4, lsl r5
	orr	r11, r11, r9, lsl r2
	mcr	p15, 0, r11, c7, c14, 2
	subs	r9, r9, #1
	bge	.Lloop_set
	subs	r4, r4, #1
	bge	.Lloop_way
.Lskip_level:
	add	r10, r10, #2
	cmp	r3, r10
	bgt	.Lloop_level
.Lfinished:
	mov	r10, #0
	mcr	p15, 2, r10, c0, c0, 0
	dsb
	isb
	pop	{r4-r11, pc}

l2x0_flush_all:
	push	{r0, r1, r2, lr}
	ldr	r0, =PL310_BASE
	ldr	r1, [r0]
	lsr	r2, r1, #6
	and	r2, r2, #0x3f
	cmp	r2, #3
	cmpne	r2, #2
	cmpne	r2, #1
	bne	2f
	ldr	r1, [r0, #0x100]
	tst	r1, #1
	beq	2f
	mov	r1, #0xff00
	orr	r1, r1, #0xff
	str	r1, [r0, #0x7fc]
1:	ldr	r1, [r0, #0x7fc]
	tst	r1, #1
	bne	1b
	mov	r1, #0
	str	r1, [r0, #0x730]	@ CACHE_SYNC
1:	ldr	r1, [r0, #0x730]
	tst	r1, #1
	bne	1b
	mov	r1, #0
	str	r1, [r0, #0x100]	@ leave it disabled: Linux sets it up
2:	dsb
	pop	{r0, r1, r2, pc}
