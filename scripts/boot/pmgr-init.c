// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pre-kernel PMGR clock gate initialization for S5L8940X / S5L8947X
 */

#include "pmgr-init.h"

static inline uint32_t mmio_read32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void mmio_write32(uintptr_t addr, uint32_t val)
{
	*(volatile uint32_t *)addr = val;
}

static void put_hex32(void (*uart_puts)(const char *), uint32_t v)
{
	static const char hex[] = "0123456789abcdef";
	char buf[9];
	int i;

	for (i = 0; i < 8; i++)
		buf[7 - i] = hex[(v >> (i * 4)) & 0xf];
	buf[8] = '\0';
	uart_puts(buf);
}

const struct pmgr_init_entry s5l8940x_pmgr_init_table[] = {
	/* Clock gates: PMGR + 0x1c0 .. 0x30c */
	{ 0x01c0, PMGR_GATE_MASK, 0x0000000a }, /* gate  0: cpu0 */
	{ 0x01c4, PMGR_GATE_MASK, 0x0000000f }, /* gate  1: cpu1 */
	{ 0x01c8, PMGR_GATE_MASK, 0x0000000a }, /* gate  2: scul2 */
	{ 0x01cc, PMGR_GATE_MASK, 0x0000000a }, /* gate  3: acp */
	{ 0x01d0, PMGR_GATE_MASK, 0x0000000a }, /* gate  4: noc0 */
	{ 0x01d4, PMGR_GATE_MASK, 0x0000000a }, /* gate  5: noc1 */
	{ 0x01d8, PMGR_GATE_MASK, 0x0000000a }, /* gate  6: cdio */
	{ 0x01dc, PMGR_GATE_MASK, 0x0000000a }, /* gate  7: rtos */
	{ 0x01e0, PMGR_GATE_MASK, 0x0000000a }, /* gate  8: aic */
	{ 0x01e4, PMGR_GATE_MASK, 0x0000000f }, /* gate  9: dwi */
	{ 0x01f8, PMGR_GATE_MASK, 0x0000000a }, /* gate 14: amc */
	{ 0x0200, PMGR_GATE_MASK, 0x0000000f }, /* gate 16: mcu0 */
	{ 0x0204, PMGR_GATE_MASK, 0x0000000f }, /* gate 17: mcu1 */
	{ 0x0208, PMGR_GATE_MASK, 0x0000000f }, /* gate 18: disp0 */
	{ 0x020c, PMGR_GATE_MASK, 0x0000000f }, /* gate 19: disp-fe */
	{ 0x0210, PMGR_GATE_MASK, 0x0000000f }, /* gate 20: rgbout */
	{ 0x0214, PMGR_GATE_MASK, 0x0000000f }, /* gate 21: disp-be */
	{ 0x0218, PMGR_GATE_MASK, 0x0000000f }, /* gate 22: venet */
	{ 0x021c, PMGR_GATE_MASK, 0x0000000f }, /* gate 23: edp */
	{ 0x0228, PMGR_GATE_MASK, 0x0000000f }, /* gate 26: scaler */
	{ 0x0230, PMGR_GATE_MASK, 0x0000000f }, /* gate 28: media */
	{ 0x0238, PMGR_GATE_MASK, 0x0000000f }, /* gate 30: usb-complex */
	{ 0x023c, PMGR_GATE_MASK, 0x0000000f }, /* gate 31: usb-otg */
	{ 0x0240, PMGR_GATE_MASK, 0x0000000f }, /* gate 32: usb-ehci */
	{ 0x0244, PMGR_GATE_MASK, 0x0000000f }, /* gate 33: usb-ohci0 */
	{ 0x0248, PMGR_GATE_MASK, 0x0000000f }, /* gate 34: usb-ohci1 */
	{ 0x024c, PMGR_GATE_MASK, 0x0000000f }, /* gate 35: usb11phy */
	{ 0x0250, PMGR_GATE_MASK, 0x0000000f }, /* gate 36: usb20phy */
	{ 0x0254, PMGR_GATE_MASK, 0x0000000f }, /* gate 37: audio */
	{ 0x0258, PMGR_GATE_MASK, 0x0000000a }, /* gate 38: hpark */
	{ 0x0260, PMGR_GATE_MASK, 0x0000000f }, /* gate 40: adbe0 */
	{ 0x0268, PMGR_GATE_MASK, 0x0000000f }, /* gate 42: dwi */
	{ 0x026c, PMGR_GATE_MASK, 0x0000000f }, /* gate 43: gpio */
	{ 0x0270, PMGR_GATE_MASK, 0x0000000f }, /* gate 44: pke */
	{ 0x0274, PMGR_GATE_MASK, 0x0000000f }, /* gate 45: aes */
	{ 0x0278, PMGR_GATE_MASK, 0x0000000f }, /* gate 46: fmi0 */
	{ 0x027c, PMGR_GATE_MASK, 0x0000000f }, /* gate 47: fmi0bch */
	{ 0x0280, PMGR_GATE_MASK, 0x0000000f }, /* gate 48: fmi1 */
	{ 0x0284, PMGR_GATE_MASK, 0x0000000f }, /* gate 49: fmi1bch */
	{ 0x0288, PMGR_GATE_MASK, 0x0000000a }, /* gate 50: cdio */
	{ 0x028c, PMGR_GATE_MASK, 0x0000000a }, /* gate 51: iop */
	{ 0x0290, PMGR_GATE_MASK, 0x0000000a }, /* gate 52: upas */
	{ 0x0294, PMGR_GATE_MASK, 0x0000000a }, /* gate 53: HperfNrt */
	{ 0x0298, PMGR_GATE_MASK, 0x0000000a }, /* gate 54: HperfRt */
	{ 0x029c, PMGR_GATE_MASK, 0x0000000a }, /* gate 55: Lperf */
	{ 0x02a0, PMGR_GATE_MASK, 0x0000000a }, /* gate 56: udma */
	{ 0x02a4, PMGR_GATE_MASK, 0x0000000a }, /* gate 57: cdma */
	{ 0x02c8, PMGR_GATE_MASK, 0x0000000f }, /* gate 66: spi0 */
	{ 0x02cc, PMGR_GATE_MASK, 0x0000000f }, /* gate 67: spi1 */
	{ 0x02d0, PMGR_GATE_MASK, 0x0000000f }, /* gate 68: spi2 */
	{ 0x02d4, PMGR_GATE_MASK, 0x0000000f }, /* gate 69: spi3 */
	{ 0x02d8, PMGR_GATE_MASK, 0x0000000f }, /* gate 70: spi4 */
	{ 0x02f4, PMGR_GATE_MASK, 0x0000000f }, /* gate 77: uart0 */
	{ 0x02f8, PMGR_GATE_MASK, 0x0000000f }, /* gate 78: uart1 */
	{ 0x02fc, PMGR_GATE_MASK, 0x0000000f }, /* gate 79: eth / uart2 */
	{ 0x0300, PMGR_GATE_MASK, 0x0000000f }, /* gate 80: hdmi / uart3 */
	{ 0x0304, PMGR_GATE_MASK, 0x0000000f }, /* gate 81: uart4 */
	{ 0x0308, PMGR_GATE_MASK, 0x0000000f }, /* gate 82: uart5 */
	{ 0x030c, PMGR_GATE_MASK, 0x0000000f }, /* gate 83: uart6 */
	{ 0x0310, PMGR_GATE_MASK, 0x0000000f }, /* gate 84: i2c0 */
	{ 0x0314, PMGR_GATE_MASK, 0x0000000f }, /* gate 85: i2c1 */
	{ 0x0318, PMGR_GATE_MASK, 0x0000000f }, /* gate 86: i2c2 */
};

const unsigned int s5l8940x_pmgr_init_count =
	sizeof(s5l8940x_pmgr_init_table) / sizeof(s5l8940x_pmgr_init_table[0]);

void pmgr_init_apply_gates(uintptr_t pmgr_base, void (*uart_puts)(const char *))
{
	unsigned int i, changed = 0;

	if (uart_puts)
		uart_puts("[boot-loader] Applying PMGR clock gates...\n");

	for (i = 0; i < s5l8940x_pmgr_init_count; i++) {
		const struct pmgr_init_entry *e = &s5l8940x_pmgr_init_table[i];
		uintptr_t addr = pmgr_base + e->offset;
		uint32_t cur, next;

		if (e->offset < 0x01c0 || e->offset > 0x0320)
			continue;

		cur = mmio_read32(addr);
		if ((cur & e->mask) == 0) {
			next = (cur & ~e->mask) | (e->value & e->mask);
			mmio_write32(addr, next);
			changed++;
		}
	}

	/* Ensure HSIC0 PHY gate (gate 90 at +0x328) is enabled */
	if ((mmio_read32(pmgr_base + 0x0328) & PMGR_GATE_MASK) == 0) {
		mmio_write32(pmgr_base + 0x0328, 0x0000000fU);
		changed++;
	}

	if (uart_puts) {
		uart_puts("[boot-loader] PMGR gates newly enabled: 0x");
		put_hex32(uart_puts, changed);
		uart_puts("\n");
	}
}
