// SPDX-License-Identifier: GPL-2.0-only
/*
 * Carve the SGX543 microkernel out of the on-device system image (/bin/sgxcarve)
 *
 * Usage:
 *   sgxcarve <image> <output>
 *
 * The GPU microkernel and its start-up data are not separate files on the
 * System volume: they are arrays inside the GPU driver module that is
 * prelinked into the stock kernel image. This tool
 *
 *   1. unwraps the image container (tagged "DATA" payload) if there is one,
 *   2. inflates the LZSS payload (4 KiB window, checked against its Adler-32),
 *   3. walks the embedded 32-bit ARM Mach-O modules and picks the GPU module
 *      by its LC_UUID, so only a module build whose layout is known is used,
 *   4. copies the known regions (by virtual address) into <output>.
 *
 * Output format (all little endian), read by
 * drivers/gpu/drm/apple-sgx/apple_sgx_hw.c:
 *   u32 magic 'SGXF', u32 version (1), u32 count, u32 header bytes
 *   u8  uuid[16]                         module build the data came from
 *   count x { u32 id, u32 offset, u32 size, u32 reserved }
 *   region data (each at its offset, 64-byte aligned)
 *
 * Exits 0 on success, 1 when no known GPU module is found and 2 on error.
 */

#include "carve_common.h"

#define SGXF_MAGIC	0x46584753u	/* "SGXF" */
#define SGXF_VERSION	1
#define SGXF_ALIGN	64

struct region {
	uint32_t id;
	uint32_t va;
	uint32_t size;
};

struct layout {
	uint8_t uuid[16];
	const char *build;
	const struct region *regions;
	unsigned int count;
};

/*
 * Region ids are shared with the kernel driver:
 *   1 boot     start-up program, placed at offset 0 of the code buffer
 *   2 ukernel  microkernel program, placed at offset 0x1000 of the code buffer
 *   3..        start-up data for the microkernel's own structures, in the order
 *              the module allocates them
 */
static const struct region regions_12h1006[] = {
	{ 1, 0x80bc7560, 0x00248 },
	{ 2, 0x80bb1a80, 0x11f88 },
	{ 3, 0x80baf950, 0x00018 },
	{ 4, 0x80baf968, 0x00038 },
	{ 5, 0x80baf9a0, 0x00018 },
	{ 6, 0x80baf9b8, 0x00350 },
	{ 7, 0x80bafd08, 0x0009c },
	{ 8, 0x80bc3a08, 0x03b58 },
	{ 9, 0x80bafda4, 0x00258 },
	{ 10, 0x80bc78c8, 0x00140 },
	{ 11, 0x80bc77d8, 0x000f0 },
	{ 12, 0x80bc77b0, 0x00028 },
};

static const struct layout layouts[] = {
	{
		.uuid = { 0x0b, 0x34, 0x38, 0x5d, 0x59, 0xf2, 0x3d, 0x54,
			  0x90, 0xd2, 0x5c, 0x2d, 0x9a, 0xaf, 0xba, 0xe5 },
		.build = "12H1006",
		.regions = regions_12h1006,
		.count = sizeof(regions_12h1006) / sizeof(regions_12h1006[0]),
	},
};

struct module {
	const uint8_t *base;	/* image the segment file offsets refer to */
	size_t len;
	const uint8_t *hdr;
	uint32_t ncmds, sizeofcmds;
};

static const uint8_t *module_uuid(const struct module *m)
{
	const uint8_t *lc = m->hdr + 28, *end = lc + m->sizeofcmds;
	uint32_t i;

	for (i = 0; i < m->ncmds && lc + 8 <= end; i++) {
		uint32_t cmd = rd32le(lc), sz = rd32le(lc + 4);

		if (sz < 8 || lc + sz > end)
			return NULL;
		if (cmd == LC_UUID && sz >= 24)
			return lc + 8;
		lc += sz;
	}
	return NULL;
}

/*
 * Map a virtual address range to bytes in the image. The module's own segments
 * may give file offsets relative to the module or to the image, so use the
 * segments of the image itself (which cover every prelinked module).
 */
static const uint8_t *module_ptr(const struct module *m, uint32_t va, uint32_t size)
{
	const uint8_t *lc = m->hdr + 28, *end = lc + m->sizeofcmds;
	uint32_t i;

	for (i = 0; i < m->ncmds && lc + 8 <= end; i++) {
		uint32_t cmd = rd32le(lc), sz = rd32le(lc + 4);

		if (sz < 8 || lc + sz > end)
			return NULL;
		if (cmd == LC_SEGMENT && sz >= 56) {
			uint32_t vmaddr = rd32le(lc + 24), fileoff = rd32le(lc + 32);
			uint32_t filesize = rd32le(lc + 36);

			if (va >= vmaddr && va - vmaddr + (uint64_t)size <= filesize &&
			    (uint64_t)fileoff + filesize <= m->len)
				return m->base + fileoff + (va - vmaddr);
		}
		lc += sz;
	}
	return NULL;
}

static const struct layout *find_module(const uint8_t *img, size_t len, struct module *m)
{
	size_t o;
	unsigned int i;

	/* Prelinked modules start on 4 KiB boundaries; the image header is one too */
	for (o = 0; o + 28 <= len; o += 4096) {
		const uint8_t *uuid;

		if (rd32le(img + o) != MH_MAGIC_32 || rd32le(img + o + 4) != CPU_TYPE_ARM)
			continue;
		m->base = img;
		m->len = len;
		m->hdr = img + o;
		m->ncmds = rd32le(img + o + 16);
		m->sizeofcmds = rd32le(img + o + 20);
		if (o + 28 + (uint64_t)m->sizeofcmds > len)
			continue;
		uuid = module_uuid(m);
		if (!uuid)
			continue;
		for (i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++)
			if (!memcmp(uuid, layouts[i].uuid, 16))
				return &layouts[i];
	}
	return NULL;
}

int main(int argc, char **argv)
{
	const struct layout *lay;
	const uint8_t *payload;
	uint8_t *raw, *img, *out;
	size_t rlen, plen, ilen, olen, off;
	struct module m, top;
	unsigned int i;
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
		return 2;
	}
	img = inflate(payload, plen, &ilen);
	free(raw);
	if (!img)
		return 2;

	lay = find_module(img, ilen, &m);
	if (!lay) {
		fprintf(stderr, "no GPU module with a known layout\n");
		free(img);
		return 1;
	}

	if (ilen < 28 || rd32le(img) != MH_MAGIC_32) {
		fprintf(stderr, "image is not a 32-bit Mach-O\n");
		free(img);
		return 2;
	}
	top.base = img;
	top.len = ilen;
	top.hdr = img;
	top.ncmds = rd32le(img + 16);
	top.sizeofcmds = rd32le(img + 20);
	if (28 + (uint64_t)top.sizeofcmds > ilen) {
		free(img);
		return 2;
	}

	olen = 16 + 16 + 16 * lay->count;
	for (i = 0; i < lay->count; i++)
		olen = (olen + SGXF_ALIGN - 1) / SGXF_ALIGN * SGXF_ALIGN + lay->regions[i].size;
	out = calloc(1, olen);
	if (!out) {
		free(img);
		return 2;
	}
	wr32le(out, SGXF_MAGIC);
	wr32le(out + 4, SGXF_VERSION);
	wr32le(out + 8, lay->count);
	wr32le(out + 12, 32 + 16 * lay->count);
	memcpy(out + 16, module_uuid(&m), 16);
	off = 32 + 16 * lay->count;
	for (i = 0; i < lay->count; i++) {
		const struct region *r = &lay->regions[i];
		const uint8_t *src = module_ptr(&top, r->va, r->size);
		uint8_t *e = out + 32 + 16 * i;

		if (!src) {
			fprintf(stderr, "region %u (0x%08x+0x%x) outside the module\n",
				r->id, r->va, r->size);
			free(out);
			free(img);
			return 2;
		}
		off = (off + SGXF_ALIGN - 1) / SGXF_ALIGN * SGXF_ALIGN;
		wr32le(e, r->id);
		wr32le(e + 4, off);
		wr32le(e + 8, r->size);
		memcpy(out + off, src, r->size);
		off += r->size;
	}
	free(img);

	f = fopen(argv[2], "wb");
	if (!f || fwrite(out, 1, olen, f) != olen || fclose(f)) {
		perror(argv[2]);
		free(out);
		return 2;
	}
	printf("build %s, %u regions, %zu bytes\n", lay->build, lay->count, olen);
	free(out);
	return 0;
}
