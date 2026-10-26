// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pre-kernel PMGR clock gate initialization for S5L8947X
 *
 * Switches on the 7 core peripheral clock gates at 0x3f101080..0x3f101144
 * using the request (bits 3:0) / status (bits 7:4) handshake protocol.
 */

#include "pmgr-init.h"

#define PMGR_SPIN	200000

static const uint32_t s5l8947x_pmgr_gates[] = {
	0x3f1010f8U, /* UART0 */
	0x3f101118U, /* I2C0 */
	0x3f101120U, /* I2C2 */
	0x3f101144U, /* ETHERNET */
	0x3f101080U, /* UPERF */
	0x3f101098U, /* USB-REG0 */
	0x3f101084U, /* USB-OTG0 */
};

static inline uint32_t rd32(uint32_t a)
{
	return *(volatile uint32_t *)a;
}

static inline void wr32(uint32_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
}

static void pmgr_switch(uint32_t reg, int on)
{
	uint32_t v = rd32(reg);
	int n;

	if (!v)
		return;
	wr32(reg, (v & ~0x10fU) | (on ? 0xfU : 0U));
	for (n = 0; n < PMGR_SPIN; n++) {
		v = rd32(reg);
		if (!((v ^ (v >> 4)) & 0xfU))
			break;
	}
}

void pmgr_init_apply_gates(void)
{
	unsigned int i;

	for (i = 0; i < sizeof(s5l8947x_pmgr_gates) / sizeof(s5l8947x_pmgr_gates[0]); i++)
		pmgr_switch(s5l8947x_pmgr_gates[i], 1);
	__asm__ volatile("dsb sy" ::: "memory");
}
