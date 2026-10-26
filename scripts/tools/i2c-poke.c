// SPDX-License-Identifier: GPL-2.0-only
/*
 * Userspace I2C diagnostic utility (/bin/i2cpoke)
 *
 * Usage:
 *   i2cpoke <bus> scan
 *   i2cpoke <bus> <addr7> <reg8> [count]
 *   i2cpoke <bus> <addr7> <reg8> = <val8>
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int i2c_read_reg8(int fd, uint8_t addr, uint8_t reg, uint8_t *buf,
			 uint16_t len)
{
	struct i2c_msg msgs[2];
	struct i2c_rdwr_ioctl_data rdwr;

	msgs[0].addr = addr;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = &reg;

	msgs[1].addr = addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = buf;

	rdwr.msgs = msgs;
	rdwr.nmsgs = 2;
	return ioctl(fd, I2C_RDWR, &rdwr);
}

static int i2c_write_reg8(int fd, uint8_t addr, uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	struct i2c_msg msg;
	struct i2c_rdwr_ioctl_data rdwr;

	msg.addr = addr;
	msg.flags = 0;
	msg.len = 2;
	msg.buf = buf;

	rdwr.msgs = &msg;
	rdwr.nmsgs = 1;
	return ioctl(fd, I2C_RDWR, &rdwr);
}

int main(int argc, char **argv)
{
	char devpath[32];
	int bus, fd;

	if (argc < 3) {
		fprintf(stderr,
			"Usage:\n"
			"  %s <bus> scan\n"
			"  %s <bus> <addr7> <reg8> [count]\n"
			"  %s <bus> <addr7> <reg8> = <val8>\n",
			argv[0], argv[0], argv[0]);
		return 1;
	}

	bus = (int)strtol(argv[1], NULL, 0);
	snprintf(devpath, sizeof(devpath), "/dev/i2c-%d", bus);
	fd = open(devpath, O_RDWR);
	if (fd < 0) {
		perror(devpath);
		return 1;
	}

	if (strcmp(argv[2], "scan") == 0) {
		int a;

		for (a = 0x08; a <= 0x77; a++) {
			uint8_t dummy = 0;

			if (i2c_read_reg8(fd, (uint8_t)a, 0x00, &dummy, 1) >= 0)
				printf("0x%02x: ACK (reg0=0x%02x)\n", a, dummy);
		}
		close(fd);
		return 0;
	}

	if (argc < 4) {
		close(fd);
		return 1;
	}

	{
		uint8_t addr = (uint8_t)strtoul(argv[2], NULL, 0);
		uint8_t reg = (uint8_t)strtoul(argv[3], NULL, 0);

		if (argc >= 6 && strcmp(argv[4], "=") == 0) {
			uint8_t val = (uint8_t)strtoul(argv[5], NULL, 0);
			uint8_t rb = 0;

			if (i2c_write_reg8(fd, addr, reg, val) < 0) {
				perror("I2C_RDWR write");
				close(fd);
				return 1;
			}
			if (i2c_read_reg8(fd, addr, reg, &rb, 1) >= 0)
				printf("i2c-%d 0x%02x[0x%02x] <= 0x%02x (readback 0x%02x)\n",
				       bus, addr, reg, val, rb);
			else
				printf("i2c-%d 0x%02x[0x%02x] <= 0x%02x\n",
				       bus, addr, reg, val);
		} else {
			unsigned int count = 1, i;
			uint8_t buf[16];

			if (argc >= 5)
				count = (unsigned int)strtoul(argv[4], NULL, 0);
			if (count == 0)
				count = 1;
			if (count > sizeof(buf))
				count = sizeof(buf);

			if (i2c_read_reg8(fd, addr, reg, buf, (uint16_t)count) < 0) {
				perror("I2C_RDWR read");
				close(fd);
				return 1;
			}
			printf("i2c-%d 0x%02x[0x%02x..0x%02x]:",
			       bus, addr, reg, reg + count - 1);
			for (i = 0; i < count; i++)
				printf(" %02x", buf[i]);
			printf("\n");
		}
	}

	close(fd);
	return 0;
}
