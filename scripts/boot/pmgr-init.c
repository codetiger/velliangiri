// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pre-kernel PMGR clock gate and CPU performance-state initialization for
 * S5L8947X.
 *
 * 1. Scales the CPU clock from the initial 212.5 MHz boot performance state
 *    (state 6, PLL4 / 4) to the full 850 MHz high-performance state
 *    (state 2, PLL4 / 1) so zImage XZ decompression and Linux run at 850 MHz.
 * 2. Switches on the 7 core peripheral clock gates at 0x3f101080..0x3f101144
 *    using the request (bits 3:0) / status (bits 7:4) handshake protocol.
 */

#include "pmgr-init.h"

#define PMGR_SPIN		200000

#define PMGR_PLL1_CTL		0x3f100008U
#define PMGR_DOUBLER_CTL	0x3f100038U
#define PMGR_PERF_STATE2_W0	0x3f100220U
#define PMGR_PERF_STATE2_W1	0x3f100224U
#define PMGR_PERF_STATE2_W2	0x3f100228U
#define PMGR_PERF_STATE2_W3	0x3f10022cU
#define PMGR_PERF_STATE_CTL	0x3f100300U

#define PMGR_PLL_ENABLE_LOCKED	0xa0000000U
#define PMGR_DOUBLER_BUSY	(1U << 30)
#define PMGR_PERF_STATE_HIGH	2U
#define PMGR_PERF_STATE_BUSY	0x00ff0000U

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

static void pmgr_boost_cpu_850mhz(void)
{
	uint32_t v;
	int n;

	/* Only switch to state 2 if PLL1 (425 MHz base CPU PLL) is enabled and locked */
	if ((rd32(PMGR_PLL1_CTL) & PMGR_PLL_ENABLE_LOCKED) != PMGR_PLL_ENABLE_LOCKED)
		return;

	/* Ensure Performance State Table entry 2 (850 MHz, div=1) is populated */
	if (rd32(PMGR_PERF_STATE2_W0) != 0x01220001U) {
		wr32(PMGR_PERF_STATE2_W0, 0x01220001U);
		wr32(PMGR_PERF_STATE2_W1, 0x00210141U);
		wr32(PMGR_PERF_STATE2_W2, 0x00000021U);
		wr32(PMGR_PERF_STATE2_W3, 0x00000000U);
	}

	/* Scale CPU clock pre-divider in 0x3f100038[4:0] to 1 and wait for ready */
	v = rd32(PMGR_DOUBLER_CTL);
	if ((v & 0x1fU) != 1U) {
		wr32(PMGR_DOUBLER_CTL, (v & ~0x1fU) | 1U);
		for (n = 0; n < PMGR_SPIN; n++) {
			if (!(rd32(PMGR_DOUBLER_CTL) & PMGR_DOUBLER_BUSY))
				break;
		}
	}

	/* Select Performance State 2 (850 MHz) and wait for transition completion */
	if ((rd32(PMGR_PERF_STATE_CTL) & 0xfU) != PMGR_PERF_STATE_HIGH) {
		wr32(PMGR_PERF_STATE_CTL, PMGR_PERF_STATE_HIGH);
		for (n = 0; n < PMGR_SPIN; n++) {
			if (!(rd32(PMGR_PERF_STATE_CTL) & PMGR_PERF_STATE_BUSY))
				break;
		}
	}
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

	pmgr_boost_cpu_850mhz();
	for (i = 0; i < sizeof(s5l8947x_pmgr_gates) / sizeof(s5l8947x_pmgr_gates[0]); i++)
		pmgr_switch(s5l8947x_pmgr_gates[i], 1);
	__asm__ volatile("dsb sy" ::: "memory");
}
