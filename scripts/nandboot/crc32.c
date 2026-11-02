// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compact table-free IEEE 802.3 CRC-32 (polynomial 0xEDB88320) for the
 * S5L8947X Stage-1 NAND boot loader.
 */

#include "manifest.h"

uint32_t boot_crc32(const void *data, uint32_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	uint32_t crc = 0xffffffffU;
	uint32_t i;
	int b;

	for (i = 0; i < len; i++) {
		crc ^= p[i];
		for (b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xedb88320U & -(crc & 1U));
	}
	return ~crc;
}
