/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * S5L8940X / S5L8947X aic,1 interrupt controller
 */

#ifndef __LINUX_IRQCHIP_APPLE_AIC1_H
#define __LINUX_IRQCHIP_APPLE_AIC1_H

#ifdef CONFIG_APPLE_AIC1
/*
 * Mask, retarget and drain every line, then re-enable the controller.
 * Clears a level the boot loader left asserted on nIRQ before init/main.c
 * turns interrupts on.
 */
void apple_aic1_reinit(void);
#else
static inline void apple_aic1_reinit(void) { }
#endif

#endif /* __LINUX_IRQCHIP_APPLE_AIC1_H */
