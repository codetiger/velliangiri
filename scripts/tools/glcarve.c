// SPDX-License-Identifier: GPL-2.0-only
/*
 * glcarve -- take the 3D pass's event PDS program out of the stock system
 * image (/bin/glcarve)
 *
 * The stock graphics driver keeps one PDS program as a template instead of
 * building it: the event program the 3D pass starts with.  It is 0xa8 bytes
 * -- five data rows and 22 instructions -- in the system-wide code cache,
 * and its three USSE program words and one data word are filled in for each
 * render.  The SGX render node needs it too: sgxframe and the Mesa driver
 * build the rest of the frame themselves and take only this.
 *
 * It is not in this tree.  stage2 runs this tool on the first boot, while the
 * System volume is mounted, and caches the result on Data; nothing is
 * committed.
 *
 * The template has no name in the cache, so it is found by its shape: a
 * 42-word window that ends with the PDS end instruction, has at least three
 * data-out instructions and no other end in front of it.  The cache header
 * names every image in it, so -l lists them and -i scans only one image's
 * __TEXT,__const section -- where the stock driver keeps the template --
 * which is faster and leaves out the other PDS programs the cache carries.
 * Several windows can still match; each is written out so sgxframe can try
 * them (it checks every pixel), and the best is also written to the name
 * asked for.
 *
 * Usage:
 *   glcarve [-a] [-n MAX] [-l] [-i NAME] CACHE OUT
 *
 *   -a       OUT is a directory: write every candidate as gl-event-<off>.pds
 *   -n MAX   stop after MAX candidates (default 64)
 *   -l       list the images whose path contains NAME (or all of them)
 *   -i NAME  scan only the image whose path contains NAME
 *
 * Exit status 0 when at least one candidate was written.
 */
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define EVENT_BYTES	0xa8
#define EVENT_WORDS	(EVENT_BYTES / 4)	/* 42 */
#define EVENT_DATA	20			/* five rows of four */
#define PDS_END		0xaf000000u
#define PDS_DOUT	0x00u			/* top five bits (0x07 with pred 7) */

struct cand {
	unsigned long long off;
	unsigned int score;
	uint32_t w[EVENT_WORDS];
};

static uint32_t le32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	       (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int ci_contains(const char *hay, const char *needle)
{
	size_t n = strlen(needle), i, j;

	if (!n)
		return 1;
	for (i = 0; hay[i]; i++) {
		for (j = 0; j < n; j++) {
			char a = hay[i + j], b = needle[j];

			if (a >= 'A' && a <= 'Z')
				a += 'a' - 'A';
			if (b >= 'A' && b <= 'Z')
				b += 'a' - 'A';
			if (a != b)
				break;
		}
		if (j == n)
			return 1;
	}
	return 0;
}

/* ---- the code cache's image table -------------------------------------- */

#define CACHE_MAPS	8
#define CACHE_IMAGES	4096
#define CACHE_PATH	256

struct cache_map {
	uint64_t addr, size, file;
};

struct cache_image {
	uint64_t addr;
	uint32_t path_off;
};

struct cache {
	struct cache_map map[CACHE_MAPS];
	unsigned int nmap, nimage;
	struct cache_image image[CACHE_IMAGES];
};

static int read_at(FILE *fp, unsigned long long off, void *buf, size_t n)
{
	if (fseeko(fp, (off_t)off, SEEK_SET))
		return -1;
	return fread(buf, 1, n, fp) == n ? 0 : -1;
}

static int cache_open(FILE *fp, struct cache *c)
{
	unsigned char h[104], b[32];
	unsigned int i, mo, mc, io, ic;

	if (read_at(fp, 0, h, sizeof(h)) || memcmp(h, "dyld_v1", 7))
		return -1;
	mo = le32(h + 16);
	mc = le32(h + 20);
	io = le32(h + 24);
	ic = le32(h + 28);
	if (mc > CACHE_MAPS || ic > CACHE_IMAGES || !mc || !ic)
		return -1;
	c->nmap = mc;
	c->nimage = ic;
	for (i = 0; i < mc; i++) {
		if (read_at(fp, mo + i * 32, b, sizeof(b)))
			return -1;
		c->map[i].addr = (uint64_t)le32(b) | (uint64_t)le32(b + 4) << 32;
		c->map[i].size = (uint64_t)le32(b + 8) | (uint64_t)le32(b + 12) << 32;
		c->map[i].file = (uint64_t)le32(b + 16) | (uint64_t)le32(b + 20) << 32;
	}
	for (i = 0; i < ic; i++) {
		if (read_at(fp, io + i * 32, b, sizeof(b)))
			return -1;
		c->image[i].addr = (uint64_t)le32(b) | (uint64_t)le32(b + 4) << 32;
		c->image[i].path_off = le32(b + 24);
	}
	return 0;
}

static unsigned long long cache_file_off(const struct cache *c, uint64_t addr)
{
	unsigned int i;

	for (i = 0; i < c->nmap; i++)
		if (addr >= c->map[i].addr && addr < c->map[i].addr + c->map[i].size)
			return c->map[i].file + (addr - c->map[i].addr);
	return 0;
}

static int cache_path(FILE *fp, const struct cache *c, unsigned int i, char *out,
		      size_t n)
{
	if (read_at(fp, c->image[i].path_off, out, n))
		return -1;
	out[n - 1] = '\0';
	return 0;
}

/*
 * Where a Mach-O image's __TEXT,__const is in the cache: the header is at
 * the image's address, and the segment and section load commands carry the
 * file offsets.  Falls back to the whole __TEXT, then the whole image.
 */
static void image_range(FILE *fp, const struct cache *c, unsigned int i,
			unsigned long long *start, unsigned long long *size)
{
	unsigned char hdr[28], lc[4096];
	unsigned long long base = cache_file_off(c, c->image[i].addr);
	uint32_t ncmds, cmdsz, at = 0, j;
	unsigned long long text_off = 0, text_size = 0;

	*start = base;
	*size = 0;
	if (!base || read_at(fp, base, hdr, sizeof(hdr)))
		return;
	if (le32(hdr) != 0xfeedfaceu && le32(hdr) != 0xfeedfacfu)
		return;
	ncmds = le32(hdr + 16);
	cmdsz = le32(hdr + 20);
	if (ncmds > 256 || cmdsz > sizeof(lc) ||
	    read_at(fp, base + sizeof(hdr), lc, cmdsz))
		return;
	for (j = 0; j < ncmds; j++) {
		uint32_t cmd, len;
		unsigned int s;

		if (at + 8 > cmdsz)
			break;
		cmd = le32(lc + at);
		len = le32(lc + at + 4);
		if (len < 8 || at + len > cmdsz)
			break;
		if ((cmd & 0x7fffffffu) == 0x1u && len >= 56) {	/* LC_SEGMENT */
			const char *seg = (const char *)lc + at + 8;
			uint32_t nsects = le32(lc + at + 48);

			if (!strncmp(seg, "__TEXT", 16)) {
				text_off = le32(lc + at + 32);
				text_size = le32(lc + at + 36);
			}
			for (s = 0; s < nsects && 56 + (s + 1) * 68 <= len; s++) {
				const unsigned char *sec = lc + at + 56 + s * 68;
				const char *sect = (const char *)sec;

				if (!strncmp(seg, "__TEXT", 16) &&
				    !strncmp(sect, "__const", 16) &&
				    le32(sec + 36)) {
					/* the first mapping's files start at
					 * 0, so its sections are relative to
					 * the image; later ones are absolute */
					*start = (text_off ? 0 : base) + le32(sec + 40);
					*size = le32(sec + 36);
					return;
				}
			}
		}
		at += len;
	}
	if (text_size) {
		/* a shared cache's first segment often has fileoff 0: the
		 * image's own mapping base is the file offset then */
		*start = text_off ? text_off : base;
		*size = text_size;
	}
}

/*
 * How much a window looks like the event program.  It has to end with the
 * PDS end word and have no other end among the instructions before it; a
 * program that branches on the event has several data-outs and a few other
 * instructions, and its data segment is mostly empty.
 */
static int score_window(const uint32_t *w)
{
	unsigned int i, douts = 0, other = 0, empties = 0, last = EVENT_WORDS;
	unsigned int ends = 0;
	int score = 0;

	/* The program ends with the PDS end word, at the last word or one
	 * before it (a word of padding after it).  It branches on the event,
	 * so there can be more than one end: the paths after the first. */
	for (i = EVENT_WORDS; i-- > EVENT_DATA;)
		if (w[i] == PDS_END) {
			last = i;
			break;
		}
	if (last != EVENT_WORDS - 1 &&
	    !(last == EVENT_WORDS - 2 && !w[EVENT_WORDS - 1]))
		return -1;
	for (i = EVENT_DATA; i < last; i++) {
		if (w[i] == PDS_END) {
			ends++;
			continue;
		}
		if ((w[i] >> 27) == PDS_DOUT)
			douts++;
		else if (w[i])
			other++;
	}
	if (douts < 3 || other < 2 || ends > 4)
		return -1;
	score += 4 + douts - 2 * ends;
	for (i = 0; i < EVENT_DATA; i++)
		if (!w[i] || w[i] == PDS_END)
			empties++;
	if (empties >= 12)
		score += 2;
	/* the reference's patch points: three DOUTU placeholders at words
	 * 0/2/4 and one info word at 16, which the driver fills in.  A stored
	 * template leaves them empty; a window that starts in code or in a
	 * format table does not. */
	if (!w[0] && !w[2] && !w[4])
		score += 12;
	if (!w[16] || w[16] == 0x30000)
		score += 4;
	return score;
}

static void write_candidate(const char *dir, const struct cand *c)
{
	char path[1024];
	unsigned char b[EVENT_BYTES];
	unsigned int i;
	FILE *fp;

	snprintf(path, sizeof(path), "%s/gl-event-%08llx.pds", dir, c->off);
	fp = fopen(path, "wb");
	if (!fp)
		return;
	for (i = 0; i < EVENT_WORDS; i++) {
		b[4 * i] = c->w[i] & 0xff;
		b[4 * i + 1] = (c->w[i] >> 8) & 0xff;
		b[4 * i + 2] = (c->w[i] >> 16) & 0xff;
		b[4 * i + 3] = (c->w[i] >> 24) & 0xff;
	}
	fwrite(b, 1, sizeof(b), fp);
	fclose(fp);
	printf("  0x%08llx score %u: %08x %08x %08x %08x ... %08x\n", c->off,
	       c->score, c->w[0], c->w[1], c->w[2], c->w[3], c->w[EVENT_WORDS - 1]);
}

int main(int argc, char **argv)
{
	unsigned char chunk[1 << 20], win[EVENT_BYTES];
	uint32_t words[EVENT_WORDS];
	static struct cache cache;
	unsigned int carry = 0, ncand = 0, maxc = 64, all = 0, list = 0;
	const char *image = NULL;
	unsigned long long start = 0, size = 0, end;
	struct cand best = { 0 }, c;
	const char *path, *out;
	int opt, have_cache;
	unsigned long long base;
	FILE *fp;

	while ((opt = getopt(argc, argv, "aln:i:")) != -1) {
		switch (opt) {
		case 'a': all = 1; break;
		case 'l': list = 1; break;
		case 'n': maxc = strtoul(optarg, NULL, 0); break;
		case 'i': image = optarg; break;
		default:
			fprintf(stderr, "usage: %s [-a] [-n MAX] [-l] [-i NAME] "
				"CACHE OUT\n", argv[0]);
			return 2;
		}
	}
	if (argc - optind != 2) {
		fprintf(stderr, "usage: %s [-a] [-n MAX] [-l] [-i NAME] CACHE OUT\n",
			argv[0]);
		return 2;
	}
	path = argv[optind];
	out = argv[optind + 1];

	fp = fopen(path, "rb");
	if (!fp) {
		fprintf(stderr, "glcarve: %s: %s\n", path, strerror(errno));
		return 1;
	}
	have_cache = cache_open(fp, &cache) == 0;
	if (!have_cache && (list || image)) {
		fprintf(stderr, "glcarve: %s is not a code cache (no dyld_v1 header)\n",
			path);
		return 1;
	}

	if (list) {
		unsigned int i;

		for (i = 0; i < cache.nimage; i++) {
			char p[CACHE_PATH];

			if (cache_path(fp, &cache, i, p, sizeof(p)))
				continue;
			if (ci_contains(p, image ? image : ""))
				printf("%u 0x%08llx %s\n", i,
				       (unsigned long long)cache.image[i].addr, p);
		}
		fclose(fp);
		return 0;
	}

	if (image) {
		unsigned int i;

		for (i = 0; i < cache.nimage; i++) {
			char p[CACHE_PATH];

			if (cache_path(fp, &cache, i, p, sizeof(p)) ||
			    !ci_contains(p, image))
				continue;
			image_range(fp, &cache, i, &start, &size);
			printf("glcarve: image %u %s at 0x%08llx (0x%llx bytes)\n",
			       i, p, start, size);
			break;
		}
		if (i == cache.nimage) {
			fprintf(stderr, "glcarve: no image named %s; -l lists them\n",
				image);
			return 1;
		}
		if (!size) {
			fprintf(stderr, "glcarve: no __TEXT,__const in %s\n", image);
			return 1;
		}
	} else {
		if (fseeko(fp, 0, SEEK_END)) {
			fprintf(stderr, "glcarve: %s: %s\n", path, strerror(errno));
			return 1;
		}
		size = (unsigned long long)ftello(fp);
	}
	end = start + size;

	printf("glcarve: scanning 0x%llx bytes at 0x%llx for a %u-byte event PDS "
	       "template\n", size, start, EVENT_BYTES);
	base = start;
	if (fseeko(fp, (off_t)start, SEEK_SET)) {
		fprintf(stderr, "glcarve: %s: %s\n", path, strerror(errno));
		return 1;
	}
	for (;;) {
		unsigned long long have = base + carry;
		size_t want = sizeof(chunk) - carry;
		size_t n, total, at;

		if (have >= end)
			break;
		if (want > end - have)
			want = (size_t)(end - have);
		n = fread(chunk + carry, 1, want, fp);
		total = carry + n;
		if (!n)
			break;
		for (at = 0; at + EVENT_BYTES <= total; at += 4) {
			unsigned int i;
			int score;

			memcpy(win, chunk + at, EVENT_BYTES);
			for (i = 0; i < EVENT_WORDS; i++)
				words[i] = le32(win + 4 * i);
			score = score_window(words);
			if (score < 0)
				continue;
			memset(&c, 0, sizeof(c));
			c.off = base + at;
			c.score = score;
			memcpy(c.w, words, sizeof(words));
			if (!ncand || score > (int)best.score)
				best = c;
			ncand++;
			if (all)
				write_candidate(out, &c);
			if (ncand >= maxc)
				goto done;
		}
		/* keep the last 41 words for the next chunk */
		carry = total - at;
		if (carry > EVENT_BYTES - 4)
			carry = EVENT_BYTES - 4;
		memmove(chunk, chunk + total - carry, carry);
		base += total - carry;
	}
done:
	fclose(fp);
	if (!ncand) {
		fprintf(stderr, "glcarve: no candidate\n");
		return 1;
	}

	if (!all) {
		FILE *o = fopen(out, "wb");
		unsigned char b[EVENT_BYTES];
		unsigned int i;

		if (!o) {
			fprintf(stderr, "glcarve: %s: %s\n", out, strerror(errno));
			return 1;
		}
		for (i = 0; i < EVENT_WORDS; i++) {
			b[4 * i] = best.w[i] & 0xff;
			b[4 * i + 1] = (best.w[i] >> 8) & 0xff;
			b[4 * i + 2] = (best.w[i] >> 16) & 0xff;
			b[4 * i + 3] = (best.w[i] >> 24) & 0xff;
		}
		fwrite(b, 1, sizeof(b), o);
		fclose(o);
		printf("glcarve: wrote %s from 0x%08llx (score %u, %u candidate%s)\n",
		       out, best.off, best.score, ncand, ncand == 1 ? "" : "s");
	} else {
		printf("glcarve: %u candidate%s in %s\n", ncand, ncand == 1 ? "" : "s",
		       out);
	}
	return 0;
}
