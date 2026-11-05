// SPDX-License-Identifier: GPL-2.0-only
/*
 * Carve the VXD video decoder firmware out of the on-device system image
 * (/bin/vxdcarve)
 *
 * Usage:
 *   vxdcarve <image> <output>
 *
 * The decoder's microcontroller firmware is not a separate file on the System
 * volume: it is a static descriptor plus two load segments inside the video
 * driver module that is prelinked into the stock kernel image. This tool
 *
 *   1. unwraps the image container (tagged "DATA" payload) if there is one,
 *   2. inflates the LZSS payload (4 KiB window, checked against its Adler-32),
 *   3. scans the prelinked modules for the firmware descriptor: two pointers to
 *      printable build strings followed by { size1, size2, offset2, src1,
 *      src2 } with offset2 == align4(size1) and src2 == src1 + size1,
 *   4. accepts it only if the segment sizes match a known layout, so a module
 *      whose firmware format is not understood fails instead of being fed to
 *      the hardware,
 *   5. copies both segments into <output>.
 *
 * Output format (all little endian), read by the VXD driver:
 *   u32 magic 'VXDF', u32 version (1), u32 image size, u32 segment count
 *   u8  uuid[16]                         module build the data came from
 *   count x { u32 destination offset, u32 size, u32 reserved, u32 reserved }
 *   segment data (each at its offset, 64-byte aligned)
 *
 * The driver fills the rest of the image with 0xcd and writes the 0x10101010
 * end marker, as the stock driver does; only the segments come from here.
 *
 * Exits 0 on success, 1 when no known firmware layout is found and 2 on error.
 */

#include "carve_common.h"

#define VXDF_MAGIC	0x46445856u	/* "VXDF" */
#define VXDF_VERSION	1
#define VXDF_ALIGN	64
#define VXDF_IMAGE_SIZE	0xa000u		/* the stock driver's upload buffer */

/* Firmware layouts whose descriptor and segments are understood. */
static const struct {
	uint32_t size1;
	uint32_t size2;
	uint32_t off2;
} layouts[] = {
	{ 0x546c, 0x1898, 0x5470 },	/* REL_1472.1_10__FSA, 24_09_2012 */
};

struct image {
	const uint8_t *base;
	size_t len;
	const uint8_t *hdr;
	uint32_t ncmds;
	uint32_t sizeofcmds;
};

/*
 * Map a virtual address range to a file offset in the image. The segments of
 * the image itself cover every prelinked module, so they are the only mapping
 * that is valid for data inside any of them.
 */
static int img_off(const struct image *im, uint32_t va, uint64_t size, size_t *off)
{
	const uint8_t *lc = im->hdr + 28, *end = lc + im->sizeofcmds;
	uint32_t i;

	for (i = 0; i < im->ncmds && lc + 8 <= end; i++) {
		uint32_t cmd = rd32le(lc), sz = rd32le(lc + 4);

		if (sz < 8 || lc + sz > end)
			return -1;
		if (cmd == LC_SEGMENT && sz >= 56) {
			uint32_t vmaddr = rd32le(lc + 24), fileoff = rd32le(lc + 32);
			uint32_t filesize = rd32le(lc + 36);

			if (va >= vmaddr && (uint64_t)(va - vmaddr) + size <= filesize &&
			    (uint64_t)fileoff + filesize <= im->len) {
				*off = fileoff + (va - vmaddr);
				return 0;
			}
		}
		lc += sz;
	}
	return -1;
}

/* True if 32 mapped bytes start with a printable, NUL-terminated string */
static int is_string(const struct image *im, uint32_t va)
{
	size_t o, i;

	if (img_off(im, va, 32, &o))
		return 0;
	for (i = 0; i < 32; i++) {
		uint8_t c = im->base[o + i];

		if (!c)
			return i >= 4;
		if (c < 0x20 || c > 0x7e)
			return 0;
	}
	return 0;
}

/* Copy the LC_UUID of the prelinked module whose segments contain @va */
static int module_uuid(const struct image *im, uint32_t va, uint8_t uuid[16])
{
	size_t o = 0;

	while (o + 28 <= im->len) {
		const uint8_t *hdr = im->base + o;
		const uint8_t *lc, *end, *id = NULL;
		uint32_t ncmds, sizeofcmds, i;
		int inside = 0;

		if (rd32le(hdr) != MH_MAGIC_32 || rd32le(hdr + 4) != CPU_TYPE_ARM)
			goto next;
		ncmds = rd32le(hdr + 16);
		sizeofcmds = rd32le(hdr + 20);
		if (o + 28 + (uint64_t)sizeofcmds > im->len)
			goto next;

		lc = hdr + 28;
		end = lc + sizeofcmds;
		for (i = 0; i < ncmds && lc + 8 <= end; i++) {
			uint32_t cmd = rd32le(lc), sz = rd32le(lc + 4);

			if (sz < 8 || lc + sz > end)
				break;
			if (cmd == LC_SEGMENT && sz >= 56) {
				uint32_t vmaddr = rd32le(lc + 24), vmsize = rd32le(lc + 28);

				/*
				 * Only __TEXT: the image's own __PRELINK_TEXT covers every
				 * prelinked module and would match all of them.
				 */
				if (!memcmp(lc + 8, "__TEXT", 7) && va >= vmaddr &&
				    va - vmaddr < vmsize)
					inside = 1;
			} else if (cmd == LC_UUID && sz >= 24) {
				id = lc + 8;
			}
			lc += sz;
		}
		if (inside && id) {
			memcpy(uuid, id, 16);
			return 1;
		}
next:
		o += 4096;
	}
	return 0;
}

static int known_layout(uint32_t size1, uint32_t size2, uint32_t off2)
{
	unsigned int i;

	for (i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++)
		if (layouts[i].size1 == size1 && layouts[i].size2 == size2 &&
		    layouts[i].off2 == off2)
			return 1;
	return 0;
}

int main(int argc, char **argv)
{
	const uint8_t *payload;
	uint8_t *raw, *img, *out;
	size_t rlen, plen, ilen, olen, o, o2, so1, so2;
	struct image im;
	uint32_t size1 = 0, size2 = 0, off2 = 0, src1 = 0, src2 = 0;
	uint32_t ver_va = 0, date_va = 0;
	uint8_t uuid[16] = { 0 };
	char ver[33] = "", date[33] = "";
	int found = 0, have_uuid;
	FILE *f;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <image> <output>\n", argv[0]);
		return 2;
	}
	raw = read_file(argv[1], &rlen);
	if (!raw) {
		perror(argv[1]);
		return 2;
	}
	payload = unwrap(raw, rlen, &plen);
	if (!payload) {
		fprintf(stderr, "no data payload in container\n");
		free(raw);
		return 2;
	}
	img = inflate(payload, plen, &ilen);
	free(raw);
	if (!img)
		return 2;

	if (ilen < 28 || rd32le(img) != MH_MAGIC_32 || rd32le(img + 4) != CPU_TYPE_ARM) {
		fprintf(stderr, "image is not a 32-bit ARM Mach-O\n");
		free(img);
		return 2;
	}
	im.base = img;
	im.len = ilen;
	im.hdr = img;
	im.ncmds = rd32le(img + 16);
	im.sizeofcmds = rd32le(img + 20);
	if (28 + (uint64_t)im.sizeofcmds > ilen) {
		fprintf(stderr, "truncated load commands\n");
		free(img);
		return 2;
	}

	/*
	 * The descriptor is { version, date, size1, size2, offset2, src1, src2 }
	 * with the two sources adjacent in the module and offset2 == align4(size1).
	 */
	for (o = 0; o + 28 <= ilen; o += 4) {
		uint32_t w0 = rd32le(img + o), w1 = rd32le(img + o + 4);
		uint32_t s1 = rd32le(img + o + 8), s2 = rd32le(img + o + 12);
		uint32_t d2 = rd32le(img + o + 16);
		uint32_t p1 = rd32le(img + o + 20), p2 = rd32le(img + o + 24);

		if (w1 <= w0 || w1 - w0 > 0x100)
			continue;
		if (!known_layout(s1, s2, d2))
			continue;
		if (p2 != p1 + s1)
			continue;
		if (!is_string(&im, w0) || !is_string(&im, w1))
			continue;
		if (img_off(&im, p1, s1, &so1) || img_off(&im, p2, s2, &so2))
			continue;
		if (found) {
			fprintf(stderr, "more than one firmware descriptor\n");
			free(img);
			return 2;
		}
		found = 1;
		ver_va = w0;
		date_va = w1;
		size1 = s1;
		size2 = s2;
		off2 = d2;
		src1 = p1;
		src2 = p2;
	}
	if (!found) {
		fprintf(stderr, "no firmware with a known layout\n");
		free(img);
		return 1;
	}

	have_uuid = module_uuid(&im, ver_va, uuid);

	/* Keep the build strings for the operator before the image is freed */
	if (!img_off(&im, ver_va, 32, &o))
		snprintf(ver, sizeof(ver), "%.32s", (const char *)img + o);
	if (!img_off(&im, date_va, 32, &o))
		snprintf(date, sizeof(date), "%.32s", (const char *)img + o);

	olen = 32 + 16 * 2;
	so1 = (olen + VXDF_ALIGN - 1) / VXDF_ALIGN * VXDF_ALIGN;
	so2 = (so1 + size1 + VXDF_ALIGN - 1) / VXDF_ALIGN * VXDF_ALIGN;
	olen = so2 + size2;
	out = calloc(1, olen);
	if (!out) {
		free(img);
		return 2;
	}
	wr32le(out, VXDF_MAGIC);
	wr32le(out + 4, VXDF_VERSION);
	wr32le(out + 8, VXDF_IMAGE_SIZE);
	wr32le(out + 12, 2);
	memcpy(out + 16, uuid, 16);
	wr32le(out + 32, 0);
	wr32le(out + 36, size1);
	wr32le(out + 48, off2);
	wr32le(out + 52, size2);
	if (img_off(&im, src1, size1, &o) || img_off(&im, src2, size2, &o2)) {
		fprintf(stderr, "segment outside the image\n");
		free(out);
		free(img);
		return 2;
	}
	memcpy(out + so1, img + o, size1);
	memcpy(out + so2, img + o2, size2);
	free(img);

	f = fopen(argv[2], "wb");
	if (!f || fwrite(out, 1, olen, f) != olen || fclose(f)) {
		perror(argv[2]);
		free(out);
		return 2;
	}
	printf("%s (%s)%s, segments 0x%x+0x%x at 0x0/0x%x, %zu bytes\n",
	       ver[0] ? ver : "unknown", date[0] ? date : "unknown",
	       have_uuid ? "" : ", no module uuid", size1, size2, off2, olen);
	free(out);
	return 0;
}
