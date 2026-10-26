/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Position-independent ARMv7 entry stub for S5L8947X boot bundle.
 *
 * Handles entry from either Stage 1 DFU loader (MMU off) or Stage 2 loader
 * (MMU on):
 *   1. Masks IRQ and FIQ, enters Supervisor mode.
 *   2. Emits 'S' on UART0 (0x32500000).
 *   3. If SCTLR.M == 1, cleans + invalidates L1 D-cache by set/way; otherwise
 *      invalidates L1 D-cache by set/way.
 *   4. Disables MMU, D-cache, I-cache, and branch prediction.
 *   5. Cleans + invalidates and disables PL310 L2 cache controller (0x3e000000).
 *   6. Arms hardware watchdog at 0x3f103020 for a 120-second reset safety net.
 *   7. Relocates the payload bundle following this stub to 0x80000000 and
 *      jumps to 0x80000000.
 */

	.syntax unified
	.arch armv7-a
	.arm

	.section .text.start, "ax"
	.global _start
_start:
	cpsid	if, #0x13

	/* Emit 'S' on UART0 (0x32500000, UTXH +0x20) */
	movw	r10, #0x0000
	movt	r10, #0x3250
	mov	r0, #'S'
	str	r0, [r10, #0x20]
	dsb	sy

	/* Check SCTLR.M (bit 0) to choose clean+invalidate vs invalidate-only */
	mrc	p15, 0, r0, c1, c0, 0
	tst	r0, #1
	beq	.L_inv_only

	/* MMU on: clean + invalidate L1 D-cache by set/way (DCCISW) */
	mov	r0, #0
	mcr	p15, 2, r0, c0, c0, 0
	isb
	mrc	p15, 1, r1, c0, c0, 0
	ubfx	r2, r1, #13, #15
	mov	r3, #0
1:	mov	r4, #0
2:	lsl	r5, r4, #30
	orr	r5, r5, r3, lsl #5
	mcr	p15, 0, r5, c7, c14, 2
	add	r4, r4, #1
	cmp	r4, #4
	blt	2b
	add	r3, r3, #1
	cmp	r3, r2
	ble	1b
	dsb	sy
	b	.L_caches_done

.L_inv_only:
	/* MMU off: invalidate L1 D-cache by set/way (DCISW) */
	mov	r0, #0
	mcr	p15, 2, r0, c0, c0, 0
	isb
	mrc	p15, 1, r1, c0, c0, 0
	ubfx	r2, r1, #13, #15
	mov	r3, #0
3:	mov	r4, #0
4:	lsl	r5, r4, #30
	orr	r5, r5, r3, lsl #5
	mcr	p15, 0, r5, c7, c6, 2
	add	r4, r4, #1
	cmp	r4, #4
	blt	4b
	add	r3, r3, #1
	cmp	r3, r2
	ble	3b
	dsb	sy

.L_caches_done:
	/* Clear SCTLR.M (0), C (2), Z (11), I (12) */
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #(1 << 0)
	bic	r0, r0, #(1 << 2)
	bic	r0, r0, #(1 << 11)
	bic	r0, r0, #(1 << 12)
	mcr	p15, 0, r0, c1, c0, 0
	isb
	dsb	sy

	/* Invalidate I-cache, branch predictor, and unified TLB */
	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0
	mcr	p15, 0, r0, c7, c5, 6
	mcr	p15, 0, r0, c8, c7, 0
	dsb	sy
	isb

	/* Flush and disable PL310 L2 cache controller at 0x3e000000 if active */
	movw	r6, #0x0000
	movt	r6, #0x3e00
	ldr	r0, [r6, #0x100]
	tst	r0, #1
	beq	.L_l2_off
	movw	r0, #0xffff
	str	r0, [r6, #0x7fc]
5:	ldr	r0, [r6, #0x7fc]
	cmp	r0, #0
	bne	5b
	mov	r0, #0
	str	r0, [r6, #0x730]
	str	r0, [r6, #0x100]
	dsb	sy
.L_l2_off:

	/* Emit 'M' on UART0 */
	mov	r0, #'M'
	str	r0, [r10, #0x20]
	dsb	sy

	/* Arm hardware watchdog at 0x3f103020 for 120 seconds (0xac53fc00 ticks) */
	movw	r6, #0x3020
	movt	r6, #0x3f10
	mov	r0, #0
	str	r0, [r6, #0x0c]
	movw	r0, #0xfc00
	movt	r0, #0xac53
	str	r0, [r6, #0x04]
	mov	r0, #0x80000000
	str	r0, [r6, #0x08]
	mov	r0, #0
	str	r0, [r6, #0x00]
	mov	r0, #4
	str	r0, [r6, #0x0c]
	dsb	sy

	/* Relocate inner bundle [r7 .. r7 + r8) to 0x80000000 */
	adr	r7, _payload_start
	ldr	r8, [r7, #-4]
	mov	r9, #0x80000000

	cmp	r7, r9
	beq	.L_jump

6:	subs	r8, r8, #32
	ldmia	r7!, {r0-r5, r11, r12}
	stmia	r9!, {r0-r5, r11, r12}
	bgt	6b

	dsb	sy
	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0
	mcr	p15, 0, r0, c7, c5, 6
	dsb	sy
	isb

.L_jump:
	/* Emit 'J' on UART0 and jump to 0x80000000 */
	mov	r0, #'J'
	str	r0, [r10, #0x20]
	dsb	sy

	mov	pc, #0x80000000

	.balign 4
	.global _payload_size
_payload_size:
	.word	BUNDLE_SIZE
_payload_start:
