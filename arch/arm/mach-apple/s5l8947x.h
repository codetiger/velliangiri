/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ARCH_ARM_MACH_APPLE_S5L8947X_H
#define __ARCH_ARM_MACH_APPLE_S5L8947X_H

#include <linux/init.h>
#include <linux/types.h>

void __init apple_s5l_pmccntr_enable_counter(void);
void __init apple_s5l_pmccntr_init(unsigned long rate);
unsigned long __init apple_s5l_calibrate_cpu_hz(void);
void __init apple_s5l_wdt_clocksource_init(void);

bool apple_s5l_wdt_available(void);
void apple_s5l_wdt_rebase_and_arm(u32 bite_ticks);
void apple_s5l_wdt_reset_enable(bool on);

#endif /* __ARCH_ARM_MACH_APPLE_S5L8947X_H */
