/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The apple_sgx render node's interface, for the standalone tools.
 *
 * The one definition is include/uapi/drm/apple_sgx_drm.h in the kernel
 * tree.  These tools build without that tree (and without libdrm), so this
 * is the same interface in the types libc has.  Keep it in step with the
 * kernel header: APPLE_SGX_UAPI_VERSION is 3.
 *
 * One copy for every tool: sgxinfo, sgxgem, sgx2d, sgxpresent, sgxframe.
 */
#ifndef TOOLS_SGX_UAPI_H
#define TOOLS_SGX_UAPI_H

#include <stdint.h>
#include <sys/ioctl.h>

#ifndef DRM_IOCTL_BASE
#define DRM_IOCTL_BASE 'd'
#endif
#ifndef DRM_COMMAND_BASE
#define DRM_COMMAND_BASE 0x40
#endif
#ifndef DRM_IOWR
#define DRM_IOWR(nr, type) _IOWR(DRM_IOCTL_BASE, nr, type)
#endif
#ifndef DRM_IOW
#define DRM_IOW(nr, type) _IOW(DRM_IOCTL_BASE, nr, type)
#endif

#define DRM_APPLE_SGX_GET_PARAM			0x00
#define DRM_APPLE_SGX_GEM_CREATE		0x01
#define DRM_APPLE_SGX_GEM_MMAP_OFFSET		0x02
#define DRM_APPLE_SGX_GEM_WAIT			0x03
#define DRM_APPLE_SGX_SUBMIT			0x04
#define DRM_APPLE_SGX_GEM_INFO			0x05
#define DRM_APPLE_SGX_BLIT			0x07

enum drm_apple_sgx_param {
	APPLE_SGX_PARAM_UAPI_VERSION = 0,
	APPLE_SGX_PARAM_CORE_ID = 1,
	APPLE_SGX_PARAM_CORE_REVISION = 2,
	APPLE_SGX_PARAM_NUM_CORES = 3,
	APPLE_SGX_PARAM_CLOCK_KHZ = 4,
	APPLE_SGX_PARAM_UKERNEL_UP = 5,
	APPLE_SGX_PARAM_VA_START = 6,
	APPLE_SGX_PARAM_VA_END = 7,
	APPLE_SGX_PARAM_CODE_BASE = 8,
	APPLE_SGX_PARAM_CODE_VA_START = 9,
	APPLE_SGX_PARAM_CODE_VA_END = 10,
	APPLE_SGX_PARAM_FB_VA = 11,
	APPLE_SGX_PARAM_FB_WIDTH = 12,
	APPLE_SGX_PARAM_FB_HEIGHT = 13,
	APPLE_SGX_PARAM_FB_STRIDE = 14,
	APPLE_SGX_PARAM_RENDERS_SUBMITTED = 15,
	APPLE_SGX_PARAM_RENDERS_DONE = 16,
	APPLE_SGX_PARAM_RENDERS_TIMED_OUT = 17,
	APPLE_SGX_PARAM_UKERNEL_BOOTS = 18,
	APPLE_SGX_PARAM_PB_VA = 19,
	APPLE_SGX_PARAM_TA_HEAP_START = 20,
	APPLE_SGX_PARAM_TA_HEAP_END = 21,
};

/* Map at drm_apple_sgx_gem_create.va instead of an address the kernel
 * picks (EEXIST if taken). */
#define APPLE_SGX_BO_FIXED_VA			(1u << 0)
/* USSE code: in the code zone, where a DOUTU can reach it. */
#define APPLE_SGX_BO_USSE_CODE			(1u << 1)
/* The GPU may only read it. */
#define APPLE_SGX_BO_GPU_READONLY		(1u << 2)
/* Without the PTE's CACHECONSISTENT bit. */
#define APPLE_SGX_BO_NOT_CACHE_CONSISTENT	(1u << 3)
/* In the TA's heap (render target data, UAPI 3). */
#define APPLE_SGX_BO_TA_HEAP			(1u << 4)
/* One physically contiguous block, so a dma-buf of it has a one-entry
 * scatter/gather table (a scanout controller imports only that). */
#define APPLE_SGX_BO_CONTIGUOUS			(1u << 5)

#define APPLE_SGX_BO_FLAGS			0x3fu

struct drm_apple_sgx_get_param {
	uint32_t param;
	uint32_t pad;
	uint64_t value;
};

struct drm_apple_sgx_gem_create {
	uint64_t size;
	uint32_t flags;
	uint32_t handle;
	uint32_t va;
	uint32_t pad;
};

struct drm_apple_sgx_gem_mmap_offset {
	uint32_t handle;
	uint32_t flags;
	uint64_t offset;
};

struct drm_apple_sgx_gem_wait {
	uint32_t handle;
	uint32_t pad;
	int64_t timeout_ns;
};

struct drm_apple_sgx_gem_info {
	uint32_t handle;
	uint32_t flags;
	uint32_t va;
	uint32_t pad;
	uint64_t size;
};

struct drm_apple_sgx_submit {
	uint64_t cmd;
	uint32_t cmd_size;
	uint32_t flags;
	uint32_t pb_va;
	uint32_t details_handle;
	uint32_t details_offset;
	uint32_t cache_control;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t in_sync_count;
	uint64_t in_syncs;
	uint32_t out_sync;
	uint32_t pad;
};

struct drm_apple_sgx_blit {
	uint32_t src_handle;
	uint32_t dst_handle;
	uint32_t src_width;
	uint32_t src_height;
	uint32_t src_stride;
	uint32_t dst_width;
	uint32_t dst_height;
	uint32_t dst_stride;
	uint32_t flags;
	uint32_t pad;
};

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

#endif /* TOOLS_SGX_UAPI_H */
