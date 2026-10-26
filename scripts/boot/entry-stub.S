/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Position-independent ARMv7 entry stub for S5L8947X boot bundle.
 *
 * Handles entry from either Stage 1 DFU loader or Stage 2 wake trampoline:
 *   1. Masks IRQ and FIQ, enters Supervisor mode.
 *   2. If SCTLR.M == 1, cleans + invalidates L1 D-cache by set/way; otherwise
 *      invalidates L1 D-cache by set/way (before touching any MMIO).
 *   3. Disables MMU, D-cache, I-cache, and branch prediction.
 *   4. Cleans + invalidates PL310 L2 cache controller (0x3e000000).
 *   5. Arms hardware watchdog at 0x3f103020 for a 120-second reset safety net.
 *   6. Relocates the 32-byte-aligned payload bundle following this stub to
 *      0x80000000 and jumps to 0x80000000.
 */

	.arch	armv7-a
	.syntax	unified
	.arm

	.equ	LOAD_ADDR,	0x80000000
	.equ	WDT_BASE,	0x3f103020
	.equ	WDT_HZ,		24000000
	.equ	PL310_BASE,	0x3e000000

#ifndef STUB_WDT_SECONDS
#define STUB_WDT_SECONDS 120
#endif

	.section .text.start, "ax"
	.global	_start
_start:
	cpsid	if
	msr	cpsr_c, #0xd3

	/* 1. L1 D-cache: clean+invalidate if MMU on, invalidate-only if MMU off */
	mrc	p15, 0, r0, c1, c0, 0
	tst	r0, #(1 << 0)
	blne	v7_flush_dcache_all
	bleq	v7_invalidate_dcache_all

	/* 2. Disable MMU, D-cache, I-cache, and branch prediction */
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #(1 << 0)
	bic	r0, r0, #(1 << 2)
	bic	r0, r0, #(1 << 12)
	bic	r0, r0, #(1 << 11)
	mcr	p15, 0, r0, c1, c0, 0
	isb	sy
	mov	r0, #0
	mcr	p15, 0, r0, c8, c7, 0
	mcr	p15, 0, r0, c7, c5, 0
	mcr	p15, 0, r0, c7, c5, 6
	dsb	sy
	isb	sy

	/* 3. Clean and invalidate PL310 L2 cache if enabled */
	bl	l2x0_flush_all

	/* 4. Disarm and re-arm hardware watchdog at 0x3f103020 for 120s */
	ldr	r0, =WDT_BASE
	mov	r1, #0
	str	r1, [r0, #0xc]
	dsb	sy
	str	r1, [r0, #0]
	ldr	r1, =(STUB_WDT_SECONDS * WDT_HZ)
	str	r1, [r0, #4]
	mov	r1, #4
	str	r1, [r0, #0xc]
	dsb	sy

	/* 5. Copy bundle payload to 0x80000000 */
	adrl	r0, _payload_start
	ldr	r1, =LOAD_ADDR
	ldr	r2, =((BUNDLE_SIZE + 31) & ~31)
1:	ldmia	r0!, {r3-r10}
	stmia	r1!, {r3-r10}
	subs	r2, r2, #32
	bhi	1b

	/* 6. Invalidate I-cache and branch predictor, then jump to 0x80000000 */
	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0
	mcr	p15, 0, r0, c7, c5, 6
	dsb	sy
	isb	sy

	ldr	r0, =LOAD_ADDR
	bx	r0

v7_flush_dcache_all:
	mov	r12, #0
	b	v7_dcache_walk
v7_invalidate_dcache_all:
	mov	r12, #1
v7_dcache_walk:
	dmb	sy
	mrc	p15, 1, r0, c0, c0, 1
	ands	r3, r0, #0x7000000
	mov	r3, r3, lsr #23
	beq	9f
	mov	r10, #0
1:	add	r2, r10, r10, lsr #1
	mov	r1, r0, lsr r2
	and	r1, r1, #7
	cmp	r1, #2
	blt	8f
	mcr	p15, 2, r10, c0, c0, 0
	isb	sy
	mrc	p15, 1, r1, c0, c0, 0
	and	r2, r1, #7
	add	r2, r2, #4
	movw	r4, #0x3ff
	ands	r4, r4, r1, lsr #3
	clz	r5, r4
	movw	r7, #0x7fff
	ands	r7, r7, r1, lsr #13
2:	mov	r9, r7
3:	orr	r11, r10, r4, lsl r5
	orr	r11, r11, r9, lsl r2
	cmp	r12, #0
	mcreq	p15, 0, r11, c7, c14, 2
	mcrne	p15, 0, r11, c7, c6, 2
	subs	r9, r9, #1
	bge	3b
	subs	r4, r4, #1
	bge	2b
8:	add	r10, r10, #2
	cmp	r3, r10
	bgt	1b
9:	mov	r10, #0
	mcr	p15, 2, r10, c0, c0, 0
	dsb	sy
	isb	sy
	bx	lr

l2x0_flush_all:
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
	cmp	r1, #0
	bne	1b
	mov	r1, #0
	str	r1, [r0, #0x730]
1:	ldr	r1, [r0, #0x730]
	tst	r1, #1
	bne	1b
2:	dsb	sy
	bx	lr

	.ltorg
	.balign	32
	.global	_payload_start
_payload_start:
	.incbin	PAYLOAD_PATH
_payload_end:
	.if (_payload_end - _payload_start) != BUNDLE_SIZE
	.error "BUNDLE_SIZE does not match the included payload bundle"
	.endif
