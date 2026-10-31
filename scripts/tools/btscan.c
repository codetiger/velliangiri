// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bluetooth LE scan over a raw HCI socket (/bin/btscan), so the radio can
 * be checked without a Bluetooth userland in the image
 *
 * Usage:
 *   btscan [seconds] [hci index]
 *
 * Brings the controller up, runs a passive LE scan and prints every
 * advertiser it hears. Exits 0 when at least one advertiser was heard.
 */

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH		31
#endif
#define BTPROTO_HCI		1
#define SOL_HCI			0
#define HCI_FILTER		2
#define HCI_CHANNEL_RAW		0
#define HCIDEVUP		_IOW('H', 201, int)

#define HCI_COMMAND_PKT		0x01
#define HCI_EVENT_PKT		0x04

#define EVT_CMD_COMPLETE	0x0e
#define EVT_CMD_STATUS		0x0f
#define EVT_LE_META		0x3e
#define LE_ADV_REPORT		0x02

#define OP_READ_BD_ADDR		0x1009
#define OP_LE_SET_SCAN_PARAM	0x200b
#define OP_LE_SET_SCAN_ENABLE	0x200c
#define OP_BCM_WRITE_BD_ADDR	0xfc01

#define MAX_DEVS		64

struct sockaddr_hci {
	unsigned short hci_family;
	unsigned short hci_dev;
	unsigned short hci_channel;
};

struct hci_filter {
	uint32_t type_mask;
	uint32_t event_mask[2];
	uint16_t opcode;
};

struct seen {
	uint8_t addr[6];
	uint8_t addr_type;
	int8_t rssi;
	unsigned int reports;
};

static struct seen devs[MAX_DEVS];
static unsigned int ndevs, nreports;

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void fmt_addr(char *buf, const uint8_t *a)
{
	sprintf(buf, "%02x:%02x:%02x:%02x:%02x:%02x",
		a[5], a[4], a[3], a[2], a[1], a[0]);
}

static void note_report(const uint8_t *p, int len)
{
	unsigned int i;
	int dlen;

	/* evt_type, addr_type, addr[6], data_len, data[], rssi */
	if (len < 10)
		return;
	dlen = p[8];
	if (len < 10 + dlen)
		return;
	nreports++;

	for (i = 0; i < ndevs; i++) {
		if (!memcmp(devs[i].addr, p + 2, 6)) {
			devs[i].reports++;
			devs[i].rssi = (int8_t)p[9 + dlen];
			return;
		}
	}
	if (ndevs == MAX_DEVS)
		return;
	memcpy(devs[ndevs].addr, p + 2, 6);
	devs[ndevs].addr_type = p[1];
	devs[ndevs].rssi = (int8_t)p[9 + dlen];
	devs[ndevs].reports = 1;
	ndevs++;
}

static void handle_event(const uint8_t *buf, int len)
{
	if (len < 3 || buf[0] != HCI_EVENT_PKT)
		return;
	/* One report per event in practice; take the first */
	if (buf[1] == EVT_LE_META && len >= 5 && buf[3] == LE_ADV_REPORT &&
	    buf[4] >= 1)
		note_report(buf + 5, len - 5);
}

/*
 * Send a command and wait for its completion; advertising reports that
 * arrive meanwhile are still counted. Returns the HCI status, or -1.
 */
static int hci_cmd(int fd, uint16_t op, const void *param, uint8_t plen,
		   uint8_t *ret, int retlen)
{
	uint8_t buf[260];
	double end;

	buf[0] = HCI_COMMAND_PKT;
	buf[1] = op & 0xff;
	buf[2] = op >> 8;
	buf[3] = plen;
	memcpy(buf + 4, param, plen);
	if (write(fd, buf, 4 + plen) != 4 + plen) {
		perror("btscan: write");
		return -1;
	}

	end = now() + 2.0;
	while (now() < end) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int n;

		if (poll(&pfd, 1, 200) <= 0)
			continue;
		n = read(fd, buf, sizeof(buf));
		if (n < 3 || buf[0] != HCI_EVENT_PKT)
			continue;
		if (buf[1] == EVT_CMD_COMPLETE && n >= 7 &&
		    (buf[4] | buf[5] << 8) == op) {
			if (ret && retlen > 0)
				memcpy(ret, buf + 7, n - 7 < retlen ? n - 7 : retlen);
			return buf[6];
		}
		if (buf[1] == EVT_CMD_STATUS && n >= 7 &&
		    (buf[5] | buf[6] << 8) == op && buf[3])
			return buf[3];
		handle_event(buf, n);
	}
	fprintf(stderr, "btscan: no completion for opcode 0x%04x\n", op);
	return -1;
}

int main(int argc, char **argv)
{
	struct sockaddr_hci sa = { .hci_family = AF_BLUETOOTH };
	struct hci_filter flt = { 0 };
	/* passive, 10 ms interval and window, public address, accept all */
	const uint8_t scan_param[] = { 0x00, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00 };
	uint8_t scan_on[] = { 0x01, 0x00 }, scan_off[] = { 0x00, 0x00 };
	uint8_t bdaddr[6];
	char addr[18];
	double secs = argc > 1 ? atof(argv[1]) : 10.0, end;
	int dev = argc > 2 ? atoi(argv[2]) : 0;
	unsigned int i;
	int fd, st;

	fd = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (fd < 0) {
		perror("btscan: socket");
		return 2;
	}
	if (ioctl(fd, HCIDEVUP, dev) < 0 && errno != EALREADY) {
		perror("btscan: bring up hci");
		return 2;
	}

	sa.hci_dev = dev;
	sa.hci_channel = HCI_CHANNEL_RAW;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("btscan: bind");
		return 2;
	}
	flt.type_mask = 1u << HCI_EVENT_PKT;
	flt.event_mask[0] = flt.event_mask[1] = 0xffffffff;
	if (setsockopt(fd, SOL_HCI, HCI_FILTER, &flt, sizeof(flt)) < 0) {
		perror("btscan: filter");
		return 2;
	}

	{
		FILE *fp = fopen("/sys/block/nand0/bmac", "r");
		unsigned int m[6];

		if (fp) {
			if (fscanf(fp, "%02x:%02x:%02x:%02x:%02x:%02x",
				   &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
				for (i = 0; i < 6; i++)
					bdaddr[i] = (uint8_t)m[5 - i];
				hci_cmd(fd, OP_BCM_WRITE_BD_ADDR, bdaddr, 6, NULL, 0);
			}
			fclose(fp);
		}
	}

	if (hci_cmd(fd, OP_READ_BD_ADDR, NULL, 0, bdaddr, sizeof(bdaddr)) == 0) {
		fmt_addr(addr, bdaddr);
		printf("hci%d address %s\n", dev, addr);
	}

	/* A scan left running by someone else rejects new parameters */
	hci_cmd(fd, OP_LE_SET_SCAN_ENABLE, scan_off, sizeof(scan_off), NULL, 0);
	st = hci_cmd(fd, OP_LE_SET_SCAN_PARAM, scan_param, sizeof(scan_param),
		     NULL, 0);
	if (st) {
		fprintf(stderr, "btscan: set scan parameters: status 0x%02x\n", st);
		return 2;
	}
	st = hci_cmd(fd, OP_LE_SET_SCAN_ENABLE, scan_on, sizeof(scan_on), NULL, 0);
	if (st) {
		fprintf(stderr, "btscan: enable scan: status 0x%02x\n", st);
		return 2;
	}

	end = now() + secs;
	while (now() < end) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		uint8_t buf[260];
		int n;

		if (poll(&pfd, 1, 200) <= 0)
			continue;
		n = read(fd, buf, sizeof(buf));
		if (n > 0)
			handle_event(buf, n);
	}
	hci_cmd(fd, OP_LE_SET_SCAN_ENABLE, scan_off, sizeof(scan_off), NULL, 0);
	close(fd);

	for (i = 0; i < ndevs; i++) {
		fmt_addr(addr, devs[i].addr);
		printf("%s %-6s rssi %4d dBm, %u reports\n", addr,
		       devs[i].addr_type ? "random" : "public", devs[i].rssi,
		       devs[i].reports);
	}
	printf("%u advertisers, %u reports in %.0f s\n", ndevs, nreports, secs);
	return ndevs ? 0 : 1;
}
