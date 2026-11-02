// SPDX-License-Identifier: GPL-2.0-only
/*
 * nand-restore: Clean-room raw NAND restore and verification utility for
 * S5L8947X managed NAND (/dev/mtd0).
 *
 * Reads a raw dump stream of (16,384 B data + 64 B metadata = 16,448 B) per
 * page from stdin (or a file), erases each target block via MEMERASE, and
 * programs non-blank pages (data + metadata together) via MEMWRITE.
 *
 * Usage:
 *   nand-restore [-s] [-b start_eb] [-n num_ebs] [-f dump.bin] /dev/mtd0
 *
 * Options:
 *   -s           Program and erase in SLC mode (sets MTD_OPS_RAW and toggles
 *                /sys/module/s5l8947x_fmi/parameters/raw_erase_slc)
 *   -b start_eb  Starting eraseblock index on the MTD device (default: 0)
 *   -n num_ebs   Maximum number of eraseblocks to restore (default: until EOF)
 *   -f file      Read input stream from file instead of stdin
 */

#include <errno.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define PAGE_DATA_SIZE	16384U
#define PAGE_OOB_SIZE	64U
#define PAGE_RAW_SIZE	(PAGE_DATA_SIZE + PAGE_OOB_SIZE)

static int set_raw_erase_slc(int enable)
{
	const char *path = "/sys/module/s5l8947x_fmi/parameters/raw_erase_slc";
	const char val = enable ? 'Y' : 'N';
	int fd = open(path, O_WRONLY);

	if (fd < 0)
		return -1;
	if (write(fd, &val, 1) != 1) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static ssize_t read_full(int fd, void *buf, size_t len)
{
	uint8_t *p = (uint8_t *)buf;
	size_t done = 0;

	while (done < len) {
		ssize_t n = read(fd, p + done, len - done);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			break;
		done += (size_t)n;
	}
	return (ssize_t)done;
}

static int page_is_blank(const uint8_t *raw)
{
	size_t i;

	for (i = 0; i < PAGE_RAW_SIZE; i++) {
		if (raw[i] != 0xffU)
			return 0;
	}
	return 1;
}

int main(int argc, char **argv)
{
	const char *in_path = NULL;
	const char *mtd_path = NULL;
	uint32_t start_eb = 0;
	uint32_t max_ebs = 0;
	int slc_raw = 0;
	struct mtd_info_user mi;
	uint32_t pages_per_eb, total_ebs, eb, pg;
	uint32_t erased = 0, programmed = 0, skipped = 0;
	uint8_t *eb_buf = NULL;
	int in_fd = STDIN_FILENO;
	int mtd_fd = -1;
	int opt, ret = 1;

	while ((opt = getopt(argc, argv, "sb:n:f:")) != -1) {
		switch (opt) {
		case 's':
			slc_raw = 1;
			break;
		case 'b':
			start_eb = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'n':
			max_ebs = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'f':
			in_path = optarg;
			break;
		default:
			fprintf(stderr,
				"Usage: %s [-s] [-b start_eb] [-n num_ebs] [-f file] /dev/mtdX\n",
				argv[0]);
			return 2;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, "Missing MTD device path\n");
		return 2;
	}
	mtd_path = argv[optind];

	if (in_path) {
		in_fd = open(in_path, O_RDONLY);
		if (in_fd < 0) {
			perror(in_path);
			return 1;
		}
	}

	mtd_fd = open(mtd_path, O_RDWR);
	if (mtd_fd < 0) {
		perror(mtd_path);
		goto out;
	}

	if (ioctl(mtd_fd, MEMGETINFO, &mi) < 0) {
		perror("MEMGETINFO");
		goto out;
	}
	if (mi.writesize != PAGE_DATA_SIZE || mi.oobsize != PAGE_OOB_SIZE ||
	    mi.erasesize == 0 || (mi.erasesize % mi.writesize) != 0) {
		fprintf(stderr, "Unexpected MTD geometry: writesize=%u oobsize=%u erasesize=%u\n",
			mi.writesize, mi.oobsize, mi.erasesize);
		goto out;
	}

	pages_per_eb = mi.erasesize / mi.writesize;
	total_ebs = mi.size / mi.erasesize;
	if (start_eb >= total_ebs) {
		fprintf(stderr, "start_eb %u >= total_ebs %u\n", start_eb, total_ebs);
		goto out;
	}

	eb_buf = malloc((size_t)pages_per_eb * PAGE_RAW_SIZE);
	if (!eb_buf) {
		perror("malloc");
		goto out;
	}

	if (slc_raw)
		set_raw_erase_slc(1);

	for (eb = start_eb; eb < total_ebs; eb++) {
		size_t want = (size_t)pages_per_eb * PAGE_RAW_SIZE;
		ssize_t got;
		struct erase_info_user ei;

		if (max_ebs && (eb - start_eb) >= max_ebs)
			break;

		got = read_full(in_fd, eb_buf, want);
		if (got == 0)
			break;
		if ((size_t)got != want) {
			fprintf(stderr, "Short read at EB %u (%zd / %zu bytes)\n",
				eb, got, want);
			goto out_slc;
		}

		ei.start = eb * mi.erasesize;
		ei.length = mi.erasesize;
		if (ioctl(mtd_fd, MEMERASE, &ei) < 0) {
			fprintf(stderr, "MEMERASE failed at EB %u (offset %#x): %s\n",
				eb, ei.start, strerror(errno));
			goto out_slc;
		}
		erased++;

		for (pg = 0; pg < pages_per_eb; pg++) {
			uint8_t *raw = eb_buf + (size_t)pg * PAGE_RAW_SIZE;
			struct mtd_write_req req;

			if (page_is_blank(raw)) {
				skipped++;
				continue;
			}
			memset(&req, 0, sizeof(req));
			req.start = (uint64_t)eb * mi.erasesize +
				    (uint64_t)pg * PAGE_DATA_SIZE;
			req.len = PAGE_DATA_SIZE;
			req.ooblen = PAGE_OOB_SIZE;
			req.usr_data = (uintptr_t)raw;
			req.usr_oob = (uintptr_t)(raw + PAGE_DATA_SIZE);
			req.mode = slc_raw ? MTD_OPS_RAW : MTD_OPS_PLACE_OOB;

			if (ioctl(mtd_fd, MEMWRITE, &req) < 0) {
				fprintf(stderr,
					"MEMWRITE failed at EB %u page %u: %s\n",
					eb, pg, strerror(errno));
				goto out_slc;
			}
			programmed++;
		}
	}

	printf("nand-restore: erased %u EBs, programmed %u pages, skipped %u blank pages\n",
	       erased, programmed, skipped);
	ret = 0;

out_slc:
	if (slc_raw)
		set_raw_erase_slc(0);
out:
	free(eb_buf);
	if (mtd_fd >= 0)
		close(mtd_fd);
	if (in_path && in_fd >= 0)
		close(in_fd);
	return ret;
}
