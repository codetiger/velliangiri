/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VELLIANGIRI_PMGR_INIT_H
#define VELLIANGIRI_PMGR_INIT_H

#include <stdint.h>

struct pmgr_init_entry {
	uint32_t offset;
	uint32_t mask;
	uint32_t value;
};

#define PMGR_GATE_MASK	0x0000000fU

extern const struct pmgr_init_entry s5l8940x_pmgr_init_table[];
extern const unsigned int s5l8940x_pmgr_init_count;

void pmgr_init_apply_gates(uintptr_t pmgr_base, void (*uart_puts)(const char *));

#endif /* VELLIANGIRI_PMGR_INIT_H */
