// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X PowerVR SGX543 GPU driver: power, GPU MMU and microkernel start-up
 *
 * Switches the PowerVR SGX543 (0x35100000, IRQ 49) clock/power domain,
 * releases the cores from reset with automatic clock gating and exposes the
 * master configuration registers.
 *
 * On request it also starts the GPU microkernel. The microkernel is not
 * shipped with Linux: stage2 carves it at boot from the stock system image on
 * the device's own System volume (scripts/tools/sgxcarve.c) into
 * /lib/firmware/s5l8947x/sgx543.fw. Starting it means
 *
 *   - building a two-level GPU page table (4 KiB pages, one directory shared
 *     by every requestor) over uncached buffers placed in fixed GPU-virtual
 *     ranges ("heaps"),
 *   - copying the firmware regions into those buffers and patching in the
 *     addresses the microkernel needs (its program contains 32-bit "load
 *     immediate" placeholders; event programs start with data words),
 *   - programming the memory interface, event and code-base registers, and
 *   - kicking core 0 and waiting until the microkernel sets bit 0 of word 0
 *     of the host control block.
 *
 * Nothing beyond that handshake (command submission, rendering) is provided.
 */

#define pr_fmt(fmt) "s5l8947x-sgx: " fmt

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

/*
 * SGX543 registers (offset from 0x35100000). The master block at 0x4000 stays
 * readable while the cores are clock gated; the per-core registers at the
 * bottom of the window hang the bus until the clock gating below has been
 * programmed, and not every offset there is implemented. Only offsets the
 * stock driver touches are used. Names are descriptive; several are inferred.
 */
#define SGX_CLKGATE0			0x0000
#define SGX_CLKGATE1			0x0004
#define SGX_CORE_RESET			0x0080
#define SGX_EVENT_ENABLE2		0x0110
#define SGX_EVENT_CLEAR2		0x0114
#define SGX_EVENT_STATUS2		0x0118
#define SGX_EVENT_STATUS		0x012c
#define SGX_EVENT_ENABLE		0x0130
#define SGX_EVENT_CLEAR			0x0134
#define SGX_EVENT_ENABLE3		0x013c
#define SGX_EVENT_CLEAR3		0x0140
#define SGX_CLKGATE_EXT			0x0310
#define SGX_USE_CODE_BASE(n)		(0x0a0c + 4 * (n))
#define SGX_USE_CODE_BASES		16
#define SGX_EVENT_OTHER_PDS_EXEC	0x0a68
#define SGX_EVENT_OTHER_PDS_DATA	0x0a6c
#define SGX_EVENT_OTHER_PDS_INFO	0x0a70
#define SGX_EVENT_KICKER		0x0ac4
#define SGX_EVENT_TIMER			0x0acc
#define SGX_BIF_CTRL			0x0c00
#define SGX_BIF_TILE1			0x0c10
#define SGX_BIF_TILE2			0x0c14
#define SGX_BIF_CTRL_INVAL		0x0c34
#define SGX_BIF_DIR_LIST_BASE(n)	((n) ? 0x0c34 + 4 * (n) : 0x0c84)
#define SGX_BIF_BANK_SET		0x0c74
#define SGX_BIF_BANK0			0x0c78
#define SGX_BIF_BANK1			0x0c7c
#define SGX_MASTER_CORES		0x4000
#define SGX_MASTER_CORE_EN		0x4004
#define SGX_MASTER_CFG0			0x4010
#define SGX_MASTER_CFG1			0x4014
#define SGX_MASTER_CLKGATE		0x4020
#define SGX_MASTER_RESET		0x4080
#define SGX_MASTER_BIF_CTRL		0x4c00
#define SGX_MASTER_BIF_4CD0		0x4cd0
#define SGX_MASTER_MEM0			0x4d00
#define SGX_MASTER_MEM1			0x4d04
#define SGX_MASTER_PDS_ENABLE		0x4a58
#define SGX_CORE(n)			(0x8000 + 0x4000 * (n))
#define SGX_CORE_PDS_ENABLE		0x0a58
#define SGX_CORE_PDS_EXEC		0x0a68
#define SGX_CORE_PDS_DATA		0x0a6c
#define SGX_CORE_PDS_INFO		0x0a70
#define SGX_CORE_EVENT_KICK		0x0ac8
#define SGX_CORE_BA0			0x0ba0
#define SGX_CORE_BB4			0x0bb4
#define SGX_CORE_BIF_CD0		0x0cd0

#define SGX_MASTER_CFG0_CORES		GENMASK(11, 8)
#define SGX_MASTER_CLKGATE_AUTO		0x00000155
#define SGX_MASTER_RESET_ALL		0x000004f0
#define SGX_MASTER_RESET_EXT		BIT(8)
#define SGX_MASTER_MEM0_VAL		0x0044c000
#define SGX_MASTER_MEM1_VAL		0x04001e40
#define SGX_CLKGATE0_AUTO		0x10155555
#define SGX_CLKGATE1_AUTO		0x05454555

/* Host-to-microkernel event the stock driver enables (bit 14) */
#define SGX_EVENT_UKERNEL		BIT(14)
#define SGX_EVENT_MASTER_ACK		BIT(31)

/* GPU MMU: 1024 directory entries of 4 MiB, 1024 table entries of 4 KiB */
#define SGX_PDE_VALID			BIT(0)
#define SGX_PTE_VALID			BIT(0)
#define SGX_PTE_READONLY		BIT(2)
#define SGX_PTE_BIT3			BIT(3)
#define SGX_PTE_BIT4			BIT(4)
#define SGX_PT_SHIFT			22
#define SGX_PT_ENTRIES			1024
#define SGX_PAGE			SZ_4K

/* Firmware file written by sgxcarve */
#define SGX_FW_NAME			"s5l8947x/sgx543.fw"
#define SGXF_MAGIC			0x46584753	/* "SGXF" */
#define SGXF_VERSION			1

enum sgx_fw_region {
	FW_BOOT = 1,		/* start-up program, code buffer offset 0 */
	FW_UKERNEL,		/* microkernel program, code buffer offset 0x1000 */
	FW_PDS_CORE,		/* per-core event program (0x18) */
	FW_PDS_A,		/* event program (0x38) */
	FW_PDS_B,		/* event program (0x18) */
	FW_PDS_C,		/* event program (0x350) */
	FW_PDS_D,		/* event program (0x9c) */
	FW_CODE2,		/* second program (0x3b58) */
	FW_PDS_OTHER,		/* "other" event program (600) */
	FW_CODE1,		/* program at code base 1 (0x140) */
	FW_PDS_E,		/* three event programs (0xf0) */
	FW_DESC,		/* descriptor block (0x28) */
	FW_REGIONS,
};

/* GPU-virtual ranges, as the stock driver lays them out */
struct sgx_heap {
	u32 base;
	u32 size;
};

static const struct sgx_heap sgx_heaps[] = {
	{ 0x84000000, 0x00080000 },	/* 0: kernel code (read-only) */
	{ 0x84800000, 0x02800000 },	/* 1: kernel data */
	{ 0x87800000, 0x04000000 },	/* 2 */
	{ 0x8c000000, 0x03000000 },	/* 3 */
	{ 0x90000000, 0x03000000 },	/* 4 */
	{ 0x94000000, 0x03000000 },	/* 5: user code (read-only) */
	{ 0x98000000, 0x48000000 },	/* 6: general */
};

/* PTE flag words as the stock driver passes them; see sgx_pte_bits() */
#define PF_CODE		1	/* read-only */
#define PF_DATA		6	/* bits 3 and 4 (heap 1 default) */
#define PF_PLAIN	0

enum sgx_buf_id {
	B_CODE,		/* boot + microkernel, code base 0 */
	B_PDS_CORE,
	B_PDS_A,
	B_PDS_B,
	B_PDS_C,
	B_PDS_D,
	B_CODE2,
	B_PDS_OTHER,
	B_CODE1,
	B_PDS_E,
	B_GLOBAL,	/* microkernel global block */
	B_CTL,		/* host control block; word 0 bit 0 = microkernel up */
	B_RING,		/* kernel command ring, 256 x 32 bytes */
	B_RING_IDX,	/* ring write/read indices */
	B_KICKER,	/* kick counter page */
	B_TRACE,
	B_DESC,
	B_TABLE,
	B_SCRATCH0,
	B_SCRATCH1,
	B_SCRATCH2,
	B_SCRATCH3,
	B_COUNT,
};

struct sgx_buf_desc {
	u32 size;
	u32 align;
	u8 heap;
	u8 flags;
	u8 fw;		/* firmware region copied in, 0 = zero-filled */
};

/* In the stock allocation order (first fit, one guard page each) */
static const struct sgx_buf_desc sgx_bufs[B_COUNT] = {
	[B_CODE]	= { 0x12f88, 0x40,     0, PF_CODE, 0 },
	[B_PDS_CORE]	= { 0x18,    0x1000,   1, PF_DATA, FW_PDS_CORE },
	[B_PDS_A]	= { 0x38,    0x1000,   1, PF_DATA, FW_PDS_A },
	[B_PDS_B]	= { 0x18,    0x1000,   1, PF_DATA, FW_PDS_B },
	[B_PDS_C]	= { 0x350,   0x1000,   1, PF_DATA, FW_PDS_C },
	[B_PDS_D]	= { 0x9c,    0x1000,   1, PF_DATA, FW_PDS_D },
	[B_CODE2]	= { 0x3b58,  0x40,     0, PF_CODE, FW_CODE2 },
	[B_PDS_OTHER]	= { 600,     0x1000,   1, PF_DATA, FW_PDS_OTHER },
	[B_CODE1]	= { 0x140,   0x40,     5, PF_CODE, FW_CODE1 },
	[B_PDS_E]	= { 0xf0,    0x1000,   6, PF_CODE, FW_PDS_E },
	[B_GLOBAL]	= { 0x1a4,   0x1000,   1, PF_DATA, 0 },
	[B_CTL]		= { 0xec,    0x1000,   1, PF_DATA, 0 },
	[B_RING]	= { 0x2000,  0x1000,   1, PF_DATA, 0 },
	[B_RING_IDX]	= { 8,       0x1000,   1, PF_DATA, 0 },
	[B_KICKER]	= { 0x1000,  0x1000,   1, PF_DATA, 0 },
	[B_TRACE]	= { 0x1500c, 0x1000,   1, PF_DATA, 0 },
	[B_DESC]	= { 0x28,    0x1000,   4, PF_CODE, FW_DESC },
	[B_TABLE]	= { 0x60,    0x1000,   6, PF_CODE, 0 },
	[B_SCRATCH0]	= { 0x14000, SZ_1M,    2, PF_PLAIN, 0 },
	[B_SCRATCH1]	= { 0x1000,  0x1000,   6, PF_PLAIN, 0 },
	[B_SCRATCH2]	= { 0x8000,  0x1000,   6, PF_PLAIN, 0 },
	[B_SCRATCH3]	= { 0x28000, 0x1000,   6, PF_PLAIN, 0 },
};

/* Code buffer layout */
#define CODE_UKERNEL			0x1000
#define CODE_UKERNEL_ENTRY		0x9d40	/* from the microkernel start */
#define CODE2_ENTRY_OTHER		0x30
#define CODE2_ENTRY_BOOT		0x70

/* Microkernel global block */
#define GLB_SELF			0x00
#define GLB_CTL				0x04
#define GLB_RING_IDX			0x08
#define GLB_14				0x14
#define GLB_KL_INV			0xc8
#define GLB_CC				0xcc
#define GLB_E0				0xe0
#define GLB_RING			0xf4
#define GLB_PD_PHYS			0xf8
#define GLB_TRACE			0x154

/* Host control block */
#define CTL_STATUS			0x00
#define CTL_STATUS_UP			BIT(0)
#define CTL_14				0x14
#define CTL_CLOCK_KHZ			0x18
#define CTL_1C				0x1c

struct sgx_buf {
	void *cpu;
	dma_addr_t dma;
	size_t len;		/* bytes allocated (whole pages) */
	u32 va;
};

struct sgx_fw_entry {
	const u8 *data;
	u32 size;
};

enum sgx_uk_state {
	UK_OFF,
	UK_RUNNING,
	UK_FAILED,
};

struct s5l8947x_sgx {
	struct device *dev;
	void __iomem *regs;
	struct clk_bulk_data *clks;
	int num_clks;
	struct miscdevice miscdev;
	struct mutex lock;
	int irq;
	bool irq_requested;
	bool powered;
	u32 cfg0;
	u32 cores;
	u32 cfg1;
	u32 clkgate0;

	/* GPU MMU */
	u32 *pd;
	dma_addr_t pd_dma;
	u32 *pt[SGX_PT_ENTRIES];
	dma_addr_t pt_dma[SGX_PT_ENTRIES];

	struct sgx_buf buf[B_COUNT];
	u32 heap_next[ARRAY_SIZE(sgx_heaps)];

	enum sgx_uk_state uk_state;
	u32 uk_ctl0;
	unsigned int uk_up_us;
	atomic_t events;
	u32 last_status;
};

static bool autostart;
module_param(autostart, bool, 0444);
MODULE_PARM_DESC(autostart, "Start the GPU microkernel at probe if its firmware is present");

static unsigned int clock_khz = 200000;
module_param(clock_khz, uint, 0644);
MODULE_PARM_DESC(clock_khz, "GPU core clock in kHz reported to the microkernel");

static inline void sgx_wr(struct s5l8947x_sgx *sgx, u32 reg, u32 val)
{
	writel(val, sgx->regs + reg);
}

/*
 * Release the cores from reset with automatic clock gating, in the order the
 * hardware expects. Called with the clocks running.
 */
static void s5l8947x_sgx_hw_init(struct s5l8947x_sgx *sgx)
{
	u32 en = 0, rst = SGX_MASTER_RESET_ALL | SGX_MASTER_RESET_EXT;
	unsigned int i;

	sgx->cfg0 = readl(sgx->regs + SGX_MASTER_CFG0);
	sgx->cfg1 = readl(sgx->regs + SGX_MASTER_CFG1);
	sgx->cores = max_t(u32, FIELD_GET(SGX_MASTER_CFG0_CORES, sgx->cfg0), 1);

	for (i = 0; i < sgx->cores; i++) {
		en |= BIT(2 * i);
		rst |= BIT(i);
	}

	sgx_wr(sgx, SGX_MASTER_CORES, sgx->cores - 1);
	sgx_wr(sgx, SGX_MASTER_CLKGATE, SGX_MASTER_CLKGATE_AUTO);
	sgx_wr(sgx, SGX_MASTER_CORE_EN, en);

	sgx_wr(sgx, SGX_MASTER_RESET, rst);
	sgx_wr(sgx, SGX_MASTER_MEM0, SGX_MASTER_MEM0_VAL);
	sgx_wr(sgx, SGX_MASTER_MEM1, SGX_MASTER_MEM1_VAL);
	sgx_wr(sgx, SGX_MASTER_RESET, 0);

	sgx_wr(sgx, SGX_CLKGATE0, SGX_CLKGATE0_AUTO);
	sgx_wr(sgx, SGX_CLKGATE1, SGX_CLKGATE1_AUTO);
	sgx_wr(sgx, SGX_CLKGATE_EXT, 1);
	sgx->clkgate0 = readl(sgx->regs + SGX_CLKGATE0);
}

/* ---- GPU MMU and buffers ------------------------------------------------ */

static u32 sgx_pte_bits(u8 flags)
{
	u32 bits = SGX_PTE_VALID;

	if (flags & 1)
		bits |= SGX_PTE_READONLY;
	if (flags & 4)
		bits |= SGX_PTE_BIT3;
	if (flags & 2)
		bits |= SGX_PTE_BIT4;
	return bits;
}

static int sgx_map(struct s5l8947x_sgx *sgx, u32 va, dma_addr_t pa, size_t len, u32 bits)
{
	size_t off;

	for (off = 0; off < len; off += SGX_PAGE) {
		u32 v = va + off, pdi = v >> SGX_PT_SHIFT;

		if (!sgx->pt[pdi]) {
			sgx->pt[pdi] = dma_alloc_coherent(sgx->dev, SGX_PAGE, &sgx->pt_dma[pdi],
							  GFP_KERNEL);
			if (!sgx->pt[pdi])
				return -ENOMEM;
			sgx->pd[pdi] = (u32)sgx->pt_dma[pdi] | SGX_PDE_VALID;
		}
		sgx->pt[pdi][(v >> 12) & (SGX_PT_ENTRIES - 1)] =
			((u32)(pa + off) & ~(SGX_PAGE - 1)) | bits;
	}
	return 0;
}

static void sgx_free_mem(struct s5l8947x_sgx *sgx)
{
	unsigned int i;

	for (i = 0; i < B_COUNT; i++) {
		if (sgx->buf[i].cpu)
			dma_free_coherent(sgx->dev, sgx->buf[i].len, sgx->buf[i].cpu,
					  sgx->buf[i].dma);
		memset(&sgx->buf[i], 0, sizeof(sgx->buf[i]));
	}
	for (i = 0; i < SGX_PT_ENTRIES; i++) {
		if (sgx->pt[i])
			dma_free_coherent(sgx->dev, SGX_PAGE, sgx->pt[i], sgx->pt_dma[i]);
		sgx->pt[i] = NULL;
	}
	if (sgx->pd)
		dma_free_coherent(sgx->dev, SGX_PAGE, sgx->pd, sgx->pd_dma);
	sgx->pd = NULL;
}

static int sgx_alloc_mem(struct s5l8947x_sgx *sgx)
{
	unsigned int i;
	int ret;

	sgx->pd = dma_alloc_coherent(sgx->dev, SGX_PAGE, &sgx->pd_dma, GFP_KERNEL);
	if (!sgx->pd)
		return -ENOMEM;
	if (upper_32_bits(sgx->pd_dma))
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(sgx_heaps); i++)
		sgx->heap_next[i] = sgx_heaps[i].base;

	for (i = 0; i < B_COUNT; i++) {
		const struct sgx_buf_desc *d = &sgx_bufs[i];
		const struct sgx_heap *h = &sgx_heaps[d->heap];
		struct sgx_buf *b = &sgx->buf[i];
		u32 va = ALIGN(sgx->heap_next[d->heap], max_t(u32, d->align, SGX_PAGE));

		b->len = PAGE_ALIGN(d->size);
		/* the stock allocator keeps one unmapped guard page after each buffer */
		if (va + b->len + SGX_PAGE > h->base + h->size)
			return -ENOSPC;
		b->cpu = dma_alloc_coherent(sgx->dev, b->len, &b->dma, GFP_KERNEL);
		if (!b->cpu)
			return -ENOMEM;
		if (upper_32_bits(b->dma))
			return -EINVAL;
		b->va = va;
		sgx->heap_next[d->heap] = va + b->len + SGX_PAGE;
		ret = sgx_map(sgx, va, b->dma, b->len, sgx_pte_bits(d->flags));
		if (ret)
			return ret;
	}
	return 0;
}

/* ---- Firmware ----------------------------------------------------------- */

static int sgx_parse_fw(struct s5l8947x_sgx *sgx, const struct firmware *fw,
			struct sgx_fw_entry *ent)
{
	const u8 *p = fw->data;
	u32 count, hdr, i;

	if (fw->size < 32 || get_unaligned_le32(p) != SGXF_MAGIC ||
	    get_unaligned_le32(p + 4) != SGXF_VERSION)
		return -EINVAL;
	count = get_unaligned_le32(p + 8);
	hdr = get_unaligned_le32(p + 12);
	if (count > 64 || hdr != 32 + 16 * count || hdr > fw->size)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		const u8 *e = p + 32 + 16 * i;
		u32 id = get_unaligned_le32(e), off = get_unaligned_le32(e + 4);
		u32 size = get_unaligned_le32(e + 8);

		if (off < hdr || size > fw->size || off > fw->size - size)
			return -EINVAL;
		if (id && id < FW_REGIONS) {
			ent[id].data = p + off;
			ent[id].size = size;
		}
	}

	if (ent[FW_BOOT].size > CODE_UKERNEL ||
	    CODE_UKERNEL + ent[FW_UKERNEL].size > sgx_bufs[B_CODE].size)
		return -EINVAL;
	for (i = 0; i < B_COUNT; i++) {
		u8 r = sgx_bufs[i].fw;

		if (r && ent[r].size != sgx_bufs[i].size)
			return -EINVAL;
	}
	for (i = FW_BOOT; i < FW_REGIONS; i++)
		if (!ent[i].data)
			return -ENOENT;
	return 0;
}

/* ---- Microkernel set-up ------------------------------------------------- */

static inline u32 *sgx_w(struct s5l8947x_sgx *sgx, enum sgx_buf_id id, u32 off)
{
	return (u32 *)((u8 *)sgx->buf[id].cpu + off);
}

static inline u32 sgx_va(struct s5l8947x_sgx *sgx, enum sgx_buf_id id)
{
	return sgx->buf[id].va;
}

/* Event-program "code address" for a program offset from code base 0 */
static inline u32 sgx_doutu(u32 off)
{
	return (off * 2) & 0x00fffff0;
}

/*
 * Replace the 32-bit immediate of a "load immediate" instruction in the code
 * buffer: imm[20:0] in the low word, imm[25:21] in high bits 8:4 and
 * imm[31:26] in high bits 17:12.
 */
static void sgx_patch_limm(struct s5l8947x_sgx *sgx, u32 off, u32 imm)
{
	u32 *ins = sgx_w(sgx, B_CODE, off);

	ins[0] = (ins[0] & 0xffe00000) | (imm & 0x001fffff);
	ins[1] = (ins[1] & 0xfffc0e0f) | ((imm >> 17) & 0x1f0) | ((imm >> 14) & 0x3f000);
}

static void sgx_fill(struct s5l8947x_sgx *sgx, const struct sgx_fw_entry *fw)
{
	u32 code = sgx_va(sgx, B_CODE), uk = CODE_UKERNEL;
	u32 glob = sgx_va(sgx, B_GLOBAL), code2 = sgx_va(sgx, B_CODE2);
	u32 pds_e = sgx_va(sgx, B_PDS_E);
	unsigned int i;
	u32 *w;

	/* Programs and event-program templates */
	memcpy(sgx_w(sgx, B_CODE, 0), fw[FW_BOOT].data, fw[FW_BOOT].size);
	memcpy(sgx_w(sgx, B_CODE, uk), fw[FW_UKERNEL].data, fw[FW_UKERNEL].size);
	for (i = 0; i < B_COUNT; i++)
		if (sgx_bufs[i].fw)
			memcpy(sgx->buf[i].cpu, fw[sgx_bufs[i].fw].data, sgx_bufs[i].size);

	/* Microkernel placeholders (offsets in the code buffer) */
	sgx_patch_limm(sgx, uk + 0x000, 0x01001000);
	sgx_patch_limm(sgx, uk + 0x010, sgx_va(sgx, B_PDS_A) >> 4);
	sgx_patch_limm(sgx, uk + 0x018, glob);
	sgx_patch_limm(sgx, uk + 0x058, 0x00800900);
	sgx_patch_limm(sgx, uk + 0x060, sgx_va(sgx, B_PDS_B) >> 4);
	sgx_patch_limm(sgx, uk + 0x0e0, sgx_va(sgx, B_PDS_C) & 0x1ffff0);
	sgx_patch_limm(sgx, uk + 0x0f0, 7);
	sgx_patch_limm(sgx, uk + 0x2b0, sgx_va(sgx, B_SCRATCH3));
	sgx_patch_limm(sgx, uk + 0x340, sgx_va(sgx, B_SCRATCH2));
	sgx_patch_limm(sgx, uk + 0x390, sgx_va(sgx, B_SCRATCH0));
	sgx_patch_limm(sgx, uk + 0x3b0, sgx_va(sgx, B_SCRATCH1));
	sgx_patch_limm(sgx, uk + 0x588, sgx_va(sgx, B_DESC));
	sgx_patch_limm(sgx, uk + 0x7f98, 0x02800900);
	sgx_patch_limm(sgx, uk + 0x7fb0, sgx_va(sgx, B_PDS_D) >> 4);
	sgx_patch_limm(sgx, uk + 0x86c8, 0x02800900);
	sgx_patch_limm(sgx, uk + 0x86e0, sgx_va(sgx, B_PDS_D) >> 4);

	/* Start-up program branch targets, in 8-byte instructions from code base 0 */
	*sgx_w(sgx, B_CODE, 0x70) |= ((uk + CODE_UKERNEL_ENTRY) & 0x7fffff) >> 3;
	*sgx_w(sgx, B_CODE, 0x240) |= ((code2 + CODE2_ENTRY_BOOT - code) & 0x7fffff) >> 3;

	/* Event-program data words */
	w = sgx_w(sgx, B_PDS_CORE, 0);
	w[0] = sgx_doutu(uk);
	w[1] = 1;
	w[2] = 0;

	w = sgx_w(sgx, B_PDS_A, 0);
	w[0] = glob;
	w[1] = 0x3f;
	w[2] = 0;
	w[4] = sgx_doutu(uk + 0x90);
	w[5] = 1;

	w = sgx_w(sgx, B_PDS_B, 0);
	w[0] = sgx_doutu(uk + 0xc0);
	w[1] = 1;
	w[2] = 0;

	w = sgx_w(sgx, B_PDS_C, 0);
	w[1] = 0x100;
	w[2] = sgx_doutu(uk + 0x10d20);
	w[3] = sgx_doutu(uk + 0xd0d0);
	w[4] = 0x200;
	w[6] = sgx_doutu(uk + 0x4340);
	w[7] = sgx_doutu(uk + 0xac50);
	w[8] = 1;
	w[9] = 0;
	w[10] = sgx_doutu(uk + 0x1470);

	w = sgx_w(sgx, B_PDS_D, 0);
	w[1] = 0x200;
	w[2] = 0;
	w[3] = 0x8000f;
	w[4] = sgx_doutu(uk + 0xd3f0);
	w[5] = 1;
	w[6] = 0x240100;
	w[7] = 0x420000;
	w[8] = sgx_doutu(uk + 0x4510);
	w[9] = 1;
	w[12] = sgx_doutu(uk + 0x11190);
	w[13] = 1;
	w[16] = sgx_doutu(uk + 0xad20);
	w[17] = 1;

	w = sgx_w(sgx, B_PDS_OTHER, 0);
	w[1] = 0x100;
	w[2] = 0;
	w[4] = glob;
	w[5] = 0x200;
	w[8] = sgx_doutu(code2 + CODE2_ENTRY_OTHER - code) | 0x08000000;
	w[9] = 1;

	w = sgx_w(sgx, B_PDS_E, 0);
	w[0x20 / 4] = 0x01000001;
	w[0x70 / 4] = 0x01000101;
	w[0xc0 / 4] = 0x01000231;

	w = sgx_w(sgx, B_DESC, 0);
	w[0] |= pds_e >> 4;
	w[3] |= sgx_va(sgx, B_TABLE);
	w[5] |= (pds_e + 0x50) >> 4;
	w[7] |= (pds_e + 0xa0) >> 4;

	w = sgx_w(sgx, B_TABLE, 0);
	for (i = 0; i < 24; i++)
		w[i] = i;

	/* Global block: where the microkernel finds everything else */
	w = sgx_w(sgx, B_GLOBAL, 0);
	w[GLB_SELF / 4] = glob;
	w[GLB_CTL / 4] = sgx_va(sgx, B_CTL);
	w[GLB_RING_IDX / 4] = sgx_va(sgx, B_RING_IDX);
	w[GLB_14 / 4] = 2;
	w[GLB_KL_INV / 4] = 0x7f800000;	/* 1.0f / 0.0f: no duty management */
	w[GLB_CC / 4] = 0x3f4ccccd;		/* 0.8f */
	w[GLB_E0 / 4] = 0x10;
	w[GLB_RING / 4] = sgx_va(sgx, B_RING);
	w[GLB_PD_PHYS / 4] = (u32)sgx->pd_dma;
	w[GLB_TRACE / 4] = sgx_va(sgx, B_TRACE);

	/* Host control block */
	w = sgx_w(sgx, B_CTL, 0);
	w[CTL_14 / 4] = 0x10;
	w[CTL_CLOCK_KHZ / 4] = clock_khz;
	w[CTL_1C / 4] = 1;
}

static void sgx_program(struct s5l8947x_sgx *sgx)
{
	u32 core0 = SGX_CORE(0);
	unsigned int i;

	/* Memory interface: every requestor uses the one page directory */
	sgx_wr(sgx, SGX_BIF_CTRL, 0);
	udelay(1);
	sgx_wr(sgx, SGX_BIF_BANK0, 0x77077);
	sgx_wr(sgx, SGX_BIF_BANK1, 0);
	sgx_wr(sgx, SGX_BIF_BANK_SET, 0);
	for (i = 0; i < 8; i++)
		sgx_wr(sgx, SGX_BIF_DIR_LIST_BASE(i), (u32)sgx->pd_dma);
	sgx_wr(sgx, SGX_BIF_TILE1, 0x0beffe00);
	sgx_wr(sgx, SGX_BIF_TILE2, 0x0cffff00);
	sgx_wr(sgx, SGX_BIF_CTRL_INVAL, 8);
	sgx_wr(sgx, SGX_MASTER_BIF_4CD0, 2);
	for (i = 0; i < sgx->cores; i++)
		sgx_wr(sgx, SGX_CORE(i) + SGX_CORE_BIF_CD0, 2);
	sgx_wr(sgx, SGX_MASTER_BIF_CTRL, 0);
	udelay(1);

	/* Fixed settings, as the stock cold start writes them */
	sgx_wr(sgx, SGX_CORE_PDS_ENABLE, 0);
	sgx_wr(sgx, SGX_EVENT_TIMER, 0);
	sgx_wr(sgx, 0x001c, 0);
	sgx_wr(sgx, 0x0a7c, 0);
	sgx_wr(sgx, 0x0a80, 0);
	sgx_wr(sgx, 0x0a00, 0x7c000);
	sgx_wr(sgx, 0x0abc, 0x4c);
	sgx_wr(sgx, 0x0aa0, 0x7fffffff);
	sgx_wr(sgx, 0x0818, 0);
	sgx_wr(sgx, 0x0804, 0x5e0);
	sgx_wr(sgx, 0x0814, 0xffff);
	sgx_wr(sgx, 0x0a74, 0x0da08200);
	sgx_wr(sgx, 0x0b30, 0x100);
	sgx_wr(sgx, 0x4808, 4);
	sgx_wr(sgx, 0x4144, 1000);
	sgx_wr(sgx, 0x414c, 0);
	sgx_wr(sgx, core0 + SGX_CORE_BA0, 0x40 >> 3);
	sgx_wr(sgx, core0 + SGX_CORE_BB4, 0);
	for (i = 1; i <= 3; i++) {
		sgx_wr(sgx, SGX_CORE(i) + SGX_CORE_BA0, 0x240 >> 3);
		sgx_wr(sgx, SGX_CORE(i) + SGX_CORE_BB4, 0);
	}
	*sgx_w(sgx, B_KICKER, 0) = 0;

	/* Events: clear everything, enable the microkernel's event */
	sgx_wr(sgx, SGX_EVENT_CLEAR, 0xffffffff);
	sgx_wr(sgx, SGX_EVENT_CLEAR2, 0xffffffff);
	sgx_wr(sgx, SGX_EVENT_CLEAR3, 0xffffffff);
	sgx_wr(sgx, SGX_EVENT_ENABLE, SGX_EVENT_UKERNEL);
	sgx_wr(sgx, SGX_EVENT_ENABLE2, 0);
	sgx_wr(sgx, SGX_EVENT_ENABLE3, 0);

	/* Code bases: 0 = microkernel buffer (data master 3), 1 = small program */
	for (i = 2; i < SGX_USE_CODE_BASES; i++)
		sgx_wr(sgx, SGX_USE_CODE_BASE(i), 0);
	sgx_wr(sgx, SGX_USE_CODE_BASE(0), sgx_va(sgx, B_CODE) >> 6 | 0x0c000000);
	sgx_wr(sgx, SGX_EVENT_OTHER_PDS_EXEC, sgx_va(sgx, B_PDS_OTHER) & ~0xf);
	sgx_wr(sgx, SGX_EVENT_OTHER_PDS_DATA, 3);
	sgx_wr(sgx, SGX_EVENT_OTHER_PDS_INFO, 0x12001);
	sgx_wr(sgx, core0 + SGX_CORE_PDS_EXEC, sgx_va(sgx, B_PDS_CORE) & ~0xf);
	sgx_wr(sgx, core0 + SGX_CORE_PDS_DATA, 1);
	sgx_wr(sgx, core0 + SGX_CORE_PDS_INFO, 0x12001);
	sgx_wr(sgx, SGX_USE_CODE_BASE(1), sgx_va(sgx, B_CODE1) >> 6);
	sgx_wr(sgx, SGX_EVENT_KICKER, sgx_va(sgx, B_KICKER));
	sgx_wr(sgx, 0x0630, 2);
	sgx_wr(sgx, core0 + SGX_CORE_PDS_ENABLE, 0x2200c000);
	sgx_wr(sgx, SGX_MASTER_PDS_ENABLE, 0x0804300f);
}

static irqreturn_t s5l8947x_sgx_irq(int irq, void *data)
{
	struct s5l8947x_sgx *sgx = data;
	u32 s1 = readl(sgx->regs + SGX_EVENT_STATUS) & SGX_EVENT_UKERNEL;

	if (!s1)
		return IRQ_NONE;
	sgx->last_status = s1;
	writel(s1 | SGX_EVENT_MASTER_ACK, sgx->regs + SGX_EVENT_CLEAR);
	atomic_inc(&sgx->events);
	return IRQ_HANDLED;
}

/* Called with sgx->lock held and the GPU powered */
static int sgx_ukernel_start(struct s5l8947x_sgx *sgx)
{
	struct sgx_fw_entry fw[FW_REGIONS] = { };
	const struct firmware *blob;
	volatile u32 *status;
	unsigned int us;
	int ret;

	if (sgx->uk_state == UK_RUNNING)
		return 0;

	ret = request_firmware(&blob, SGX_FW_NAME, sgx->dev);
	if (ret) {
		dev_err(sgx->dev, "no microkernel firmware (%s): %d\n", SGX_FW_NAME, ret);
		return ret;
	}
	ret = sgx_parse_fw(sgx, blob, fw);
	if (ret) {
		dev_err(sgx->dev, "%s: unrecognised layout: %d\n", SGX_FW_NAME, ret);
		goto out_fw;
	}

	sgx_free_mem(sgx);
	ret = sgx_alloc_mem(sgx);
	if (ret) {
		dev_err(sgx->dev, "GPU memory set-up failed: %d\n", ret);
		goto out_mem;
	}
	sgx_fill(sgx, fw);
	release_firmware(blob);
	blob = NULL;

	/* Fresh reset and clock gating before the memory interface is set up */
	s5l8947x_sgx_hw_init(sgx);

	if (sgx->irq > 0 && !sgx->irq_requested) {
		ret = devm_request_irq(sgx->dev, sgx->irq, s5l8947x_sgx_irq, 0,
				       dev_name(sgx->dev), sgx);
		if (ret)
			goto out_mem;
		sgx->irq_requested = true;
	}

	sgx_program(sgx);

	status = sgx_w(sgx, B_CTL, CTL_STATUS);
	*status = 0;
	*sgx_w(sgx, B_KICKER, 0) = 1;
	wmb();
	sgx_wr(sgx, SGX_CORE(0) + SGX_CORE_EVENT_KICK, 1);

	for (us = 0; us < 100000; us += 10) {
		if (*status & CTL_STATUS_UP)
			break;
		udelay(10);
	}
	sgx->uk_ctl0 = *status;
	if (!(sgx->uk_ctl0 & CTL_STATUS_UP)) {
		dev_err(sgx->dev, "microkernel did not come up (ctl0=0x%08x, events %d)\n",
			sgx->uk_ctl0, atomic_read(&sgx->events));
		sgx->uk_state = UK_FAILED;
		return -ETIMEDOUT;	/* memory stays in place: the GPU may still use it */
	}
	sgx->uk_up_us = us;
	sgx->uk_state = UK_RUNNING;
	dev_info(sgx->dev, "microkernel running (up after %u us, code at GPU 0x%08x)\n",
		 us, sgx_va(sgx, B_CODE));
	return 0;

out_mem:
	sgx_free_mem(sgx);
out_fw:
	release_firmware(blob);
	return ret;
}

/* Called with sgx->lock held */
static int s5l8947x_sgx_power_set(struct s5l8947x_sgx *sgx, bool on)
{
	int ret;

	if (on == sgx->powered)
		return 0;

	if (!on) {
		if (sgx->irq_requested) {
			sgx_wr(sgx, SGX_EVENT_ENABLE, 0);
			devm_free_irq(sgx->dev, sgx->irq, sgx);
			sgx->irq_requested = false;
		}
		clk_bulk_disable_unprepare(sgx->num_clks, sgx->clks);
		sgx->powered = false;
		/* The GPU no longer runs, so its memory can go */
		sgx_free_mem(sgx);
		sgx->uk_state = UK_OFF;
		return 0;
	}

	ret = clk_bulk_prepare_enable(sgx->num_clks, sgx->clks);
	if (ret)
		return ret;
	sgx->powered = true;
	s5l8947x_sgx_hw_init(sgx);
	return 0;
}

static ssize_t power_state_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct s5l8947x_sgx *sgx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", sgx->powered ? "on" : "standby");
}

static ssize_t power_state_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct s5l8947x_sgx *sgx = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&sgx->lock);
	if (sysfs_streq(buf, "on") || sysfs_streq(buf, "1"))
		ret = s5l8947x_sgx_power_set(sgx, true);
	else if (sysfs_streq(buf, "off") || sysfs_streq(buf, "0") ||
		 sysfs_streq(buf, "standby"))
		ret = s5l8947x_sgx_power_set(sgx, false);
	else
		ret = -EINVAL;
	mutex_unlock(&sgx->lock);

	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_RW(power_state);

static ssize_t core_info_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct s5l8947x_sgx *sgx = dev_get_drvdata(dev);

	return sysfs_emit(buf,
			  "model=PowerVR-SGX543 mmio=0x35100000 irq=%d cfg0=0x%08x cfg1=0x%08x cores=%lu clkgate=0x%08x\n",
			  sgx->irq, sgx->cfg0, sgx->cfg1,
			  FIELD_GET(SGX_MASTER_CFG0_CORES, sgx->cfg0),
			  sgx->clkgate0);
}
static DEVICE_ATTR_RO(core_info);

static const char * const sgx_uk_names[] = {
	[UK_OFF] = "off",
	[UK_RUNNING] = "running",
	[UK_FAILED] = "failed",
};

static ssize_t ukernel_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct s5l8947x_sgx *sgx = dev_get_drvdata(dev);
	u32 ctl0 = sgx->uk_ctl0;

	mutex_lock(&sgx->lock);
	if (sgx->uk_state != UK_OFF && sgx->buf[B_CTL].cpu)
		ctl0 = *(volatile u32 *)sgx_w(sgx, B_CTL, CTL_STATUS);
	mutex_unlock(&sgx->lock);

	return sysfs_emit(buf, "%s ctl0=0x%08x up_us=%u events=%d code=0x%08x\n",
			  sgx_uk_names[sgx->uk_state], ctl0, sgx->uk_up_us,
			  atomic_read(&sgx->events), sgx->buf[B_CODE].va);
}

static ssize_t ukernel_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct s5l8947x_sgx *sgx = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&sgx->lock);
	if (sysfs_streq(buf, "start")) {
		ret = s5l8947x_sgx_power_set(sgx, true);
		if (!ret)
			ret = sgx_ukernel_start(sgx);
	} else if (sysfs_streq(buf, "stop")) {
		/* Gating the clocks stops the microkernel; power back on in reset */
		ret = s5l8947x_sgx_power_set(sgx, false);
		if (!ret)
			ret = s5l8947x_sgx_power_set(sgx, true);
	} else {
		ret = -EINVAL;
	}
	mutex_unlock(&sgx->lock);

	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_RW(ukernel);

static struct attribute *s5l8947x_sgx_attrs[] = {
	&dev_attr_power_state.attr,
	&dev_attr_core_info.attr,
	&dev_attr_ukernel.attr,
	NULL,
};
ATTRIBUTE_GROUPS(s5l8947x_sgx);

static ssize_t s5l8947x_sgx_misc_read(struct file *file, char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	struct s5l8947x_sgx *sgx =
		container_of(file->private_data, struct s5l8947x_sgx, miscdev);
	char info[192];
	int len;

	len = scnprintf(info, sizeof(info),
			"SGX543 mmio=0x35100000 irq=%d state=%s cfg0=0x%08x cfg1=0x%08x clkgate=0x%08x ukernel=%s\n",
			sgx->irq, sgx->powered ? "on" : "standby",
			sgx->cfg0, sgx->cfg1, sgx->clkgate0, sgx_uk_names[sgx->uk_state]);
	return simple_read_from_buffer(ubuf, count, ppos, info, len);
}

static const struct file_operations s5l8947x_sgx_fops = {
	.owner	= THIS_MODULE,
	.read	= s5l8947x_sgx_misc_read,
	.llseek	= default_llseek,
};

static int s5l8947x_sgx_probe(struct platform_device *pdev)
{
	struct s5l8947x_sgx *sgx;
	int ret;

	sgx = devm_kzalloc(&pdev->dev, sizeof(*sgx), GFP_KERNEL);
	if (!sgx)
		return -ENOMEM;

	sgx->dev = &pdev->dev;
	mutex_init(&sgx->lock);
	atomic_set(&sgx->events, 0);
	platform_set_drvdata(pdev, sgx);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	sgx->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sgx->regs))
		return PTR_ERR(sgx->regs);

	sgx->irq = platform_get_irq_optional(pdev, 0);

	sgx->num_clks = devm_clk_bulk_get_all(&pdev->dev, &sgx->clks);
	if (sgx->num_clks <= 0)
		return dev_err_probe(&pdev->dev, sgx->num_clks ?: -ENODEV,
				     "failed to get clocks\n");

	ret = s5l8947x_sgx_power_set(sgx, true);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to power on\n");

	sgx->miscdev.minor = MISC_DYNAMIC_MINOR;
	sgx->miscdev.name = "sgx0";
	sgx->miscdev.fops = &s5l8947x_sgx_fops;
	sgx->miscdev.parent = &pdev->dev;
	ret = misc_register(&sgx->miscdev);
	if (ret) {
		s5l8947x_sgx_power_set(sgx, false);
		return ret;
	}

	dev_info(&pdev->dev,
		 "PowerVR SGX543 registered (/dev/sgx0, irq=%d, cfg0=0x%08x cfg1=0x%08x)\n",
		 sgx->irq, sgx->cfg0, sgx->cfg1);

	if (autostart) {
		mutex_lock(&sgx->lock);
		sgx_ukernel_start(sgx);
		mutex_unlock(&sgx->lock);
	}
	return 0;
}

static void s5l8947x_sgx_remove(struct platform_device *pdev)
{
	struct s5l8947x_sgx *sgx = platform_get_drvdata(pdev);

	misc_deregister(&sgx->miscdev);
	mutex_lock(&sgx->lock);
	s5l8947x_sgx_power_set(sgx, false);
	sgx_free_mem(sgx);
	mutex_unlock(&sgx->lock);
}

static const struct of_device_id s5l8947x_sgx_of_match[] = {
	{ .compatible = "apple,s5l8947x-sgx" },
	{ .compatible = "img,sgx543" },
	{ },
};
MODULE_DEVICE_TABLE(of, s5l8947x_sgx_of_match);

static struct platform_driver s5l8947x_sgx_driver = {
	.probe	= s5l8947x_sgx_probe,
	.remove	= s5l8947x_sgx_remove,
	.driver	= {
		.name		= "s5l8947x-sgx",
		.of_match_table	= s5l8947x_sgx_of_match,
		.dev_groups	= s5l8947x_sgx_groups,
	},
};
module_platform_driver(s5l8947x_sgx_driver);

MODULE_DESCRIPTION("S5L8947X PowerVR SGX543 GPU power, MMU and microkernel start-up");
MODULE_FIRMWARE(SGX_FW_NAME);
MODULE_LICENSE("GPL");
