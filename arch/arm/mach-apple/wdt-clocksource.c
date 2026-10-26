// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X 24 MHz watchdog counter clocksource, sched_clock, CPU frequency
 * calibration, and platform restart handler.
 *
 * Register layout at 0x3f103020 (mapped at 0xfee01020 by s5l8947x_map_io):
 *   +0x00  CUR_TIME   32-bit counter clocked at 24 MHz; writing 0 resets it
 *   +0x04  BITE_TIME  reset threshold; when CUR_TIME >= BITE_TIME and bit 2
 *                     of CTRL is set, the SoC asserts hardware reset
 *   +0x08  BARK_TIME  interrupt threshold
 *   +0x0c  CTRL       bit 0 = bark IRQ enable, bit 1 = bark ack,
 *                     bit 2 (0x4) = system reset enable
 *
 * Unlike Cortex-A9 PMCCNTR (which stops during WFI), CUR_TIME runs off the
 * always-on 24 MHz crystal oscillator. Because feeding the watchdog requires
 * writing 0 to CUR_TIME, we accumulate the elapsed ticks into wdt_offset
 * under a raw spinlock + seqcount before zeroing CUR_TIME, preserving a
 * continuous 64-bit monotonic counter.
 */

#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/sched_clock.h>
#include <linux/seqlock.h>
#include <linux/spinlock.h>

#define S5L_WDT_VIRT_BASE	0xfee01020UL
#define WDT_CUR_TIME		0x00
#define WDT_BITE_TIME		0x04
#define WDT_BARK_TIME		0x08
#define WDT_CTRL		0x0c

#define WDT_CTRL_RESET_EN	(1U << 2)

#define WDT_RATE_HZ		24000000UL
/* 600-second default kernel timeout before userspace /dev/watchdog takes over */
#define WDT_BOOT_BITE_SECONDS	600U
#define WDT_BOOT_BITE_TICKS	(WDT_BOOT_BITE_SECONDS * WDT_RATE_HZ)

extern void apple_s5l_early_puts(const char *s);
extern void apple_s5l_early_puthex(u32 v);
extern void __init apple_s5l_pmccntr_set_freq(unsigned long hz);

static inline void __iomem *wdt_base(void)
{
	return (void __iomem *)S5L_WDT_VIRT_BASE;
}

static DEFINE_RAW_SPINLOCK(wdt_lock);
static seqcount_raw_spinlock_t wdt_seq = SEQCNT_RAW_SPINLOCK_INITIALIZER(wdt_seq, &wdt_lock);
static u64 wdt_offset;
static u32 wdt_bite_ticks = WDT_BOOT_BITE_TICKS;
static bool wdt_userspace_active;

static u64 notrace s5l_wdt_read_ticks(void)
{
	unsigned int seq;
	u64 off;
	u32 cur;

	do {
		seq = raw_read_seqcount_begin(&wdt_seq);
		off = READ_ONCE(wdt_offset);
		cur = readl_relaxed(wdt_base() + WDT_CUR_TIME);
	} while (read_seqcount_retry(&wdt_seq, seq));

	return off + (u64)cur;
}

/*
 * Rebase the watchdog counter: add current CUR_TIME into wdt_offset, reprogram
 * BITE_TIME to new_bite_ticks, and zero CUR_TIME.
 */
static void s5l_wdt_rebase_locked(u32 new_bite_ticks)
{
	void __iomem *b = wdt_base();
	u32 cur;

	write_seqcount_begin(&wdt_seq);
	writel_relaxed(0, b + WDT_CTRL);
	cur = readl_relaxed(b + WDT_CUR_TIME);
	WRITE_ONCE(wdt_offset, wdt_offset + (u64)cur);
	wdt_bite_ticks = new_bite_ticks;
	writel_relaxed(new_bite_ticks, b + WDT_BITE_TIME);
	writel_relaxed(0x80000000U, b + WDT_BARK_TIME);
	writel_relaxed(0, b + WDT_CUR_TIME);
	writel_relaxed(WDT_CTRL_RESET_EN, b + WDT_CTRL);
	dsb(sy);
	write_seqcount_end(&wdt_seq);
}

void apple_s5l_wdt_takeover(u32 timeout_seconds)
{
	unsigned long flags;
	u32 ticks;

	if (timeout_seconds < 5U)
		timeout_seconds = 5U;
	if (timeout_seconds > 170U)
		timeout_seconds = 170U;
	ticks = timeout_seconds * WDT_RATE_HZ;

	raw_spin_lock_irqsave(&wdt_lock, flags);
	wdt_userspace_active = true;
	s5l_wdt_rebase_locked(ticks);
	raw_spin_unlock_irqrestore(&wdt_lock, flags);
}

void apple_s5l_wdt_feed(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&wdt_lock, flags);
	s5l_wdt_rebase_locked(wdt_bite_ticks);
	raw_spin_unlock_irqrestore(&wdt_lock, flags);
}

static u64 notrace s5l_wdt_sched_clock_read(void)
{
	return s5l_wdt_read_ticks();
}

static u64 s5l_wdt_cs_read(struct clocksource *cs)
{
	void __iomem *b = wdt_base();
	u32 cur = readl_relaxed(b + WDT_CUR_TIME);

	/*
	 * Before userspace opens /dev/watchdog, automatically rebase if
	 * CUR_TIME passes half of the boot bite window.
	 */
	if (unlikely(!wdt_userspace_active && cur > (WDT_BOOT_BITE_TICKS / 2U))) {
		unsigned long flags;

		if (raw_spin_trylock_irqsave(&wdt_lock, flags)) {
			if (!wdt_userspace_active &&
			    readl_relaxed(b + WDT_CUR_TIME) > (WDT_BOOT_BITE_TICKS / 2U))
				s5l_wdt_rebase_locked(WDT_BOOT_BITE_TICKS);
			raw_spin_unlock_irqrestore(&wdt_lock, flags);
		}
	}

	return s5l_wdt_read_ticks();
}

static struct clocksource s5l_wdt_clocksource = {
	.name	= "apple-wdt-24m",
	.rating	= 350,
	.read	= s5l_wdt_cs_read,
	.mask	= CLOCKSOURCE_MASK(56),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

/*
 * Restart handler: program BITE_TIME = 24000 (1 ms at 24 MHz), CUR_TIME = 0,
 * CTRL = WDT_CTRL_RESET_EN, and spin until the hardware watchdog resets the SoC.
 */
static int s5l_wdt_restart_notify(struct notifier_block *this,
				  unsigned long mode, void *cmd)
{
	void __iomem *b = wdt_base();

	apple_s5l_early_puts("\n[s5l8947x-wdt] system restart requested\n");
	writel_relaxed(0, b + WDT_CTRL);
	writel_relaxed(24000U, b + WDT_BITE_TIME);
	writel_relaxed(0x80000000U, b + WDT_BARK_TIME);
	writel_relaxed(0, b + WDT_CUR_TIME);
	writel_relaxed(WDT_CTRL_RESET_EN, b + WDT_CTRL);
	dsb(sy);

	for (;;)
		cpu_relax();

	return NOTIFY_DONE;
}

static struct notifier_block s5l_wdt_restart_nb = {
	.notifier_call	= s5l_wdt_restart_notify,
	.priority	= 200,
};

static inline u32 read_pmccntr_local(void)
{
	u32 val;

	asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(val));
	return val;
}

static unsigned long __init s5l_calibrate_cpu_hz(void __iomem *b)
{
	/* Measure PMCCNTR delta across 24,000 ticks (1 ms) of the 24 MHz WDT */
	const u32 window = 24000U;
	u32 w0, c0, c1;

	w0 = readl_relaxed(b + WDT_CUR_TIME);
	c0 = read_pmccntr_local();
	while ((readl_relaxed(b + WDT_CUR_TIME) - w0) < window)
		cpu_relax();
	c1 = read_pmccntr_local();

	return (unsigned long)(c1 - c0) * 1000UL;
}

static int __init s5l_wdt_clocksource_init(void)
{
	void __iomem *b = wdt_base();
	unsigned long cpu_hz;
	u32 t0, t1;
	int i;

	writel_relaxed(0, b + WDT_CTRL);
	writel_relaxed(WDT_BOOT_BITE_TICKS, b + WDT_BITE_TIME);
	writel_relaxed(0x80000000U, b + WDT_BARK_TIME);
	writel_relaxed(0, b + WDT_CUR_TIME);
	writel_relaxed(WDT_CTRL_RESET_EN, b + WDT_CTRL);
	dsb(sy);

	t0 = readl_relaxed(b + WDT_CUR_TIME);
	for (i = 0; i < 1000; i++)
		cpu_relax();
	t1 = readl_relaxed(b + WDT_CUR_TIME);

	apple_s5l_early_puts("[s5l8947x-wdt] 24M counter delta=");
	apple_s5l_early_puthex(t1 - t0);
	apple_s5l_early_puts("\n");

	if (t1 == t0) {
		apple_s5l_early_puts("[s5l8947x-wdt] ERROR: WDT CUR_TIME did not advance\n");
		return -ENODEV;
	}

	cpu_hz = s5l_calibrate_cpu_hz(b);
	apple_s5l_early_puts("[s5l8947x-wdt] calibrated CPU freq Hz=0x");
	apple_s5l_early_puthex((u32)cpu_hz);
	apple_s5l_early_puts("\n");
	if (cpu_hz >= 50000000UL && cpu_hz <= 2000000000UL)
		apple_s5l_pmccntr_set_freq(cpu_hz);

	clocksource_register_hz(&s5l_wdt_clocksource, WDT_RATE_HZ);
	sched_clock_register(s5l_wdt_sched_clock_read, 56, WDT_RATE_HZ);
	register_restart_handler(&s5l_wdt_restart_nb);

	pr_info("s5l8947x-wdt: 24 MHz clocksource + sched_clock + restart handler active (CPU %lu MHz)\n",
		cpu_hz / 1000000UL);
	return 0;
}
core_initcall(s5l_wdt_clocksource_init);
