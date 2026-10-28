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
#include <asm/mach/map.h>

#include "s5l8947x.h"

#define S5L_AIC_PHYS	0x3f200000UL
#define S5L_AIC_VIRT	0xf0500000UL

/* PMGR CPU performance-state controller (see drivers/cpufreq/s5l8947x-cpufreq.c) */
#define PMGR_PHYS_BASE		0x3f100000UL
#define PMGR_PLL1_CTL		0x0008
#define PMGR_DOUBLER_CTL	0x0038
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

static struct map_desc s5l8947x_aic_desc __initdata = {
	.virtual	= S5L_AIC_VIRT,
	.pfn		= __phys_to_pfn(S5L_AIC_PHYS),
	.length		= SZ_1M,
	.type		= MT_DEVICE,
};

static void __init s5l8947x_map_io(void)
{
	iotable_init(&s5l8947x_aic_desc, 1);
}

static void __init s5l8947x_init_irq(void)
{
	irqchip_init();
	/* Mainline does not call this for a machine without auxiliary overrides */
	l2x0_of_init(0, ~0U);
}

/*
 * Switch the CPU to the 850 MHz performance state before the PMCCNTR rate
 * is calibrated, in case the boot loader left it in a slower one.
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
		if (readl_relaxed(pmgr + PMGR_PERF_STATE2_W0) != 0x01220001U) {
			writel_relaxed(0x01220001U, pmgr + PMGR_PERF_STATE2_W0);
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
	unsigned long cpu_hz;

	/* A custom init_time replaces the default of_clk_init() call */
	of_clk_init(NULL);

	apple_s5l_pmccntr_enable();
	s5l8947x_pmgr_boost_cpu();
	cpu_hz = apple_s5l_calibrate_cpu_hz();
	apple_s5l_wdt_clocksource_init();
	apple_s5l_pmccntr_init(cpu_hz);
}

static const char * const s5l8947x_dt_compat[] __initconst = {
	"apple,s5l8940x",
	"apple,s5l8947x",
	NULL,
};

DT_MACHINE_START(APPLE_S5L, "S5L8947X (Device Tree)")
	.dt_compat	= s5l8947x_dt_compat,
	.map_io		= s5l8947x_map_io,
	.init_irq	= s5l8947x_init_irq,
	.init_time	= s5l8947x_init_time,
MACHINE_END
