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

#include "carve_common.h"

#define IOPFW_SIZE	0x24000u
#define IOPFW_BUILD_OFF	0x210u		/* "s5l8942xiop" in the image */
#define IOPFW_CONFIG_OFF 0x13000u
#define IOPFW_PTAIL_OFF	0x13360u

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
