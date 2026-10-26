// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8940X / S5L8947X machine support and early diagnostic helpers
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of_platform.h>
#include <linux/serial_core.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/system_misc.h>

#include "s5l8947x.h"

/*
 * Static early mappings:
 *  - UART0 at 0x32500000 -> 0xfee00000
 *  - WDT   at 0x3f103000 -> 0xfee01000
 *  - USB0  at 0x36100000 -> 0xfee10000
 */
#define S5L_UART0_PHYS		0x32500000UL
#define S5L_UART0_VIRT		0xfee00000UL
#define S5L_UART0_SIZE		SZ_4K

#define S5L_WDT_PAGE_PHYS	0x3f103000UL
#define S5L_WDT_PAGE_VIRT	0xfee01000UL
#define S5L_WDT_PAGE_SIZE	SZ_4K
#define S5L_WDT_OFF		0x20UL

#define S5L_USB0_PHYS		0x36100000UL
#define S5L_USB0_VIRT		0xfee10000UL
#define S5L_USB0_SIZE		SZ_4K

#define S5L_UTRSTAT		0x10
#define S5L_UTXH		0x20
#define S5L_UFSTAT		0x18
#define S5L_UTRSTAT_TXFE	(1 << 1)
#define S5L_UFSTAT_TXFULL	(1 << 9)

static struct map_desc s5l8947x_io_desc[] __initdata = {
	{
		.virtual	= S5L_UART0_VIRT,
		.pfn		= __phys_to_pfn(S5L_UART0_PHYS),
		.length		= S5L_UART0_SIZE,
		.type		= MT_DEVICE,
	},
	{
		.virtual	= S5L_WDT_PAGE_VIRT,
		.pfn		= __phys_to_pfn(S5L_WDT_PAGE_PHYS),
		.length		= S5L_WDT_PAGE_SIZE,
		.type		= MT_DEVICE,
	},
	{
		.virtual	= S5L_USB0_VIRT,
		.pfn		= __phys_to_pfn(S5L_USB0_PHYS),
		.length		= S5L_USB0_SIZE,
		.type		= MT_DEVICE,
	},
};

void apple_s5l_early_putc(char c)
{
	void __iomem *base = (void __iomem *)S5L_UART0_VIRT;
	int limit = 100000;

	if (c == '\n')
		apple_s5l_early_putc('\r');

	while (limit-- > 0) {
		u32 ufstat = readl_relaxed(base + S5L_UFSTAT);
		u32 utrstat = readl_relaxed(base + S5L_UTRSTAT);

		if (!(ufstat & S5L_UFSTAT_TXFULL) &&
		    (utrstat & S5L_UTRSTAT_TXFE))
			break;
	}
	writel_relaxed((u32)(unsigned char)c, base + S5L_UTXH);
}

void apple_s5l_early_puts(const char *s)
{
	while (*s)
		apple_s5l_early_putc(*s++);
}

void apple_s5l_early_puthex(u32 v)
{
	static const char hex[] = "0123456789abcdef";
	int i;

	for (i = 28; i >= 0; i -= 4)
		apple_s5l_early_putc(hex[(v >> i) & 0xf]);
}

void apple_s5l_wdt_crumb(u32 val)
{
	void __iomem *wdt = (void __iomem *)(S5L_WDT_PAGE_VIRT + S5L_WDT_OFF);

	writel_relaxed(val, wdt + 0x08);
	dsb(sy);
}

void apple_s5l_wdt_kick(u32 seconds)
{
	void __iomem *wdt = (void __iomem *)(S5L_WDT_PAGE_VIRT + S5L_WDT_OFF);
	u32 ticks = seconds * 24000000U;

	writel_relaxed(0, wdt + 0x0c);
	writel_relaxed(ticks, wdt + 0x04);
	writel_relaxed(ticks ? 0x80000000U : 0x00000000U, wdt + 0x08);
	writel_relaxed(0, wdt + 0x00);
	writel_relaxed(ticks ? 4 : 0, wdt + 0x0c);
	dsb(sy);
}

static void __init s5l8947x_map_io(void)
{
	iotable_init(s5l8947x_io_desc, ARRAY_SIZE(s5l8947x_io_desc));
	apple_s5l_early_puts("\n[s5l8947x] map_io ready\n");
}

extern void __init apple_s5l_pmccntr_init(void);

static void __init s5l8947x_init_early(void)
{
	apple_s5l_early_puts("[s5l8947x] init_early\n");
	apple_s5l_pmccntr_init();
}

static void __init s5l8947x_init_machine(void)
{
	apple_s5l_early_puts("[s5l8947x] init_machine -> of_platform_default_populate\n");
	of_platform_default_populate(NULL, NULL, NULL);
}

static const char * const s5l8947x_dt_compat[] __initconst = {
	"apple,s5l8947x",
	"apple,j33iap",
	"apple,s5l8940x",
	"apple,s5l89xx",
	NULL,
};

DT_MACHINE_START(APPLE_S5L, "S5L8947X")
	.dt_compat	= s5l8947x_dt_compat,
	.map_io		= s5l8947x_map_io,
	.init_early	= s5l8947x_init_early,
	.init_machine	= s5l8947x_init_machine,
MACHINE_END
