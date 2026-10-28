// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8940X / S5L8947X 24 MHz watchdog counter -- CPU frequency calibration,
 * clocksource, sched_clock, delay timer, restart handler, and counter rebase
 * helpers for /dev/watchdog.
 *
 * Register layout at 0x3f103020:
 *   +0x00  CUR_TIME   32-bit counter clocked at 24 MHz; writable
 *   +0x04  BITE_TIME  reset threshold; when CUR_TIME reaches BITE_TIME and
 *                     CTRL bit 2 is set, the SoC resets
 *   +0x08  BARK_TIME  second comparator
 *   +0x0c  CTRL       bit 1 = write-1-clear status, bit 2 = RESET_EN
 */

#define pr_fmt(fmt) "apple-wdt: " fmt

#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/printk.h>
#include <linux/processor.h>
#include <linux/reboot.h>
#include <linux/sched_clock.h>
#include <linux/seqlock.h>
#include <linux/spinlock.h>

#include <asm/delay.h>

#include "s5l8947x.h"

#define WDT_CUR_TIME		0x00
#define WDT_BITE_TIME		0x04
#define WDT_BARK_TIME		0x08
#define WDT_CTRL		0x0c
#define WDT_CTRL_RESET_EN	BIT(2)

static void __iomem *wdt_base;

static DEFINE_RAW_SPINLOCK(wdt_lock);
static seqcount_raw_spinlock_t wdt_seq = SEQCNT_RAW_SPINLOCK_ZERO(wdt_seq, &wdt_lock);
static u32 wdt_offset;

static inline u32 notrace apple_wdt_ticks(void)
{
	unsigned int seq;
	u32 v;

	do {
		seq = raw_read_seqcount_begin(&wdt_seq);
		v = wdt_offset + readl_relaxed(wdt_base + WDT_CUR_TIME);
	} while (read_seqcount_retry(&wdt_seq, seq));

	return v;
}

static u64 notrace apple_wdt_sched_read(void)
{
	return apple_wdt_ticks();
}

static u64 apple_wdt_cs_read(struct clocksource *cs)
{
	return apple_wdt_ticks();
}

static struct clocksource apple_wdt_cs = {
	.name	= "apple-wdt-24m",
	.rating	= 350,
	.read	= apple_wdt_cs_read,
	.mask	= CLOCKSOURCE_MASK(32),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

static unsigned long notrace apple_wdt_delay_read(void)
{
	return (unsigned long)apple_wdt_ticks();
}

static struct delay_timer apple_wdt_delay = {
	.read_current_timer	= apple_wdt_delay_read,
	.freq			= APPLE_S5L_WDT_HZ,
};

bool apple_s5l_wdt_available(void)
{
	return wdt_base != NULL;
}

void apple_s5l_wdt_rebase_and_arm(u32 bite_ticks)
{
	unsigned long flags;
	u32 had;

	raw_spin_lock_irqsave(&wdt_lock, flags);
	write_seqcount_begin(&wdt_seq);
	had = readl_relaxed(wdt_base + WDT_CUR_TIME);
	writel_relaxed(0, wdt_base + WDT_CUR_TIME);
	wdt_offset += had;
	writel_relaxed(bite_ticks, wdt_base + WDT_BITE_TIME);
	write_seqcount_end(&wdt_seq);
	raw_spin_unlock_irqrestore(&wdt_lock, flags);
}

void apple_s5l_wdt_reset_enable(bool on)
{
	writel_relaxed(on ? WDT_CTRL_RESET_EN : 0, wdt_base + WDT_CTRL);
	wmb();
}

static int apple_s5l_wdt_restart(struct notifier_block *nb, unsigned long mode,
				 void *cmd)
{
	writel_relaxed(0, wdt_base + WDT_CTRL);
	writel_relaxed(0, wdt_base + WDT_CUR_TIME);
	writel_relaxed(APPLE_S5L_WDT_HZ / 1000, wdt_base + WDT_BITE_TIME);
	writel_relaxed(WDT_CTRL_RESET_EN, wdt_base + WDT_CTRL);
	wmb();
	while ((u32)readl_relaxed(wdt_base + WDT_CUR_TIME) < APPLE_S5L_WDT_HZ)
		cpu_relax();
	pr_emerg("watchdog bite did not reset the SoC\n");
	return NOTIFY_DONE;
}

static struct notifier_block apple_s5l_wdt_restart_nb = {
	.notifier_call	= apple_s5l_wdt_restart,
	.priority	= 128,
};

void __init apple_s5l_wdt_clocksource_init(void)
{
	if (!wdt_base) {
		pr_err("counter not mapped, wall time stays on PMCCNTR\n");
		return;
	}

	clocksource_register_hz(&apple_wdt_cs, APPLE_S5L_WDT_HZ);
	sched_clock_register(apple_wdt_sched_read, 32, APPLE_S5L_WDT_HZ);
	register_current_timer_delay(&apple_wdt_delay);
	pr_info("24 MHz watchdog counter registered as clocksource, sched_clock, and delay_timer\n");

	if (register_restart_handler(&apple_s5l_wdt_restart_nb))
		pr_err("could not register restart handler\n");
	else
		pr_info("restart handler registered (watchdog bite, 1 ms)\n");
}

unsigned long __init apple_s5l_calibrate_cpu_hz(void)
{
	struct device_node *np;
	u32 w0, w1, c0, c1, wd, cd;
	u64 hz;

	np = of_find_compatible_node(NULL, NULL, "apple,s5l8940x-wdt-timer");
	if (!np)
		np = of_find_compatible_node(NULL, NULL, "apple,s5l8947x-wdt");
	if (!np) {
		pr_err("no wdt node found; CPU rate stays on fallback\n");
		return 0;
	}

	wdt_base = of_iomap(np, 0);
	of_node_put(np);
	if (!wdt_base) {
		pr_err("cannot map wdt counter\n");
		return 0;
	}

	w0 = readl_relaxed(wdt_base + WDT_CUR_TIME);
	c0 = apple_s5l_pmccntr_read();
	while ((u32)(readl_relaxed(wdt_base + WDT_CUR_TIME) - w0) < APPLE_S5L_WDT_HZ / 20)
		cpu_relax();
	w1 = readl_relaxed(wdt_base + WDT_CUR_TIME);
	c1 = apple_s5l_pmccntr_read();

	wd = w1 - w0;
	cd = c1 - c0;
	if (!wd) {
		pr_err("watchdog counter is not running\n");
		return 0;
	}

	hz = mul_u32_u32(cd, APPLE_S5L_WDT_HZ);
	do_div(hz, wd);

	pr_info("calibrated %u CPU cycles per %u ticks of 24 MHz => CPU = %llu Hz\n",
		cd, wd, hz);

	return (unsigned long)hz;
}
