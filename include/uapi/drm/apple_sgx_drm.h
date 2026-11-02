/* SPDX-License-Identifier: MIT */
/*
 * S5L8947X PowerVR SGX543: the render node's interface.
 *
 * EXPERIMENTAL.  This is the interface test programs and the Mesa driver
 * talk to the GPU through.  It follows the interface first published with
 * the clean-room SGX543 work in Project Cascadia; the kernel side here is
 * adapted to the S5L8947X.  APPLE_SGX_PARAM_UAPI_VERSION says which revision
 * a kernel has.
 *
 * The model, kept as small as the hardware allows:
 *
 *  - One GPU address space, shared by every client (the microkernel's own
 *    buffers are in it too, at 0x80000000).  Buffers are GEM objects mapped
 *    into it for their whole life; the kernel picks the address (64 KiB
 *    aligned for buffers of 64 KiB or more: a render target needs it), or
 *    the caller asks for one (APPLE_SGX_BO_FIXED_VA).  A buffer is either
 *    backed by shmem pages or, with APPLE_SGX_BO_CONTIGUOUS, by one
 *    physically contiguous block that a display controller can scan out
 *    (its dma-buf has a single scatter/gather entry).  dma-bufs from other
 *    drivers can be imported too: PRIME maps them at import time.  The GPU's
 *    own pointer encodings decide where things may go -- PDS data pointers only reach
 *    0x80000000 and up, and USSE code is addressed relative to a code base
 *    register -- so the usable range and the code zone are parameters.
 *
 *  - USE_CODE_BASE_3 and USE_CODE_BASE_5 point at
 *    APPLE_SGX_PARAM_CODE_BASE.  A DOUTU word for code at GPU address A is
 *    ((A - CODE_BASE) / 8) << 4 | 3, the index in 20 bits: code has to be
 *    within 8 MiB of the base.  Buffers created with
 *    APPLE_SGX_BO_USSE_CODE land there.
 *
 *  - A render (the TA, then the 3D pass) is submitted as the TA command the
 *    stock driver puts in a render context's CCB: the kernel copies it into
 *    the render queue, points its completion at its own memory, writes the
 *    render context into the render details (+0x20, +0xa4), sets the
 *    parameter buffer descriptor, and kicks.  The job is done when the
 *    microkernel clears the render details' word at +0x24 after the 3D pass.
 *    Renders run one at a time, in submission order across all clients.
 *
 *  - The 2D engine, through BLIT: a scaled copy from a source rectangle to
 *    a destination buffer, using the transfer queue.  The kernel builds the
 *    transfer itself (the USSE/PDS programs, the PDS block and the
 *    parameter page), so userspace only names buffers and sizes; the call
 *    returns when the copy is over.
 *
 *  - Synchronisation is syncobjs: a submit waits for in_syncs and signals
 *    out_sync when its 3D pass is over.  Every buffer the render touches
 *    must be listed in bo_handles; they stay alive and mapped until then.
 */
#ifndef _APPLE_SGX_DRM_H_
#define _APPLE_SGX_DRM_H_

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define DRM_APPLE_SGX_GET_PARAM			0x00
#define DRM_APPLE_SGX_GEM_CREATE		0x01
#define DRM_APPLE_SGX_GEM_MMAP_OFFSET		0x02
#define DRM_APPLE_SGX_GEM_WAIT			0x03
#define DRM_APPLE_SGX_SUBMIT			0x04
#define DRM_APPLE_SGX_GEM_INFO			0x05
#define DRM_APPLE_SGX_BLIT			0x07

#define DRM_IOCTL_APPLE_SGX_GET_PARAM \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APPLE_SGX_GET_PARAM, struct drm_apple_sgx_get_param)
#define DRM_IOCTL_APPLE_SGX_GEM_CREATE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APPLE_SGX_GEM_CREATE, struct drm_apple_sgx_gem_create)
#define DRM_IOCTL_APPLE_SGX_GEM_MMAP_OFFSET \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APPLE_SGX_GEM_MMAP_OFFSET, struct drm_apple_sgx_gem_mmap_offset)
#define DRM_IOCTL_APPLE_SGX_GEM_WAIT \
	DRM_IOW(DRM_COMMAND_BASE + DRM_APPLE_SGX_GEM_WAIT, struct drm_apple_sgx_gem_wait)
#define DRM_IOCTL_APPLE_SGX_SUBMIT \
	DRM_IOW(DRM_COMMAND_BASE + DRM_APPLE_SGX_SUBMIT, struct drm_apple_sgx_submit)
#define DRM_IOCTL_APPLE_SGX_GEM_INFO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APPLE_SGX_GEM_INFO, struct drm_apple_sgx_gem_info)
#define DRM_IOCTL_APPLE_SGX_BLIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APPLE_SGX_BLIT, struct drm_apple_sgx_blit)

/* ---- GET_PARAM ---------------------------------------------------------- */

enum drm_apple_sgx_param {
	APPLE_SGX_PARAM_UAPI_VERSION = 0,	/* 3 (2: no kernel PB, 1: no GEM_INFO) */
	APPLE_SGX_PARAM_CORE_ID = 1,		/* MASTER_CORE_ID */
	APPLE_SGX_PARAM_CORE_REVISION = 2,	/* CORE_REVISION */
	APPLE_SGX_PARAM_NUM_CORES = 3,		/* the number of SGX cores */
	APPLE_SGX_PARAM_CLOCK_KHZ = 4,		/* the GPU clock */
	APPLE_SGX_PARAM_UKERNEL_UP = 5,		/* 1 while the microkernel runs */

	/* GPU addresses: [VA_START, VA_END) is where buffers may be mapped,
	 * [CODE_VA_START, CODE_VA_END) the code zone, CODE_BASE what
	 * USE_CODE_BASE_3 and _5 hold (see above). */
	APPLE_SGX_PARAM_VA_START = 6,
	APPLE_SGX_PARAM_VA_END = 7,
	APPLE_SGX_PARAM_CODE_BASE = 8,
	APPLE_SGX_PARAM_CODE_VA_START = 9,
	APPLE_SGX_PARAM_CODE_VA_END = 10,

	/* The screen: the display pipe's live scanout, which the kernel maps
	 * at FB_VA and BLIT can copy into (dst_handle 0).  0 while there is
	 * no display or it is not scanning anything out.  Linear, 32 bits a
	 * pixel, BGRA in memory. */
	APPLE_SGX_PARAM_FB_VA = 11,
	APPLE_SGX_PARAM_FB_WIDTH = 12,
	APPLE_SGX_PARAM_FB_HEIGHT = 13,
	APPLE_SGX_PARAM_FB_STRIDE = 14,

	/* Renders submitted, completed, and timed out since the module loaded;
	 * microkernel (re)starts. */
	APPLE_SGX_PARAM_RENDERS_SUBMITTED = 15,
	APPLE_SGX_PARAM_RENDERS_DONE = 16,
	APPLE_SGX_PARAM_RENDERS_TIMED_OUT = 17,
	APPLE_SGX_PARAM_UKERNEL_BOOTS = 18,

	/* UAPI 3: the kernel's parameter buffer (a submit with pb_va 0 uses
	 * it), and where APPLE_SGX_BO_TA_HEAP buffers go -- above the TA's
	 * base, 0x87800000, within the 256 MiB the TA's addresses reach. */
	APPLE_SGX_PARAM_PB_VA = 19,
	APPLE_SGX_PARAM_TA_HEAP_START = 20,
	APPLE_SGX_PARAM_TA_HEAP_END = 21,
};

struct drm_apple_sgx_get_param {
	__u32 param;		/* in: enum drm_apple_sgx_param */
	__u32 pad;		/* in: 0 */
	__u64 value;		/* out */
};

/* ---- GEM_CREATE --------------------------------------------------------- */

/* Map at drm_apple_sgx_gem_create.va instead of an address the kernel
 * picks.  Fails with EEXIST if anything is mapped there already. */
#define APPLE_SGX_BO_FIXED_VA			(1 << 0)
/* USSE code: in the code zone, where a DOUTU can reach it. */
#define APPLE_SGX_BO_USSE_CODE			(1 << 1)
/* The GPU may only read it (PTE READONLY). */
#define APPLE_SGX_BO_GPU_READONLY		(1 << 2)
/* Without the PTE's CACHECONSISTENT bit, which every other buffer has (the
 * microkernel only sees CPU writes to cache-consistent pages). */
#define APPLE_SGX_BO_NOT_CACHE_CONSISTENT	(1 << 3)
/* In the TA's heap (UAPI 3): render target data, whose state buffer the 3D
 * register block names by its offset from the TA's base. */
#define APPLE_SGX_BO_TA_HEAP			(1 << 4)
/* One physically contiguous block, so a dma-buf of the buffer has a table of
 * one entry: what a scanout controller with no scatter/gather needs to
 * import it (the display's DMA-GEM import rejects anything else).  Comes
 * from the platform's CMA pool where there is one, so it is limited to a
 * few megabytes and the address is not suitable for TA/USSE code.
 * Kernels without it reject GEM_CREATE with -EINVAL, which is how to
 * probe for it without a version bump. */
#define APPLE_SGX_BO_CONTIGUOUS			(1 << 5)

#define APPLE_SGX_BO_FLAGS			0x3f

struct drm_apple_sgx_gem_create {
	__u64 size;		/* in: bytes, rounded up to 4 KiB */
	__u32 flags;		/* in: APPLE_SGX_BO_* */
	__u32 handle;		/* out */
	__u32 va;		/* in with APPLE_SGX_BO_FIXED_VA, out: the GPU address */
	__u32 pad;		/* in: 0 */
};

/* ---- GEM_MMAP_OFFSET: the offset to mmap() the buffer at (write-combined) */

struct drm_apple_sgx_gem_mmap_offset {
	__u32 handle;		/* in */
	__u32 flags;		/* in: 0 */
	__u64 offset;		/* out */
};

/* ---- GEM_WAIT: until no render that lists the buffer is running -------- */

struct drm_apple_sgx_gem_wait {
	__u32 handle;		/* in */
	__u32 pad;		/* in: 0 */
	__s64 timeout_ns;	/* in: absolute, CLOCK_MONOTONIC; 0 just polls */
};

/* ---- SUBMIT: one render, the TA and then the 3D pass -------------------- */

#define APPLE_SGX_TA_CMD_MIN			0x11c
#define APPLE_SGX_TA_CMD_MAX			0x200

struct drm_apple_sgx_submit {
	/* in: user pointer to the TA command, as the stock render submit
	 * builds it: word 0 its size in bytes, a multiple of 8 between
	 * APPLE_SGX_TA_CMD_MIN and _MAX; the kernel rewrites words 0, 0x64,
	 * 0x68 and 0x6c. */
	__u64 cmd;
	__u32 cmd_size;		/* in: bytes copied from cmd; >= word 0 */
	__u32 flags;		/* in: 0 */

	__u32 pb_va;		/* in: the parameter buffer's descriptor; 0: the
				 * kernel's, shared by every client (UAPI 3) */
	__u32 details_handle;	/* in: the buffer holding the render details */
	__u32 details_offset;	/* in: where in it, 4-byte aligned */
	__u32 cache_control;	/* in: SGXMKIF_CC_* to add to the kick, normally 0 */

	__u64 bo_handles;	/* in: user pointer to __u32 handles */
	__u32 bo_handle_count;
	__u32 in_sync_count;
	__u64 in_syncs;		/* in: user pointer to __u32 syncobj handles */
	__u32 out_sync;		/* in: syncobj to signal when it is done, or 0 */
	__u32 pad;		/* in: 0 */
};

/* ---- GEM_INFO: what a handle is, for one that came from a dma-buf ------ */

/* PRIME_FD_TO_HANDLE takes a dma-buf from another device too (a display's
 * scanout buffer, for instance); the import is mapped into the GPU address
 * space at once.  This says what the handle is and where the GPU sees it.
 * UAPI version 2 on. */
struct drm_apple_sgx_gem_info {
	__u32 handle;		/* in */
	__u32 flags;		/* out: the APPLE_SGX_BO_* it was made with */
	__u32 va;		/* out: its GPU address */
	__u32 pad;		/* in: 0 */
	__u64 size;		/* out: bytes */
};

/* ---- BLIT: the 2D engine, a scaled copy --------------------------------- */

/*
 * The transfer queue's blit: the source rectangle (BGRA8888, linear) is
 * scaled into the destination buffer's whole dst_width x dst_height area.
 * Both buffers have to be the render node's (GEM_CREATE or an imported
 * dma-buf), and the kernel keeps them alive and mapped until the transfer
 * is over; the ioctl returns when it is, or -ETIMEDOUT.
 *
 * dst_handle 0 is the screen: the display pipe's live scanout, which the
 * kernel has mapped already.  Read dst_width, dst_height and dst_stride
 * from APPLE_SGX_PARAM_FB_* and pass them back unchanged; the copy only
 * fills the buffer the pipe is showing.  So a program can render into its
 * own buffer and put it on the display with one call, with no CPU copy and
 * without a compositor.
 *
 * The 2D engine works in tiles: buffers of at least 64 KiB are mapped at
 * 64 KiB-aligned GPU addresses, and the destination stride has to be a
 * multiple of 4 bytes (a multiple of 64 is the natural choice).  A buffer
 * meant for a display to scan out wants APPLE_SGX_BO_CONTIGUOUS.
 */
struct drm_apple_sgx_blit {
	__u32 src_handle;	/* in: the source buffer */
	__u32 dst_handle;	/* in: the destination buffer, 0 = the screen */
	__u32 src_width;	/* in: source rectangle, pixels (1..4096) */
	__u32 src_height;
	__u32 src_stride;	/* in: bytes a source line, a multiple of 4 */
	__u32 dst_width;	/* in: destination size, pixels (1..4096) */
	__u32 dst_height;
	__u32 dst_stride;	/* in: bytes a destination line, a multiple of 4 */
	__u32 flags;		/* in: 0 */
	__u32 pad;		/* in: 0 */
};

#if defined(__cplusplus)
}
#endif

#endif /* _APPLE_SGX_DRM_H_ */
