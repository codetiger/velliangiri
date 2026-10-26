// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8940X / S5L8947X machine support
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/irqchip.h>
#include <linux/kernel.h>
#include <linux/of_platform.h>
#include <linux/sizes.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#include "s5l8947x.h"

#define S5L_AIC_PHYS	0x3f200000UL
#define S5L_AIC_VIRT	0xf0500000UL

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

static void __init s5l8947x_init_early(void)
{
}

static void __init s5l8947x_init_irq(void)
{
	irqchip_init();
}

static void __init s5l8947x_init_time(void)
{
	unsigned long cpu_hz;

	apple_s5l_pmccntr_enable_counter();
	cpu_hz = apple_s5l_calibrate_cpu_hz();
	apple_s5l_wdt_clocksource_init();
	apple_s5l_pmccntr_init(cpu_hz);
}

static const char * const s5l8947x_dt_compat[] __initconst = {
	"apple,s5l8940x",
	"apple,s5l8942x",
	"apple,s5l8947x",
	"apple,j33iap",
	"apple,s5l89xx",
	NULL,
};

DT_MACHINE_START(APPLE_S5L, "S5L8947X (Device Tree)")
	.dt_compat	= s5l8947x_dt_compat,
	.map_io		= s5l8947x_map_io,
	.init_early	= s5l8947x_init_early,
	.init_irq	= s5l8947x_init_irq,
	.init_time	= s5l8947x_init_time,
	.l2c_aux_val	= 0,
	.l2c_aux_mask	= 0,
MACHINE_END
