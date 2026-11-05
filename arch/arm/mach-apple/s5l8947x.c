// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8940X / S5L8947X machine support
 */

#define pr_fmt(fmt) "s5l8947x: " fmt

#include <linux/clk-provider.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irqchip.h>
#include <linux/kernel.h>
#include <linux/of_platform.h>
#include <linux/sizes.h>

#include <asm/hardware/cache-l2x0.h>
#include <asm/mach/arch.h>

#include "s5l8947x.h"

static void __init s5l8947x_init_irq(void)
{
	irqchip_init();
	/* Mainline does not call this for a machine without auxiliary overrides */
	l2x0_of_init(0, ~0U);
}

/*
 * Switch the CPU to the 850 MHz performance state at early boot, in case the
 * boot loader left it in a slower one.
 */
static void __init s5l8947x_pmgr_boost_cpu(void)
{
	void __iomem *pmgr;
	u32 pll1, dbl, ctl;
	int i;

	pmgr = ioremap(PMGR_PHYS_BASE, SZ_4K);
	if (!pmgr)
		return;

	pll1 = readl_relaxed(pmgr + PMGR_PLL1_CTL);
	if ((pll1 & PMGR_PLL_ENABLE_LOCKED) == PMGR_PLL_ENABLE_LOCKED) {
		/* W0[3:0] = 2 keeps the UART peripheral clock as in the other states */
		if (readl_relaxed(pmgr + PMGR_PERF_STATE2_W0) != 0x01220002U) {
			writel_relaxed(0x01220002U, pmgr + PMGR_PERF_STATE2_W0);
			writel_relaxed(0x00210141U, pmgr + PMGR_PERF_STATE2_W1);
			writel_relaxed(0x00000021U, pmgr + PMGR_PERF_STATE2_W2);
			writel_relaxed(0x00000000U, pmgr + PMGR_PERF_STATE2_W3);
		}

		dbl = readl_relaxed(pmgr + PMGR_DOUBLER_CTL);
		if ((dbl & 0x1fU) != 1U) {
			writel_relaxed((dbl & ~0x1fU) | 1U, pmgr + PMGR_DOUBLER_CTL);
			for (i = 0; i < PMGR_TRANSITION_SPIN; i++) {
				if (!(readl_relaxed(pmgr + PMGR_DOUBLER_CTL) & PMGR_DOUBLER_BUSY))
					break;
			}
		}

		ctl = readl_relaxed(pmgr + PMGR_PERF_STATE_CTL);
		if ((ctl & 0xfU) != PMGR_PERF_STATE_HIGH) {
			writel_relaxed(PMGR_PERF_STATE_HIGH, pmgr + PMGR_PERF_STATE_CTL);
			for (i = 0; i < PMGR_TRANSITION_SPIN; i++) {
				if (!(readl_relaxed(pmgr + PMGR_PERF_STATE_CTL) & PMGR_PERF_STATE_BUSY))
					break;
			}
		}
	}

	pr_info("PMGR CPU performance state %u active (PLL1=0x%08x, DOUBLER=0x%08x)\n",
		readl_relaxed(pmgr + PMGR_PERF_STATE_CTL) & 0xfU,
		pll1, readl_relaxed(pmgr + PMGR_DOUBLER_CTL));
	iounmap(pmgr);
}

static void __init s5l8947x_init_time(void)
{
	/* A custom init_time replaces the default of_clk_init() call */
	of_clk_init(NULL);

	apple_s5l_pmccntr_enable();
	s5l8947x_pmgr_boost_cpu();
	apple_s5l_wdt_clocksource_init();
}

static const char * const s5l8947x_dt_compat[] __initconst = {
	"apple,s5l8940x",
	"apple,s5l8947x",
	NULL,
};

DT_MACHINE_START(APPLE_S5L, "S5L8947X (Device Tree)")
	.dt_compat	= s5l8947x_dt_compat,
	.init_irq	= s5l8947x_init_irq,
	.init_time	= s5l8947x_init_time,
MACHINE_END
