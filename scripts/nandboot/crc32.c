// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compact 16-entry nibble-table IEEE 802.3 CRC-32 (polynomial 0xEDB88320)
 * for the S5L8947X Stage-1 NAND boot loader.
 */

#include "manifest.h"

static const uint32_t crc32_tab16[16] = {
	0x00000000U, 0x1db71064U, 0x3b6e20c8U, 0x26d930acU,
	0x76dc4190U, 0x6b6b51f4U, 0x4db26158U, 0x5005713cU,
	0xedb88320U, 0xf00f9344U, 0xd6d6a3e8U, 0xcb61b38cU,
	0x9b64c2b0U, 0x86d3d2d4U, 0xa00ae278U, 0xbdbdf21cU,
};

uint32_t boot_crc32(const void *data, uint32_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	uint32_t crc = 0xffffffffU;
	uint32_t i;

	for (i = 0; i < len; i++) {
		crc ^= p[i];
		crc = (crc >> 4) ^ crc32_tab16[crc & 0x0fU];
		crc = (crc >> 4) ^ crc32_tab16[crc & 0x0fU];
	}
	return ~crc;
}
