// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8940X / S5L8947X Interrupt Controller (aic,1) and 24 MHz AIC Timer
 *
 * Register map (base 0x3f200000 on S5L8947X):
 *   0x0000  REVISION
 *   0x0004  INFO         [15:0] = nr_irq (192 on S5L8940X/S5L8947X)
 *   0x0010  CONFIG       bit 0 = ENABLE, bits [31:29] = 0xE0000000
 *   0x0020  TIME_LO      24 MHz free-running counter low 32 bits
 *   0x0028  TIME_HI      24 MHz free-running counter high 32 bits
 *   0x2000  WHOAMI       current CPU alias window base
 *   0x2004  EVENT        shared event acknowledge register
 *   0x2008  IPI_SEND     per-CPU bitmask
 *   0x200c  IPI_ACK
 *   0x2010  TMR_CFG      local timer config (bit 0 = enable, init = 0xe)
 *   0x2014  TMR_CNT      32-bit countdown in 24 MHz ticks
 *   0x2018  TMR_STAT     local timer status (write 1 to clear)
 *   0x201c  LOCAL_MASK_SET  bit 1 = local timer mask
 *   0x2020  LOCAL_MASK_CLR  bit 1 = local timer unmask
 *   0x2024  IPI_MASK_SET
 *   0x2028  IPI_MASK_CLR
 *   0x3000  TARGET_CPU   4 bytes * nr_irq (per-IRQ CPU bitmask)
 *   0x4000  SW_SET       4 bytes * ceil(nr_irq / 32)
 *   0x4080  SW_CLR
 *   0x4100  MASK_SET     1 = masked
 *   0x4180  MASK_CLR     write 1 to unmask
 *   0x4200  HW_STATE     read-only line level
 *   0x5000 + cpu*0x80:   explicit per-CPU window (same layout as 0x2000)
 */

#define pr_fmt(fmt) "apple-aic1: " fmt

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clockchips.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/math64.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/processor.h>
#include <linux/slab.h>
#include <linux/smp.h>

#include <asm/exception.h>
#include <asm/ptrace.h>
#ifdef CONFIG_SMP
#include <asm/smp.h>
#endif

#define AIC1_INFO		0x0004
#define AIC1_INFO_NR_IRQ	GENMASK(15, 0)

#define AIC1_CONFIG		0x0010
#define AIC1_CONFIG_ENABLE	BIT(0)
#define AIC1_CONFIG_IMPL	0xE0000000

#define AIC1_WHOAMI		0x2000
#define AIC1_EVENT		0x2004
#define AIC1_IPI_SEND		0x2008
#define AIC1_IPI_ACK		0x200c
#define AIC1_IPI_MASK_SET	0x2024
#define AIC1_IPI_MASK_CLR	0x2028
#define AIC1_IPI_SELF		BIT(31)
#define AIC1_IPI_OTHER		BIT(0)
#define AIC1_IPI_SEND_CPU(cpu)	BIT(cpu)

#define AIC1_TARGET_CPU		0x3000
#define AIC1_SW_SET		0x4000
#define AIC1_SW_CLR		0x4080
#define AIC1_MASK_SET		0x4100
#define AIC1_MASK_CLR		0x4180

#define AIC1_CPU_EVENT(cpu)	(0x5004 + ((cpu) << 7))
#define AIC1_CPU_IPI_SET(cpu)	(0x5008 + ((cpu) << 7))
#define AIC1_CPU_IPI_ACK(cpu)	(0x500c + ((cpu) << 7))
#define AIC1_CPU_IPI_MASK_SET(cpu) (0x5024 + ((cpu) << 7))
#define AIC1_CPU_IPI_MASK_CLR(cpu) (0x5028 + ((cpu) << 7))

#define AIC1_EVENT_TYPE		GENMASK(31, 16)
#define AIC1_EVENT_NUM		GENMASK(15, 0)
#define AIC1_EVENT_TYPE_HW	1
#define AIC1_EVENT_TYPE_IPI	4

#define AIC1_MAX_IRQ		1024
#define AIC1_FALLBACK_NR_IRQ	192
#define AIC1_NR_SWIPI		32
#ifdef CONFIG_SMP
#define AIC1_NR_CPUS		2
#else
#define AIC1_NR_CPUS		1
#endif

/* Built-in 24 MHz timebase and per-CPU countdown timer */
#define AIC1_TIME_LO		0x0020
#define AIC1_TIME_HI		0x0028
#define AIC1_TMR_CFG		0x10
#define AIC1_TMR_CFG_ENABLE	BIT(0)
#define AIC1_TMR_CFG_INIT	0xe
#define AIC1_TMR_CNT		0x14
#define AIC1_TMR_STAT		0x18
#define AIC1_LOCAL_MASK_SET	0x1c
#define AIC1_LOCAL_MASK_CLR	0x20
#define AIC1_LOCAL_TIMER	BIT(1)
#define AIC1_EVENT_TYPE_LOCAL	7
#define AIC1_EVENT_NUM_TIMER	1
#define AIC1_TIMER_HZ		24000000
#define AIC1_ALIAS_WINDOW	0x2000
#define AIC1_CPU_WINDOW(cpu)	(0x5000 + ((cpu) << 7))

struct apple_aic1 {
	void __iomem		*base;
	unsigned int		nr_irq;
	struct irq_domain	*domain;
};

static struct apple_aic1 *apple_aic1;

static void aic1_tmr_event(struct apple_aic1 *aic);

static inline u32 aic1_read(struct apple_aic1 *aic, u32 reg)
{
	return readl_relaxed(aic->base + reg);
}

static inline void aic1_write(struct apple_aic1 *aic, u32 reg, u32 val)
{
	writel_relaxed(val, aic->base + reg);
}

static u32 aic1_cpu_event_off(void)
{
	return AIC1_CPU_EVENT(smp_processor_id());
}

static void aic1_mask_all(struct apple_aic1 *aic)
{
	unsigned int i, nr_words = DIV_ROUND_UP(aic->nr_irq, 32);

	for (i = 0; i < nr_words; i++) {
		aic1_write(aic, AIC1_MASK_SET + i * 4, ~0U);
		aic1_write(aic, AIC1_SW_CLR + i * 4, ~0U);
	}
}

static void aic1_target_all(struct apple_aic1 *aic, u32 dest)
{
	unsigned int i;

	for (i = 0; i < aic->nr_irq; i++)
		aic1_write(aic, AIC1_TARGET_CPU + i * 4, dest);
}

static void aic1_ipi_quiesce(struct apple_aic1 *aic)
{
	unsigned int cpu;

	aic1_write(aic, AIC1_IPI_MASK_SET, ~0U);
	aic1_write(aic, AIC1_IPI_ACK, AIC1_IPI_SELF | AIC1_IPI_OTHER);
	aic1_write(aic, AIC1_IPI_MASK_SET, ~0U);
	for (cpu = 0; cpu < AIC1_NR_CPUS; cpu++) {
		aic1_write(aic, AIC1_CPU_IPI_ACK(cpu),
			   AIC1_IPI_SELF | AIC1_IPI_OTHER);
		aic1_write(aic, AIC1_CPU_IPI_MASK_SET(cpu), ~0U);
	}
}

static u32 aic1_read_event(struct apple_aic1 *aic)
{
	u32 e5 = aic1_read(aic, aic1_cpu_event_off());

	if (e5)
		return e5;
	return aic1_read(aic, AIC1_EVENT);
}

static unsigned int aic1_drain_events(struct apple_aic1 *aic)
{
	unsigned int limit = aic->nr_irq * 2 + 16;
	unsigned int n = 0;
	unsigned int cpu;

	while (n < limit) {
		u32 any = aic1_read(aic, AIC1_EVENT);

		for (cpu = 0; cpu < AIC1_NR_CPUS; cpu++)
			any |= aic1_read(aic, AIC1_CPU_EVENT(cpu));
		if (!any)
			break;
		n++;
	}

	return n;
}

static void aic1_clear_sticky_nirq(struct apple_aic1 *aic)
{
	u32 cfg;

	aic1_mask_all(aic);
	aic1_target_all(aic, 0);
	aic1_ipi_quiesce(aic);
	aic1_drain_events(aic);
	aic1_mask_all(aic);

	cfg = aic1_read(aic, AIC1_CONFIG);
	cfg |= AIC1_CONFIG_IMPL;
	cfg &= ~AIC1_CONFIG_ENABLE;
	aic1_write(aic, AIC1_CONFIG, cfg);
}

static void aic1_enable_hw(struct apple_aic1 *aic)
{
	u32 cfg;

	cfg = aic1_read(aic, AIC1_CONFIG);
	cfg |= AIC1_CONFIG_IMPL;
	aic1_write(aic, AIC1_CONFIG, cfg);
	cfg = aic1_read(aic, AIC1_CONFIG);
	cfg |= AIC1_CONFIG_ENABLE;
	aic1_write(aic, AIC1_CONFIG, cfg);
}

static unsigned int aic1_event_hwirq(struct apple_aic1 *aic, u32 event)
{
	u32 type = FIELD_GET(AIC1_EVENT_TYPE, event);
	u32 num = FIELD_GET(AIC1_EVENT_NUM, event);

	if (!event)
		return aic->nr_irq;

	if (type == AIC1_EVENT_TYPE_HW) {
		if (num < aic->nr_irq)
			return num;
		return aic->nr_irq;
	}

	if (event < aic->nr_irq)
		return event;

	return aic->nr_irq;
}

void apple_aic1_quiesce(void);
void apple_aic1_quiesce(void)
{
	struct apple_aic1 *aic = apple_aic1;

	if (!aic)
		return;

	aic1_clear_sticky_nirq(aic);
}

void apple_aic1_enable(void);
void apple_aic1_enable(void)
{
	struct apple_aic1 *aic = apple_aic1;

	if (!aic)
		return;

	aic1_enable_hw(aic);
}

void apple_aic1_rearm(void);
void apple_aic1_rearm(void)
{
	struct apple_aic1 *aic = apple_aic1;
	unsigned int i, nr_words;

	if (!aic)
		return;

	nr_words = DIV_ROUND_UP(aic->nr_irq, 32);

	/* Keep every HW line masked and free of stale SW triggers. */
	for (i = 0; i < nr_words; i++) {
		aic1_write(aic, AIC1_MASK_SET + i * 4, ~0U);
		aic1_write(aic, AIC1_SW_CLR + i * 4, ~0U);
	}

	/* Route every line at CPU0. */
	for (i = 0; i < aic->nr_irq; i++)
		aic1_write(aic, AIC1_TARGET_CPU + i * 4, BIT(0));

	aic1_drain_events(aic);

	/* Unmask IPIs for CPU0. */
	aic1_write(aic, AIC1_IPI_MASK_CLR, AIC1_IPI_OTHER | AIC1_IPI_SELF);
	aic1_write(aic, AIC1_CPU_IPI_MASK_CLR(0), ~0U);

	aic1_enable_hw(aic);
	dsb(sy);
}

#ifdef CONFIG_SMP
static void aic1_handle_ipi(struct pt_regs *regs)
{
	struct apple_aic1 *aic = apple_aic1;

	if (!aic)
		return;

	aic1_write(aic, AIC1_IPI_ACK, AIC1_IPI_OTHER);
	ipi_mux_process();
	aic1_write(aic, AIC1_IPI_MASK_CLR, AIC1_IPI_OTHER);
}

static void aic1_ipi_send_single(unsigned int cpu)
{
	struct apple_aic1 *aic = apple_aic1;

	if (!aic || cpu >= AIC1_NR_CPUS)
		return;

	aic1_write(aic, AIC1_IPI_SEND, AIC1_IPI_SEND_CPU(cpu));
}

static int __init aic1_init_smp(struct apple_aic1 *aic)
{
	int base_ipi;
	unsigned int cpu;

	base_ipi = ipi_mux_create(AIC1_NR_SWIPI, aic1_ipi_send_single);
	if (WARN_ON(base_ipi <= 0))
		return -ENODEV;

	set_smp_ipi_range(base_ipi, AIC1_NR_SWIPI);

	aic1_write(aic, AIC1_IPI_MASK_CLR, AIC1_IPI_OTHER | AIC1_IPI_SELF);
	for (cpu = 0; cpu < AIC1_NR_CPUS; cpu++)
		aic1_write(aic, AIC1_CPU_IPI_MASK_CLR(cpu), ~0U);

	return 0;
}
#else
static void aic1_handle_ipi(struct pt_regs *regs) { }
static int __init aic1_init_smp(struct apple_aic1 *aic) { return 0; }
#endif

static void aic1_irq_mask(struct irq_data *d)
{
	struct apple_aic1 *aic = irq_data_get_irq_chip_data(d);
	irq_hw_number_t hw = irqd_to_hwirq(d);

	aic1_write(aic, AIC1_MASK_SET + (hw >> 5) * 4, BIT(hw & 31));
}

static void aic1_irq_unmask(struct irq_data *d)
{
	struct apple_aic1 *aic = irq_data_get_irq_chip_data(d);
	irq_hw_number_t hw = irqd_to_hwirq(d);

	aic1_write(aic, AIC1_TARGET_CPU + hw * 4, BIT(0));
	aic1_write(aic, AIC1_MASK_CLR + (hw >> 5) * 4, BIT(hw & 31));
}

static void aic1_irq_eoi(struct irq_data *d)
{
	if (!irqd_irq_masked(d) && !irqd_irq_disabled(d))
		aic1_irq_unmask(d);
}

static struct irq_chip aic1_chip = {
	.name		= "APPLE-AIC1",
	.irq_mask	= aic1_irq_mask,
	.irq_unmask	= aic1_irq_unmask,
	.irq_eoi	= aic1_irq_eoi,
};

extern unsigned long irq_err_count;

static void __exception_irq_entry aic1_handle_irq(struct pt_regs *regs)
{
	struct apple_aic1 *aic = apple_aic1;
	unsigned int limit, n = 0, last_hw = ~0U, same = 0;
	u32 event;

	if (!aic)
		return;

	event = aic1_read_event(aic);
	if (!event) {
		irq_err_count++;
		return;
	}

	limit = aic->nr_irq * 2 + 16;
	do {
		u32 type = FIELD_GET(AIC1_EVENT_TYPE, event);
		u32 num = FIELD_GET(AIC1_EVENT_NUM, event);
		unsigned int hw;

		n++;
		if (type == AIC1_EVENT_TYPE_IPI) {
			aic1_handle_ipi(regs);
			continue;
		}

		if (type == AIC1_EVENT_TYPE_LOCAL) {
			if (num == AIC1_EVENT_NUM_TIMER)
				aic1_tmr_event(aic);
			else
				pr_err_ratelimited("AIC-TIMER: unexpected local event %#x\n",
						   event);
			continue;
		}

		hw = aic1_event_hwirq(aic, event);
		if (hw == last_hw)
			same++;
		last_hw = hw;
		if (hw < aic->nr_irq)
			generic_handle_domain_irq(aic->domain, hw);
		else if (event)
			pr_err_ratelimited("unknown EVENT %#x (type=%u num=%u)\n",
					   event, type, num);
	} while (n < limit && (event = aic1_read_event(aic)));

	if (n >= limit) {
		if (last_hw < aic->nr_irq) {
			aic1_write(aic, AIC1_MASK_SET + (last_hw >> 5) * 4,
				   BIT(last_hw & 31));
			pr_err("aic,1: hwirq %u fired %u times in one entry (%u events) -- masked\n",
			       last_hw, same + 1, n);
		} else {
			pr_err("aic,1: %u events in one entry, last %#x -- nothing masked\n",
			       n, event);
		}
	}
}

static int aic1_irq_domain_map(struct irq_domain *d, unsigned int virq,
			       irq_hw_number_t hw)
{
	struct apple_aic1 *aic = d->host_data;

	irq_domain_set_info(d, virq, hw, &aic1_chip, aic, handle_fasteoi_irq,
			    NULL, NULL);
	irq_set_probe(virq);

	return 0;
}

static const struct irq_domain_ops aic1_irq_domain_ops = {
	.map	= aic1_irq_domain_map,
	.xlate	= irq_domain_xlate_onecell,
};

static int __init aic1_of_init(struct device_node *node,
			       struct device_node *parent)
{
	struct apple_aic1 *aic;
	void __iomem *regs;
	u32 info;

	if (apple_aic1) {
		pr_err("only one aic,1 instance supported\n");
		return -EEXIST;
	}

	regs = of_iomap(node, 0);
	if (!regs) {
		pr_err("failed to map MMIO\n");
		return -ENOMEM;
	}

	aic = kzalloc(sizeof(*aic), GFP_KERNEL);
	if (!aic) {
		iounmap(regs);
		return -ENOMEM;
	}
	aic->base = regs;

	info = aic1_read(aic, AIC1_INFO);
	aic->nr_irq = FIELD_GET(AIC1_INFO_NR_IRQ, info);
	if (!aic->nr_irq || aic->nr_irq > AIC1_MAX_IRQ) {
		pr_warn("INFO %#010x out of range, using %u lines\n",
			info, AIC1_FALLBACK_NR_IRQ);
		aic->nr_irq = AIC1_FALLBACK_NR_IRQ;
	}

	aic1_clear_sticky_nirq(aic);

	aic->domain = irq_domain_add_linear(node, aic->nr_irq,
					    &aic1_irq_domain_ops, aic);
	if (!aic->domain) {
		pr_err("failed to create IRQ domain\n");
		kfree(aic);
		iounmap(regs);
		return -ENOMEM;
	}

	apple_aic1 = aic;
	set_handle_irq(aic1_handle_irq);

	aic1_init_smp(aic);
	apple_aic1_rearm();

	pr_info("aic,1: %u IRQs, WHOAMI=%u, CONFIG=%#x\n",
		aic->nr_irq, aic1_read(aic, AIC1_WHOAMI),
		aic1_read(aic, AIC1_CONFIG));

	return 0;
}

/* ------------------------------------------------------------ AIC timer -- */

static u32 aic1_tmr_win = AIC1_ALIAS_WINDOW;
static unsigned int aic1_tmr_events;
static u64 aic1_tmr_last;
static bool aic1_tmr_registered;
static struct clock_event_device aic1_clkevt;

static inline u32 aic1_tmr_read(struct apple_aic1 *aic, u32 reg)
{
	return aic1_read(aic, aic1_tmr_win + reg);
}

static inline void aic1_tmr_write(struct apple_aic1 *aic, u32 reg, u32 val)
{
	aic1_write(aic, aic1_tmr_win + reg, val);
}

static u64 aic1_time(struct apple_aic1 *aic)
{
	u32 hi, lo, hi2;

	do {
		hi = aic1_read(aic, AIC1_TIME_HI);
		lo = aic1_read(aic, AIC1_TIME_LO);
		hi2 = aic1_read(aic, AIC1_TIME_HI);
	} while (hi != hi2);

	return ((u64)hi << 32) | lo;
}

static void aic1_tmr_disarm(struct apple_aic1 *aic)
{
	aic1_tmr_write(aic, AIC1_LOCAL_MASK_SET, AIC1_LOCAL_TIMER);
	aic1_tmr_write(aic, AIC1_TMR_CFG,
		       aic1_tmr_read(aic, AIC1_TMR_CFG) & ~AIC1_TMR_CFG_ENABLE);
	aic1_tmr_write(aic, AIC1_TMR_STAT, 1);
}

static void aic1_tmr_arm(struct apple_aic1 *aic, u32 delta)
{
	/* Park the countdown, enable, clear status, load delta, and unmask. */
	aic1_tmr_write(aic, AIC1_TMR_CNT, ~0U);
	aic1_tmr_write(aic, AIC1_TMR_CFG,
		       aic1_tmr_read(aic, AIC1_TMR_CFG) | AIC1_TMR_CFG_ENABLE);
	aic1_tmr_write(aic, AIC1_TMR_STAT, 1);
	aic1_tmr_write(aic, AIC1_TMR_CNT, delta);
	aic1_tmr_write(aic, AIC1_LOCAL_MASK_CLR, AIC1_LOCAL_TIMER);
}

static void aic1_tmr_event(struct apple_aic1 *aic)
{
	aic1_tmr_events++;
	aic1_tmr_last = aic1_time(aic);
	aic1_tmr_disarm(aic);

	if (aic1_tmr_registered && aic1_clkevt.event_handler)
		aic1_clkevt.event_handler(&aic1_clkevt);
}

static int aic1_ce_set_next_event(unsigned long delta,
				  struct clock_event_device *ce)
{
	aic1_tmr_arm(apple_aic1, delta);
	return 0;
}

static int aic1_ce_shutdown(struct clock_event_device *ce)
{
	aic1_tmr_disarm(apple_aic1);
	return 0;
}

static struct clock_event_device aic1_clkevt = {
	.name			= "apple-aic1-timer",
	.features		= CLOCK_EVT_FEAT_ONESHOT,
	.rating			= 400,
	.set_next_event		= aic1_ce_set_next_event,
	.set_state_shutdown	= aic1_ce_shutdown,
	.set_state_oneshot	= aic1_ce_shutdown,
	.tick_resume		= aic1_ce_shutdown,
};

static bool __init aic1_tmr_try(struct apple_aic1 *aic, u32 win,
				const char *what)
{
	unsigned int before = READ_ONCE(aic1_tmr_events);
	u32 cfg0, cnt_a, cnt_b;
	u64 t0, t;

	aic1_tmr_win = win;
	cfg0 = aic1_tmr_read(aic, AIC1_TMR_CFG);
	aic1_tmr_write(aic, AIC1_TMR_CFG, AIC1_TMR_CFG_INIT);

	t0 = aic1_time(aic);
	aic1_tmr_arm(aic, AIC1_TIMER_HZ / 100);
	cnt_a = aic1_tmr_read(aic, AIC1_TMR_CNT);
	do {
		if (READ_ONCE(aic1_tmr_events) != before)
			break;
		cpu_relax();
		t = aic1_time(aic);
	} while (t - t0 < AIC1_TIMER_HZ / 5);
	cnt_b = aic1_tmr_read(aic, AIC1_TMR_CNT);

	if (READ_ONCE(aic1_tmr_events) != before) {
		pr_info("AIC-TIMER: %s window %#x: 10 ms shot fired after %llu us (CFG was %#x)\n",
			what, win,
			div_u64((aic1_tmr_last - t0) * 1000000ULL, AIC1_TIMER_HZ),
			cfg0);
		return true;
	}

	aic1_tmr_disarm(aic);
	aic1_tmr_write(aic, AIC1_TMR_CFG, cfg0);
	pr_warn("AIC-TIMER: %s window %#x: nothing in 200 ms (CFG=%#x CNT %#x->%#x STAT=%#x)\n",
		what, win, cfg0, cnt_a, cnt_b, aic1_tmr_read(aic, AIC1_TMR_STAT));
	return false;
}

static int __init aic1_timer_init(void)
{
	struct apple_aic1 *aic = apple_aic1;

	if (!aic)
		return 0;

	if (!aic1_tmr_try(aic, AIC1_ALIAS_WINDOW, "alias") &&
	    !aic1_tmr_try(aic, AIC1_CPU_WINDOW(0), "cpu0")) {
		aic1_tmr_win = AIC1_ALIAS_WINDOW;
		pr_err("AIC-TIMER: no timer event in either window\n");
		return 0;
	}

	aic1_tmr_registered = true;
	aic1_clkevt.cpumask = cpumask_of(0);
	clockevents_config_and_register(&aic1_clkevt, AIC1_TIMER_HZ,
					0xf, 0x7fffffff);
	pr_info("AIC-TIMER: clockevent registered at 24 MHz, rating %d\n",
		aic1_clkevt.rating);
	return 0;
}
arch_initcall(aic1_timer_init);

IRQCHIP_DECLARE(apple_aic1, "aic,1", aic1_of_init);
IRQCHIP_DECLARE(apple_aic1_vendor, "apple,aic1", aic1_of_init);
IRQCHIP_DECLARE(apple_aic1_compat, "apple,aic", aic1_of_init);
IRQCHIP_DECLARE(apple_s5l8940x_aic, "apple,s5l8940x-aic", aic1_of_init);
IRQCHIP_DECLARE(apple_s5l8947x_aic, "apple,s5l8947x-aic", aic1_of_init);
