/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * S5L8947X PowerVR SGX543 -- shared state for the hardware side
 * (apple_sgx_hw.c: power, MMU, microkernel, queues, debugfs) and the render
 * node (apple_sgx_drm.c).
 *
 * This driver is derived from the clean-room SGX543 work published in
 * Project Cascadia (GPL-2.0-only) and adapted to the S5L8947X: two GFX power
 * states in the PMGR, a single 96 KiB register window, and the microkernel
 * carved from this device's own system image by sgxcarve.
 */
#ifndef __APPLE_SGX_H__
#define __APPLE_SGX_H__

#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct apple_sgx_drm;
struct clk_bulk_data;
struct dentry;
struct device;
struct drm_device;
struct drm_file;
struct drm_gem_object;
struct page;
struct resource;

/* The register window: six 16 KiB banks.  Bank 0 is the broadcast bank and
 * has no read path -- a read hangs the bus.  Bank 1 is the master, bank
 * 2 + n is core n. */
#define SGX_BANK_SIZE			0x4000
#define SGX_BCAST			0x0000
#define SGX_MASTER			(1 * SGX_BANK_SIZE)
#define SGX_CORE(n)			((2 + (n)) * SGX_BANK_SIZE)
#define SGX_MAX_CORES			4

/* ---- the GPU's MMU ------------------------------------------------------ */

#define SGX_PAGE_SIZE			0x1000
#define SGX_PD_ENTRIES			1024
#define SGX_PDE_VALID			BIT(0)
#define SGX_PTE_VALID			BIT(0)
#define SGX_PTE_READONLY		BIT(2)
#define SGX_PTE_CACHECONSISTENT		BIT(3)
#define SGX_PTE_EDMPROTECT		BIT(4)

/*
 * The GPU's address space, one for everything:
 *
 *   0x80000000-0x807fffff the microkernel's buffers and the kernel's queues.
 *                         The microkernel carries no address of its own
 *                         (every one is patched in or handed over in a
 *                         register), so the layout is ours.  Everything stays
 *                         within 8 MiB of the code, because some addresses
 *                         are given as offsets from it in 20-bit fields.
 *   0x80800000-0xefffffff render node buffers, and what debugfs "map" puts
 *                         there
 *     0x90000000-+16 MiB  the framebuffer window (reserved)
 *     0x9a000000-+8 MiB   the USSE code zone; USE_CODE_BASE_3 and _5 point
 *                         at its start.  8 MiB is all a DOUTU or a PHAS can
 *                         reach: they name a program by its index from the
 *                         base, 20 bits of 8-byte instructions.
 *     0x89000000-+8.5 MiB the kernel's parameter buffer, shared by clients
 *     0x8a000000-0x977fffff  the TA's heap: render target data, within
 *                         256 MiB of the TA's base
 *     0xa8000000-         where the kernel picks addresses, top down,
 *                         up to the tiled window
 *     0xe0000000-+256 MiB the BIF's tiled window 1 (stride 4096): only at
 *                         an address asked for
 *   0xf0000000-           the BIF's tiled window 2 (stride 8192), unused
 *
 * PDS data pointers carry bit 31 implied, so everything a PDS program reads
 * has to be at 0x80000000 or above.
 */
#define SGX_VA_BASE			0x80000000u
#define SGX_FB_VA			0x90000000u
#define SGX_FB_WINDOW			SZ_16M
#define SGX_USER_VA_START		0x80800000u
#define SGX_USER_VA_END			0xf0000000u
#define SGX_CODE_BASE			0x9a000000u
#define SGX_CODE_VA_END			(SGX_CODE_BASE + SZ_8M)
#define SGX_AUTO_VA_START		0xa8000000u
/* The TA's base (its requests' page numbers count from here), the kernel's
 * parameter buffer, and where TA-heap buffers go: within 256 MiB of the
 * base.  A parameter buffer at 0x8cc00000-0x8ef00000 has been observed to
 * hang renders; this window is the one measured to work. */
#define SGX_TA_BASE			0x87800000u
#define SGX_PB_VA			0x89000000u
#define SGX_TA_HEAP_START		0x8a000000u
#define SGX_TA_HEAP_END			0x97800000u
#define SGX_TILED_VA_START		0xe0000000u

/* The 2D engine's own code, PDS block and parameter page.  The USSE programs
 * are named by a 20-bit index from zero, so the code lives at 0x1000; the
 * other two are reserved in the render node's address space (apple_sgx_drm.c)
 * and kept out of every buffer object's way. */
#define SGX_BLIT_CODE_VA		0x00001000u
#define SGX_BLIT_CODE_SIZE		0x7000u
#define SGX_BLIT_BLOCK_VA		0x980f3000u
#define SGX_BLIT_BLOCK_SIZE		0x10000u
#define SGX_BLIT_PARAM_VA		0x8c006000u
#define SGX_BLIT_PARAM_SIZE		0x2000u

/* The GPU's core clock in kHz.  The S5L8947X GFX domain runs from the
 * 513 MHz PLL divided by two; the microkernel only uses this to scale its
 * timers, so an exact value is not critical. */
#define SGX_CLOCK_KHZ			256500

/* What a kernel CCB command asks the microkernel to invalidate before it
 * runs. */
#define SGX_CC_INVAL_BIF_PT		BIT(0)
#define SGX_CC_INVAL_BIF_PD		BIT(1)
#define SGX_CC_INVAL_BIF_SL		BIT(2)
#define SGX_CC_INVAL_DATA		BIT(3)

/* Where in the scratch buffer the render node's completions land (words
 * 0 and 1 are debugfs's). */
#define SGX_SCRATCH_RENDER		0x40

enum sgx_buf_id {
	B_CODE,
	B_PDS_6D8, B_PDS_6E0, B_PDS_6E8, B_PDS_6F0, B_PDS_6F8,
	B_CODE2,
	B_PDS_704,
	B_TAB_784, B_TAB_78C,
	B_740, B_HOST, B_750, B_758, B_KICKER, B_768,
	B_TAB_774, B_IDX_77C,
	B_794, B_798, B_79C, B_7A0,
	B_TQ_CTX, B_TQ_CCB, B_TQ_CTL,	/* one transfer queue */
	B_R_CTX, B_R_CCB, B_R_CTL,	/* one render queue (TA + 3D) */
	B_SCRATCH,			/* where test commands write */
	B_NUM
};

struct sgx_buf {
	u32 *cpu;
	dma_addr_t dma;
	u32 va;
	size_t size;
	struct page **pages;	/* set: allocated page by page (map_extra) */
	dma_addr_t *page_dma;
	struct device *dev;
	bool phys;		/* an existing physical range (map_phys), not ours */
};

#define SGX_MAX_EXTRA			32

struct apple_sgx {
	struct device *dev;
	void __iomem *regs;
	resource_size_t size;
	struct clk_bulk_data *clks;
	int num_clks;
	unsigned int ncores;
	u32 core_id, core_rev;
	bool brn_31195;
	struct dentry *debugfs;
	struct mutex lock;

	/* the MMU */
	struct mutex mmu_lock;		/* the page tables */
	u32 *pd;
	dma_addr_t pd_dma;
	u32 *pt[SGX_PD_ENTRIES];
	dma_addr_t pt_dma[SGX_PD_ENTRIES];
	u32 va_next;
	atomic_t mmu_dirty;		/* PTEs changed since the last kick */

	struct sgx_buf buf[B_NUM];
	bool bufs_ready;

	/* buffers mapped at a chosen GPU address (debugfs, replay tools) */
	struct sgx_buf extra[SGX_MAX_EXTRA];
	int nextra;
	int boot_result;	/* 0 never tried, 1 acknowledged, -errno */
	unsigned int boots;

	/* the interrupt */
	int irq;
	bool irq_on;
	unsigned int irq_count, irq_unhandled, irq_run;
	u32 irq_events[SGX_MAX_CORES];	/* every event bit seen, per core */
	u32 irq_host_flags;		/* host control's interrupt flags, last */

	/* command submission */
	bool quiet;			/* no log line per microkernel command */
	spinlock_t ccb_lock;		/* the kernel CCB */
	struct work_struct boot_work;
	u32 r_seq;

	/* the render node */
	struct apple_sgx_drm *drm;

	/* the 2D engine: its programs and state, made at the first blit */
	struct mutex blit_lock;
	struct sgx_buf blit[3];		/* code, PDS block, parameter page */
	bool blit_ready;
	u32 blit_seq;
};

/* apple_sgx_hw.c */
int apple_sgx_mmu_map(struct apple_sgx *sgx, u32 va, dma_addr_t pa, size_t size,
		      u32 flags);
void apple_sgx_mmu_unmap(struct apple_sgx *sgx, u32 va, size_t size);
bool apple_sgx_up(struct apple_sgx *sgx);
int apple_sgx_restart(struct apple_sgx *sgx);
int apple_sgx_render_queue(struct apple_sgx *sgx, const u32 *cmd, u32 len, u32 pb,
			   u32 *details, u32 done_va, u32 seq, u32 cache_control, u32 *at);
bool apple_sgx_render_waiting(struct apple_sgx *sgx, u32 at, const u32 *details);
int apple_sgx_render_rekick(struct apple_sgx *sgx);
int apple_sgx_blit(struct apple_sgx *sgx, u32 src_va, u32 src_w, u32 src_h, u32 src_stride,
		   u32 dst_va, u32 dst_w, u32 dst_h, u32 dst_stride);
void apple_sgx_report(struct apple_sgx *sgx, const char *why);

/* apple_sgx_drm.c */
int apple_sgx_drm_init(struct apple_sgx *sgx);
void apple_sgx_drm_fini(struct apple_sgx *sgx);
bool apple_sgx_drm_busy(struct apple_sgx *sgx);
int apple_sgx_drm_reserve(struct apple_sgx *sgx, int slot, u32 va, u32 size);
void apple_sgx_drm_unreserve(struct apple_sgx *sgx, int slot);
u32 apple_sgx_bo_va(struct drm_gem_object *obj);

#endif
