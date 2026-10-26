// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cortex-A9 PMCCNTR cycle counter enablement and delay_timer registration
 * for S5L8940X / S5L8947X.
 *
 * Provides a hardware-backed delay_timer (udelay/mdelay) calibrated against
 * the 24 MHz watchdog counter in wdt-clocksource.c. Note that Cortex-A9
 * stops PMCCNTR during WFI, so timekeeping and sched_clock are provided by
 * the 24 MHz watchdog counter instead.
 */

#include <linux/clocksource.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/timex.h>

#include "s5l8947x.h"

#define S5L_CPU_FREQ_DEFAULT_HZ	800000000UL

static inline u32 read_pmccntr(void)
{
	u32 val;

	asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(val));
	return val;
}

static inline void enable_pmccntr(void)
{
	u32 val;

	/* PMUSERENR = 1 (allow userspace read) */
	val = 1;
	asm volatile("mcr p15, 0, %0, c9, c14, 0" : : "r"(val));

	/* PMCR: E=1 (enable), P=1 (reset event counters), C=1 (reset CCNT), D=0 (/1) */
	asm volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(val));
	val |= (1 << 0) | (1 << 1) | (1 << 2);
	val &= ~(1 << 3);
	asm volatile("mcr p15, 0, %0, c9, c12, 0" : : "r"(val));

	/* PMCNTENSET: enable cycle counter (bit 31) */
	val = (1U << 31);
	asm volatile("mcr p15, 0, %0, c9, c12, 1" : : "r"(val));

	/* PMOVSR: clear overflow flags */
	val = 0xffffffffU;
	asm volatile("mcr p15, 0, %0, c9, c12, 3" : : "r"(val));
	isb();
}

static unsigned long pmccntr_read_delay(void)
{
	return read_pmccntr();
}

static struct delay_timer pmccntr_delay_timer = {
	.read_current_timer	= pmccntr_read_delay,
	.freq			= S5L_CPU_FREQ_DEFAULT_HZ,
};

void __init apple_s5l_pmccntr_set_freq(unsigned long hz)
{
	if (!hz)
		return;
	pmccntr_delay_timer.freq = hz;
	/* Re-run registration so loops_per_jiffy and timer_conversion_shift update */
	register_current_timer_delay(&pmccntr_delay_timer);
}

void __init apple_s5l_pmccntr_init(void)
{
	u32 c0, c1;

	enable_pmccntr();
	c0 = read_pmccntr();
	asm volatile("nop; nop; nop; nop; nop; nop; nop; nop");
	c1 = read_pmccntr();

	apple_s5l_early_puts("[s5l8947x] PMCCNTR delta=");
	apple_s5l_early_puthex(c1 - c0);
	apple_s5l_early_puts("\n");

	if (c1 == c0) {
		apple_s5l_early_puts("[s5l8947x] WARNING: PMCCNTR did not advance\n");
		return;
	}

	register_current_timer_delay(&pmccntr_delay_timer);
	apple_s5l_early_puts("[s5l8947x] PMCCNTR delay_timer registered\n");
}
