// SPDX-License-Identifier: GPL-2.0-only
/*
 * Carve a Broadcom Bluetooth patch out of a larger file (/bin/hcdcarve)
 *
 * Usage:
 *   hcdcarve <file> <name> <output>
 *
 * A patch is a run of vendor HCI commands (opcode group 0x3f: opcode,
 * little endian, then a length byte and the parameters) that starts with
 * Write RAM (0xfc4c) and ends with Launch RAM (0xfc4e). Every such run in
 * <file> longer than 1 KiB is a candidate. A patch writes the local name
 * the controller reports once it runs, so the candidate whose bytes
 * contain <name> (case-insensitive) is the one written to <output>.
 *
 * Exits 0 when exactly one distinct patch matches, 1 when none does and
 * 2 on ambiguity or error.
 */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OP_HI_VENDOR	0xfc
#define OP_WRITE_RAM	0x4c
#define OP_LAUNCH_RAM	0x4e
#define MIN_PATCH	1024

static const uint8_t *data;
static size_t size;
/* 1-bit bitmap of records already visited as part of a valid HCI command run */
static uint8_t *visited;

static inline int is_visited(size_t o)
{
	return (visited[o >> 3] >> (o & 7)) & 1;
}

static inline void mark_visited(size_t o)
{
	visited[o >> 3] |= (uint8_t)(1u << (o & 7));
}

static size_t walk(size_t start)
{
	size_t o = start, e = 0;

	for (;;) {
		if (o + 3 > size || data[o + 1] != OP_HI_VENDOR || is_visited(o))
			break;
		mark_visited(o);
		if (data[o] == OP_LAUNCH_RAM) {
			e = o + 3 + data[o + 2];
			if (e > size)
				e = 0;
			break;
		}
		o += 3 + data[o + 2];
		if (o >= size)
			break;
	}
	return e;
}

static int contains(const uint8_t *p, size_t len, const char *name)
{
	size_t n = strlen(name), i, j;
	uint8_t c0;

	if (!n || len < n)
		return 0;
	c0 = (uint8_t)tolower((unsigned char)name[0]);
	for (i = 0; i + n <= len; i++) {
		if ((uint8_t)tolower(p[i]) != c0)
			continue;
		for (j = 1; j < n; j++)
			if (tolower(p[i + j]) != tolower((unsigned char)name[j]))
				break;
		if (j == n)
			return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	size_t start, end, best_start = 0, best_len = 0, len;
	FILE *f;
	long sz;
	uint8_t *buf;

	if (argc != 4 || !argv[2][0]) {
		fprintf(stderr, "usage: hcdcarve <file> <name> <output>\n");
		return 2;
	}
	f = fopen(argv[1], "rb");
	if (!f || fseek(f, 0, SEEK_END) || (sz = ftell(f)) <= 0 ||
	    fseek(f, 0, SEEK_SET)) {
		perror(argv[1]);
		return 2;
	}
	size = (size_t)sz;
	buf = malloc(size);
	visited = calloc((size + 7) / 8, 1);
	if (!buf || !visited || fread(buf, 1, size, f) != size) {
		fprintf(stderr, "hcdcarve: cannot read %s\n", argv[1]);
		return 2;
	}
	fclose(f);
	data = buf;

	for (start = 0; start + 3 <= size; start++) {
		if (data[start] != OP_WRITE_RAM || data[start + 1] != OP_HI_VENDOR)
			continue;
		/* only the first record of a run starts a candidate */
		if (is_visited(start))
			continue;
		end = walk(start);
		if (!end || end - start < MIN_PATCH)
			continue;
		len = end - start;
		if (!contains(data + start, len, argv[2]))
			continue;
		if (best_len && (len != best_len ||
				 memcmp(data + start, data + best_start, len))) {
			fprintf(stderr, "hcdcarve: more than one patch matches '%s'\n",
				argv[2]);
			return 2;
		}
		best_start = start;
		best_len = len;
	}
	if (!best_len) {
		fprintf(stderr, "hcdcarve: no patch matches '%s'\n", argv[2]);
		return 1;
	}

	f = fopen(argv[3], "wb");
	if (!f || fwrite(data + best_start, 1, best_len, f) != best_len ||
	    fclose(f)) {
		perror(argv[3]);
		return 2;
	}
	printf("%zu bytes at offset %zu\n", best_len, best_start);
	return 0;
}
