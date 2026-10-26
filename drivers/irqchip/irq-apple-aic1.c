// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8940X / S5L8947X Interrupt Controller (aic,1) and 24 MHz AIC Timer
 *
 * Register map (base 0x3f200000 on S5L8947X):
 *   0x0000  REVISION
 *   0x0004  INFO         [9:0] = nr_irq (192 on S5L8940X/S5L8947X)
 *   0x0010  CONFIG       bit 0 = ENABLE, bit 31 = IMPL
 *   0x0020  TIME_LO      24 MHz free-running counter low 32 bits
 *   0x0028  TIME_HI      24 MHz free-running counter high 32 bits
 *   0x2000  WHOAMI       current CPU index
 *   0x2004  EVENT        shared event acknowledge register
 *   0x2008  IPI_SEND     per-CPU bitmask
 *   0x200c  IPI_ACK
 *   0x2024  IPI_MASK_SET
 *   0x2028  IPI_MASK_CLR
 *   0x3000  TARGET_CPU   4 bytes * nr_irq (per-IRQ CPU bitmask)
 *   0x4000  SW_SET       4 bytes * ceil(nr_irq / 32)
 *   0x4080  SW_CLR
 *   0x4100  MASK_SET     1 = masked
 *   0x4180  MASK_CLR     write 1 to unmask
 *   0x4200  HW_STATE     read-only line level
 *   0x5000 + cpu*0x80:
 *     +0x04 CPU_EVENT    per-CPU event acknowledge register
 *     +0x08 CPU_IPI_SEND
 *     +0x0c CPU_IPI_ACK
 *     +0x10 TMR_LO       64-bit compare low word (24 MHz timebase)
 *     +0x14 TMR_HI       64-bit compare high word
 *     +0x18 TMR_CTRL     bit 0 = enable
 *     +0x20 TMR_STAT     bit 0 = pending (write 1 to clear)
 *     +0x24 CPU_IPI_MASK_SET
 *     +0x28 CPU_IPI_MASK_CLR
 */

#define pr_fmt(fmt) "aic1: " fmt

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
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/smp.h>

#include <asm/exception.h>

#define AIC1_REVISION		0x0000
#define AIC1_INFO		0x0004
#define   AIC1_INFO_NR_IRQ	GENMASK(9, 0)
#define AIC1_CONFIG		0x0010
#define   AIC1_CONFIG_ENABLE	BIT(0)
#define   AIC1_CONFIG_PREFER_P0	BIT(28)
#define   AIC1_CONFIG_IMPL	BIT(31)
#define AIC1_TIME_LO		0x0020
#define AIC1_TIME_HI		0x0028
#define AIC1_WHOAMI		0x2000
#define AIC1_EVENT		0x2004
#define   AIC1_EVENT_NUM	GENMASK(15, 0)
#define   AIC1_EVENT_TYPE	GENMASK(31, 16)
#define   AIC1_EVENT_TYPE_NONE	0
#define   AIC1_EVENT_TYPE_HW	1
#define   AIC1_EVENT_TYPE_IPI	4
#define   AIC1_EVENT_TYPE_LOCAL	7
#define   AIC1_EVENT_IPI_OTHER	1
#define   AIC1_EVENT_IPI_SELF	2
#define   AIC1_EVENT_LOCAL_TMR	1
#define AIC1_IPI_SEND		0x2008
#define AIC1_IPI_ACK		0x200c
#define AIC1_IPI_MASK_SET	0x2024
#define AIC1_IPI_MASK_CLR	0x2028
#define   AIC1_IPI_OTHER	BIT(0)
#define   AIC1_IPI_SELF		BIT(31)

#define AIC1_CPU_REGION		0x5000
#define AIC1_CPU_STRIDE		0x80
#define AIC1_CPU_WHOAMI(c)	(AIC1_CPU_REGION + (c) * AIC1_CPU_STRIDE + 0x00)
#define AIC1_CPU_EVENT(c)	(AIC1_CPU_REGION + (c) * AIC1_CPU_STRIDE + 0x04)
#define AIC1_CPU_IPI_SEND(c)	(AIC1_CPU_REGION + (c) * AIC1_CPU_STRIDE + 0x08)
#define AIC1_CPU_IPI_ACK(c)	(AIC1_CPU_REGION + (c) * AIC1_CPU_STRIDE + 0x0c)
#define AIC1_CPU_IPI_MASK_SET(c) (AIC1_CPU_REGION + (c) * AIC1_CPU_STRIDE + 0x24)
#define AIC1_CPU_IPI_MASK_CLR(c) (AIC1_CPU_REGION + (c) * AIC1_CPU_STRIDE + 0x28)

/* Per-CPU local 24 MHz timer registers in CPU0 bank */
#define AIC1_CPU0_TMR_LO	0x5010
#define AIC1_CPU0_TMR_HI	0x5014
#define AIC1_CPU0_TMR_CTRL	0x5018
#define AIC1_CPU0_TMR_STAT	0x5020

#define AIC1_TARGET_CPU		0x3000
#define AIC1_SW_SET		0x4000
#define AIC1_SW_CLR		0x4080
#define AIC1_MASK_SET		0x4100
#define AIC1_MASK_CLR		0x4180
#define AIC1_HW_STATE		0x4200

#define AIC1_MAX_IRQ		0x400
#define AIC1_FALLBACK_NR_IRQ	192
#define AIC1_NR_CPUS		2

#define AIC1_TMR_RATE		24000000UL
#define AIC1_TMR_PERIOD		(AIC1_TMR_RATE / HZ)

struct apple_aic1 {
	void __iomem *base;
	struct irq_domain *domain;
	unsigned int nr_irq;
};

static struct apple_aic1 *apple_aic1 __read_mostly;
static void (*aic1_local_tmr_handler)(void) __read_mostly;

static inline u32 aic1_read(struct apple_aic1 *aic, u32 reg)
{
	return readl_relaxed(aic->base + reg);
}

static inline void aic1_write(struct apple_aic1 *aic, u32 reg, u32 val)
{
	writel_relaxed(val, aic->base + reg);
}

static unsigned int aic1_cpu_event_off(void)
{
	unsigned int cpu = 0;

#ifdef CONFIG_SMP
	cpu = raw_smp_processor_id();
	if (cpu >= AIC1_NR_CPUS)
		cpu = 0;
#endif
	return AIC1_CPU_EVENT(cpu);
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

void apple_aic1_rearm(void)
{
	struct apple_aic1 *aic = apple_aic1;
	unsigned int i, nr_words;

	if (!aic)
		return;

	nr_words = DIV_ROUND_UP(aic->nr_irq, 32);

	/* Keep all hardware lines masked until drivers explicitly request them */
	for (i = 0; i < nr_words; i++) {
		aic1_write(aic, AIC1_MASK_SET + i * 4, ~0U);
		aic1_write(aic, AIC1_SW_CLR + i * 4, ~0U);
	}

	/* Route all hardware lines to CPU0 so unmasking immediately delivers */
	aic1_target_all(aic, BIT(0));

	/* Unmask IPIs and clear any stale IPI state */
	aic1_write(aic, AIC1_IPI_ACK, AIC1_IPI_SELF | AIC1_IPI_OTHER);
	aic1_write(aic, AIC1_IPI_MASK_CLR, AIC1_IPI_SELF | AIC1_IPI_OTHER);
	for (i = 0; i < AIC1_NR_CPUS; i++) {
		aic1_write(aic, AIC1_CPU_IPI_ACK(i),
			   AIC1_IPI_SELF | AIC1_IPI_OTHER);
		aic1_write(aic, AIC1_CPU_IPI_MASK_CLR(i),
			   AIC1_IPI_SELF | AIC1_IPI_OTHER);
	}

	aic1_drain_events(aic);
	aic1_enable_hw(aic);
	dsb(sy);
}

static void aic1_irq_mask(struct irq_data *d)
{
	struct apple_aic1 *aic = irq_data_get_irq_chip_data(d);
	irq_hw_number_t hw = irqd_to_hwirq(d);

	aic1_write(aic, AIC1_MASK_SET + (hw / 32) * 4, BIT(hw % 32));
}

static void aic1_irq_unmask(struct irq_data *d)
{
	struct apple_aic1 *aic = irq_data_get_irq_chip_data(d);
	irq_hw_number_t hw = irqd_to_hwirq(d);

	aic1_write(aic, AIC1_TARGET_CPU + hw * 4, BIT(0));
	aic1_write(aic, AIC1_MASK_CLR + (hw / 32) * 4, BIT(hw % 32));
	dsb(sy);
}

static void aic1_irq_eoi(struct irq_data *d)
{
	/*
	 * Reading EVENT in aic1_handle_irq() acknowledges the interrupt and
	 * automatically masks the hardware line. For active unmasked lines,
	 * re-unmask on EOI.
	 */
	if (!irqd_irq_masked(d))
		aic1_irq_unmask(d);
}

static int aic1_irq_set_type(struct irq_data *d, unsigned int flow_type)
{
	switch (flow_type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_NONE:
	case IRQ_TYPE_LEVEL_HIGH:
	case IRQ_TYPE_EDGE_RISING:
		return 0;
	default:
		return -EINVAL;
	}
}

#ifdef CONFIG_SMP
static int aic1_irq_set_affinity(struct irq_data *d,
				 const struct cpumask *mask_val, bool force)
{
	struct apple_aic1 *aic = irq_data_get_irq_chip_data(d);
	irq_hw_number_t hw = irqd_to_hwirq(d);
	unsigned int cpu;

	if (force)
		cpu = cpumask_first(mask_val);
	else
		cpu = cpumask_any_and(mask_val, cpu_online_mask);

	if (cpu >= nr_cpu_ids || cpu >= AIC1_NR_CPUS)
		return -EINVAL;

	aic1_write(aic, AIC1_TARGET_CPU + hw * 4, BIT(cpu));
	irq_data_update_effective_affinity(d, cpumask_of(cpu));

	return IRQ_SET_MASK_OK_NOCOPY;
}
#endif

static struct irq_chip aic1_chip = {
	.name			= "aic,1",
	.irq_mask		= aic1_irq_mask,
	.irq_unmask		= aic1_irq_unmask,
	.irq_eoi		= aic1_irq_eoi,
	.irq_set_type		= aic1_irq_set_type,
#ifdef CONFIG_SMP
	.irq_set_affinity	= aic1_irq_set_affinity,
#endif
};

#ifdef CONFIG_SMP
static int aic1_ipi_virq_base __ro_after_init;

static void aic1_ipi_send_mask(struct irq_data *d, const struct cpumask *mask)
{
	struct apple_aic1 *aic = apple_aic1;
	unsigned int cpu = raw_smp_processor_id();
	u32 send = cpumask_bits(mask)[0] & GENMASK(AIC1_NR_CPUS - 1, 0);

	if (!send)
		return;

	dsb(ishst);
	aic1_write(aic, AIC1_IPI_SEND, send);
	if (cpu < AIC1_NR_CPUS)
		aic1_write(aic, AIC1_CPU_IPI_SEND(cpu), send);
	dsb(sy);
}

static void aic1_handle_ipi(struct apple_aic1 *aic, u32 num)
{
	unsigned int cpu = raw_smp_processor_id();
	u32 ack;

	if (num == AIC1_EVENT_IPI_SELF)
		ack = AIC1_IPI_SELF;
	else
		ack = AIC1_IPI_OTHER;

	aic1_write(aic, AIC1_IPI_ACK, ack);
	if (cpu < AIC1_NR_CPUS)
		aic1_write(aic, AIC1_CPU_IPI_ACK(cpu), ack);

	ipi_mux_process();

	aic1_write(aic, AIC1_IPI_MASK_CLR, ack);
	if (cpu < AIC1_NR_CPUS)
		aic1_write(aic, AIC1_CPU_IPI_MASK_CLR(cpu), ack);
}

static int __init aic1_init_smp(struct apple_aic1 *aic)
{
	int virq;
	unsigned int cpu;

	aic1_write(aic, AIC1_IPI_ACK, AIC1_IPI_SELF | AIC1_IPI_OTHER);
	aic1_write(aic, AIC1_IPI_MASK_CLR, AIC1_IPI_SELF | AIC1_IPI_OTHER);
	for (cpu = 0; cpu < AIC1_NR_CPUS; cpu++) {
		aic1_write(aic, AIC1_CPU_IPI_ACK(cpu),
			   AIC1_IPI_SELF | AIC1_IPI_OTHER);
		aic1_write(aic, AIC1_CPU_IPI_MASK_CLR(cpu),
			   AIC1_IPI_SELF | AIC1_IPI_OTHER);
	}

	virq = ipi_mux_create(AIC1_NR_CPUS, aic1_ipi_send_mask);
	if (virq <= 0) {
		pr_err("failed to create IPI mux: %d\n", virq);
		return virq ? virq : -ENOMEM;
	}

	aic1_ipi_virq_base = virq;
	set_smp_ipi_range(virq, nr_ipi_get());
	return 0;
}
#else
static inline void aic1_handle_ipi(struct apple_aic1 *aic, u32 num) { }
static inline int aic1_init_smp(struct apple_aic1 *aic) { return 0; }
#endif

static void __exception_irq_entry aic1_handle_irq(struct pt_regs *regs)
{
	struct apple_aic1 *aic = apple_aic1;
	unsigned int limit = aic->nr_irq + 16;
	unsigned int n = 0;
	u32 event = 0;

	while (n < limit) {
		u32 type, num;
		unsigned int hwirq;

		event = aic1_read_event(aic);
		if (!event)
			break;
		n++;

		type = FIELD_GET(AIC1_EVENT_TYPE, event);
		num = FIELD_GET(AIC1_EVENT_NUM, event);

		if (type == AIC1_EVENT_TYPE_IPI) {
			aic1_handle_ipi(aic, num);
			continue;
		}

		if (type == AIC1_EVENT_TYPE_LOCAL &&
		    num == AIC1_EVENT_LOCAL_TMR &&
		    aic1_local_tmr_handler) {
			aic1_local_tmr_handler();
			continue;
		}

		hwirq = aic1_event_hwirq(aic, event);
		if (hwirq < aic->nr_irq) {
			if (generic_handle_domain_irq(aic->domain, hwirq)) {
				aic1_write(aic,
					   AIC1_MASK_SET + (hwirq / 32) * 4,
					   BIT(hwirq % 32));
				aic1_write(aic,
					   AIC1_SW_CLR + (hwirq / 32) * 4,
					   BIT(hwirq % 32));
			}
			continue;
		}

		aic1_ipi_quiesce(aic);
		break;
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

	pr_info("initialized (%u IRQs, WHOAMI=%u, CONFIG=%#x)\n",
		aic->nr_irq, aic1_read(aic, AIC1_WHOAMI),
		aic1_read(aic, AIC1_CONFIG));

	return 0;
}

/*
 * AIC1 built-in 24 MHz local timer (clock_event_device)
 */
static u64 aic1_tmr_read(struct apple_aic1 *aic)
{
	u32 hi, lo, hi2;

	do {
		hi = aic1_read(aic, AIC1_TIME_HI);
		lo = aic1_read(aic, AIC1_TIME_LO);
		hi2 = aic1_read(aic, AIC1_TIME_HI);
	} while (hi != hi2);

	return ((u64)hi << 32) | lo;
}

static void aic1_tmr_arm(struct apple_aic1 *aic, unsigned long delta)
{
	u64 exp = aic1_tmr_read(aic) + delta;

	aic1_write(aic, AIC1_CPU0_TMR_CTRL, 0);
	aic1_write(aic, AIC1_CPU0_TMR_HI, upper_32_bits(exp));
	aic1_write(aic, AIC1_CPU0_TMR_LO, lower_32_bits(exp));
	aic1_write(aic, AIC1_CPU0_TMR_STAT, 1);
	aic1_write(aic, AIC1_CPU0_TMR_CTRL, 1);
	dsb(sy);
}

static int aic1_tmr_set_next_event(unsigned long delta,
				   struct clock_event_device *ce)
{
	aic1_tmr_arm(apple_aic1, delta);
	return 0;
}

static int aic1_tmr_shutdown(struct clock_event_device *ce)
{
	struct apple_aic1 *aic = apple_aic1;

	aic1_write(aic, AIC1_CPU0_TMR_CTRL, 0);
	aic1_write(aic, AIC1_CPU0_TMR_STAT, 1);
	dsb(sy);
	return 0;
}

static int aic1_tmr_set_periodic(struct clock_event_device *ce)
{
	aic1_tmr_arm(apple_aic1, AIC1_TMR_PERIOD);
	return 0;
}

static struct clock_event_device aic1_clockevent = {
	.name			= "apple-aic1-timer",
	.features		= CLOCK_EVT_FEAT_PERIODIC |
				  CLOCK_EVT_FEAT_ONESHOT,
	.rating			= 400,
	.set_next_event		= aic1_tmr_set_next_event,
	.set_state_shutdown	= aic1_tmr_shutdown,
	.set_state_periodic	= aic1_tmr_set_periodic,
	.set_state_oneshot	= aic1_tmr_shutdown,
	.tick_resume		= aic1_tmr_shutdown,
};

static void aic1_tmr_irq(void)
{
	struct apple_aic1 *aic = apple_aic1;

	aic1_write(aic, AIC1_CPU0_TMR_CTRL, 0);
	aic1_write(aic, AIC1_CPU0_TMR_STAT, 1);
	if (clockevent_state_periodic(&aic1_clockevent))
		aic1_tmr_arm(aic, AIC1_TMR_PERIOD);
	if (aic1_clockevent.event_handler)
		aic1_clockevent.event_handler(&aic1_clockevent);
}

static int __init aic1_timer_init(void)
{
	struct apple_aic1 *aic = apple_aic1;
	u64 t0, t1;

	if (!aic)
		return 0;

	t0 = aic1_tmr_read(aic);
	udelay(10);
	t1 = aic1_tmr_read(aic);
	if (t1 == t0) {
		pr_warn("24 MHz timebase at +0x20 did not advance\n");
		return 0;
	}

	aic1_local_tmr_handler = aic1_tmr_irq;
	aic1_clockevent.cpumask = cpumask_of(0);
	clockevents_config_and_register(&aic1_clockevent, AIC1_TMR_RATE,
					0xf, 0x7fffffff);
	pr_info("registered 24 MHz local timer clockevent\n");
	return 0;
}
arch_initcall(aic1_timer_init);

IRQCHIP_DECLARE(apple_aic1, "apple,aic", aic1_of_init);
IRQCHIP_DECLARE(apple_s5l8940x_aic, "apple,s5l8940x-aic", aic1_of_init);
IRQCHIP_DECLARE(apple_s5l8947x_aic, "apple,s5l8947x-aic", aic1_of_init);
