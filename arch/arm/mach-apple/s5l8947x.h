/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ARCH_ARM_MACH_APPLE_S5L8947X_H
#define __ARCH_ARM_MACH_APPLE_S5L8947X_H

#include <linux/init.h>
#include <linux/types.h>

void apple_s5l_early_putc(char c);
void apple_s5l_early_puts(const char *s);
void apple_s5l_early_puthex(u32 v);
void apple_s5l_wdt_crumb(u32 val);
void apple_s5l_wdt_kick(u32 seconds);

void __init apple_s5l_pmccntr_init(void);
void __init apple_s5l_pmccntr_set_freq(unsigned long hz);

void apple_s5l_wdt_takeover(u32 timeout_seconds);
void apple_s5l_wdt_feed(void);

#endif /* __ARCH_ARM_MACH_APPLE_S5L8947X_H */
