// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8940X / S5L8947X PMGR CPU Frequency Scaling (cpufreq) Driver
 *
 * Drives the hardware PMGR performance-state controller at 0x3f100000:
 *   - 0x0008  PLL1_CTL        425 MHz base CPU PLL (P=12, M=425, S=1)
 *   - 0x0038  DOUBLER_CTL     CPU clock doubler / pre-divider ([4:0] = 1, 2, 4;
 *                             bit 30 = transition busy)
 *   - 0x0200  PERF_STATE_TBL  16-byte entries indexed by performance state:
 *               State 2 (0x0220): 850.0 MHz (PLL1 x 2 / 1, doubler_div = 1)
 *               State 3 (0x0230): 425.0 MHz (PLL1 / 1,     doubler_div = 2)
 *               State 6 (0x0260): 212.5 MHz (PLL1 / 2,     doubler_div = 4)
 *   - 0x0300  PERF_STATE_CTL  [3:0] = active performance state index,
 *                             [23:16] = transition busy mask (0x00ff0000)
 */

#define pr_fmt(fmt) "s5l8947x-cpufreq: " fmt

#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>

#define PMGR_PHYS_BASE		0x3f100000UL
#define PMGR_PLL1_CTL		0x0008
#define PMGR_DOUBLER_CTL	0x0038
#define PMGR_PERF_STATE_TBL(n)	(0x0200 + ((n) * 0x10))
#define PMGR_PERF_STATE_CTL	0x0300

#define PMGR_PLL_ENABLE_LOCKED	0xa0000000U
#define PMGR_DOUBLER_BUSY	BIT(30)
#define PMGR_PERF_STATE_BUSY	0x00ff0000U
#define PMGR_TRANSITION_SPIN	200000
#define S5L8947X_TRANSITION_NS	10000

struct s5l8947x_opp {
	unsigned int freq_khz;
	u32 perf_state;
	u32 doubler_div;
	u32 tbl[4];
};

/*
 * States 6, 3, and 2 share identical SDRAM/display/peripheral bus divisors
 * (W1 = 0x00210141), scaling only the Cortex-A9 core clock so 720p60 HDMI
 * scanout, USB 2.0 OTG DMA, and Fast Ethernet remain glitch-free.
 *
 * W0[3:0] divides the peripheral clock that feeds the UARTs. It must stay
 * at 2 in every state: with 1 in the 850 MHz state the UARTs ran at twice
 * their programmed baud rate whenever the CPU was boosted.
 */
static const struct s5l8947x_opp s5l8947x_opps[] = {
	{
		.freq_khz	= 212500,
		.perf_state	= 6,
		.doubler_div	= 4,
		.tbl		= { 0x04220002U, 0x00210141U, 0x00000022U, 0x00000000U },
	},
	{
		.freq_khz	= 425000,
		.perf_state	= 3,
		.doubler_div	= 2,
		.tbl		= { 0x02220002U, 0x00210141U, 0x00000022U, 0x00000000U },
	},
	{
		.freq_khz	= 850000,
		.perf_state	= 2,
		.doubler_div	= 1,
		.tbl		= { 0x01220002U, 0x00210141U, 0x00000021U, 0x00000000U },
	},
};

static struct cpufreq_frequency_table s5l8947x_freq_table[] = {
	{ .flags = 0, .driver_data = 0, .frequency = 212500 },
	{ .flags = 0, .driver_data = 1, .frequency = 425000 },
	{ .flags = 0, .driver_data = 2, .frequency = 850000 },
	{ .flags = 0, .driver_data = 0, .frequency = CPUFREQ_TABLE_END },
};

static void __iomem *pmgr_base;
static DEFINE_RAW_SPINLOCK(pmgr_perf_lock);

static void s5l8947x_pmgr_ensure_opp_tables(void)
{
	unsigned int i, w;
	u32 off;

	for (i = 0; i < ARRAY_SIZE(s5l8947x_opps); i++) {
		off = PMGR_PERF_STATE_TBL(s5l8947x_opps[i].perf_state);
		if (readl_relaxed(pmgr_base + off) != s5l8947x_opps[i].tbl[0]) {
			for (w = 0; w < 4; w++)
				writel_relaxed(s5l8947x_opps[i].tbl[w],
					       pmgr_base + off + w * 4);
		}
	}
}

static unsigned int s5l8947x_cpufreq_get(unsigned int cpu)
{
	u32 state;
	unsigned int i;

	if (!pmgr_base)
		return 850000;

	state = readl_relaxed(pmgr_base + PMGR_PERF_STATE_CTL) & 0xfU;
	for (i = 0; i < ARRAY_SIZE(s5l8947x_opps); i++) {
		if (s5l8947x_opps[i].perf_state == state)
			return s5l8947x_opps[i].freq_khz;
	}

	/* State 4 (deep low-power) also runs the CPU at 212.5 MHz */
	if (state == 4)
		return 212500;

	return 850000;
}

static int s5l8947x_cpufreq_target_index(struct cpufreq_policy *policy,
					 unsigned int index)
{
	const struct s5l8947x_opp *opp;
	unsigned long flags;
	u32 dbl, ctl;
	int i;

	if (index >= ARRAY_SIZE(s5l8947x_opps))
		return -EINVAL;

	opp = &s5l8947x_opps[index];

	raw_spin_lock_irqsave(&pmgr_perf_lock, flags);

	dbl = readl_relaxed(pmgr_base + PMGR_DOUBLER_CTL);
	if ((dbl & 0x1fU) != opp->doubler_div) {
		writel_relaxed((dbl & ~0x1fU) | opp->doubler_div,
			       pmgr_base + PMGR_DOUBLER_CTL);
		for (i = 0; i < PMGR_TRANSITION_SPIN; i++) {
			if (!(readl_relaxed(pmgr_base + PMGR_DOUBLER_CTL) &
			      PMGR_DOUBLER_BUSY))
				break;
		}
	}

	ctl = readl_relaxed(pmgr_base + PMGR_PERF_STATE_CTL);
	if ((ctl & 0xfU) != opp->perf_state) {
		writel_relaxed(opp->perf_state, pmgr_base + PMGR_PERF_STATE_CTL);
		for (i = 0; i < PMGR_TRANSITION_SPIN; i++) {
			if (!(readl_relaxed(pmgr_base + PMGR_PERF_STATE_CTL) &
			      PMGR_PERF_STATE_BUSY))
				break;
		}
	}

	/*
	 * Keep loops_per_jiffy synchronized with the active CPU clock so
	 * /proc/cpuinfo BogoMIPS reflects the current operating point, while
	 * udelay() uses the constant 24 MHz hardware delay_timer.
	 */
	loops_per_jiffy = (opp->freq_khz * 1000UL) / HZ;

	raw_spin_unlock_irqrestore(&pmgr_perf_lock, flags);
	return 0;
}

static int s5l8947x_cpufreq_init(struct cpufreq_policy *policy)
{
	cpufreq_generic_init(policy, s5l8947x_freq_table, S5L8947X_TRANSITION_NS);
	policy->cur = s5l8947x_cpufreq_get(policy->cpu);
	loops_per_jiffy = (policy->cur * 1000UL) / HZ;
	return 0;
}

static struct cpufreq_driver s5l8947x_cpufreq_driver = {
	.name		= "s5l8947x-cpufreq",
	.flags		= CPUFREQ_NEED_INITIAL_FREQ_CHECK,
	.verify		= cpufreq_generic_frequency_table_verify,
	.target_index	= s5l8947x_cpufreq_target_index,
	.get		= s5l8947x_cpufreq_get,
	.init		= s5l8947x_cpufreq_init,
	.attr		= cpufreq_generic_attr,
};

static int __init s5l8947x_cpufreq_driver_init(void)
{
	u32 pll1;
	int ret;

	if (!of_machine_is_compatible("apple,s5l8947x") &&
	    !of_machine_is_compatible("apple,s5l8940x"))
		return -ENODEV;

	pmgr_base = ioremap(PMGR_PHYS_BASE, SZ_4K);
	if (!pmgr_base)
		return -ENOMEM;

	pll1 = readl_relaxed(pmgr_base + PMGR_PLL1_CTL);
	if ((pll1 & PMGR_PLL_ENABLE_LOCKED) != PMGR_PLL_ENABLE_LOCKED) {
		pr_err("PLL1 (0x%08x) not locked; aborting cpufreq registration\n",
		       pll1);
		iounmap(pmgr_base);
		pmgr_base = NULL;
		return -ENODEV;
	}

	s5l8947x_pmgr_ensure_opp_tables();

	ret = cpufreq_register_driver(&s5l8947x_cpufreq_driver);
	if (ret) {
		pr_err("failed to register cpufreq driver: %d\n", ret);
		iounmap(pmgr_base);
		pmgr_base = NULL;
		return ret;
	}

	pr_info("registered PMGR cpufreq driver (212.5 / 425.0 / 850.0 MHz, cur=%u kHz)\n",
		s5l8947x_cpufreq_get(0));
	return 0;
}
late_initcall(s5l8947x_cpufreq_driver_init);
