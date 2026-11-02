// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dual-manifest validation, A/B slot selection, trial-counter update, and
 * kernel Image + DTB extent loading for the S5L8947X Stage-1 NAND boot loader.
 */

#include "manifest.h"

#define SCRATCH_PAGE_ADDR	0x84000000U

_Static_assert(sizeof(struct boot_slot) == 88, "boot_slot must be 88 bytes");
_Static_assert(sizeof(struct boot_manifest) == 256, "boot_manifest must be 256 bytes");

static void mem_copy(void *dst, const void *src, uint32_t len)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;
	uint32_t i;

	for (i = 0; i < len; i++)
		d[i] = s[i];
}

static void mem_fill(void *dst, uint8_t val, uint32_t len)
{
	uint8_t *d = (uint8_t *)dst;
	uint32_t i;

	for (i = 0; i < len; i++)
		d[i] = val;
}

static int manifest_valid(const struct boot_manifest *m)
{
	if (m->magic != BOOT_MANIFEST_MAGIC ||
	    m->version != BOOT_MANIFEST_VERSION ||
	    m->active_slot > 1U)
		return 0;
	return boot_crc32(m, sizeof(*m) - sizeof(uint32_t)) == m->manifest_crc32;
}

static int load_slot(const struct boot_slot *s, void *image_dst, void *dtb_dst)
{
	uint8_t *img = (uint8_t *)image_dst;
	uint8_t *dtb = (uint8_t *)dtb_dst;
	uint32_t loaded = 0;
	uint32_t e, p;

	if (!s->image_size || !s->dtb_size || !s->dtb_extent.page_count)
		return -1;

	for (e = 0; e < 8U && loaded < s->image_size; e++) {
		uint32_t row = s->image_extents[e].start_row;
		uint32_t count = s->image_extents[e].page_count;

		if (!count)
			break;
		for (p = 0; p < count && loaded < s->image_size; p++) {
			if (fmi_boot_read_page(row + p, img + loaded, (void *)0))
				return -1;
			loaded += FMI_PAGE_SIZE;
		}
	}
	if (loaded < s->image_size)
		return -1;
	if (boot_crc32(img, s->image_size) != s->image_crc32)
		return -1;

	loaded = 0;
	for (p = 0; p < s->dtb_extent.page_count && loaded < s->dtb_size; p++) {
		if (fmi_boot_read_page(s->dtb_extent.start_row + p,
				       dtb + loaded, (void *)0))
			return -1;
		loaded += FMI_PAGE_SIZE;
	}
	if (loaded < s->dtb_size)
		return -1;
	if (boot_crc32(dtb, s->dtb_size) != s->dtb_crc32)
		return -1;

	return 0;
}

int boot_load_from_nand(void *image_dst, void *dtb_dst, uint8_t *chosen_slot)
{
	uint8_t *scratch = (uint8_t *)SCRATCH_PAGE_ADDR;
	struct boot_manifest m0, m1, m;
	int v0 = 0, v1 = 0;
	uint32_t update_row = MANIFEST_REPLICA_ROW;
	uint8_t slot;

	if (fmi_boot_init())
		return -1;

	if (!fmi_boot_read_page(MANIFEST_PRIMARY_ROW, scratch, (void *)0)) {
		mem_copy(&m0, scratch, sizeof(m0));
		v0 = manifest_valid(&m0);
	}
	if (!fmi_boot_read_page(MANIFEST_REPLICA_ROW, scratch, (void *)0)) {
		mem_copy(&m1, scratch, sizeof(m1));
		v1 = manifest_valid(&m1);
	}
	if (!v0 && !v1)
		return -1;

	if (v0 && (!v1 || m0.seq >= m1.seq)) {
		mem_copy(&m, &m0, sizeof(m));
		update_row = MANIFEST_REPLICA_ROW;
	} else {
		mem_copy(&m, &m1, sizeof(m));
		update_row = MANIFEST_PRIMARY_ROW;
	}

	slot = m.active_slot;
	if (m.pending_slot <= 1U) {
		if (m.boot_tries_left > 0U) {
			slot = m.pending_slot;
			m.boot_tries_left--;
			m.seq++;
			m.manifest_crc32 = boot_crc32(&m, sizeof(m) - sizeof(uint32_t));
			mem_fill(scratch, 0xffU, FMI_PAGE_SIZE);
			mem_copy(scratch, &m, sizeof(m));
			if (!fmi_boot_erase_block(update_row))
				fmi_boot_write_page(update_row, scratch, (void *)0);
		}
	}

	if (!load_slot(&m.slots[slot], image_dst, dtb_dst)) {
		if (chosen_slot)
			*chosen_slot = slot;
		return 0;
	}

	/* Automatic fallback to the alternate slot on CRC or read failure */
	slot ^= 1U;
	if (!load_slot(&m.slots[slot], image_dst, dtb_dst)) {
		if (chosen_slot)
			*chosen_slot = slot;
		return 0;
	}

	return -1;
}
