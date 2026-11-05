// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bare-metal FMI0 + Central DMA (CDMA) page reader for the S5L8947X
 * Stage-1 NAND boot loader.
 *
 * Uses the exact register model from drivers/mtd/devices/s5l8947x-fmi.c and
 * drivers/dma/s5l8947x-cdma.c:
 *   FMI0 host       0x31200000 (data FIFO +0x14, meta FIFO +0x18)
 *   FMI0 sequencer  0x31240000
 *   FMI0 ECC        0x31280000
 *   CDMA global     0x37000000
 *   CDMA channel 5  0x37005000 (FMI0 data, request 0, 32-bit width)
 *   CDMA channel 6  0x37006000 (FMI0 meta, request 1, 8-bit width)
 */

#include "manifest.h"

#define FMI_HOST	0x31200000U
#define FMI_SEQ		0x31240000U
#define FMI_ECC		0x31280000U

#define FMI_H_CFG	0x00U
#define FMI_H_CMD	0x04U
#define FMI_H_STAT	0x0cU
#define FMI_H_IRQEN	0x10U
#define FMI_H_DATA	0x14U
#define FMI_H_META	0x18U
#define FMI_H_FIFO	0x1cU
#define FMI_H_SHAPE	0x34U

#define FMI_S_CTRL	0x00U
#define FMI_S_TIMING	0x08U
#define FMI_S_CE	0x0cU
#define FMI_S_GO	0x10U
#define FMI_S_CMD	0x14U
#define FMI_S_ADDR0	0x18U
#define FMI_S_ADDR1	0x1cU
#define FMI_S_NADDR	0x20U
#define FMI_S_24	0x24U
#define FMI_S_IRQEN	0x40U
#define FMI_S_STAT	0x44U
#define FMI_S_NSTAT	0x48U
#define FMI_S_MATCH	0x4cU

#define FMI_E_SECT	0x0cU
#define FMI_E_14	0x14U

#define MN_CMD_RESET		0xffU
#define MN_CMD_READ_ID		0x90U
#define MN_CMD_SET_FEATURE	0xefU
#define MN_CMD_CONFIRM		0xe7U
#define MN_CMD_READ		0x370aU
#define MN_CMD_PROGRAM		0x8aU
#define MN_CMD_PROG_CONFIRM	0x17U
#define MN_CMD_ERASE		0xd76aU
#define MN_CMD_OP_STATUS	0x7d77U
#define MN_CMD_NEXT_STATUS	0x79U
#define MN_CMD_DATA_OUT		0x7aU
#define MN_CMD_END		0x77U

#define MN_ST_FAIL		0x01U
#define MN_ST_GENERAL_ERROR	0x10U

#define CDMA_BASE	0x37000000U
#define CDMA_CH5	0x37005000U
#define CDMA_CH6	0x37006000U

#define CDMA_CSR	0x00U
#define CDMA_DCR	0x04U
#define CDMA_DAR	0x08U
#define CDMA_DBR	0x0cU
#define CDMA_CAR	0x14U

#define CDMA_CSR_RUN		(1U << 0)
#define CDMA_CSR_ABORT		(1U << 1)
#define CDMA_CSR_KICK		(1U << 2)
#define CDMA_CSR_DONE_IE	(1U << 3)
#define CDMA_CSR_ERR_IE		(1U << 4)
#define CDMA_CSR_ACTIVE		(1U << 16)
#define CDMA_CSR_ERR		(1U << 18)
#define CDMA_CSR_DONE		(1U << 19)
#define CDMA_CSR_SEG		(1U << 20)
#define CDMA_CSR_READY		(1U << 21)
#define CDMA_CSR_STATUS		(CDMA_CSR_ERR | CDMA_CSR_DONE | CDMA_CSR_SEG)
#define CDMA_CSR_START		((2U << 24) | CDMA_CSR_DONE_IE | CDMA_CSR_ERR_IE | \
				 CDMA_CSR_ABORT | CDMA_CSR_RUN)

struct cdma_hw_seg {
	uint32_t next;
	uint32_t ctl;
	uint32_t addr;
	uint32_t len;
	uint32_t reserved[4];
} __attribute__((aligned(32)));

static struct cdma_hw_seg data_segs[5];
static struct cdma_hw_seg meta_segs[2];
static uint8_t dummy_meta[FMI_OOB_SIZE] __attribute__((aligned(32)));

static inline void mmio_w32(uint32_t addr, uint32_t val)
{
	*(volatile uint32_t *)addr = val;
}

static inline uint32_t mmio_r32(uint32_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void mmio_w8(uint32_t addr, uint8_t val)
{
	*(volatile uint8_t *)addr = val;
}

static inline uint8_t mmio_r8(uint32_t addr)
{
	return *(volatile uint8_t *)addr;
}

static void delay_loops(uint32_t loops)
{
	while (loops--)
		__asm__ volatile("nop");
}

static void fmi_quiesce(void)
{
	mmio_w32(FMI_SEQ + FMI_S_IRQEN, 0);
	mmio_w32(FMI_HOST + FMI_H_IRQEN, 0);
	mmio_w32(FMI_SEQ + FMI_S_STAT, 0x31ffffU);
	mmio_w32(FMI_HOST + FMI_H_STAT, 0xfU);
}

static void fmi_ce(int on)
{
	mmio_w32(FMI_SEQ + FMI_S_CE, on ? 1U : 0U);
}

static int fmi_wait_seq_idle(void)
{
	uint32_t i;

	for (i = 0; i < 2000000U; i++) {
		if (!(mmio_r32(FMI_SEQ + FMI_S_GO) & 0xbU))
			return 0;
	}
	return -1;
}

static int fmi_cmd(uint32_t cmd, uint32_t go, uint32_t addr0, uint32_t addr1,
		   unsigned int naddr)
{
	mmio_w32(FMI_SEQ + FMI_S_CMD, cmd);
	if (go & (1U << 3)) {
		mmio_w32(FMI_SEQ + FMI_S_ADDR0, addr0);
		mmio_w32(FMI_SEQ + FMI_S_ADDR1, addr1);
		mmio_w32(FMI_SEQ + FMI_S_NADDR, naddr - 1U);
	}
	mmio_w32(FMI_SEQ + FMI_S_STAT, 0x31ffffU);
	mmio_w32(FMI_SEQ + FMI_S_GO, go);
	return fmi_wait_seq_idle();
}

static int fmi_status_from(uint32_t cmd, uint32_t go)
{
	uint32_t i;

	mmio_w32(FMI_SEQ + FMI_S_MATCH, 0x4040U);
	mmio_w32(FMI_SEQ + FMI_S_24, 0);
	if (fmi_cmd(cmd, go | 0x50U, 0, 0, 0))
		return -1;
	for (i = 0; i < 2000000U; i++) {
		if (mmio_r32(FMI_SEQ + FMI_S_STAT) & (1U << 5)) {
			mmio_w32(FMI_SEQ + FMI_S_STAT, 0x31ffffU);
			return (int)(mmio_r32(FMI_SEQ + FMI_S_NSTAT) & 0xffU);
		}
	}
	return -1;
}

static int fmi_pio(void *buf, unsigned int len, int write)
{
	uint32_t *w = (uint32_t *)buf;
	uint8_t *b = (uint8_t *)buf;
	uint32_t shape = ((len & 0x7ffU) << 8) | 1U;
	uint32_t words = len / 4U;
	uint32_t i, t;

	mmio_w32(FMI_HOST + FMI_H_CMD, 6U);
	fmi_quiesce();
	mmio_w32(FMI_HOST + FMI_H_CFG, 5U);
	mmio_w32(FMI_HOST + FMI_H_SHAPE, shape);
	mmio_w32(FMI_HOST + FMI_H_CMD, write ? 5U : 3U);

	if (write) {
		for (i = 0; i < words; i++)
			mmio_w32(FMI_HOST + FMI_H_DATA, w[i]);
		for (i = words * 4U; i < len; i++)
			mmio_w8(FMI_HOST + FMI_H_DATA, b[i]);
	} else {
		for (t = 0; t < 2000000U; t++) {
			if (mmio_r32(FMI_HOST + FMI_H_FIFO) & 0x18U)
				break;
		}
		for (i = 0; i < words; i++)
			w[i] = mmio_r32(FMI_HOST + FMI_H_DATA);
		for (i = words * 4U; i < len; i++)
			b[i] = mmio_r8(FMI_HOST + FMI_H_DATA);
	}

	for (t = 0; t < 2000000U; t++) {
		if (mmio_r32(FMI_HOST + FMI_H_STAT) & 2U) {
			mmio_w32(FMI_HOST + FMI_H_STAT, 2U);
			return 0;
		}
	}
	return -1;
}

static void fmi_op_end(void)
{
	fmi_cmd(MN_CMD_END, 1U, 0, 0, 0);
	fmi_ce(0);
	fmi_quiesce();
}

static int fmi_set_feature(uint16_t feat, uint32_t val)
{
	uint8_t buf[4] = {
		(uint8_t)val,
		(uint8_t)(val >> 8),
		(uint8_t)(val >> 16),
		(uint8_t)(val >> 24),
	};
	int st;

	fmi_ce(1);
	fmi_quiesce();
	if (fmi_cmd(MN_CMD_SET_FEATURE, 1U | 8U, feat, 0, 2) ||
	    fmi_pio(buf, 4, 1) ||
	    fmi_cmd(MN_CMD_CONFIRM, 1U, 0, 0, 0)) {
		fmi_op_end();
		return -1;
	}
	st = fmi_status_from(MN_CMD_OP_STATUS, 3U);
	fmi_op_end();
	return (st < 0 || (st & (MN_ST_FAIL | MN_ST_GENERAL_ERROR))) ? -1 : 0;
}

static void cdma_abort_chan(uint32_t base)
{
	uint32_t i;

	mmio_w32(base + CDMA_CSR, CDMA_CSR_ABORT);
	for (i = 0; i < 100000U; i++) {
		if (!(mmio_r32(base + CDMA_CSR) & CDMA_CSR_ACTIVE))
			break;
	}
	mmio_w32(base + CDMA_CSR, CDMA_CSR_STATUS);
}

static void cdma_start_chan(uint32_t base, uint32_t dcr, uint32_t dar,
			    uint32_t first_seg_pa)
{
	uint32_t i;

	cdma_abort_chan(base);
	mmio_w32(base + CDMA_DCR, dcr);
	mmio_w32(base + CDMA_DAR, dar);
	mmio_w32(base + CDMA_DBR, 0);
	mmio_w32(base + CDMA_CAR, first_seg_pa);
	mmio_w32(base + CDMA_CSR, mmio_r32(base + CDMA_CSR) | CDMA_CSR_KICK);
	for (i = 0; i < 100000U; i++) {
		if (mmio_r32(base + CDMA_CSR) & CDMA_CSR_READY)
			break;
	}
	mmio_w32(base + CDMA_CSR, CDMA_CSR_START);
}

int fmi_boot_init(void)
{
	uint8_t id[8] __attribute__((aligned(4)));
	int st;

	/* Enable CDMA channels 5 and 6 in the global enable-set register */
	mmio_w32(CDMA_BASE + 0x00U, (1U << 5) | (1U << 6));
	cdma_abort_chan(CDMA_CH5);
	cdma_abort_chan(CDMA_CH6);

	/* Reset FMI0 controller with conservative slow timing first */
	mmio_w32(FMI_HOST + FMI_H_CMD, 6U);
	mmio_w32(FMI_SEQ + FMI_S_CTRL, 1U);
	mmio_w32(FMI_SEQ + FMI_S_TIMING, 0xffffU);
	mmio_w32(FMI_ECC + FMI_E_14, 0);
	fmi_quiesce();

	/* Reset managed NAND package on CE0 */
	fmi_ce(1);
	fmi_quiesce();
	fmi_cmd(MN_CMD_RESET, 1U, 0, 0, 0);
	delay_loops(2000000U);
	st = fmi_status_from(MN_CMD_OP_STATUS, 3U);
	fmi_op_end();
	if (st < 0 || (st & MN_ST_GENERAL_ERROR))
		return -1;

	/* Verify PPN signature (0x50, 0x50, 0x4e) */
	fmi_ce(1);
	fmi_quiesce();
	if (fmi_cmd(MN_CMD_READ_ID, 1U | 8U, 0, 0, 1) ||
	    fmi_pio(id, 8, 0)) {
		fmi_ce(0);
		return -1;
	}
	fmi_ce(0);
	if (id[0] != 0x50 || id[1] != 0x50 || id[2] != 0x4e)
		return -1;

	/* Move package from low-power to asynchronous interface mode */
	if (fmi_set_feature(0x0180U, 2U))
		return -1;

	/* Set measured fast asynchronous timing (0x2111) */
	mmio_w32(FMI_SEQ + FMI_S_TIMING, 0x2111U);
	return 0;
}

int fmi_boot_read_page(uint32_t row, void *data_dst, void *meta_dst)
{
	uint32_t data_pa = (uint32_t)data_dst;
	uint32_t meta_pa = (uint32_t)(meta_dst ? meta_dst : dummy_meta);
	uint32_t shape;
	uint32_t i, t;
	int st;

	fmi_ce(1);
	fmi_quiesce();
	if (fmi_cmd(MN_CMD_READ, 1U | 8U | 2U, row, 0, 4)) {
		fmi_op_end();
		return -1;
	}
	st = fmi_status_from(MN_CMD_OP_STATUS, 3U);
	if (st < 0 || (st & MN_ST_GENERAL_ERROR)) {
		fmi_op_end();
		return -1;
	}
	if (fmi_cmd(MN_CMD_DATA_OUT, 1U, 0, 0, 0)) {
		fmi_op_end();
		return -1;
	}

	/* Build 4 x 4 KiB data segments + stop entry */
	for (i = 0; i < 4U; i++) {
		data_segs[i].next = (uint32_t)&data_segs[i + 1U];
		data_segs[i].ctl = (i == 3U) ? 0x02000303U : 0x02000003U;
		data_segs[i].addr = data_pa + i * 4096U;
		data_segs[i].len = 4096U;
	}
	data_segs[4].next = 0;
	data_segs[4].ctl = 0;
	data_segs[4].addr = 0;
	data_segs[4].len = 0;

	/* Build 1 x 64 B metadata segment + stop entry */
	meta_segs[0].next = (uint32_t)&meta_segs[1];
	meta_segs[0].ctl = 0x02000303U;
	meta_segs[0].addr = meta_pa;
	meta_segs[0].len = FMI_OOB_SIZE;
	meta_segs[1].next = 0;
	meta_segs[1].ctl = 0;
	meta_segs[1].addr = 0;
	meta_segs[1].len = 0;

	__asm__ volatile("dsb sy" ::: "memory");

	cdma_start_chan(CDMA_CH5, (0U << 16) | (1U << 7) | (2U << 2),
			0x00200014U, (uint32_t)&data_segs[0]);
	cdma_start_chan(CDMA_CH6, (1U << 16) | (1U << 7),
			0x00200018U, (uint32_t)&meta_segs[0]);

	mmio_w32(FMI_HOST + FMI_H_CMD, 6U);
	fmi_quiesce();
	mmio_w32(FMI_ECC + FMI_E_SECT, 1U);
	mmio_w32(FMI_SEQ + FMI_S_TIMING, 0x2111U | (1U << 20));
	mmio_w32(FMI_HOST + FMI_H_CFG, 3U);

	/* 4 logical pages of 4 x 1024B sectors + 16B meta */
	shape = 4U | (1024U << 8) | (16U << 19) | (16U << 25);
	mmio_w32(FMI_HOST + FMI_H_SHAPE, shape);

	for (i = 0; i < 4U; i++) {
		mmio_w32(FMI_HOST + FMI_H_STAT, 2U);
		mmio_w32(FMI_HOST + FMI_H_CMD, 3U);
		for (t = 0; t < 2000000U; t++) {
			if (mmio_r32(FMI_HOST + FMI_H_STAT) & 2U)
				break;
		}
		if (t == 2000000U) {
			cdma_abort_chan(CDMA_CH5);
			cdma_abort_chan(CDMA_CH6);
			mmio_w32(FMI_SEQ + FMI_S_TIMING, 0x2111U);
			fmi_op_end();
			return -1;
		}
		mmio_w32(FMI_HOST + FMI_H_STAT, 2U);
		mmio_w32(FMI_ECC + FMI_E_SECT, 1U);
	}

	for (t = 0; t < 2000000U; t++) {
		if ((mmio_r32(CDMA_CH5 + CDMA_CSR) & (CDMA_CSR_DONE | CDMA_CSR_SEG)) &&
		    (mmio_r32(CDMA_CH6 + CDMA_CSR) & (CDMA_CSR_DONE | CDMA_CSR_SEG)))
			break;
	}
	mmio_w32(CDMA_CH5 + CDMA_CSR, CDMA_CSR_STATUS);
	mmio_w32(CDMA_CH6 + CDMA_CSR, CDMA_CSR_STATUS);
	mmio_w32(FMI_SEQ + FMI_S_TIMING, 0x2111U);
	fmi_op_end();

	return (st == 0x40 || st == 0x42 || st == 0x49) ? 0 : -1;
}
