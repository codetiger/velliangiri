// SPDX-License-Identifier: GPL-2.0-only
/*
 * Carve the S5L8947X IOP firmware out of the on-device system image
 * (/bin/iopcarve)
 *
 * Usage:
 *   iopcarve <image> <output>
 *
 * The IOP coprocessor firmware is not a separate file on the System volume:
 * it is the fwFirmwareImage data inside the IOP_s5l8942xiop_firmware module
 * that is prelinked into the stock kernel image.  This tool
 *
 *   1. unwraps the image container (tagged "DATA" payload) if there is one,
 *   2. inflates the LZSS payload (4 KiB window, checked against its Adler-32),
 *   3. finds the image by its build string ("s5l8942xiop" at +0x210) and
 *      checks the ARM vector table, the configuration magic and the page-table
 *      size at the offsets the stock loader patches,
 *   4. copies the 0x24000-byte image to <output>.
 *
 * Exits 0 on success, 1 when no known image is found and 2 on error.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IOPFW_SIZE	0x24000u
#define IOPFW_BUILD_OFF	0x210u		/* "s5l8942xiop" in the image */
#define IOPFW_CONFIG_OFF 0x13000u
#define IOPFW_PTAIL_OFF	0x13360u

#define IMG_MAGIC_LE	0x496d6733u
#define TAG_DATA_LE	0x44415441u
#define COMP_BE		0x636f6d70u
#define LZSS_BE		0x6c7a7373u
#define COMP_HDR_SIZE	0x180

static uint32_t rd32le(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t rd32be(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static uint32_t adler32(const uint8_t *p, size_t n)
{
	uint32_t a = 1, b = 0;

	while (n) {
		size_t k = n < 5552 ? n : 5552;

		n -= k;
		while (k--) {
			a += *p++;
			b += a;
		}
		a %= 65521;
		b %= 65521;
	}
	return b << 16 | a;
}

static size_t lzss_inflate(uint8_t *dst, size_t dlen, const uint8_t *src,
			   size_t slen)
{
	uint8_t ring[4096 + 17];
	unsigned int r = 4096 - 18, flags = 0;
	size_t si = 0, di = 0;

	memset(ring, ' ', 4096 - 18);
	for (;;) {
		flags >>= 1;
		if (!(flags & 0x100)) {
			if (si >= slen)
				break;
			flags = src[si++] | 0xff00;
		}
		if (flags & 1) {
			uint8_t c;

			if (si >= slen)
				break;
			c = src[si++];
			if (di < dlen)
				dst[di++] = c;
			ring[r++] = c;
			r &= 4095;
		} else {
			unsigned int i, j, k;

			if (si + 2 > slen)
				break;
			i = src[si++];
			j = src[si++];
			i |= (j & 0xf0) << 4;
			j = (j & 0x0f) + 2;
			for (k = 0; k <= j; k++) {
				uint8_t c = ring[(i + k) & 4095];

				if (di < dlen)
					dst[di++] = c;
				ring[r++] = c;
				r &= 4095;
			}
		}
	}
	return di;
}

static uint8_t *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf = NULL;
	long n;

	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 &&
	    fseek(f, 0, SEEK_SET) == 0) {
		buf = malloc(n);
		if (buf && fread(buf, 1, n, f) != (size_t)n) {
			free(buf);
			buf = NULL;
		}
		*len = n;
	}
	fclose(f);
	return buf;
}

static const uint8_t *unwrap(const uint8_t *buf, size_t len, size_t *plen)
{
	size_t o;

	*plen = len;
	if (len < 20 || rd32le(buf) != IMG_MAGIC_LE)
		return buf;
	for (o = 20; o + 12 <= len;) {
		uint32_t tag = rd32le(buf + o), total = rd32le(buf + o + 4);
		uint32_t dlen = rd32le(buf + o + 8);

		if (total < 12 || o + total > len)
			break;
		if (tag == TAG_DATA_LE && dlen <= total - 12) {
			*plen = dlen;
			return buf + o + 12;
		}
		o += total;
	}
	return NULL;
}

static uint8_t *inflate(const uint8_t *p, size_t len, size_t *olen)
{
	uint32_t sum, ulen, clen;
	uint8_t *out;

	if (len < COMP_HDR_SIZE || rd32be(p) != COMP_BE ||
	    rd32be(p + 4) != LZSS_BE) {
		out = malloc(len);
		if (out)
			memcpy(out, p, len);
		*olen = len;
		return out;
	}
	sum = rd32be(p + 8);
	ulen = rd32be(p + 12);
	clen = rd32be(p + 16);
	if (clen > len - COMP_HDR_SIZE || ulen > 256u << 20) {
		fprintf(stderr, "bad compression header\n");
		return NULL;
	}
	out = malloc(ulen);
	if (!out)
		return NULL;
	if (lzss_inflate(out, ulen, p + COMP_HDR_SIZE, clen) != ulen) {
		fprintf(stderr, "short LZSS stream\n");
		free(out);
		return NULL;
	}
	if (adler32(out, ulen) != sum) {
		fprintf(stderr, "Adler-32 mismatch\n");
		free(out);
		return NULL;
	}
	*olen = ulen;
	return out;
}

static int known_image(const uint8_t *p)
{
	return rd32le(p) == 0xea00000eu &&
	       rd32le(p + IOPFW_CONFIG_OFF) == 0x636e6667u &&
	       rd32le(p + IOPFW_PTAIL_OFF) == 0x4000u &&
	       !memcmp(p + IOPFW_BUILD_OFF, "s5l8942xiop", 11);
}

int main(int argc, char **argv)
{
	static const char magic[] = "s5l8942xiop";
	size_t len, plen, ulen, i;
	const uint8_t *payload;
	uint8_t *raw, *data;
	const uint8_t *found = NULL;
	FILE *out;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <image> <output>\n", argv[0]);
		return 2;
	}

	raw = read_file(argv[1], &len);
	if (!raw) {
		fprintf(stderr, "%s: cannot read\n", argv[1]);
		return 2;
	}
	payload = unwrap(raw, len, &plen);
	if (!payload) {
		fprintf(stderr, "%s: no DATA payload\n", argv[1]);
		free(raw);
		return 2;
	}
	data = inflate(payload, plen, &ulen);
	if (!data) {
		free(raw);
		return 2;
	}

	for (i = IOPFW_BUILD_OFF; i + sizeof(magic) <= ulen; i++) {
		size_t base;

		if (memcmp(data + i, magic, sizeof(magic) - 1))
			continue;
		base = i - IOPFW_BUILD_OFF;
		if (base + IOPFW_SIZE > ulen)
			continue;
		if (known_image(data + base)) {
			found = data + base;
			break;
		}
	}
	if (!found) {
		fprintf(stderr, "no known IOP firmware image found\n");
		free(data);
		free(raw);
		return 1;
	}

	out = fopen(argv[2], "wb");
	if (!out) {
		perror(argv[2]);
		free(data);
		free(raw);
		return 2;
	}
	if (fwrite(found, 1, IOPFW_SIZE, out) != IOPFW_SIZE ||
	    fclose(out) != 0) {
		fprintf(stderr, "%s: write failed\n", argv[2]);
		free(data);
		free(raw);
		return 2;
	}

	printf("IOP firmware %s (%s), %u bytes\n",
	       found + IOPFW_BUILD_OFF, found + 0x280, IOPFW_SIZE);
	free(data);
	free(raw);
	return 0;
}
