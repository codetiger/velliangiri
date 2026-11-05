/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VELLIANGIRI_NANDBOOT_MANIFEST_H
#define VELLIANGIRI_NANDBOOT_MANIFEST_H

#include <stdint.h>

#define BOOT_MANIFEST_MAGIC	0x564c4254U	/* "VLBT" */
#define BOOT_MANIFEST_VERSION	1U
#define BOOT_SLOT_INVALID	0xffU

/*
 * Interleaved SLC geometry for S5L8947X managed NAND:
 *   row = (slc << 20) | (cau << 19) | (block << 8) | page
 *
 * The 64 MiB 'boot' partition spans PEBs 0..31 (physical blocks 1..16 of
 * CAU 0 and CAU 1, 128 SLC pages of 16,384 bytes = 2 MiB per PEB):
 *   PEB 0     (CAU 0, block 1, row 0x00100100)  manifest primary
 *   PEB 1     (CAU 1, block 1, row 0x00180100)  manifest replica
 *   PEB 2..15 (blocks 2..8 of CAU 0/1)          slot A (28 MiB)
 *   PEB 16..29(blocks 9..15 of CAU 0/1)         slot B (28 MiB)
 *   PEB 30..31(block 16 of CAU 0/1)             spare (4 MiB)
 */
#define FMI_PAGE_SIZE		16384U
#define FMI_OOB_SIZE		64U
#define FMI_SLC_PAGES		128U
#define FMI_SLC_BLOCK_SIZE	(FMI_PAGE_SIZE * FMI_SLC_PAGES)

#define MANIFEST_PRIMARY_ROW	0x00100100U	/* SLC=1, CAU=0, block=1, page=0 */
#define MANIFEST_REPLICA_ROW	0x00180100U	/* SLC=1, CAU=1, block=1, page=0 */

struct boot_extent {
	uint32_t start_row;	/* Full 32-bit FMI row address (with BIT(20) set) */
	uint32_t page_count;	/* Number of 16 KiB SLC pages in this block */
};

struct boot_slot {
	uint32_t image_size;	/* Exact byte length of uncompressed Image */
	uint32_t image_crc32;	/* IEEE 802.3 CRC-32 of Image */
	uint32_t dtb_size;	/* Exact byte length of DTB */
	uint32_t dtb_crc32;	/* IEEE 802.3 CRC-32 of DTB */
	struct boot_extent image_extents[8];
	struct boot_extent dtb_extent;
};

struct boot_manifest {
	uint32_t magic;		/* BOOT_MANIFEST_MAGIC ("VLBT") */
	uint32_t version;	/* BOOT_MANIFEST_VERSION (1) */
	uint32_t seq;		/* Monotonically increasing sequence number */
	uint8_t  active_slot;	/* 0 = slot A, 1 = slot B */
	uint8_t  pending_slot;	/* 0/1 during OTA trial, 0xFF when stable */
	uint8_t  boot_tries_left;
	uint8_t  reserved;
	struct boot_slot slots[2];
	uint8_t  pad[60];	/* Pad total structure size to 256 bytes */
	uint32_t manifest_crc32;/* IEEE 802.3 CRC-32 of bytes 0..251 */
};

uint32_t boot_crc32(const void *data, uint32_t len);

int fmi_boot_init(void);
int fmi_boot_read_page(uint32_t row, void *data_dst, void *meta_dst);

int boot_load_from_nand(void *image_dst, void *dtb_dst, uint8_t *chosen_slot);

#endif /* VELLIANGIRI_NANDBOOT_MANIFEST_H */
