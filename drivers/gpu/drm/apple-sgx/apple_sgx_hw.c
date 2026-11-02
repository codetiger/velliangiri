// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X PowerVR SGX543 -- the hardware side: power-on, register access,
 * the GPU's MMU, the microkernel, its queues and the debugfs tools.  The
 * render node (apple_sgx_drm.c) sits on top of it.
 *
 * At probe this switches the SGX on, runs the initialisation the stock
 * driver runs, checks that the master and every core answer, registers the
 * render node, and starts the microkernel (autoboot): page tables and
 * buffers, the microkernel loaded from the device's own firmware, kicked,
 * and waited for.  From then on it takes commands through the kernel CCB,
 * the command queue: power and perf counters, transfers, and renders (the
 * TA and 3D pass) from the render node or debugfs.
 *
 * Power is two PMGR power states, GFX_SYS (0x3f101024) then GFX
 * (0x3f101028), taken as clocks from the gate driver in the order the DT
 * lists them.  Nothing else: the GFX clocks already run, and HPERF-NRT is
 * not needed.
 *
 * The register window is six 16 KiB banks:
 *
 *   bank 0       broadcast.  Written to reach every core at once; it has no
 *                read path, and a read HANGS THE BUS.
 *   bank 1       the master.
 *   bank 2 + n   core n.  Reads hang until MASTER_CLKGATECTL gives the core
 *                a clock.
 *
 * Every access goes through sgx_read()/sgx_write(), which refuse bank 0
 * reads and any core bank this SGX does not have, so neither mistake can be
 * made by accident.
 *
 * The microkernel is the device's own firmware.  It is not in this tree:
 * stage2 carves it from the system image on the device's System volume
 * (scripts/tools/sgxcarve.c) into /lib/firmware/s5l8947x/sgx543.fw.  The
 * loader checks the module build and every region size before it loads a
 * byte.
 *
 * Register names come from the public PowerVR DDK (sgxmpdefs.h,
 * sgx544defs.h, sgxmmu.h) and from observed hardware behaviour.  This
 * driver is derived from the clean-room SGX543 work published in Project
 * Cascadia (GPL-2.0-only) and adapted to the S5L8947X.
 */
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include "apple_sgx.h"

/* Per-core registers, at SGX_CORE(n) or, for writes, SGX_BCAST. */
#define SGX_CLKGATECTL			0x000
#define SGX_CLKGATECTL2			0x004
#define SGX_CLKGATESTATUS		0x008
#define SGX_POWER			0x01c
#define SGX_CORE_ID			0x020
#define SGX_CORE_REVISION		0x024
#define SGX_SOFT_RESET			0x080
#define SGX_EVENT_HOST_ENABLE2		0x110
#define SGX_EVENT_HOST_CLEAR2		0x114
#define SGX_EVENT_STATUS2		0x118
#define SGX_EVENT_STATUS		0x12c
#define SGX_EVENT_HOST_ENABLE		0x130
#define SGX_EVENT_HOST_CLEAR		0x134
#define SGX_REG_13C			0x13c
#define SGX_REG_140			0x140
#define SGX_REG_310			0x310	/* the stock driver writes 1; not in the DDK */
#define SGX_USE_CODE_BASE(i)		(0xa0c + 4 * (i))
#define SGX_EVENT_PDS_EXEC		0xa68	/* names guessed: exec, data, info */
#define SGX_EVENT_PDS_DATA		0xa6c
#define SGX_EVENT_PDS_INFO		0xa70
#define SGX_EVENT_KICKER		0xac4
#define SGX_EVENT_KICK			0xac8
#define SGX_EVENT_TIMER			0xacc
#define SGX_BOOT_ENTRY			0xba0	/* entry into the boot program / 8 */
#define SGX_BIF_CTRL			0xc00
#define SGX_BIF_INT_STAT		0xc04
#define SGX_BIF_FAULT			0xc08
#define SGX_BIF_TILE1			0xc10
#define SGX_BIF_TILE2			0xc14
#define SGX_BIF_CTRL_INVAL		0xc34
#define SGX_BIF_DIR_LIST_BASE(i)	((i) ? 0xc34 + 4 * (i) : 0xc84)
#define SGX_BIF_BANK_SET		0xc74
#define SGX_BIF_BANK0			0xc78
#define SGX_BIF_BANK1			0xc7c
#define SGX_BIF_MEM_REQ_STAT		0xca8
#define SGX_BIF_MMU_CTRL		0xcd0

/* Master registers, offsets into the whole window. */
#define SGX_MASTER_CORE			0x4000	/* enabled cores - 1 */
#define SGX_MASTER_CLKGATECTL		0x4004	/* 2 bits per core */
#define SGX_MASTER_CLKGATESTATUS	0x4008
#define SGX_MASTER_CORE_ID		0x4010
#define SGX_MASTER_CORE_REVISION	0x4014
#define SGX_MASTER_CLKGATECTL2		0x4020
#define SGX_MASTER_CLKGATESTATUS2	0x4024
#define SGX_MASTER_SOFT_RESET		0x4080
#define SGX_MASTER_EVENT_STATUS2	0x4118
#define SGX_MASTER_EVENT_STATUS		0x412c
#define SGX_MASTER_REG_4144		0x4144
#define SGX_MASTER_REG_414C		0x414c
#define SGX_MASTER_REG_4808		0x4808
#define SGX_MASTER_REG_4A58		0x4a58
#define SGX_MASTER_BIF_CTRL		0x4c00
#define SGX_MASTER_BIF_INT_STAT		0x4c04
#define SGX_MASTER_BIF_FAULT		0x4c08
#define SGX_MASTER_BIF_MEM_REQ_STAT	0x4ca8
#define SGX_MASTER_BIF_MMU_CTRL		0x4cd0
#define SGX_MASTER_SLC_CTRL		0x4d00
#define SGX_MASTER_SLC_CTRL_BYPASS	0x4d04

#define SGX_CORE_ID_ID(v)		((v) >> 16)
#define SGX_CORE_ID_CORES(v)		(((v) >> 8) & 0xf)
#define SGX_ID_543			0x0119

/* MASTER_SOFT_RESET: IPF, DPM, VDM, SLC, PTLA (0x4f0), BIF (0x100), and one
 * bit per core. */
#define SGX_MASTER_RESET_ALL		0x5f0

/* The stock driver's values.  The bypass word is BYP_CC (0x4000000) plus, on parts with
 * erratum 31195, the USE0-3 and TA requestors (0x1e40) -- the DDK's
 * FIX_HW_BRN_31195. */
#define SGX_SLC_CTRL_VAL		0x0044c000
#define SGX_SLC_BYPASS_VAL		0x04000000
#define SGX_SLC_BYPASS_BRN_31195	0x00001e40

/* ---- the GPU's MMU (sgxmmu.h) ------------------------------------------ */

/* The page flags the stock driver gives each buffer.  Its allocator takes
 * explicit flags, or else by heap: heap 1 -> cache-consistent and
 * EDM-protected, heap 0 -> read-only; the PTE writer turns them
 * into these bits.  CACHECONSISTENT is not optional: without it the
 * microkernel keeps the kernel CCB's write offset in its data cache from
 * the first read on and never sees a command. */
#define PTE_RO				SGX_PTE_READONLY
#define PTE_SHARED			(SGX_PTE_CACHECONSISTENT | SGX_PTE_EDMPROTECT)

/* ---- the firmware file (scripts/tools/sgxcarve.c) ---------------------- */

/*
 * sgxcarve writes a small container: a header, a region table, then the
 * bytes of each region.  Region 1 is the start-up program, region 2 the
 * microkernel, regions 3.. are the PDS templates and code tables in the
 * order the stock driver allocates them.  The loader refuses a container
 * whose module build or region sizes it does not know.
 */
#define SGX_FW_NAME			"s5l8947x/sgx543.fw"
#define SGXF_MAGIC			0x46584753	/* "SGXF" */
#define SGXF_VERSION			1
#define SGXF_MAX_REGIONS		64
#define SGXF_HDR_SIZE			32
#define SGXF_ENTRY_SIZE			16

/* Module build 12H1006, the one this loader's offsets are for. */
static const u8 sgx_fw_uuid[16] = {
	0x0b, 0x34, 0x38, 0x5d, 0x59, 0xf2, 0x3d, 0x54,
	0x90, 0xd2, 0x5c, 0x2d, 0x9a, 0xaf, 0xba, 0xe5,
};

enum sgx_fw_region_id {
	FW_BOOT = 1,
	FW_UKERNEL,
	FW_PDS_6D8,
	FW_PDS_6E0,
	FW_PDS_6E8,
	FW_PDS_6F0,
	FW_PDS_6F8,
	FW_CODE2,
	FW_PDS_704,
	FW_TAB_78C,
	FW_TAB_784,
	FW_TAB_774,
	FW_REGIONS,
};

struct sgx_fw_region {
	const u8 *data;
	u32 size;
};

/* Sizes of the regions this loader patches by offset. */
#define UK_CODE_SIZE			0x11f88
#define UK_CODE2_SIZE			0x3b58
#define UK_BOOT_SIZE			0x248

/* The code buffer: the start-up program at 0, the microkernel at 0x1000,
 * and the entry points into the start-up program for core 0 and the
 * others. */
#define UK_START			0x1000
#define UK_ENTRY_CORE0			0x40
#define UK_ENTRY_OTHERS			0x240

/* ---- commands: the DDK's kernel CCB ------------------------------------ */

/* The B_750 buffer is the kernel CCB, 256 commands; B_758 holds the write
 * offset and the read offset the microkernel advances as it takes each
 * command.  A host fills the slot at the write offset, bumps the offset and
 * the kicker count, and kicks core 0. */
#define SGX_CCB_SIZE			256

struct sgx_ccb_cmd {
	u32 service;		/* the handler: USE code address / 8 */
	u32 cache_control;	/* SGX_CC_INVAL_* */
	u32 data[6];
};

/* The handlers the microkernel exposes, as offsets into its code.  TA and
 * TRANSFER come from the render and transfer submits, POWER from the power
 * state change, HWPERF from the performance-counter request, and RECOVER
 * for a context after a hardware recovery. */
enum sgx_cmd_type {
	SGX_CMD_TA,
	SGX_CMD_POWER,
	SGX_CMD_TRANSFER,
	SGX_CMD_RECOVER,
	SGX_CMD_HWPERF,
	SGX_CMD_NUM
};

static const struct {
	u32 off;
	const char *name;
} uk_handlers[SGX_CMD_NUM] = {
	[SGX_CMD_TA]		= { 0x1fa8, "TA" },
	[SGX_CMD_POWER]		= { 0x1bf0, "POWER" },
	[SGX_CMD_TRANSFER]	= { 0x1e80, "TRANSFER" },
	[SGX_CMD_RECOVER]	= { 0x21d0, "RECOVER" },
	[SGX_CMD_HWPERF]	= { 0x21d8, "HWPERF" },
};

/* POWER's Data[1], and what the microkernel sets in the power status when
 * it is done. */
#define SGX_POWERCMD_POWEROFF		1
#define SGX_POWERCMD_IDLE		2
#define SGX_POWERCMD_RESUME		3
#define SGX_POWMAN_IDLE_COMPLETE	BIT(2)
#define SGX_POWMAN_POWEROFF_COMPLETE	BIT(3)
#define SGX_POWMAN_NO_WORK		BIT(5)

/* The host control block (B_HOST) is the DDK's SGXMKIF_HOST_CTL as built
 * with hardware recovery, without the erratum workaround.  Word indices. */
#define HOST_INIT_STATUS		0	/* bit 0: the microkernel is up */
#define HOST_POWER_STATUS		1	/* SGX_POWMAN_* */
#define HOST_CLEANUP_STATUS		2
#define HOST_UK_LOCKUPS			3
#define HOST_HWR_SAMPLE_RATE		5
#define HOST_UK_TIMER_CLOCK		6
#define HOST_APM_SAMPLE_RATE		7
#define HOST_INTERRUPT_FLAGS		8
#define HOST_INTERRUPT_CLEAR		9
#define HOST_TIME_WRAPS			12
#define HOST_HOST_CLOCK			13
#define HOST_ASSERT_FAIL		14

static const struct sgx_buf_desc {
	const char *name;
	u32 size, align;
	u8 pte;			/* PTE_RO, PTE_SHARED or 0 */
	u8 fw;			/* firmware region copied in, 0 = zero */
} sgx_buf_descs[B_NUM] = {
	[B_CODE]	= { "code",	UK_START + UK_CODE_SIZE, 0x1000, PTE_RO, 0 },
	[B_PDS_6D8]	= { "pds 6d8",	0x18,	0x1000, PTE_SHARED, FW_PDS_6D8 },
	[B_PDS_6E0]	= { "pds 6e0",	0x38,	0x1000, PTE_SHARED, FW_PDS_6E0 },
	[B_PDS_6E8]	= { "pds 6e8",	0x18,	0x1000, PTE_SHARED, FW_PDS_6E8 },
	[B_PDS_6F0]	= { "pds 6f0",	0x350,	0x1000, PTE_SHARED, FW_PDS_6F0 },
	[B_PDS_6F8]	= { "pds 6f8",	0x9c,	0x1000, PTE_SHARED, FW_PDS_6F8 },
	[B_CODE2]	= { "code2",	UK_CODE2_SIZE, 0x1000, PTE_RO, FW_CODE2 },
	[B_PDS_704]	= { "pds 704",	0x258,	0x1000, PTE_SHARED, FW_PDS_704 },
	[B_TAB_784]	= { "tab 784",	0xf0,	0x1000, PTE_RO, FW_TAB_784 },
	[B_TAB_78C]	= { "tab 78c",	0x140,	0x1000, PTE_RO, FW_TAB_78C },
	[B_740]		= { "ctl 740",	0x1a4,	0x1000, PTE_SHARED, 0 },
	[B_HOST]	= { "host 748",	0xec,	0x1000, PTE_SHARED, 0 },
	[B_750]		= { "750",	0x2000,	0x1000, PTE_SHARED, 0 },
	[B_758]		= { "758",	8,	0x1000, PTE_SHARED, 0 },
	[B_KICKER]	= { "kicker 760", 0x1000, 0x1000, PTE_SHARED, 0 },
	[B_768]		= { "768",	0x1500c, 0x1000, PTE_SHARED, 0 },
	[B_TAB_774]	= { "tab 774",	0x28,	0x1000, PTE_RO, FW_TAB_774 },
	[B_IDX_77C]	= { "idx 77c",	0x60,	0x1000, PTE_RO, 0 },
	[B_794]		= { "794",	0x14000, 0x100000, 0, 0 },
	[B_798]		= { "798",	0x1000,	0x1000, 0, 0 },
	[B_79C]		= { "79c",	0x8000,	0x1000, 0, 0 },
	[B_7A0]		= { "7a0",	0x28000, 0x1000, 0, 0 },
	/* one transfer queue */
	[B_TQ_CTX]	= { "tq ctx",	0x24,	0x1000, PTE_SHARED, 0 },
	[B_TQ_CCB]	= { "tq ccb",	0x10000, 0x1000, PTE_SHARED, 0 },
	[B_TQ_CTL]	= { "tq ctl",	8,	0x1000, PTE_SHARED, 0 },
	/* one render queue (TA + 3D) */
	[B_R_CTX]	= { "r ctx",	0x40,	0x1000, PTE_SHARED, 0 },
	[B_R_CCB]	= { "r ccb",	0x10000, 0x1000, PTE_SHARED, 0 },
	[B_R_CTL]	= { "r ctl",	8,	0x1000, PTE_SHARED, 0 },
	[B_SCRATCH]	= { "scratch",	0x1000,	0x1000, PTE_SHARED, 0 },
};

/* A clock-gating mode: 1 keeps the clock on, 2 lets the hardware gate it.
 * The stock driver uses 2 when automatic clock gating is enabled; bring-up
 * wants 1. */
static unsigned int clock_mode = 1;
module_param(clock_mode, uint, 0444);
MODULE_PARM_DESC(clock_mode, "SGX clock gating: 1 = always on (default), 2 = automatic");

/* The interrupt is taken only when asked for, from the next microkernel
 * boot on: what raises it and how it clears is still being learnt. */
static bool use_irq;
module_param_named(irq, use_irq, bool, 0644);
MODULE_PARM_DESC(irq, "take the SGX interrupt (AIC 49) from the next microkernel boot");

/* Start the microkernel at probe; off leaves it to the first render node
 * client or to debugfs. */
static bool autoboot = true;
module_param(autoboot, bool, 0444);
MODULE_PARM_DESC(autoboot, "start the microkernel at boot (default on)");

/* What a kick asks the microkernel to invalidate when page tables changed
 * since the last one, the DDK's way: the cache-control word of the kernel
 * CCB command.  The DDK's mmu.c asks for the page table and directory
 * caches and, on multi-core parts, the system level cache with them, which
 * holds PDEs and PTEs there.  Before the first kick the boot has done it. */
static unsigned int mmu_inval_cc = SGX_CC_INVAL_BIF_PT | SGX_CC_INVAL_BIF_PD |
				   SGX_CC_INVAL_BIF_SL;
module_param(mmu_inval_cc, uint, 0644);
MODULE_PARM_DESC(mmu_inval_cc, "cache-control sent with a render after the page tables changed (default 7)");

/* Unhandled interrupts in a row before the line is switched off. */
#define SGX_IRQ_STORM			16

/* The only way to a register.  A read of bank 0 or of a core bank this SGX
 * does not have would hang the bus, so it is refused -- with a warning,
 * since it can only be a bug. */
static bool sgx_readable(struct apple_sgx *sgx, u32 off)
{
	if (off >= SGX_MASTER && off < SGX_CORE(0))
		return true;
	return off >= SGX_CORE(0) && off < SGX_CORE(sgx->ncores);
}

static u32 sgx_read(struct apple_sgx *sgx, u32 off)
{
	if (WARN_ONCE(!sgx_readable(sgx, off),
		      "apple-sgx: refused read at +0x%05x\n", off))
		return ~0u;
	return readl(sgx->regs + off);
}

static void sgx_write(struct apple_sgx *sgx, u32 off, u32 val)
{
	/* Bank 0 is write-only, which is what it is for. */
	if (WARN_ONCE(off >= SGX_CORE(sgx->ncores),
		      "apple-sgx: refused write at +0x%05x\n", off))
		return;
	writel(val, sgx->regs + off);
}

/* The same mode in every 2-bit field listed by shift. */
static u32 sgx_modes(unsigned int m, const u8 *shifts, int n)
{
	u32 v = 0;

	while (n--)
		v |= m << shifts[n];
	return v;
}

static void sgx_init(struct apple_sgx *sgx)
{
	/* CLKGATECTL: ISP, ISP2, TSP, TE, MTE, DPM, VDM, PDS, IDXFIFO, TA,
	 * BIF_CORE.  CLKGATECTL2: PBE, TCU_L2, UCACHEL2, USE0, ITR0, TEX0,
	 * USE1, ITR1, TEX1, DCU_L2, DCU1_L0L1, DCU0_L0L1 (sgx544defs.h). */
	static const u8 ctl[] = { 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20 };
	static const u8 ctl2[] = { 0, 2, 4, 6, 8, 10, 14, 16, 18, 22, 24, 26 };
	static const u8 master2[] = { 2, 4, 6, 8 };
	unsigned int m = clock_mode, i;
	u32 cores = 0, bypass = SGX_SLC_BYPASS_VAL;

	for (i = 0; i < sgx->ncores; i++)
		cores |= m << (2 * i);
	if (sgx->brn_31195)
		bypass |= SGX_SLC_BYPASS_BRN_31195;

	/* Which cores exist, and the master's and each core's clocks.  The
	 * cores have no clock until MASTER_CLKGATECTL gives them one. */
	sgx_write(sgx, SGX_MASTER_CORE, sgx->ncores - 1);
	sgx_write(sgx, SGX_MASTER_CLKGATECTL2,
		  sgx_modes(m, master2, ARRAY_SIZE(master2)) | 1);
	sgx_write(sgx, SGX_MASTER_CLKGATECTL, cores);

	/* Reset, with the system-level cache set up while it is held. */
	sgx_write(sgx, SGX_MASTER_SOFT_RESET,
		  SGX_MASTER_RESET_ALL | GENMASK(sgx->ncores - 1, 0));
	sgx_write(sgx, SGX_MASTER_SLC_CTRL, SGX_SLC_CTRL_VAL);
	sgx_write(sgx, SGX_MASTER_SLC_CTRL_BYPASS, bypass);
	sgx_write(sgx, SGX_MASTER_SOFT_RESET, 0);
	/* The DDK waits 100 SGX clocks here: 1 us at ~100 MHz. */
	udelay(10);

	/* Every core's module clocks, through the broadcast bank.  Bit 28
	 * (SYSTEM_CLKG) goes with mode 1 only, as the stock driver has it. */
	sgx_write(sgx, SGX_BCAST + SGX_CLKGATECTL,
		  sgx_modes(m, ctl, ARRAY_SIZE(ctl)) |
		  (m == 1 ? BIT(28) : 0));
	sgx_write(sgx, SGX_BCAST + SGX_CLKGATECTL2,
		  sgx_modes(m, ctl2, ARRAY_SIZE(ctl2)));
	sgx_write(sgx, SGX_BCAST + SGX_REG_310, 1);
}

/* ---- the MMU ------------------------------------------------------------ */

/* Page tables are allocated as needed and kept.  A change to a live mapping
 * only reaches the GPU once its MMU caches are invalidated: before the
 * microkernel boots that is the boot, afterwards the next kick asks the
 * microkernel for it (mmu_dirty, SGX_CC_INVAL_BIF_*). */
int apple_sgx_mmu_map(struct apple_sgx *sgx, u32 va, dma_addr_t pa, size_t size,
		      u32 flags)
{
	size_t done;
	int ret = 0;

	mutex_lock(&sgx->mmu_lock);
	for (done = 0; done < size; done += SGX_PAGE_SIZE) {
		u32 v = va + done, pde = v >> 22, pte = (v >> 12) & 0x3ff;

		if (!sgx->pt[pde]) {
			sgx->pt[pde] = dmam_alloc_coherent(sgx->dev, SGX_PAGE_SIZE,
							   &sgx->pt_dma[pde], GFP_KERNEL);
			if (!sgx->pt[pde]) {
				ret = -ENOMEM;
				break;
			}
			sgx->pd[pde] = lower_32_bits(sgx->pt_dma[pde]) | SGX_PDE_VALID;
		}
		sgx->pt[pde][pte] = lower_32_bits(pa + done) | flags | SGX_PTE_VALID;
	}
	atomic_set(&sgx->mmu_dirty, 1);
	mutex_unlock(&sgx->mmu_lock);
	return ret;
}

void apple_sgx_mmu_unmap(struct apple_sgx *sgx, u32 va, size_t size)
{
	size_t done;

	mutex_lock(&sgx->mmu_lock);
	for (done = 0; done < size; done += SGX_PAGE_SIZE) {
		u32 v = va + done, pde = v >> 22, pte = (v >> 12) & 0x3ff;

		if (sgx->pt[pde])
			sgx->pt[pde][pte] = 0;
	}
	atomic_set(&sgx->mmu_dirty, 1);
	mutex_unlock(&sgx->mmu_lock);
}

static int sgx_buf_alloc(struct apple_sgx *sgx, enum sgx_buf_id id)
{
	const struct sgx_buf_desc *d = &sgx_buf_descs[id];
	struct sgx_buf *b = &sgx->buf[id];
	int ret;

	b->size = ALIGN(d->size, SGX_PAGE_SIZE);
	b->cpu = dmam_alloc_coherent(sgx->dev, b->size, &b->dma, GFP_KERNEL);
	if (!b->cpu)
		return -ENOMEM;
	b->va = ALIGN(sgx->va_next, d->align);
	sgx->va_next = b->va + b->size;
	ret = apple_sgx_mmu_map(sgx, b->va, b->dma, b->size, d->pte);
	if (ret)
		return ret;
	dev_dbg(sgx->dev, "%-10s va 0x%08x pa %pad size 0x%zx\n",
		d->name, b->va, &b->dma, b->size);
	return 0;
}

static int sgx_bufs_alloc(struct apple_sgx *sgx)
{
	int i, ret;

	if (sgx->bufs_ready)
		return 0;
	sgx->va_next = SGX_VA_BASE;
	for (i = 0; i < B_NUM; i++) {
		ret = sgx_buf_alloc(sgx, i);
		if (ret)
			return ret;
	}
	if (sgx->va_next - SGX_VA_BASE > SZ_8M)
		return -E2BIG;
	sgx->bufs_ready = true;
	return 0;
}

/* ---- the microkernel -------------------------------------------------- */

/* A LIMM's 32-bit immediate lives in three fields: bits 20:0 of the low
 * word, and imm[31:26] / imm[25:21] at bits 17:12 / 8:4 of the high word
 * (as the stock driver's loader does it). */
static void uk_set_limm(u32 *insn, u32 imm)
{
	insn[0] = (insn[0] & ~GENMASK(20, 0)) | (imm & GENMASK(20, 0));
	insn[1] = (insn[1] & 0xfffc0e0f) | ((imm >> 14) & 0x3f000) |
		  ((imm >> 17) & 0x1f0);
}

/* A USE code address as the PDS programs take it: relative to the code
 * buffer, in 8-byte units, shifted. */
static u32 uk_use(struct apple_sgx *sgx, u32 va)
{
	return ((va - sgx->buf[B_CODE].va) << 1) & 0xfffff0;
}

static u32 uk_at(struct apple_sgx *sgx, u32 off)
{
	return sgx->buf[B_CODE].va + UK_START + off;
}

static void sgx_uk_fill(struct apple_sgx *sgx, const struct sgx_fw_region *fw)
{
	struct sgx_buf *b = sgx->buf;
	u32 *code = b[B_CODE].cpu, *uk = code + UK_START / 4;
	u32 *pds, *t, i;
	static const struct { u32 off; u32 imm; } consts[] = {
		{ 0x0000, 0x01001000 }, { 0x0058, 0x00800900 }, { 0x00f0, 7 },
		{ 0x7f98, 0x02800900 }, { 0x86c8, 0x02800900 },
	};

	/* Contents, as the loader and setup leave them. */
	for (i = 0; i < B_NUM; i++) {
		const struct sgx_buf_desc *d = &sgx_buf_descs[i];

		memset(b[i].cpu, 0, b[i].size);
		if (d->fw)
			memcpy(b[i].cpu, fw[d->fw].data, d->size);
	}
	memcpy(code, fw[FW_BOOT].data, fw[FW_BOOT].size);
	memcpy(uk, fw[FW_UKERNEL].data, fw[FW_UKERNEL].size);
	for (i = 0; i < 24; i++)
		b[B_IDX_77C].cpu[i] = i;

	/* The microkernel's LIMMs that carry addresses and constants. */
	for (i = 0; i < ARRAY_SIZE(consts); i++)
		uk_set_limm(uk + consts[i].off / 4, consts[i].imm);
	uk_set_limm(uk + 0x010 / 4, b[B_PDS_6E0].va >> 4);
	uk_set_limm(uk + 0x018 / 4, b[B_740].va);
	uk_set_limm(uk + 0x060 / 4, b[B_PDS_6E8].va >> 4);
	uk_set_limm(uk + 0x0e0 / 4, b[B_PDS_6F0].va & ~0xfu);
	uk_set_limm(uk + 0x2b0 / 4, b[B_7A0].va);
	uk_set_limm(uk + 0x340 / 4, b[B_79C].va);
	uk_set_limm(uk + 0x390 / 4, b[B_794].va);
	uk_set_limm(uk + 0x3b0 / 4, b[B_798].va);
	uk_set_limm(uk + 0x588 / 4, b[B_TAB_774].va);
	uk_set_limm(uk + 0x7fb0 / 4, b[B_PDS_6F8].va >> 4);
	uk_set_limm(uk + 0x86e0 / 4, b[B_PDS_6F8].va >> 4);

	/* The boot program: where core 0 continues in the microkernel, and
	 * where the others go into the second code buffer. */
	code[0x70 / 4] |= ((UK_START + 0x9d40) >> 3) & 0xfffff;
	code[0x240 / 4] |= ((b[B_CODE2].va + 0x70 - b[B_CODE].va) >> 3) & 0xfffff;

	/* The PDS programs, pointed at microkernel entry points. */
	pds = b[B_PDS_6D8].cpu;
	pds[0] = uk_use(sgx, uk_at(sgx, 0));
	pds[1] = 1;
	pds[2] = 0;
	pds = b[B_PDS_6E0].cpu;
	pds[0] = b[B_740].va;
	pds[1] = 0x3f;
	pds[2] = 0;
	pds[4] = uk_use(sgx, uk_at(sgx, 0x90));
	pds[5] = 1;
	pds = b[B_PDS_6E8].cpu;
	pds[0] = uk_use(sgx, uk_at(sgx, 0xc0));
	pds[1] = 1;
	pds[2] = 0;
	pds = b[B_PDS_6F0].cpu;
	pds[0] = 0;
	pds[1] = 0x100;
	pds[2] = uk_use(sgx, uk_at(sgx, 0x10d20));
	pds[3] = uk_use(sgx, uk_at(sgx, 0xd0d0));
	pds[4] = 0x200;
	pds[6] = uk_use(sgx, uk_at(sgx, 0x4340));
	pds[7] = uk_use(sgx, uk_at(sgx, 0xac50));
	pds[8] = 1;
	pds[9] = 0;
	pds[10] = uk_use(sgx, uk_at(sgx, 0x1470));
	pds = b[B_PDS_6F8].cpu;
	pds[0] = 0;
	pds[1] = 0x200;
	pds[2] = 0;
	pds[3] = 0x8000f;
	pds[4] = uk_use(sgx, uk_at(sgx, 0xd3f0));
	pds[5] = 1;
	pds[6] = 0x240100;
	pds[7] = 0x420000;
	pds[8] = uk_use(sgx, uk_at(sgx, 0x4510));
	pds[9] = 1;
	pds[12] = uk_use(sgx, uk_at(sgx, 0x11190));
	pds[13] = 1;
	pds[16] = uk_use(sgx, uk_at(sgx, 0xad20));
	pds[17] = 1;
	pds = b[B_PDS_704].cpu;
	pds[0] = 0;
	pds[1] = 0x100;
	pds[2] = 0;
	pds[4] = b[B_740].va;
	pds[5] = 0x200;
	pds[8] = uk_use(sgx, b[B_CODE2].va + 0x30) | 0x8000000;
	pds[9] = 1;

	/* The blob's tables, linked up. */
	t = b[B_TAB_774].cpu;
	t[0] |= b[B_TAB_784].va >> 4;
	t[5] |= (b[B_TAB_784].va + 0x50) >> 4;
	t[7] |= (b[B_TAB_784].va + 0xa0) >> 4;
	t[3] |= b[B_IDX_77C].va;
	t = b[B_TAB_784].cpu;
	t[0x20 / 4] = 0x1000001;
	t[0x70 / 4] = 0x1000101;
	t[0xc0 / 4] = 0x1000231;

	/* The block the microkernel finds through the LIMM at +0x18: the other
	 * buffers and the page directory.  The timing and duty-cycle
	 * parameters at +0xb0..+0xd8 are the stock driver's defaults; this
	 * board has no device-tree overrides. */
	t = b[B_740].cpu;
	t[0] = b[B_740].va;
	t[1] = b[B_HOST].va;
	t[2] = b[B_758].va;
	t[0x14 / 4] = 2;
	/* The stock driver's defaults when the device tree has no duty or
	 * scale properties. */
	t[0xb0 / 4] = 0x3aba40d9;
	t[0xb4 / 4] = 0x44af0000;	/* 1400.0 */
	t[0xb8 / 4] = 0x3f628c7a;
	t[0xbc / 4] = 0x42451141;
	t[0xc0 / 4] = 0x41c2f9f9;
	t[0xc4 / 4] = 0x428c79f8;
	t[0xc8 / 4] = 0x3a3b3ee7;	/* 1 / 1400.0 */
	t[0xcc / 4] = 0x3f4ccccd;
	t[0xd0 / 4] = 0x3f7f9724;
	t[0xd4 / 4] = 0x3ad1b717;
	t[0xe0 / 4] = 0x10;
	t[0xf4 / 4] = b[B_750].va;
	t[0xf8 / 4] = lower_32_bits(sgx->pd_dma);
	t[0x154 / 4] = b[B_768].va;

	/* The transfer queue's hardware context: valid, the
	 * channel's number (set by the base class; 0 here), the
	 * page directory, its CCB and the CCB's control words. */
	t = b[B_TQ_CTX].cpu;
	t[0] = 1;
	t[1] = 0;
	t[2] = lower_32_bits(sgx->pd_dma);
	t[3] = b[B_TQ_CCB].va;
	t[4] = b[B_TQ_CTL].va;

	/* The render queue's context: the same, plus word 6 = the parameter
	 * buffer descriptor ("rkick" sets it). */
	t = b[B_R_CTX].cpu;
	t[0] = 1;
	t[1] = 0;
	t[2] = lower_32_bits(sgx->pd_dma);
	t[3] = b[B_R_CCB].va;
	t[4] = b[B_R_CTL].va;

	/* Host control parameters.  The microkernel's
	 * timer period is "hclk" / 1000 -- ticks per millisecond -- and the
	 * lockup check runs every 16 periods; active power management and duty
	 * management stay off (sample rate 0, enable byte 0). */
	t = b[B_HOST].cpu;
	t[HOST_HWR_SAMPLE_RATE] = 0x10;
	t[HOST_UK_TIMER_CLOCK] = SGX_CLOCK_KHZ;
	t[0x1c / 4] = 1;		/* not an FPGA */
	t[0x98 / 4] = 0x44af2000;	/* 1401.0 */
	t[0x9c / 4] = 0x44af2000;

	/* Everything above is in write-combined memory; it has to be out
	 * before the GPU is told to look. */
	wmb();
}

static void sgx_uk_regs(struct apple_sgx *sgx)
{
	struct sgx_buf *b = sgx->buf;
	unsigned int n;
	int i;

	/* The BIF: one page directory for every requestor, then the MMU on
	 * (the stock driver's cold initialisation). */
	sgx_write(sgx, SGX_BCAST + SGX_BIF_CTRL, 0);
	udelay(10);
	sgx_write(sgx, SGX_BCAST + SGX_BIF_BANK0, 0x77077);
	sgx_write(sgx, SGX_BCAST + SGX_BIF_BANK1, 0);
	sgx_write(sgx, SGX_BCAST + SGX_BIF_BANK_SET, 0);
	for (i = 0; i < 8; i++)
		sgx_write(sgx, SGX_BCAST + SGX_BIF_DIR_LIST_BASE(i),
			  lower_32_bits(sgx->pd_dma));
	/* Tiled windows, as the DDK lays the register out (sgx543defs.h):
	 * first MiB in 11:0, last in 23:12, configuration in 27:24.  0xb is
	 * stride 4096 -- measured on 2026-10-04: within each 64 KiB, address
	 * bits 11:8 and 15:12 trade places, 256-byte x 16-line tiles -- and
	 * 0xc presumably 8192.  The render node keeps its buffers out of
	 * them (apple_sgx.h). */
	sgx_write(sgx, SGX_BCAST + SGX_BIF_TILE1, 0xbeffe00);	/* 0xe0000000-0xefffffff */
	sgx_write(sgx, SGX_BCAST + SGX_BIF_TILE2, 0xcffff00);	/* 0xf0000000-0xffffffff */
	sgx_write(sgx, SGX_BCAST + SGX_BIF_CTRL_INVAL, 8);
	sgx_write(sgx, SGX_MASTER_BIF_MMU_CTRL, 2);
	for (n = 0; n < sgx->ncores; n++)
		sgx_write(sgx, SGX_CORE(n) + SGX_BIF_MMU_CTRL, 2);
	sgx_write(sgx, SGX_MASTER_BIF_CTRL, 0);
	udelay(10);

	/* The rest, in the stock driver's order; names unknown
	 * for most. */
	sgx_write(sgx, SGX_BCAST + 0xa58, 0);
	sgx_write(sgx, SGX_BCAST + SGX_EVENT_TIMER, 0);
	sgx_write(sgx, SGX_BCAST + SGX_POWER, 0);
	sgx_write(sgx, SGX_BCAST + 0xa7c, 0);
	sgx_write(sgx, SGX_BCAST + 0xa80, 0);
	sgx_write(sgx, SGX_BCAST + 0xa00, 0x7c000);
	sgx_write(sgx, SGX_BCAST + 0xabc, 0x4c);
	sgx_write(sgx, SGX_BCAST + 0xaa0, 0x7fffffff);
	sgx_write(sgx, SGX_BCAST + 0x818, 0);
	sgx_write(sgx, SGX_BCAST + 0x804, 0x5e0);
	sgx_write(sgx, SGX_BCAST + 0x814, 0xffff);
	sgx_write(sgx, SGX_BCAST + 0xa74, 0x0da08200);
	sgx_write(sgx, SGX_BCAST + 0xb30, 0x100);
	sgx_write(sgx, SGX_MASTER_REG_4808, 4);
	sgx_write(sgx, SGX_MASTER_REG_4144, 1000);
	sgx_write(sgx, SGX_MASTER_REG_414C, 0);
	for (n = 0; n < sgx->ncores; n++) {
		sgx_write(sgx, SGX_CORE(n) + SGX_BOOT_ENTRY,
			  (n ? UK_ENTRY_OTHERS : UK_ENTRY_CORE0) >> 3);
		sgx_write(sgx, SGX_CORE(n) + SGX_BOOT_ENTRY + 0x14, 0);
	}

	/* Events, code bases and the event PDS programs. */
	sgx_write(sgx, SGX_BCAST + SGX_EVENT_HOST_CLEAR, ~0u);
	sgx_write(sgx, SGX_BCAST + SGX_EVENT_HOST_CLEAR2, ~0u);
	sgx_write(sgx, SGX_BCAST + SGX_REG_140, ~0u);
	sgx_write(sgx, SGX_BCAST + SGX_EVENT_HOST_ENABLE, 0x4000);
	sgx_write(sgx, SGX_BCAST + SGX_EVENT_HOST_ENABLE2, 0);
	sgx_write(sgx, SGX_BCAST + SGX_REG_13C, 0);
	for (i = 2; i <= 15; i++)
		sgx_write(sgx, SGX_BCAST + SGX_USE_CODE_BASE(i), 0);
	sgx_write(sgx, SGX_BCAST + SGX_USE_CODE_BASE(0),
		  0x0c000000 | (b[B_CODE].va >> 6));
	sgx_write(sgx, SGX_BCAST + SGX_EVENT_PDS_EXEC, b[B_PDS_704].va & ~0xfu);
	sgx_write(sgx, SGX_BCAST + SGX_EVENT_PDS_DATA, 3);
	sgx_write(sgx, SGX_BCAST + SGX_EVENT_PDS_INFO, 0x12001);
	sgx_write(sgx, SGX_CORE(0) + SGX_EVENT_PDS_EXEC, b[B_PDS_6D8].va & ~0xfu);
	sgx_write(sgx, SGX_CORE(0) + SGX_EVENT_PDS_DATA, 1);
	sgx_write(sgx, SGX_CORE(0) + SGX_EVENT_PDS_INFO, 0x12001);
	sgx_write(sgx, SGX_BCAST + SGX_USE_CODE_BASE(1), b[B_TAB_78C].va >> 6);
	sgx_write(sgx, SGX_BCAST + SGX_EVENT_KICKER, b[B_KICKER].va);
	sgx_write(sgx, SGX_BCAST + 0x630, 2);
	sgx_write(sgx, SGX_CORE(0) + 0xa58, 0x2200c000);
	sgx_write(sgx, SGX_MASTER_REG_4A58, 0x0804300f);
}

/*
 * Read the container sgxcarve writes: a 32-byte header (magic, version,
 * region count, header size, module UUID), a 16-byte entry per region
 * (id, offset, size), then the region bytes.  The loader refuses a module
 * build or a region size it does not know, so a mismatched firmware can
 * never be half-loaded.
 */
static int sgx_parse_fw(struct apple_sgx *sgx, const struct firmware *fw,
			struct sgx_fw_region *regions)
{
	const u8 *p = fw->data;
	u32 count, hdr, i;

	if (fw->size < SGXF_HDR_SIZE ||
	    get_unaligned_le32(p) != SGXF_MAGIC ||
	    get_unaligned_le32(p + 4) != SGXF_VERSION)
		return -EINVAL;
	count = get_unaligned_le32(p + 8);
	hdr = get_unaligned_le32(p + 12);
	if (count < FW_REGIONS - 1 || count > SGXF_MAX_REGIONS ||
	    hdr != SGXF_HDR_SIZE + SGXF_ENTRY_SIZE * count || hdr > fw->size)
		return -EINVAL;
	if (memcmp(p + 16, sgx_fw_uuid, sizeof(sgx_fw_uuid))) {
		dev_err(sgx->dev, "%s: unknown module build\n", SGX_FW_NAME);
		return -EINVAL;
	}

	for (i = 0; i < count; i++) {
		const u8 *e = p + SGXF_HDR_SIZE + SGXF_ENTRY_SIZE * i;
		u32 id = get_unaligned_le32(e);
		u32 off = get_unaligned_le32(e + 4);
		u32 size = get_unaligned_le32(e + 8);

		if (off < hdr || size > fw->size || off > fw->size - size)
			return -EINVAL;
		if (id && id < SGXF_MAX_REGIONS) {
			regions[id].data = p + off;
			regions[id].size = size;
		}
	}

	for (i = FW_BOOT; i < FW_REGIONS; i++)
		if (!regions[i].data)
			return -ENOENT;
	if (regions[FW_BOOT].size != UK_BOOT_SIZE ||
	    regions[FW_UKERNEL].size != UK_CODE_SIZE)
		return -EINVAL;
	for (i = 0; i < B_NUM; i++) {
		u8 id = sgx_buf_descs[i].fw;

		if (id && regions[id].size != sgx_buf_descs[i].size)
			return -EINVAL;
	}
	return 0;
}

static int sgx_boot_ukernel(struct apple_sgx *sgx)
{
	struct sgx_fw_region regions[SGXF_MAX_REGIONS] = { };
	const struct firmware *fw;
	u32 *host, val;
	unsigned int n;
	int ret;

	ret = request_firmware(&fw, SGX_FW_NAME, sgx->dev);
	if (ret) {
		if (ret == -ENOENT)
			dev_info(sgx->dev,
				 "%s not provisioned yet; the first render-node client will retry\n",
				 SGX_FW_NAME);
		else
			dev_err(sgx->dev, "no %s: %d\n", SGX_FW_NAME, ret);
		return ret;
	}
	ret = sgx_parse_fw(sgx, fw, regions);
	if (ret) {
		dev_err(sgx->dev, "%s: unrecognised layout: %d\n", SGX_FW_NAME, ret);
		goto out;
	}

	ret = sgx_bufs_alloc(sgx);
	if (ret) {
		dev_err(sgx->dev, "GPU buffers: %d\n", ret);
		goto out;
	}
	/* Quiet while the GPU is reset under it; back on once it is up. */
	if (sgx->irq_on) {
		disable_irq(sgx->irq);
		sgx->irq_on = false;
	}

	/* From a clean GPU: clocks and master reset again, then load. */
	sgx_init(sgx);
	sgx_uk_fill(sgx, regions);
	/* sgx_uk_regs() invalidates the MMU's caches (BIF_CTRL_INVAL): what
	 * was mapped until now needs nothing more from the first kick */
	atomic_set(&sgx->mmu_dirty, 0);
	sgx_uk_regs(sgx);

	/* The buffers sit at GPU addresses from 0x80000000, which is also where
	 * this machine's RAM is: a requestor left in MMU bypass (the reset value
	 * of MASTER_BIF_CTRL has VDM, IPF and DPM in bypass, bits 17-19) would
	 * write straight into the kernel.  So nothing is kicked unless the MMU
	 * is on for everyone. */
	for (n = 0; n <= sgx->ncores; n++) {
		u32 off = n ? SGX_CORE(n - 1) + SGX_BIF_CTRL : SGX_MASTER_BIF_CTRL;

		val = sgx_read(sgx, off);
		if (val & GENMASK(31, 16)) {
			dev_err(sgx->dev, "MMU bypass still on (+0x%05x = 0x%08x), not starting\n",
				off, val);
			ret = -EIO;
			goto out;
		}
	}
	dev_info(sgx->dev, "starting the microkernel: code at GPU 0x%08x, page directory %pad\n",
		 sgx->buf[B_CODE].va, &sgx->pd_dma);

	/* The handshake: host-control word 0 cleared, the kick count bumped,
	 * core 0 kicked; the microkernel sets bit 0 when it is up
	 * (as the stock driver's boot path does). */
	host = sgx->buf[B_HOST].cpu;
	WRITE_ONCE(host[0], 0);
	WRITE_ONCE(sgx->buf[B_KICKER].cpu[0], 1);
	wmb();
	sgx_write(sgx, SGX_CORE(0) + SGX_EVENT_KICK, 1);

	ret = read_poll_timeout(READ_ONCE, val, val & 1, 100, 500000, false,
				host[0]);
	if (ret) {
		dev_err(sgx->dev, "microkernel did not answer: host[0] 0x%08x, "
			"core0 EVENT_STATUS 0x%08x BIF_INT_STAT 0x%08x BIF_FAULT 0x%08x, "
			"master BIF_INT_STAT 0x%08x BIF_FAULT 0x%08x\n",
			READ_ONCE(host[0]),
			sgx_read(sgx, SGX_CORE(0) + SGX_EVENT_STATUS),
			sgx_read(sgx, SGX_CORE(0) + SGX_BIF_INT_STAT),
			sgx_read(sgx, SGX_CORE(0) + SGX_BIF_FAULT),
			sgx_read(sgx, SGX_MASTER_BIF_INT_STAT),
			sgx_read(sgx, SGX_MASTER_BIF_FAULT));
		ret = -ETIMEDOUT;
	} else {
		dev_info(sgx->dev, "microkernel is up: host[0] 0x%08x\n",
			 READ_ONCE(host[0]));
		/* The code base the render node's programs are relative to:
		 * the two the stock GL driver uses (its are 0, code at 0x1000).
		 * Set once the microkernel runs, as sgx2d's packs did. */
		sgx_write(sgx, SGX_BCAST + SGX_USE_CODE_BASE(3), SGX_CODE_BASE >> 6);
		sgx_write(sgx, SGX_BCAST + SGX_USE_CODE_BASE(5), SGX_CODE_BASE >> 6);
		if (use_irq && sgx->irq > 0) {
			sgx->irq_run = 0;
			sgx->irq_on = true;
			enable_irq(sgx->irq);
			dev_info(sgx->dev, "interrupt %d on\n", sgx->irq);
		}
	}
out:
	release_firmware(fw);
	return ret;
}

/* The microkernel signals the host with SW_EVENT (EVENT_HOST_ENABLE is
 * 0x4000, as the stock driver sets it).  The stock handler reads
 * EVENT_STATUS in the broadcast bank and clears through EVENT_HOST_CLEAR
 * with MASTER_INTERRUPT (bit 31) added, as the DDK's SGX_ISRHandler does.
 * Bank 0 reads hung the bus during bring-up (before the cores had clocks),
 * so this reads each core's own bank instead and clears it there.  If the
 * source is somewhere else, the line would never drop: after
 * SGX_IRQ_STORM unhandled interrupts in a row it is switched off. */
static irqreturn_t sgx_irq_handler(int irq, void *data)
{
	struct apple_sgx *sgx = data;
	bool handled = false, cleared = true;
	unsigned int n;

	for (n = 0; n < sgx->ncores; n++) {
		u32 base = SGX_CORE(n), st;

		st = sgx_read(sgx, base + SGX_EVENT_STATUS) &
		     sgx_read(sgx, base + SGX_EVENT_HOST_ENABLE);
		if (!st)
			continue;
		sgx_write(sgx, base + SGX_EVENT_HOST_CLEAR, st | BIT(31));
		sgx->irq_events[n] |= st;
		handled = true;
		if (sgx_read(sgx, base + SGX_EVENT_STATUS) & st)
			cleared = false;
	}
	if (!handled)
		sgx->irq_unhandled++;
	/* No event found, or one that does not clear: either way the line
	 * would stay up forever. */
	if (!handled || !cleared) {
		if (++sgx->irq_run >= SGX_IRQ_STORM) {
			disable_irq_nosync(irq);
			sgx->irq_on = false;
			dev_err(sgx->dev, "interrupt: %u in a row %s, switched off\n",
				sgx->irq_run, handled ? "with an event that does not clear" :
				"with no core event");
		}
		if (!handled)
			return IRQ_NONE;
	} else {
		sgx->irq_run = 0;
	}
	sgx->irq_count++;
	if (sgx->bufs_ready)
		sgx->irq_host_flags =
			READ_ONCE(sgx->buf[B_HOST].cpu[HOST_INTERRUPT_FLAGS]);
	return IRQ_HANDLED;
}

/* One command through the kernel CCB, the way the stock driver does it, then wait
 * for the microkernel to take it (its read offset reaches ours). */
static int sgx_send_cmd(struct apple_sgx *sgx, enum sgx_cmd_type type,
			u32 cache_control, u32 data0, u32 data1)
{
	u32 *ctl = sgx->buf[B_758].cpu, *kicker = sgx->buf[B_KICKER].cpu;
	struct sgx_ccb_cmd *cmd;
	u32 wo, ro, val;
	ktime_t start;
	int ret;

	unsigned long flags;

	if (sgx->boot_result != 1)
		return -ENODEV;
	spin_lock_irqsave(&sgx->ccb_lock, flags);
	wo = READ_ONCE(ctl[0]);
	ro = READ_ONCE(ctl[1]);
	if (wo >= SGX_CCB_SIZE || ro >= SGX_CCB_SIZE) {
		spin_unlock_irqrestore(&sgx->ccb_lock, flags);
		dev_err(sgx->dev, "CCB offsets out of range: write %u read %u\n",
			wo, ro);
		return -EIO;
	}
	if (((wo + 1) & (SGX_CCB_SIZE - 1)) == ro) {
		spin_unlock_irqrestore(&sgx->ccb_lock, flags);
		return -EBUSY;
	}

	cmd = (struct sgx_ccb_cmd *)sgx->buf[B_750].cpu + wo;
	memset(cmd, 0, sizeof(*cmd));
	cmd->service = (UK_START + uk_handlers[type].off) >> 3;
	cmd->cache_control = cache_control;
	cmd->data[0] = data0;
	cmd->data[1] = data1;
	wmb();

	wo = (wo + 1) & (SGX_CCB_SIZE - 1);
	WRITE_ONCE(ctl[0], wo);
	WRITE_ONCE(kicker[0], (READ_ONCE(kicker[0]) + 1) & 0xff);
	wmb();
	start = ktime_get();
	sgx_write(sgx, SGX_CORE(0) + SGX_EVENT_KICK, 1);

	/* taken in tens of microseconds; never sleeps */
	ret = read_poll_timeout_atomic(READ_ONCE, val, val == wo, 1, 500000, false,
				       ctl[1]);
	spin_unlock_irqrestore(&sgx->ccb_lock, flags);
	if (!sgx->quiet || ret)
	dev_info(sgx->dev,
		 "command %s (cc 0x%x, data 0x%x 0x%x): %s after %lld us, CCB write %u read %u, "
		 "core0 EVENT_STATUS 0x%08x\n",
		 uk_handlers[type].name, cache_control, data0, data1,
		 ret ? "NOT TAKEN" : "taken",
		 ktime_us_delta(ktime_get(), start), wo, READ_ONCE(ctl[1]),
		 sgx_read(sgx, SGX_CORE(0) + SGX_EVENT_STATUS));
	return ret;
}

/* POWER, and for power-off and idle the answer in the power status
 * (the DDK's SGXPrePowerState).  After a power-off
 * the microkernel has stopped; only a new boot starts it again. */
static int sgx_power_cmd(struct apple_sgx *sgx, u32 powercmd)
{
	u32 *host = sgx->buf[B_HOST].cpu, done = 0, val;
	int ret;

	if (powercmd == SGX_POWERCMD_POWEROFF)
		done = SGX_POWMAN_POWEROFF_COMPLETE;
	else if (powercmd == SGX_POWERCMD_IDLE)
		done = SGX_POWMAN_IDLE_COMPLETE;
	else if (powercmd != SGX_POWERCMD_RESUME)
		return -EINVAL;

	ret = sgx_send_cmd(sgx, SGX_CMD_POWER, 0, 0, powercmd);
	if (ret || !done)
		return ret;
	ret = read_poll_timeout(READ_ONCE, val, val & done, 10, 500000, false,
				host[HOST_POWER_STATUS]);
	dev_info(sgx->dev, "power status 0x%08x: %s\n", val,
		 ret ? "no answer" : powercmd == SGX_POWERCMD_POWEROFF ?
		 "powered off" : "idle");
	if (ret)
		return ret;
	/* Keep NO_WORK and clear the status for the next request. */
	WRITE_ONCE(host[HOST_POWER_STATUS], 0);
	wmb();
	if (powercmd == SGX_POWERCMD_POWEROFF)
		sgx->boot_result = 0;
	return 0;
}

/* A transfer command, as the stock transfer queue builds it: 0x140
 * bytes in the channel's CCB, whose write offset counts bytes.  From the
 * GL payload (type 2, 0x7c bytes) come the register words at +0x00..+0x43
 * and +0x78..+0x9f; the kernel adds +0xa0 the size, +0xa4 a word the payload
 * must leave 0 -- by elimination the
 * DDK's SGXMKIF_TQFLAGS_* --, +0xa8 from the command descriptor, and:
 *
 *   +0xac/+0xb0   {address, value}: written by the microkernel when the
 *                 command is done
 *   +0xb4         number of dependencies, then from +0xb8 {address, value}
 *                 pairs: the command waits until (s32)(*address - value) >= 0
 *   +0x108        0x1800000
 *
 * (Both measured: a dependency that is not met stalls the queue until the
 * word changes AND the microkernel is kicked again; it does not poll.)
 * Then TRANSFER goes through the kernel CCB with Data[1] = the hardware
 * context, whose word 0 the microkernel clears when the queue is empty.
 *
 * "tq FLAGS VAL" sends one with no register words and no dependencies, its
 * completion written to the scratch buffer, and waits for VAL there.  With
 * FLAGS = 0x20 (DUMMYTRANSFER in the DDK: "uKernel only updates syncobjects
 * / status values") that is all the microkernel does. */
#define SGX_TQ_CMD_SIZE			0x140
#define SGX_TQ_CCB_SIZE			0x10000

/* A TA command: 0x120 bytes for this SGX (rgen.py's CMD_SIZE); the render
 * node takes up to SGX_R_CMD_MAX (apple_sgx_drm.h's APPLE_SGX_TA_CMD_*). */
#define SGX_R_CMD_MIN			0x11c
#define SGX_R_CMD_MAX			0x200

static int sgx_tq_cmd(struct apple_sgx *sgx, u32 flags, u32 val)
{
	u32 *ctl = sgx->buf[B_TQ_CTL].cpu, *scratch = sgx->buf[B_SCRATCH].cpu;
	u32 sva = sgx->buf[B_SCRATCH].va, *ctx = sgx->buf[B_TQ_CTX].cpu;
	u32 wo, *cmd, got;
	int ret;

	if (sgx->boot_result != 1)
		return -ENODEV;
	wo = READ_ONCE(ctl[0]);
	if (wo + SGX_TQ_CMD_SIZE > SGX_TQ_CCB_SIZE)
		return -ENOSPC;		/* no wrap-around; boot again */

	cmd = sgx->buf[B_TQ_CCB].cpu + wo / 4;
	memset(cmd, 0, SGX_TQ_CMD_SIZE);
	cmd[0xa0 / 4] = SGX_TQ_CMD_SIZE;
	cmd[0xa4 / 4] = flags;
	cmd[0xac / 4] = sva;
	cmd[0xb0 / 4] = val;
	cmd[0x108 / 4] = 0x01800000;
	WRITE_ONCE(scratch[0], 0);
	wmb();
	WRITE_ONCE(ctl[0], wo + SGX_TQ_CMD_SIZE);
	WRITE_ONCE(ctx[0], 1);
	wmb();

	ret = sgx_send_cmd(sgx, SGX_CMD_TRANSFER,
			   atomic_xchg(&sgx->mmu_dirty, 0) ? mmu_inval_cc : 0,
			   0, sgx->buf[B_TQ_CTX].va);
	if (ret)
		return ret;
	ret = read_poll_timeout(READ_ONCE, got, got == val, 10, 500000, false,
				scratch[0]);
	dev_info(sgx->dev,
		 "transfer (flags 0x%x): scratch 0x%08x 0x%08x (%s), tq write 0x%x read 0x%x ctx 0x%x, "
		 "BIF_FAULT core0 0x%08x master 0x%08x, host lockups %u assert 0x%08x\n",
		 flags, READ_ONCE(scratch[0]), READ_ONCE(scratch[1]),
		 ret ? "NOT WRITTEN" : "written", READ_ONCE(ctl[0]), READ_ONCE(ctl[1]),
		 READ_ONCE(ctx[0]),
		 sgx_read(sgx, SGX_CORE(0) + SGX_BIF_FAULT),
		 sgx_read(sgx, SGX_MASTER_BIF_FAULT),
		 READ_ONCE(sgx->buf[B_HOST].cpu[HOST_UK_LOCKUPS]),
		 READ_ONCE(sgx->buf[B_HOST].cpu[HOST_ASSERT_FAIL]));
	return ret;
}

/* ---- replay tools: memory at chosen GPU addresses ------------------------ */

static void sgx_extra_free(void *data)
{
	struct sgx_buf *b = data;
	unsigned int i, n = b->size >> PAGE_SHIFT;

	if (b->phys) {
		if (b->cpu)
			iounmap(b->cpu);
		return;
	}
	if (b->cpu)
		vunmap(b->cpu);
	for (i = 0; i < n && b->pages[i]; i++) {
		if (b->page_dma[i])
			dma_unmap_page(b->dev, b->page_dma[i], PAGE_SIZE, DMA_BIDIRECTIONAL);
		__free_page(b->pages[i]);
	}
	kvfree(b->pages);
	kvfree(b->page_dma);
}

/* "map VA SIZE": fresh zeroed memory at GPU address VA, cache-consistent
 * like the shared buffers but without EDMPROTECT: on an EDM-protected page
 * only the microkernel may write, and the pixel back end's write of a
 * render target faults (BIF_INT_STAT 0x000b0020).  Mappings stay until
 * reboot; the next microkernel boot invalidates the MMU's caches, so map
 * first and boot after.
 *
 * The memory comes page by page: the GPU's MMU maps every page on its own,
 * so nothing here needs to be contiguous (the CMA pool is only 64 MiB, and
 * textures and the parameter buffer want more).  The CPU sees the pages
 * through one write-combining mapping, as it does dma_alloc_coherent
 * memory; the pages are cleaned out of the cache when they are mapped for
 * the device and never touched cached again. */
static int sgx_map_extra(struct apple_sgx *sgx, u32 va, u32 size)
{
	struct sgx_buf *b;
	unsigned int i, n;
	int i2, ret;

	if (!sgx->bufs_ready)
		return -ENODEV;
	size = ALIGN(size, SGX_PAGE_SIZE);
	if ((va & (SGX_PAGE_SIZE - 1)) || !size || va + size < va ||
	    (va < sgx->va_next && va + size > SGX_VA_BASE))
		return -EINVAL;
	for (i2 = 0; i2 < sgx->nextra; i2++)
		if (va < sgx->extra[i2].va + sgx->extra[i2].size &&
		    va + size > sgx->extra[i2].va)
			return -EEXIST;
	if (sgx->nextra == ARRAY_SIZE(sgx->extra))
		return -ENOSPC;
	/* the render node's buffers live in the same address space */
	ret = apple_sgx_drm_reserve(sgx, sgx->nextra, va, size);
	if (ret)
		return ret;
	b = &sgx->extra[sgx->nextra];
	memset(b, 0, sizeof(*b));
	b->size = size;
	b->dev = sgx->dev;
	n = size >> PAGE_SHIFT;
	b->pages = kvcalloc(n, sizeof(*b->pages), GFP_KERNEL);
	b->page_dma = kvcalloc(n, sizeof(*b->page_dma), GFP_KERNEL);
	if (!b->pages || !b->page_dma) {
		kvfree(b->pages);
		kvfree(b->page_dma);
		apple_sgx_drm_unreserve(sgx, sgx->nextra);
		return -ENOMEM;
	}
	ret = devm_add_action_or_reset(sgx->dev, sgx_extra_free, b);
	if (ret) {
		apple_sgx_drm_unreserve(sgx, sgx->nextra);
		return ret;
	}
	for (i = 0; i < n; i++) {
		dma_addr_t dma;

		ret = -ENOMEM;
		b->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!b->pages[i])
			goto fail;
		dma = dma_map_page(sgx->dev, b->pages[i], 0, PAGE_SIZE, DMA_BIDIRECTIONAL);
		if (dma_mapping_error(sgx->dev, dma))
			goto fail;
		b->page_dma[i] = dma;
		ret = apple_sgx_mmu_map(sgx, va + i * PAGE_SIZE, dma, PAGE_SIZE,
				  SGX_PTE_CACHECONSISTENT);
		if (ret)
			goto fail;
	}
	ret = -ENOMEM;
	b->cpu = vmap(b->pages, n, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
	if (!b->cpu)
		goto fail;
	b->va = va;
	b->dma = b->page_dma[0];
	sgx->nextra++;
	dev_info(sgx->dev, "mapped GPU 0x%08x-0x%08x (%u pages)\n", va, va + size, n);
	return 0;

fail:	/* the PTEs written so far point at pages freed here */
	apple_sgx_mmu_unmap(sgx, va, size);
	devm_release_action(sgx->dev, sgx_extra_free, b);
	apple_sgx_drm_unreserve(sgx, sgx->nextra);
	return ret;
}

/*
 * "map_phys VA PA SIZE": map an existing physical range (the display's
 * current scanout buffer) into the GPU address space.  The range belongs to
 * another driver and is never freed here.  Map it before the next
 * microkernel boot, which invalidates the MMU caches; a transfer or render
 * can then draw into it.
 */
static int sgx_map_phys(struct apple_sgx *sgx, u32 va, u32 pa, u32 size)
{
	struct sgx_buf *b;
	int i, ret;

	dev_info(sgx->dev, "map_phys va=0x%08x pa=0x%08x size=0x%x\n",
		 va, pa, size);
	if (!sgx->bufs_ready)
		return -ENODEV;
	size = ALIGN(size, SGX_PAGE_SIZE);
	if ((va & (SGX_PAGE_SIZE - 1)) || (pa & (SGX_PAGE_SIZE - 1)) ||
	    !size || va + size < va || pa + size < pa ||
	    (va < sgx->va_next && va + size > SGX_VA_BASE)) {
		dev_err(sgx->dev, "map_phys: bad range\n");
		return -EINVAL;
	}
	for (i = 0; i < sgx->nextra; i++)
		if (va < sgx->extra[i].va + sgx->extra[i].size &&
		    va + size > sgx->extra[i].va) {
			dev_err(sgx->dev, "map_phys: overlaps extra %d\n", i);
			return -EEXIST;
		}
	if (sgx->nextra == ARRAY_SIZE(sgx->extra))
		return -ENOSPC;
	ret = apple_sgx_drm_reserve(sgx, sgx->nextra, va, size);
	if (ret) {
		dev_err(sgx->dev, "map_phys: reserve failed: %d\n", ret);
		return ret;
	}
	b = &sgx->extra[sgx->nextra];
	memset(b, 0, sizeof(*b));
	b->va = va;
	b->size = size;
	b->dma = pa;
	b->dev = sgx->dev;
	b->phys = true;
	/* The CPU mapping is only for debugfs reads; the GPU mapping below is
	 * what matters.  ioremap_wc can refuse a large RAM range, so a missing
	 * CPU view is not an error. */
	b->cpu = (u32 *)ioremap_wc(pa, size);
	if (!b->cpu)
		dev_info(sgx->dev, "map_phys: no CPU view, GPU only\n");
	ret = devm_add_action_or_reset(sgx->dev, sgx_extra_free, b);
	if (ret) {
		dev_err(sgx->dev, "map_phys: action failed: %d\n", ret);
		apple_sgx_drm_unreserve(sgx, sgx->nextra);
		return ret;
	}
	ret = apple_sgx_mmu_map(sgx, va, pa, size, 0);
	if (ret) {
		dev_err(sgx->dev, "map_phys: mmu_map failed: %d\n", ret);
		devm_release_action(sgx->dev, sgx_extra_free, b);
		apple_sgx_drm_unreserve(sgx, sgx->nextra);
		return ret;
	}
	sgx->nextra++;
	dev_info(sgx->dev, "mapped physical 0x%08x-0x%08x at GPU 0x%08x\n",
		 pa, pa + size, va);
	return 0;
}

/* The CPU side of GPU address va, and how many bytes follow it there. */
static u8 *sgx_va_cpu(struct apple_sgx *sgx, u32 va, size_t *avail)
{
	int i;

	for (i = 0; sgx->bufs_ready && i < B_NUM + sgx->nextra; i++) {
		struct sgx_buf *b = i < B_NUM ? &sgx->buf[i] : &sgx->extra[i - B_NUM];

		if (va >= b->va && va - b->va < b->size) {
			*avail = b->size - (va - b->va);
			return b->cpu ? (u8 *)b->cpu + (va - b->va) : NULL;
		}
	}
	return NULL;
}

/* apple-sgx/mem: the file offset is the GPU address. */
static ssize_t sgx_mem_rw(struct file *file, char __user *ubuf, const char __user *wbuf,
			  size_t len, loff_t *ppos)
{
	struct apple_sgx *sgx = file->private_data;
	size_t done = 0, avail, n;
	u8 *p;

	if (*ppos < 0 || *ppos > U32_MAX)
		return -EINVAL;
	mutex_lock(&sgx->lock);
	while (done < len) {
		p = sgx_va_cpu(sgx, *ppos + done, &avail);
		if (!p)
			break;
		n = min(len - done, avail);
		if (ubuf ? copy_to_user(ubuf + done, p, n) :
			   copy_from_user(p, wbuf + done, n)) {
			mutex_unlock(&sgx->lock);
			return -EFAULT;
		}
		done += n;
	}
	wmb();
	mutex_unlock(&sgx->lock);
	if (!done && len)
		return -EFAULT;		/* nothing mapped there */
	*ppos += done;
	return done;
}

static ssize_t sgx_mem_read(struct file *file, char __user *ubuf, size_t len, loff_t *ppos)
{
	return sgx_mem_rw(file, ubuf, NULL, len, ppos);
}

static ssize_t sgx_mem_write(struct file *file, const char __user *ubuf, size_t len,
			     loff_t *ppos)
{
	return sgx_mem_rw(file, NULL, ubuf, len, ppos);
}

static const struct file_operations sgx_mem_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = sgx_mem_read,
	.write = sgx_mem_write,
	.llseek = default_llseek,
};

/* "tqkick": the 0x140-byte command at the transfer CCB's write offset has
 * been put there through apple-sgx/mem; queue it and send TRANSFER.  Waits
 * for scratch word 0 to change (point the completion write there). */
static int sgx_tq_kick(struct apple_sgx *sgx)
{
	u32 *ctl = sgx->buf[B_TQ_CTL].cpu, *ctx = sgx->buf[B_TQ_CTX].cpu;
	u32 *scratch = sgx->buf[B_SCRATCH].cpu, wo, got;
	unsigned int n;
	int ret;

	if (sgx->boot_result != 1)
		return -ENODEV;
	wo = READ_ONCE(ctl[0]);
	if (wo + SGX_TQ_CMD_SIZE > SGX_TQ_CCB_SIZE)
		return -ENOSPC;
	WRITE_ONCE(scratch[0], 0);
	wmb();
	WRITE_ONCE(ctl[0], wo + SGX_TQ_CMD_SIZE);
	WRITE_ONCE(ctx[0], 1);
	wmb();
	ret = sgx_send_cmd(sgx, SGX_CMD_TRANSFER,
			   atomic_xchg(&sgx->mmu_dirty, 0) ? mmu_inval_cc : 0,
			   0, sgx->buf[B_TQ_CTX].va);
	if (ret)
		return ret;
	ret = read_poll_timeout(READ_ONCE, got, got, 10, 500000, false, scratch[0]);
	dev_info(sgx->dev, "tqkick at 0x%x: scratch 0x%08x (%s), tq read 0x%x ctx 0x%x; "
		 "master BIF_INT_STAT 0x%08x BIF_FAULT 0x%08x\n",
		 wo, READ_ONCE(scratch[0]), ret ? "not done" : "done",
		 READ_ONCE(ctl[1]), READ_ONCE(ctx[0]),
		 sgx_read(sgx, SGX_MASTER_BIF_INT_STAT), sgx_read(sgx, SGX_MASTER_BIF_FAULT));
	for (n = 0; n < sgx->ncores; n++)
		dev_info(sgx->dev, "  core%u EVENT_STATUS 0x%08x BIF_INT_STAT 0x%08x BIF_FAULT 0x%08x\n",
			 n, sgx_read(sgx, SGX_CORE(n) + SGX_EVENT_STATUS),
			 sgx_read(sgx, SGX_CORE(n) + SGX_BIF_INT_STAT),
			 sgx_read(sgx, SGX_CORE(n) + SGX_BIF_FAULT));
	return ret;
}

/* ---- the 2D engine ---------------------------------------------------------
 *
 * A blit is the transfer queue's copy: USSE programs sample the source,
 * scale it and write the pixel back, a PDS block names them and holds their
 * data segments, and a parameter page holds the objects and the region
 * header.  This is the reference's tqgen.py, ported: the pieces are built
 * here from the two buffers' addresses and sizes, so a blit can only name
 * memory the caller already owns.
 *
 * The USSE programs are named by a 20-bit code index from zero (a DOUTU's
 * program field), so the code lives at 0x1000; the PDS block and the
 * parameter page sit at fixed addresses that the render node reserves
 * (SGX_BLIT_BLOCK_VA, SGX_BLIT_PARAM_VA) so no buffer object can take them.
 */

#define SGX_PHAS			0xfa44070000000000ULL

static u64 sgx_blit_limm(u32 reg, u32 imm, bool pa, bool end)
{
	u64 w = 0xfca0000000000000ULL;

	if (pa)
		w |= 0x0000000200000000ULL;
	w |= (u64)(reg & 0x7f) << 21;
	w |= (u64)((imm >> 26) & 0x3f) << 44;
	w |= (u64)((imm >> 21) & 0x1f) << 36;
	w |= imm & 0x1fffff;
	if (end)
		w |= 1ULL << 50;
	return w;
}

/* a USSE program's address in a DOUTU word: its index, and code base 10 */
static u32 sgx_blit_doutu(u32 va)
{
	return ((va >> 3) << 4) | 0xa;
}

/* a PDS data pointer: bit 31 implied, the address in 16-byte units */
static u32 sgx_blit_pds_ptr(u32 va, u32 flag)
{
	return (flag << 28) | ((va - SGX_VA_BASE) >> 4);
}

/* a vertex coordinate: 16-bit fixed point, 4 fraction bits, +1024 px guard */
static u32 sgx_blit_pos(u32 px)
{
	return 0x4000 + px * 16;
}

static u32 sgx_blit_xy_px(u32 x, u32 y)
{
	return sgx_blit_pos(x) << 16 | sgx_blit_pos(y);
}

/* a 64-bit instruction word at code address VA */
static void sgx_blit_word(u32 *code, u32 va, unsigned int i, u64 w)
{
	u32 at = (va - SGX_BLIT_CODE_VA) / 4 + 2 * i;

	code[at] = lower_32_bits(w);
	code[at + 1] = upper_32_bits(w);
}

/* The pixel program (sample the texel into o0), the two end-of-tile programs
 * and the pixel-back-end program, whose six state words come from the
 * destination: its address, stride and size. */
static void sgx_blit_code(u32 *code, u32 dst_va, u32 dw, u32 dh, u32 dst_stride_px)
{
	u64 pbe[6];
	unsigned int i;

	memset(code, 0, SGX_BLIT_CODE_SIZE);
	pbe[0] = 0x00110000;
	pbe[1] = dst_va;
	pbe[2] = dst_stride_px / 2 - 1;
	pbe[3] = 0;
	pbe[4] = 0;
	pbe[5] = ((dh - 1) << 12) | (dw - 1);

	sgx_blit_word(code, 0x1400, 0, SGX_PHAS);
	sgx_blit_word(code, 0x1400, 1, 0x50850009a0000000ULL);
	sgx_blit_word(code, 0x1c00, 0, SGX_PHAS);
	sgx_blit_word(code, 0x1c00, 1, 0xf834800000000000ULL);
	sgx_blit_word(code, 0x1c40, 0, SGX_PHAS);
	sgx_blit_word(code, 0x1c40, 1, sgx_blit_limm(0, 0, false, false));
	sgx_blit_word(code, 0x1c40, 2, sgx_blit_limm(1, 0, false, false));
	sgx_blit_word(code, 0x1c40, 3, 0xfb26000081200000ULL);
	sgx_blit_word(code, 0x1c80, 0, SGX_PHAS);
	sgx_blit_word(code, 0x1c80, 1, sgx_blit_limm(1, 0, true, false));
	sgx_blit_word(code, 0x1c80, 2, sgx_blit_limm(2, 1, true, false));
	sgx_blit_word(code, 0x1c80, 3, sgx_blit_limm(4, 2, true, false));
	sgx_blit_word(code, 0x1c80, 4, sgx_blit_limm(8, 3, true, true));
	sgx_blit_word(code, 0x1d40, 0, SGX_PHAS);
	sgx_blit_word(code, 0x1d40, 1, 0x488b0281a00c0000ULL);
	sgx_blit_word(code, 0x1d40, 2, 0xe9a30084a0000000ULL);
	sgx_blit_word(code, 0x1d40, 3, 0xf920000000000000ULL);
	for (i = 0; i < 6; i++)
		sgx_blit_word(code, 0x1d40, 4 + i, sgx_blit_limm(i, pbe[i], false, false));
	sgx_blit_word(code, 0x1d40, 10, 0xfb24000003200082ULL);
}

/* The PDS block: the end-of-tile program and its constants, and one object
 * per buffer (the destination's and the source's texture state).  The
 * sources of these tables are tqgen.py's PDS_MAIN, PDS_MAIN_CONST and
 * PDS_TEX. */
static const u32 sgx_blit_pds_main[] = {
	0x9380000c, 0xcf800630, 0xcf800870, 0xf7700070, 0xcf086070, 0xc7606030,
	0xcf800a70, 0xf7700070, 0xcf0a6070, 0xc7606030, 0x07600b05, 0xaf000000,
	0x94800014, 0xcf0c8270, 0x8700c100, 0xcf800c70, 0xc70e6070, 0x100010e0,
	0x07018185, 0xaf000000, 0x07041185, 0xaf000000,
};

static const u32 sgx_blit_pds_const[] = {
	0xffff0000, 0x000000ff, 0x0000ff00, 0x000000ff, 0x0000ff00,
	0x00000100, 0x00020000, 0xfffeffff, 0x00000000, 0x00030000,
};

static const u32 sgx_blit_pds_tex[] = {
	0x07000185, 0x07000c02, 0x07041004, 0xaf000000,
};

static void sgx_blit_object(u32 *w, u32 base, u32 va, u32 width, u32 height,
			    u32 stride_px)
{
	u32 d = base + 0x20;

	w[base / 4] = 0xaf000000;		/* the object's program: end */
	w[d / 4 + 0] = sgx_blit_doutu(0x1400);	/* start the pixel program */
	w[d / 4 + 1] = 0xa;
	w[d / 4 + 2] = 0;
	w[d / 4 + 3] = 0xf800;
	/* a linear BGRA texture: the stride in the format word, the size in
	 * the size word, and 0x10000000 marks it linear */
	w[d / 4 + 4] = ((stride_px / 4 - 2) << 16) | 0x0e90;
	w[d / 4 + 5] = 0xcc000000 | ((width - 1) << 12) | (height - 1);
	w[d / 4 + 6] = va;
	w[d / 4 + 7] = 0x10000000;
	memcpy(&w[(d + 0x20) / 4], sgx_blit_pds_tex, sizeof(sgx_blit_pds_tex));
}

static void sgx_blit_block(u32 *w, u32 src_va, u32 sw, u32 sh, u32 src_stride_px,
			   u32 dst_va, u32 dw, u32 dh, u32 dst_stride_px)
{
	memset(w, 0, SGX_BLIT_BLOCK_SIZE);
	w[0x00 / 4] = sgx_blit_doutu(0x1c40);
	w[0x08 / 4] = sgx_blit_doutu(0x1d40);
	w[0x10 / 4] = sgx_blit_doutu(0x1c00);
	memcpy(&w[0x1c / 4], sgx_blit_pds_const, sizeof(sgx_blit_pds_const));
	memcpy(&w[0x50 / 4], sgx_blit_pds_main, sizeof(sgx_blit_pds_main));
	sgx_blit_object(w, 0x100, dst_va, dw, dh, dst_stride_px);
	sgx_blit_object(w, 0x160, src_va, sw, sh, src_stride_px);
}

/* The parameter page: the background object (the triangle that covers the
 * whole target), the quad object (the destination rectangle) and the region
 * header, whose offset is what the command names. */
static void sgx_blit_param(u32 *w, u32 block_va, u32 dw, u32 dh)
{
	u32 bg_off = SGX_BLIT_PARAM_VA & 0x00ffffff;

	memset(w, 0, SGX_BLIT_PARAM_SIZE);
	w[0] = sgx_blit_pds_ptr(block_va + 0x100, 0);
	w[1] = 0x0801e000;
	w[2] = sgx_blit_pds_ptr(block_va + 0x120, 1);
	w[5] = 0x3f800000;
	w[8] = 0x3f800000;
	w[9] = 2;
	w[11] = sgx_blit_xy_px(0, 0);
	w[12] = 0x3f800000;
	w[13] = sgx_blit_xy_px(0, 2 * dh);
	w[14] = 0x3f800000;
	w[15] = sgx_blit_xy_px(2 * dw, 0);
	w[16] = 0x3f800000;

	w[0x40 + 0] = sgx_blit_pds_ptr(block_va + 0x160, 0);
	w[0x40 + 1] = 0x0801e000;
	w[0x40 + 2] = sgx_blit_pds_ptr(block_va + 0x180, 1);
	w[0x40 + 6] = 0x3f800000;
	w[0x40 + 7] = 0x3f800000;
	w[0x40 + 8] = 0x3f800000;
	w[0x40 + 9] = 0x3f800000;
	w[0x40 + 11] = 0x01d80000;
	w[0x40 + 12] = 0x4e504a30;
	w[0x40 + 13] = 2;
	w[0x40 + 15] = sgx_blit_xy_px(0, 0);
	w[0x40 + 16] = 0x3f800000;
	w[0x40 + 17] = sgx_blit_xy_px(0, dh);
	w[0x40 + 18] = 0x3f800000;
	w[0x40 + 19] = sgx_blit_xy_px(dw, dh);
	w[0x40 + 20] = 0x3f800000;
	w[0x40 + 21] = sgx_blit_xy_px(dw, 0);
	w[0x40 + 22] = 0x3f800000;

	w[0x80] = 0x80000000;
	w[0x81] = 0x46400f03;
	w[0x82] = 0x0c000000 | (((bg_off + 0x100) >> 2) & 0xffffff) | 0xb;
	w[0x83] = 0xc0000000;
}

static void sgx_blit_cmd(u32 *c, u32 block_va, u32 dw, u32 dh, u32 completion_va,
			 u32 val)
{
	u32 bg_off = SGX_BLIT_PARAM_VA & 0x00ffffff;

	memset(c, 0, SGX_TQ_CMD_SIZE);
	c[0x04 / 4] = 0x300;		/* what the stock driver writes in every */
	c[0x08 / 4] = 0x3f800000;	/* transfer */
	c[0x0c / 4] = 2;
	c[0x14 / 4] = 0x100;
	c[0x18 / 4] = SGX_BLIT_PARAM_VA & 0xff000000;
	c[0x20 / 4] = 0x88;
	c[0x38 / 4] = 0x80;
	c[0x00 / 4] = 0x02000000 | (bg_off >> 4);
	c[0x10 / 4] = bg_off + 0x200;	/* the region header */
	c[0x24 / 4] = block_va;
	c[0x28 / 4] = 5;
	c[0x2c / 4] = 0x4000;
	c[0x34 / 4] = ((dw - 1) / 32) << 16 | ((dh - 1) / 32);
	c[0xa0 / 4] = SGX_TQ_CMD_SIZE;
	c[0xac / 4] = completion_va;
	c[0xb0 / 4] = val;
	c[0x108 / 4] = 0x01800000;
}

static bool sgx_blit_range_taken(struct apple_sgx *sgx, u32 va, u32 size)
{
	int i;

	for (i = 0; i < sgx->nextra; i++)
		if (va < sgx->extra[i].va + sgx->extra[i].size &&
		    va + size > sgx->extra[i].va)
			return true;
	return false;
}

static void sgx_blit_free(struct apple_sgx *sgx)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sgx->blit); i++) {
		struct sgx_buf *b = &sgx->blit[i];

		if (!b->cpu)
			continue;
		apple_sgx_mmu_unmap(sgx, b->va, b->size);
		dmam_free_coherent(sgx->dev, b->size, b->cpu, b->dma);
		memset(b, 0, sizeof(*b));
	}
	sgx->blit_ready = false;
}

/* The blit's three pieces, made at the first blit and kept. */
static int sgx_blit_alloc(struct apple_sgx *sgx)
{
	static const struct {
		u32 va, size;
	} d[] = {
		{ SGX_BLIT_CODE_VA, SGX_BLIT_CODE_SIZE },
		{ SGX_BLIT_BLOCK_VA, SGX_BLIT_BLOCK_SIZE },
		{ SGX_BLIT_PARAM_VA, SGX_BLIT_PARAM_SIZE },
	};
	unsigned int i;
	int ret;

	if (sgx->blit_ready)
		return 0;
	for (i = 0; i < ARRAY_SIZE(d); i++) {
		struct sgx_buf *b = &sgx->blit[i];

		if (sgx_blit_range_taken(sgx, d[i].va, d[i].size))
			return -EEXIST;
		b->size = d[i].size;
		b->va = d[i].va;
		b->cpu = dmam_alloc_coherent(sgx->dev, b->size, &b->dma,
					     GFP_KERNEL);
		if (!b->cpu) {
			sgx_blit_free(sgx);
			return -ENOMEM;
		}
		ret = apple_sgx_mmu_map(sgx, b->va, b->dma, b->size,
					SGX_PTE_CACHECONSISTENT);
		if (ret) {
			sgx_blit_free(sgx);
			return ret;
		}
	}
	sgx->blit_ready = true;
	return 0;
}

/*
 * One 2D copy: the source rectangle (BGRA8888, linear) scaled into the
 * destination's whole dst_w x dst_h area.  Both addresses are the GPU
 * addresses of buffers the caller owns; the caller has already checked them.
 * One transfer queue, so one blit at a time.
 */
int apple_sgx_blit(struct apple_sgx *sgx, u32 src_va, u32 src_w, u32 src_h, u32 src_stride,
		   u32 dst_va, u32 dst_w, u32 dst_h, u32 dst_stride)
{
	u32 *ctl = sgx->buf[B_TQ_CTL].cpu, *ctx = sgx->buf[B_TQ_CTX].cpu;
	u32 *scratch = sgx->buf[B_SCRATCH].cpu, *cmd, wo, got, seq;
	u32 c[SGX_TQ_CMD_SIZE / 4];
	int ret;

	if (!src_w || src_w > 4096 || !src_h || src_h > 4096 ||
	    !dst_w || dst_w > 4096 || !dst_h || dst_h > 4096)
		return -EINVAL;
	if ((src_stride & 3) || (dst_stride & 3) ||
	    src_stride < src_w * 4 || dst_stride < dst_w * 4 ||
	    src_stride < 16 || dst_stride < 16)
		return -EINVAL;
	if ((src_va & 3) || (dst_va & 3))
		return -EINVAL;

	mutex_lock(&sgx->blit_lock);
	if (sgx->boot_result != 1) {
		ret = -ENODEV;
		goto out;
	}
	ret = sgx_blit_alloc(sgx);
	if (ret)
		goto out;

	sgx_blit_code(sgx->blit[0].cpu, dst_va, dst_w, dst_h, dst_stride / 4);
	sgx_blit_block(sgx->blit[1].cpu, src_va, src_w, src_h, src_stride / 4,
		       dst_va, dst_w, dst_h, dst_stride / 4);
	sgx_blit_param(sgx->blit[2].cpu, SGX_BLIT_BLOCK_VA, dst_w, dst_h);

	wo = READ_ONCE(ctl[0]);
	if (wo + SGX_TQ_CMD_SIZE > SGX_TQ_CCB_SIZE) {
		if (READ_ONCE(ctl[1]) != wo) {
			ret = -EBUSY;	/* the queue has not drained */
			goto out;
		}
		WRITE_ONCE(ctl[0], 0);	/* it has: start over at the bottom */
		wo = 0;
	}

	seq = ++sgx->blit_seq;
	if (!seq)
		seq = ++sgx->blit_seq;
	sgx_blit_cmd(c, SGX_BLIT_BLOCK_VA, dst_w, dst_h, sgx->buf[B_SCRATCH].va,
		     seq);
	cmd = sgx->buf[B_TQ_CCB].cpu + wo / 4;
	memcpy(cmd, c, SGX_TQ_CMD_SIZE);

	WRITE_ONCE(scratch[0], 0);
	wmb();
	WRITE_ONCE(ctl[0], wo + SGX_TQ_CMD_SIZE);
	WRITE_ONCE(ctx[0], 1);
	wmb();
	sgx->quiet = true;
	ret = sgx_send_cmd(sgx, SGX_CMD_TRANSFER,
			   atomic_xchg(&sgx->mmu_dirty, 0) ? mmu_inval_cc : 0,
			   0, sgx->buf[B_TQ_CTX].va);
	sgx->quiet = false;
	if (ret) {
		dev_err(sgx->dev, "blit: TRANSFER not sent: %d\n", ret);
		goto out;
	}
	ret = read_poll_timeout(READ_ONCE, got, got == seq, 10, 500000, false,
				scratch[0]);
	if (ret) {
		dev_err(sgx->dev,
			"blit %ux%u -> %ux%u: no completion (scratch 0x%08x, queue 0x%x/0x%x), BIF_FAULT 0x%08x\n",
			src_w, src_h, dst_w, dst_h, READ_ONCE(scratch[0]),
			READ_ONCE(ctl[1]), READ_ONCE(ctl[0]),
			sgx_read(sgx, SGX_CORE(0) + SGX_BIF_FAULT));
		ret = -ETIMEDOUT;
	}
out:
	mutex_unlock(&sgx->blit_lock);
	return ret;
}

/* One render into the render queue: the TA command (LEN bytes, word 0 its
 * size) copied into the render CCB, the parameter buffer descriptor PB
 * into the render context (word 6), the context into the render target's
 * "render details" (DETAILS, +0x20 and +0xa4), the
 * command's completion (+0x64 flag, +0x68 address, +0x6c value) pointed
 * at DONE_VA/SEQ -- written when the TA is done -- and TA sent through the
 * kernel CCB with Data1 = the context.  sgx->lock held.
 *
 * The CCB wraps the way the stock driver does: offsets are 16-bit, and a command after
 * which a command as large as any (SGX_R_CMD_MAX) would not fit is
 * stretched to the end, so the next one starts at 0. */
int apple_sgx_render_queue(struct apple_sgx *sgx, const u32 *src, u32 len, u32 pb,
			   u32 *details, u32 done_va, u32 seq, u32 cache_control, u32 *at)
{
	u32 *ctl = sgx->buf[B_R_CTL].cpu, *ctx = sgx->buf[B_R_CTX].cpu;
	u32 cva = sgx->buf[B_R_CTX].va, *cmd, wo, size;
	int ret;

	if (sgx->boot_result != 1)
		return -ENODEV;
	if (len < SGX_R_CMD_MIN || len > SGX_R_CMD_MAX || (len & 7))
		return -EINVAL;
	wo = READ_ONCE(ctl[0]);
	if (wo + len > SGX_TQ_CCB_SIZE)
		return -EIO;		/* the last command did not wrap */
	size = len;
	if (SGX_TQ_CCB_SIZE - wo - len < SGX_R_CMD_MAX)
		size = SGX_TQ_CCB_SIZE - wo;
	if (((READ_ONCE(ctl[1]) + 0xffff - wo) & 0xffff) <= size)
		return -EBUSY;

	if (at)
		*at = wo;
	cmd = sgx->buf[B_R_CCB].cpu + wo / 4;
	memcpy(cmd, src, len);
	memset(cmd + len / 4, 0, size - len);
	cmd[0] = size;
	cmd[0x64 / 4] |= 1;
	cmd[0x68 / 4] = done_va;
	cmd[0x6c / 4] = seq;
	details[0x20 / 4] = cva;
	details[0xa4 / 4] = cva;
	ctx[6] = pb;
	wmb();
	WRITE_ONCE(ctl[0], (wo + size) & (SGX_TQ_CCB_SIZE - 1));
	WRITE_ONCE(ctx[0], 1);
	wmb();
	if (atomic_xchg(&sgx->mmu_dirty, 0))
		cache_control |= mmu_inval_cc;
	sgx->quiet = true;
	ret = sgx_send_cmd(sgx, SGX_CMD_TA, cache_control, 0, cva);
	sgx->quiet = false;
	return ret;
}

/* Whether the render queued at AT is still where it was put: the
 * microkernel has not read it (its read offset is AT) and has not started
 * it (the render details' +0x24, set when the TA starts, is 0). */
bool apple_sgx_render_waiting(struct apple_sgx *sgx, u32 at, const u32 *details)
{
	return READ_ONCE(sgx->buf[B_R_CTL].cpu[1]) == at && !READ_ONCE(details[0x24 / 4]);
}

/* TA again for the render queue.  Now and then the microkernel takes the
 * TA command from the kernel CCB and leaves the render it names in the
 * queue -- seen on the first render after a start, about one in three --
 * and nothing makes it look again; the stock driver too kicks pending work again
 * (after a power-up).  The context is marked as having work,
 * as for the first kick. */
int apple_sgx_render_rekick(struct apple_sgx *sgx)
{
	u32 *ctx = sgx->buf[B_R_CTX].cpu;
	int ret;

	if (sgx->boot_result != 1)
		return -ENODEV;
	WRITE_ONCE(ctx[0], 1);
	wmb();
	sgx->quiet = true;
	ret = sgx_send_cmd(sgx, SGX_CMD_TA, 0, 0, sgx->buf[B_R_CTX].va);
	sgx->quiet = false;
	return ret;
}

/* What the GPU was doing, for a render that did not finish. */
void apple_sgx_report(struct apple_sgx *sgx, const char *why)
{
	unsigned int n;

	if (!sgx->bufs_ready)
		return;
	dev_err(sgx->dev, "%s: r CCB write 0x%x read 0x%x, ctx0 0x%x, lockups %u, "
		"master BIF_INT_STAT 0x%08x BIF_FAULT 0x%08x\n", why,
		READ_ONCE(sgx->buf[B_R_CTL].cpu[0]), READ_ONCE(sgx->buf[B_R_CTL].cpu[1]),
		READ_ONCE(sgx->buf[B_R_CTX].cpu[0]),
		READ_ONCE(sgx->buf[B_HOST].cpu[HOST_UK_LOCKUPS]),
		sgx_read(sgx, SGX_MASTER_BIF_INT_STAT), sgx_read(sgx, SGX_MASTER_BIF_FAULT));
	for (n = 0; n < sgx->ncores; n++)
		dev_err(sgx->dev, "  core%u EVENT_STATUS 0x%08x STATUS2 0x%08x BIF_INT_STAT 0x%08x "
			"BIF_FAULT 0x%08x\n", n,
			sgx_read(sgx, SGX_CORE(n) + SGX_EVENT_STATUS),
			sgx_read(sgx, SGX_CORE(n) + SGX_EVENT_STATUS2),
			sgx_read(sgx, SGX_CORE(n) + SGX_BIF_INT_STAT),
			sgx_read(sgx, SGX_CORE(n) + SGX_BIF_FAULT));
}

/* "rkick PB DET CMD [CC]": one render (TA, then 3D) from memory put in place
 * through apple-sgx/mem.  PB = the parameter buffer descriptor (context word
 * 6), DET = the render target's "render details" (its two entries get the
 * context's address at +0x20 and +0xa4), CMD = a TA
 * command (word 0 = its size) to copy into the render CCB.  Its completion
 * (+0x68/+0x6c, written when the TA is done) goes to scratch word 0 and is
 * waited for.  Quiet unless something fails. */
static int sgx_r_kick(struct apple_sgx *sgx, u32 pb, u32 det, u32 cmdva, u32 cc)
{
	u32 *scratch = sgx->buf[B_SCRATCH].cpu, *src, *d, got, seq;
	size_t avail;
	int ret;

	if (sgx->boot_result != 1)
		return -ENODEV;
	src = (u32 *)sgx_va_cpu(sgx, cmdva, &avail);
	if (!src || avail < SGX_R_CMD_MIN || src[0] > avail)
		return -EFAULT;
	d = (u32 *)sgx_va_cpu(sgx, det, &avail);
	if (!d || avail < 0xa8 || !sgx_va_cpu(sgx, pb, &avail))
		return -EFAULT;
	seq = ++sgx->r_seq ? sgx->r_seq : ++sgx->r_seq;
	WRITE_ONCE(scratch[0], 0);
	ret = apple_sgx_render_queue(sgx, src, src[0], pb, d, sgx->buf[B_SCRATCH].va,
				     seq, cc, NULL);
	if (ret)
		return ret;
	ret = read_poll_timeout(READ_ONCE, got, got == seq, 10, 500000, false, scratch[0]);
	if (ret)
		apple_sgx_report(sgx, "render not done");
	return ret;
}

/* ---- debugfs -------------------------------------------------------------- */

static const struct {
	u32 off;
	const char *name;
} sgx_master_regs[] = {
	{ SGX_MASTER_CORE,		"MASTER_CORE" },
	{ SGX_MASTER_CLKGATECTL,	"MASTER_CLKGATECTL" },
	{ SGX_MASTER_CLKGATESTATUS,	"MASTER_CLKGATESTATUS" },
	{ SGX_MASTER_CORE_ID,		"MASTER_CORE_ID" },
	{ SGX_MASTER_CORE_REVISION,	"MASTER_CORE_REVISION" },
	{ SGX_MASTER_CLKGATECTL2,	"MASTER_CLKGATECTL2" },
	{ SGX_MASTER_CLKGATESTATUS2,	"MASTER_CLKGATESTATUS2" },
	{ SGX_MASTER_SOFT_RESET,	"MASTER_SOFT_RESET" },
	{ SGX_MASTER_EVENT_STATUS,	"MASTER_EVENT_STATUS" },
	{ SGX_MASTER_EVENT_STATUS2,	"MASTER_EVENT_STATUS2" },
	{ SGX_MASTER_BIF_CTRL,		"MASTER_BIF_CTRL" },
	{ SGX_MASTER_BIF_INT_STAT,	"MASTER_BIF_INT_STAT" },
	{ SGX_MASTER_BIF_FAULT,		"MASTER_BIF_FAULT" },
	{ SGX_MASTER_BIF_MEM_REQ_STAT,	"MASTER_BIF_MEM_REQ_STAT" },
	{ SGX_MASTER_SLC_CTRL,		"MASTER_SLC_CTRL" },
	{ SGX_MASTER_SLC_CTRL_BYPASS,	"MASTER_SLC_CTRL_BYPASS" },
}, sgx_core_regs[] = {
	{ SGX_CLKGATECTL,		"CLKGATECTL" },
	{ SGX_CLKGATECTL2,		"CLKGATECTL2" },
	{ SGX_CLKGATESTATUS,		"CLKGATESTATUS" },
	{ SGX_CORE_ID,			"CORE_ID" },
	{ SGX_CORE_REVISION,		"CORE_REVISION" },
	{ SGX_SOFT_RESET,		"SOFT_RESET" },
	{ SGX_EVENT_STATUS,		"EVENT_STATUS" },
	{ SGX_EVENT_STATUS2,		"EVENT_STATUS2" },
	{ SGX_BIF_CTRL,			"BIF_CTRL" },
	{ SGX_BIF_INT_STAT,		"BIF_INT_STAT" },
	{ SGX_BIF_FAULT,		"BIF_FAULT" },
	{ SGX_BIF_MEM_REQ_STAT,		"BIF_MEM_REQ_STAT" },
};

static int sgx_regs_show(struct seq_file *s, void *unused)
{
	struct apple_sgx *sgx = s->private;
	unsigned int i, n;

	mutex_lock(&sgx->lock);
	for (i = 0; i < ARRAY_SIZE(sgx_master_regs); i++)
		seq_printf(s, "+0x%05x  %-24s 0x%08x\n", sgx_master_regs[i].off,
			   sgx_master_regs[i].name,
			   sgx_read(sgx, sgx_master_regs[i].off));
	for (n = 0; n < sgx->ncores; n++)
		for (i = 0; i < ARRAY_SIZE(sgx_core_regs); i++) {
			u32 off = SGX_CORE(n) + sgx_core_regs[i].off;

			seq_printf(s, "+0x%05x  core%u %-18s 0x%08x\n", off, n,
				   sgx_core_regs[i].name, sgx_read(sgx, off));
		}
	if (sgx->bufs_ready) {
		static const struct { u8 w; const char *name; } hw[] = {
			{ HOST_INIT_STATUS, "init status" },
			{ HOST_POWER_STATUS, "power status" },
			{ HOST_CLEANUP_STATUS, "cleanup status" },
			{ HOST_UK_LOCKUPS, "ukernel lockups" },
			{ HOST_UK_TIMER_CLOCK, "ukernel timer clock" },
			{ HOST_INTERRUPT_FLAGS, "interrupt flags" },
			{ HOST_INTERRUPT_CLEAR, "interrupt clear" },
			{ HOST_TIME_WRAPS, "time wraps" },
			{ HOST_ASSERT_FAIL, "assert fail" },
		};
		u32 *host = sgx->buf[B_HOST].cpu, *ctl = sgx->buf[B_758].cpu;

		seq_printf(s, "page directory pa %pad\n", &sgx->pd_dma);
		for (i = 0; i < ARRAY_SIZE(hw); i++)
			seq_printf(s, "host +0x%02x %-20s 0x%08x\n", hw[i].w * 4,
				   hw[i].name, READ_ONCE(host[hw[i].w]));
		seq_printf(s, "CCB write %u read %u, kicker %u\n",
			   READ_ONCE(ctl[0]), READ_ONCE(ctl[1]),
			   READ_ONCE(sgx->buf[B_KICKER].cpu[0]));
		seq_printf(s, "tq CCB write 0x%x read 0x%x; scratch 0x%08x 0x%08x\n",
			   READ_ONCE(sgx->buf[B_TQ_CTL].cpu[0]),
			   READ_ONCE(sgx->buf[B_TQ_CTL].cpu[1]),
			   READ_ONCE(sgx->buf[B_SCRATCH].cpu[0]),
			   READ_ONCE(sgx->buf[B_SCRATCH].cpu[1]));
		seq_printf(s, "r CCB write 0x%x read 0x%x; r ctx 0x%08x word0 0x%x; renders %u\n",
			   READ_ONCE(sgx->buf[B_R_CTL].cpu[0]),
			   READ_ONCE(sgx->buf[B_R_CTL].cpu[1]),
			   sgx->buf[B_R_CTX].va, READ_ONCE(sgx->buf[B_R_CTX].cpu[0]),
			   sgx->r_seq);
		for (i = 0; i < B_NUM; i++)
			seq_printf(s, "buffer %-10s va 0x%08x pa %pad size 0x%zx\n",
				   sgx_buf_descs[i].name, sgx->buf[i].va,
				   &sgx->buf[i].dma, sgx->buf[i].size);
		for (i = 0; i < sgx->nextra; i++)
			seq_printf(s, "mapped            va 0x%08x pa %pad size 0x%zx\n",
				   sgx->extra[i].va, &sgx->extra[i].dma, sgx->extra[i].size);
	}
	seq_printf(s, "interrupt %d: %s, %u handled, %u unhandled; events core0 0x%08x core1 0x%08x; host interrupt flags 0x%08x\n",
		   sgx->irq, sgx->irq_on ? "on" : "off", sgx->irq_count,
		   sgx->irq_unhandled, sgx->irq_events[0], sgx->irq_events[1],
		   sgx->irq_host_flags);
	seq_printf(s, "microkernel: %s (%d)\n",
		   sgx->boot_result == 1 ? "up" :
		   sgx->boot_result ? "failed" : "not started", sgx->boot_result);
	mutex_unlock(&sgx->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sgx_regs);

/* Start (or restart) the microkernel; sgx->lock held. */
static int sgx_boot(struct apple_sgx *sgx)
{
	int ret;

	sgx->boot_result = 0;
	ret = sgx_boot_ukernel(sgx);
	sgx->boot_result = ret ? ret : 1;
	sgx->boots++;
	return ret;
}

static void sgx_boot_work(struct work_struct *work)
{
	struct apple_sgx *sgx = container_of(work, struct apple_sgx, boot_work);
	int ret;

	mutex_lock(&sgx->lock);
	ret = sgx_boot(sgx);
	mutex_unlock(&sgx->lock);
	if (ret && ret != -ENOENT)
		dev_err(sgx->dev, "microkernel did not start: %d\n", ret);
}

bool apple_sgx_up(struct apple_sgx *sgx)
{
	return READ_ONCE(sgx->boot_result) == 1;
}

/* A new start of the microkernel, from a reset GPU: every queue empty, every
 * mapping kept.  For the render node, after a render that never ended. */
int apple_sgx_restart(struct apple_sgx *sgx)
{
	int ret;

	mutex_lock(&sgx->lock);
	ret = sgx_boot(sgx);
	mutex_unlock(&sgx->lock);
	return ret;
}

/* A boot from debugfs would pull the queues out from under the render
 * node's clients. */
static ssize_t sgx_boot_write(struct file *file, const char __user *ubuf,
			      size_t len, loff_t *ppos)
{
	struct apple_sgx *sgx = file->private_data;
	int ret;

	mutex_lock(&sgx->lock);
	ret = apple_sgx_drm_busy(sgx) ? -EBUSY : sgx_boot(sgx);
	mutex_unlock(&sgx->lock);
	return ret ? ret : len;
}

static const struct file_operations sgx_boot_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = sgx_boot_write,
	.llseek = noop_llseek,
};

/* Commands that need nothing but the microkernel itself (those that touch
 * the queues or the address map are refused while the render node is open):
 *   "hwperf N"   SETHWPERFSTATUS with status N (0 = counters off)
 *   "power N"    POWER: 1 power off, 2 idle, 3 resume (after idle)
 *   "tq F [V]"   a transfer with flags F and no work but two writes of V
 *   "map VA SZ"  memory at GPU address VA (see apple-sgx/mem)
 *   "map_phys VA PA SZ"  an existing physical range at GPU address VA
 *   "tqkick"     send the transfer command placed at the transfer CCB
 *   "rkick PB DET CMD [CC]"  one render (see sgx_r_kick) */
static ssize_t sgx_cmd_write(struct file *file, const char __user *ubuf,
			     size_t len, loff_t *ppos)
{
	struct apple_sgx *sgx = file->private_data;
	char buf[96], word[16];
	long long v[6] = { 0 };
	u32 arg, arg2, a[4];
	int n, ret;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;
	n = sscanf(buf, "%15s %lli %lli %lli %lli %lli %lli", word, &v[0],
		   &v[1], &v[2], &v[3], &v[4], &v[5]);
	if (n < 1)
		return -EINVAL;
	arg = v[0];
	arg2 = n > 1 ? v[1] : 0x1234;
	a[0] = v[2];
	a[1] = v[3];
	a[2] = v[4];
	a[3] = v[5];

	mutex_lock(&sgx->lock);
	if (apple_sgx_drm_busy(sgx) &&
	    (!strcmp(word, "power") || !strcmp(word, "tq") || !strcmp(word, "tqkick") ||
	     !strcmp(word, "rkick") || !strcmp(word, "map") ||
	     !strcmp(word, "map_phys")))
		ret = -EBUSY;	/* the render node has clients */
	else if (!strcmp(word, "hwperf"))
		ret = sgx_send_cmd(sgx, SGX_CMD_HWPERF, 0, arg, 0);
	else if (!strcmp(word, "power"))
		ret = sgx_power_cmd(sgx, arg);
	else if (!strcmp(word, "tq"))
		ret = sgx_tq_cmd(sgx, arg, arg2);
	else if (!strcmp(word, "map"))
		ret = sgx_map_extra(sgx, arg, arg2);
	else if (!strcmp(word, "map_phys"))
		ret = sgx_map_phys(sgx, arg, arg2, a[0]);
	else if (!strcmp(word, "tqkick"))
		ret = sgx_tq_kick(sgx);
	else if (!strcmp(word, "rkick"))
		ret = sgx_r_kick(sgx, arg, arg2, a[0], a[1]);
	else
		ret = -EINVAL;
	mutex_unlock(&sgx->lock);
	return ret ? ret : len;
}

static const struct file_operations sgx_cmd_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = sgx_cmd_write,
	.llseek = noop_llseek,
};

/* ---- the device ----------------------------------------------------------- */

static void sgx_power_off(void *data)
{
	struct apple_sgx *sgx = data;

	clk_bulk_disable_unprepare(sgx->num_clks, sgx->clks);
}

static int apple_sgx_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_sgx *sgx;
	struct resource *res;
	u32 id, rev, cid, crev;
	unsigned int n, cores;
	int ret;

	if (clock_mode != 1 && clock_mode != 2)
		return dev_err_probe(dev, -EINVAL, "clock_mode must be 1 or 2\n");

	sgx = devm_kzalloc(dev, sizeof(*sgx), GFP_KERNEL);
	if (!sgx)
		return -ENOMEM;
	sgx->dev = dev;
	mutex_init(&sgx->lock);
	mutex_init(&sgx->mmu_lock);
	mutex_init(&sgx->blit_lock);
	spin_lock_init(&sgx->ccb_lock);
	INIT_WORK(&sgx->boot_work, sgx_boot_work);
	sgx->brn_31195 = of_property_read_bool(dev->of_node, "apple,brn-31195");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	/* one page directory for everything, from the start: the render node
	 * maps buffers before the microkernel's first boot too */
	sgx->pd = dmam_alloc_coherent(dev, SGX_PAGE_SIZE, &sgx->pd_dma, GFP_KERNEL);
	if (!sgx->pd)
		return -ENOMEM;

	sgx->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(sgx->regs))
		return PTR_ERR(sgx->regs);
	sgx->size = resource_size(res);
	if (sgx->size < SGX_CORE(1))
		return dev_err_probe(dev, -EINVAL, "register window too small\n");

	/* GFX_SYS, then GFX: clk_bulk enables in DT order. */
	sgx->num_clks = devm_clk_bulk_get_all(dev, &sgx->clks);
	if (sgx->num_clks < 0)
		return dev_err_probe(dev, sgx->num_clks, "no power-state clocks\n");
	ret = clk_bulk_prepare_enable(sgx->num_clks, sgx->clks);
	if (ret)
		return dev_err_probe(dev, ret, "cannot power the GPU on\n");
	ret = devm_add_action_or_reset(dev, sgx_power_off, sgx);
	if (ret)
		return ret;

	/* The master answers as soon as it has power.  ncores is still 0, so
	 * only master reads get through. */
	id = sgx_read(sgx, SGX_MASTER_CORE_ID);
	rev = sgx_read(sgx, SGX_MASTER_CORE_REVISION);
	cores = SGX_CORE_ID_CORES(id);
	if (SGX_CORE_ID_ID(id) != SGX_ID_543)
		return dev_err_probe(dev, -ENODEV,
				     "CORE_ID 0x%08x is not an SGX543 -- powered?\n", id);
	if (!cores || cores > SGX_MAX_CORES || SGX_CORE(cores) > sgx->size)
		return dev_err_probe(dev, -ENODEV,
				     "CORE_ID 0x%08x says %u cores\n", id, cores);
	sgx->ncores = cores;
	sgx->core_id = id;
	sgx->core_rev = rev;

	sgx_init(sgx);

	/* Now the cores have clocks and can be read.  Each reports the same
	 * ID and revision as the master, or something is wrong. */
	for (n = 0; n < sgx->ncores; n++) {
		cid = sgx_read(sgx, SGX_CORE(n) + SGX_CORE_ID);
		crev = sgx_read(sgx, SGX_CORE(n) + SGX_CORE_REVISION);
		if (cid != id || crev != rev)
			return dev_err_probe(dev, -EIO,
				"core %u: CORE_ID 0x%08x rev 0x%08x, master 0x%08x 0x%08x\n",
				n, cid, crev, id, rev);
	}

	/* Requested now, switched on only by a microkernel boot with irq=1. */
	sgx->irq = platform_get_irq_optional(pdev, 0);
	if (sgx->irq > 0) {
		ret = devm_request_irq(dev, sgx->irq, sgx_irq_handler,
				       IRQF_NO_AUTOEN, dev_name(dev), sgx);
		if (ret)
			return dev_err_probe(dev, ret, "interrupt %d\n", sgx->irq);
	}

	dev_info(dev, "SGX543MP%u rev %u.%u.%u, %u core(s) up, clocks %s\n",
		 sgx->ncores, (rev >> 16) & 0xff, (rev >> 8) & 0xff, rev & 0xff,
		 sgx->ncores, clock_mode == 1 ? "on" : "auto");

	sgx->debugfs = debugfs_create_dir("apple-sgx", NULL);
	debugfs_create_file("regs", 0400, sgx->debugfs, sgx, &sgx_regs_fops);
	debugfs_create_file("boot", 0200, sgx->debugfs, sgx, &sgx_boot_fops);
	debugfs_create_file("cmd", 0200, sgx->debugfs, sgx, &sgx_cmd_fops);
	debugfs_create_file("mem", 0600, sgx->debugfs, sgx, &sgx_mem_fops);
	platform_set_drvdata(pdev, sgx);

	ret = apple_sgx_drm_init(sgx);
	if (ret) {
		debugfs_remove_recursive(sgx->debugfs);
		return dev_err_probe(dev, ret, "render node\n");
	}
	if (autoboot)
		schedule_work(&sgx->boot_work);
	return 0;
}

static void apple_sgx_remove(struct platform_device *pdev)
{
	struct apple_sgx *sgx = platform_get_drvdata(pdev);

	cancel_work_sync(&sgx->boot_work);
	apple_sgx_drm_fini(sgx);
	debugfs_remove_recursive(sgx->debugfs);
}

static const struct of_device_id apple_sgx_of_match[] = {
	{ .compatible = "apple,s5l8947x-sgx" },
	{ .compatible = "apple,s5l8940x-sgx" },
	{ }
};
MODULE_DEVICE_TABLE(of, apple_sgx_of_match);

static struct platform_driver apple_sgx_driver = {
	.probe = apple_sgx_probe,
	.remove = apple_sgx_remove,
	.driver = {
		.name = "apple-sgx",
		.of_match_table = apple_sgx_of_match,
		/* the render node's buffers outlive an unbind */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(apple_sgx_driver);

MODULE_DESCRIPTION("S5L8947X PowerVR SGX543: microkernel, queues, render node");
MODULE_FIRMWARE(SGX_FW_NAME);
MODULE_LICENSE("GPL");
