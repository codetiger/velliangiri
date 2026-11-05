/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * shell_gpu.h - Direct-UAPI PowerVR SGX543MP1 TBDR 2D UI Compositor
 *
 * Offloads all tvshell and media-gallery 2D UI rendering (rounded cards,
 * anti-aliased outlines, gradients, shadows, glows, FreeType glyphs, and
 * M2SC-scaled photo textures) to the SGX543 TA/3D hardware pipeline via
 * /dev/dri/renderD128, freeing Cortex-A9 CPU, NEON, L2 cache, and DRAM
 * bandwidth for concurrent LLM inference.
 */

#ifndef TVSHELL_GPU_H
#define TVSHELL_GPU_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Initialize the SGX543 direct-UAPI compositor.
 * When drm_fd >= 0 and num_scanout_bufs > 0, imports the scanout DMABUF FDs
 * via PRIME into the SGX MMU for zero-copy PBE tile flush and background reload.
 * When drm_fd < 0 (headless mode), allocates internal 1280x720 SGX render target
 * BOs so --bench and --dump-ppm execute on real SGX543 hardware.
 */
bool shell_gpu_init(int drm_fd, int num_scanout_bufs,
		    const int *scanout_dmabuf_fds, int width, int height);
void shell_gpu_close(void);
bool shell_gpu_active(void);

/* Number of quads queued in the current frame */
uint32_t shell_gpu_draw_count(void);

/*
 * Kick SGX543 TA/3D pass and wait for completion.
 * dst_buf_idx: target scanout buffer index (0..2)
 * bg_buf_idx:  previous completed scanout buffer index for hardware tile reload
 *              (pass -1 on full-screen clear frames to skip DRAM reload)
 */
int shell_gpu_flush(int dst_buf_idx, int bg_buf_idx);

/* Readback pointer and DMABUF FD for headless / --dump-ppm / --bench mode */
const uint32_t *shell_gpu_get_headless_pixels(int buf_idx);
int shell_gpu_target_dmabuf_fd(int buf_idx);

/* Core 2D UI primitives */
void shell_gpu_draw_rect(int x, int y, int w, int h,
			 uint32_t color, uint8_t alpha);
void shell_gpu_draw_grad_v(int x, int y, int w, int h,
			   uint32_t c_top, uint8_t a_top,
			   uint32_t c_bot, uint8_t a_bot);
void shell_gpu_draw_grad_h(int x, int y, int w, int h,
			   uint32_t c_left, uint8_t a_left,
			   uint32_t c_right, uint8_t a_right);
void shell_gpu_draw_shimmer_bar(int x, int y, int w, int h, int radius,
				uint32_t base_color, uint32_t highlight_color,
				int phase_pct);
void shell_gpu_draw_card_bottom_scrim(int card_x, int card_y,
				      int card_w, int card_h,
				      int radius, int scrim_h);
void shell_gpu_draw_top_scrim(int w, int h);
void shell_gpu_draw_bottom_scrim(int y0, int w, int h);
void shell_gpu_draw_rounded_rect(int x, int y, int w, int h, int r,
				 uint32_t color, uint8_t alpha);
void shell_gpu_draw_rounded_outline(int x, int y, int w, int h, int r,
				    int thickness, uint32_t color, uint8_t alpha);

/* Atlas 0 FreeType glyph & vector symbol shelf packer */
bool shell_gpu_upload_glyph(uint16_t w, uint16_t h,
			    const uint8_t *alpha_bitmap,
			    uint16_t *out_u, uint16_t *out_v);
void shell_gpu_draw_glyph(int x, int y, int w, int h,
			  uint16_t atlas_u, uint16_t atlas_v,
			  uint32_t color, uint8_t alpha);

/* Zero-copy Atlas 1/2/3 photo card & fullscreen bitmap drawing */
bool shell_gpu_draw_rounded_bitmap(int x, int y, int w, int h, int r,
				   const uint32_t *pixels, int src_stride);
bool shell_gpu_draw_bitmap_rect(int dst_x, int dst_y,
				int src_x, int src_y, int w, int h,
				const uint32_t *pixels, int src_stride,
				uint8_t alpha);

/*
 * Zero-copy DMABUF & Atlas accessors for M2SC0/1 hardware scalers (shell_media.c).
 * Atlas 1 (2048x1024 POT texture BO):
 *   - 10 Card slots (276x312) at y = 0, 312
 *   - 10 Tile slots (276x164) at y = 624, 788
 *   - Persistent static bitmap cache & dynamic staging columns at x >= 1402
 * Atlas 2 & 3 (1280x720 contiguous DMABUF BOs):
 *   - Fullscreen background & photo viewer slots
 */
uint32_t *shell_gpu_atlas1_slot_ptr(bool is_tile, int slot_idx,
				    int *out_x, int *out_y,
				    int *out_stride_bytes,
				    int *out_full_w, int *out_full_h);
int shell_gpu_fs_atlas_dmabuf_fd(int fs_idx, uint32_t **out_ptr,
				 int *out_stride_bytes);

/* Shared 2x2-supersampled SDF corner coverage tables (shell_display.c) */
uint8_t shell_sdf_corner_cov(int radius, int cy, int cx);
uint8_t shell_sdf_corner_stroke(int radius, int thickness, int cy, int cx);

#endif /* TVSHELL_GPU_H */
