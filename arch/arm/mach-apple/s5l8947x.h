/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ARCH_ARM_MACH_APPLE_S5L8947X_H
#define __ARCH_ARM_MACH_APPLE_S5L8947X_H

#include <linux/bits.h>
#include <linux/compiler.h>
#include <linux/init.h>
#include <linux/types.h>
#include <asm/barrier.h>

/* The watchdog counter ticks at 24 MHz and keeps running in WFI */
#define APPLE_S5L_WDT_HZ	24000000U

/* Shared PMGR CPU performance-state controller registers (0x3f100000) */
#define PMGR_PHYS_BASE		0x3f100000UL
#define PMGR_PLL1_CTL		0x0008
#define PMGR_DOUBLER_CTL	0x0038
#define PMGR_PERF_STATE_TBL(n)	(0x0200 + ((n) * 0x10))
#define PMGR_PERF_STATE2_W0	0x0220
#define PMGR_PERF_STATE2_W1	0x0224
#define PMGR_PERF_STATE2_W2	0x0228
#define PMGR_PERF_STATE2_W3	0x022c
#define PMGR_PERF_STATE_CTL	0x0300

#define PMGR_PLL_ENABLE_LOCKED	0xa0000000U
#define PMGR_DOUBLER_BUSY	BIT(30)
#define PMGR_PERF_STATE_HIGH	2U
#define PMGR_PERF_STATE_BUSY	0x00ff0000U
#define PMGR_TRANSITION_SPIN	200000

static inline u32 notrace apple_s5l_pmccntr_read(void)
{
	u32 cyc;

	asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(cyc));
	return cyc;
}

static inline void apple_s5l_pmccntr_enable(void)
{
	u32 v;

	/* PMUSERENR: allow user + privileged access to PMU cycle counter */
	v = 1u | (1u << 2);
	asm volatile("mcr p15, 0, %0, c9, c14, 0" :: "r"(v));
	v = 0x80000000u;
	asm volatile("mcr p15, 0, %0, c9, c12, 2" :: "r"(v));
	v = (1u << 0) | (1u << 1) | (1u << 2);
	asm volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"(v));
	v = 0x80000000u;
	asm volatile("mcr p15, 0, %0, c9, c12, 1" :: "r"(v));
	v = 1u;
	asm volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"(v));
	isb();
}

void __init apple_s5l_wdt_clocksource_init(void);

bool apple_s5l_wdt_available(void);
void apple_s5l_wdt_rebase_and_arm(u32 bite_ticks);
void apple_s5l_wdt_reset_enable(bool on);

#endif /* __ARCH_ARM_MACH_APPLE_S5L8947X_H */
