// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X pre-kernel C loader (runs at 0x80000000)
 *
 * Layout inside the boot bundle relocated to 0x80000000:
 *   0x80000000 .. 0x80007fff  pre-kernel loader (this binary, padded to 32 KB)
 *   0x80008000 .. +zImage     Linux zImage (with embedded XZ initramfs)
 *   0x82000000 .. +dtb        Flattened Device Tree (s5l8947x-j33iap.dtb)
 *
 * Responsibilities:
 *   1. Assert all required PMGR clock gates so peripheral blocks are clocked
 *      before kernel driver probe.
 *   2. Initialize 720p60 HDMI scanout at 0x9ec00000 for simple-framebuffer.
 *   3. Verify zImage (0x016f2818) and DTB (0xd00dfeed) magic numbers.
 *   4. Jump to zImage at 0x80008000 with r0=0, r1=0xffffffff, r2=0x82000000.
 */

#include <stdint.h>
#include "pmgr-init.h"
#include "hdmi-720p.h"

#define UART0_BASE	0x32500000UL
#define UTRSTAT		0x10
#define UFSTAT		0x18
#define UTXH		0x20
#define UTRSTAT_TXFE	(1U << 1)
#define UFSTAT_TXFULL	(1U << 9)

#define ZIMAGE_PHYS	0x80008000U
#define DTB_PHYS	0x82000000U
#define PMGR_PHYS	0x3f100000UL

extern void jump_to_linux(uint32_t zimage_phys, uint32_t dtb_phys);

static inline uint32_t reg_read32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void reg_write32(uintptr_t addr, uint32_t val)
{
	*(volatile uint32_t *)addr = val;
}

static void uart_putc(char c)
{
	int timeout = 200000;

	if (c == '\n')
		uart_putc('\r');

	while (timeout-- > 0) {
		uint32_t ufstat = reg_read32(UART0_BASE + UFSTAT);
		uint32_t utrstat = reg_read32(UART0_BASE + UTRSTAT);

		if (!(ufstat & UFSTAT_TXFULL) && (utrstat & UTRSTAT_TXFE))
			break;
	}
	reg_write32(UART0_BASE + UTXH, (uint32_t)(unsigned char)c);
}

static void uart_puts(const char *s)
{
	while (*s)
		uart_putc(*s++);
}

static void uart_puthex(uint32_t v)
{
	static const char hex[] = "0123456789abcdef";
	int i;

	for (i = 28; i >= 0; i -= 4)
		uart_putc(hex[(v >> i) & 0xf]);
}

void linux_boot_main(void)
{
	uint32_t zmagic, fdt_magic;

	uart_puts("\n\n=== Velliangiri S5L8947X Pre-Kernel Loader ===\n");

	pmgr_init_apply_gates(PMGR_PHYS, uart_puts);

	if (hdmi_720p_init() == 0)
		uart_puts("[boot-loader] 720p60 HDMI scanout active @ 0x9ec00000\n");
	else
		uart_puts("[boot-loader] HDMI PHY lock skipped (headless)\n");

	zmagic = reg_read32(ZIMAGE_PHYS + 0x24);
	fdt_magic = reg_read32(DTB_PHYS);

	uart_puts("[boot-loader] zImage @ 0x");
	uart_puthex(ZIMAGE_PHYS);
	uart_puts(" magic=0x");
	uart_puthex(zmagic);
	uart_puts("  DTB @ 0x");
	uart_puthex(DTB_PHYS);
	uart_puts(" magic=0x");
	uart_puthex(fdt_magic);
	uart_puts("\n");

	if (zmagic != 0x016f2818U) {
		uart_puts("[boot-loader] FATAL: invalid zImage magic\n");
		for (;;)
			;
	}
	if (fdt_magic != 0xedfe0dd0U) {
		uart_puts("[boot-loader] FATAL: invalid DTB magic\n");
		for (;;)
			;
	}

	uart_puts("[boot-loader] Jumping to Linux 6.12 kernel...\n");
	jump_to_linux(ZIMAGE_PHYS, DTB_PHYS);
}
