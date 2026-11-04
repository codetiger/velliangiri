// SPDX-License-Identifier: GPL-2.0-only
/*
 * Media Gallery on Velliangiri OS — Standalone 1280x720 Fullscreen Media App
 *
 * Dedicated fullscreen TV application (/bin/media-gallery) for browsing and
 * playing all on-device photos (.jpeg) and 720p videos (.mov) in the gallery
 * folder (/mnt/data/linux/media) with hardware V4L2 JPEG + M2SC acceleration,
 * DRM double-buffered page flipping, and synchronized 48 kHz stereo ALSA audio.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "tvshell.h"
#include "shell_assets.h"

/* Fullscreen 4x2 Media Grid Geometry (inside 1280x720 safe area 64px L/R, 32px T/B) */
#define MG_SAFE_X		64
#define MG_SAFE_W		1152

#define MG_HEADER_Y		28
#define MG_HEADER_H		48

#define MG_GRID_X		66
#define MG_GRID_Y		88
#define MG_TILE_COLS		4
#define MG_TILE_ROWS		2
#define MG_TILES_PER_PAGE	(MG_TILE_COLS * MG_TILE_ROWS)
#define MG_TILE_W		275
#define MG_TILE_H		238
#define MG_TILE_ART_H		164
#define MG_TILE_GAP_X		16
#define MG_TILE_GAP_Y		16
#define MG_TILE_RADIUS		16
#define MG_HUD_AUTOHIDE_MS	3000ULL

#define MG_FOOTER_Y		596
#define MG_FOOTER_H		54

enum mg_view_id {
	MG_VIEW_GRID = 0,	/* Fullscreen 4x2 Media Gallery Grid */
	MG_VIEW_PLAYER = 1,	/* Edge-to-Edge 1280x720 Video Player & Photo Viewer */
};

enum mg_focus_zone {
	MG_ZONE_GRID = 0,	/* Media tiles in the 4x2 grid */
	MG_ZONE_TABS = 1,	/* Top filter tabs: All / Videos / Photos */
};

enum mg_filter_kind {
	MG_FILTER_ALL = 0,
	MG_FILTER_VIDEOS = 1,
	MG_FILTER_PHOTOS = 2,
	MG_FILTER_COUNT = 3,
};

static volatile sig_atomic_t g_running = 1;

static struct {
	enum shell_theme_id theme_id;
	enum mg_view_id view;
	enum mg_focus_zone focus_zone;
	enum mg_filter_kind filter;
	int tab_focus_idx;

	/* Master list scanned from gallery folder */
	struct shell_media_item all_items[SHELL_MAX_GALLERY_ITEMS];
	int all_count;
	int video_count;
	int photo_count;

	/* Filtered index mapping into all_items[] */
	int filtered_map[SHELL_MAX_GALLERY_ITEMS];
	int filtered_count;

	/* Grid selection & pagination */
	int selected_idx;	/* 0 .. filtered_count - 1 */
	int current_page;	/* selected_idx / MG_TILES_PER_PAGE */
	int preload_cursor;	/* Background preloader index into all_items[] */
	bool preload_done;

	/* Incremental dirty flags for 60 FPS grid navigation */
	bool bg_valid;
	bool header_dirty;
	bool footer_dirty;
	uint8_t dirty_tiles;	/* Bitmask 0..7 for the 8 visible tiles */

	/* Fullscreen Player / Viewer state */
	struct giri_media_card player_card;
	bool player_is_video;
	bool player_playing;
	bool player_hud_visible;
	bool player_hud_dirty;
	int player_fps;
	int player_total_frames;
	int player_frame_idx;
	int player_elapsed_sec;
	int player_duration_sec;
	uint64_t player_last_tick_ms;
	uint64_t player_frame_tick_ms;
	uint64_t player_hud_shown_ms;
	uint64_t slideshow_last_ms;
} g_mg;

#ifdef MEDIA_GALLERY_STANDALONE
static void handle_sig(int sig)
{
	(void)sig;
	g_running = 0;
}
#endif

static void mark_grid_full_redraw(void)
{
	g_mg.bg_valid = false;
	g_mg.header_dirty = true;
	g_mg.footer_dirty = true;
	g_mg.dirty_tiles = 0xffU;
}

static void make_card_from_item(const struct shell_media_item *it,
				int idx, struct giri_media_card *mc)
{
	memset(mc, 0, sizeof(*mc));
	snprintf(mc->id, sizeof(mc->id), "mg_%d", idx);
	shell_font_fit_clean_phrase(it->title, mc->title,
				    sizeof(mc->title), SHELL_LIMIT_CARD_TITLE);
	shell_font_fit_clean_phrase(it->subtitle, mc->subtitle,
				    sizeof(mc->subtitle), SHELL_LIMIT_CARD_SUB);
	snprintf(mc->badge, sizeof(mc->badge), "%s",
		 it->is_video ? "VIDEO" : "PHOTO");
	snprintf(mc->meta_pill, sizeof(mc->meta_pill), "%s",
		 it->is_video ? "720P" : "HD");
	mc->kind = it->is_video ? GIRI_CARD_LOCAL_VIDEO : GIRI_CARD_LOCAL_PHOTO;
	mc->preset_art = it->is_video ? GIRI_ART_MONSOON_VIDEO : GIRI_ART_DAWN_RIDGE;
	snprintf(mc->uri, sizeof(mc->uri), "%s", it->path);
	snprintf(mc->detail_text, sizeof(mc->detail_text), "%s",
		 it->detail[0] ? it->detail : it->subtitle);
}

static void apply_filter(enum mg_filter_kind filter)
{
	int i;

	g_mg.filter = filter;
	g_mg.filtered_count = 0;

	for (i = 0; i < g_mg.all_count; i++) {
		bool keep = true;

		if (filter == MG_FILTER_VIDEOS)
			keep = g_mg.all_items[i].is_video;
		else if (filter == MG_FILTER_PHOTOS)
			keep = !g_mg.all_items[i].is_video;

		if (keep)
			g_mg.filtered_map[g_mg.filtered_count++] = i;
	}

	g_mg.selected_idx = 0;
	g_mg.current_page = 0;
	mark_grid_full_redraw();
}

static void scan_gallery_folder(void)
{
	int i;

	g_mg.all_count = shell_media_scan_device(g_mg.all_items,
						 SHELL_MAX_GALLERY_ITEMS,
						 false);
	g_mg.video_count = 0;
	g_mg.photo_count = 0;
	for (i = 0; i < g_mg.all_count; i++) {
		if (g_mg.all_items[i].is_video)
			g_mg.video_count++;
		else
			g_mg.photo_count++;
	}
	apply_filter(g_mg.filter);
}

/* =========================================================================
 * 1. Fullscreen 4x2 Media Grid Compositor
 * ========================================================================= */

static void render_header_bar(const struct shell_theme *th, bool clear_bg)
{
	int hx = MG_SAFE_X;
	int hy = MG_HEADER_Y;
	int i;

	if (clear_bg)
		shell_draw_rect(MG_SAFE_X, hy - 4, MG_SAFE_W, MG_HEADER_H + 8, th->bg);

	/* App badge icon + Title */
	shell_draw_rounded_rect(hx, hy + 4, 40, 40, 11, th->accent);
	shell_draw_symbol(SHELL_SYM_IMAGE, hx + 10, hy + 14, 20, 0xfffaf6f0U);
	hx += 52;

	hx += shell_font_draw_vcentered(SHELL_FONT_SANS_BOLD, 24,
					hx, hy, MG_HEADER_H, th->ink,
					"Media Gallery", 240) + 14;

	/* Folder / count subtitle pill */
	char sub_buf[64];
	snprintf(sub_buf, sizeof(sub_buf), "%d Videos · %d Photos",
		 g_mg.video_count, g_mg.photo_count);
	int sub_w = shell_font_measure_role(SHELL_FONT_MONO_MEDIUM, 13, sub_buf) + 24;
	shell_draw_rounded_rect(hx, hy + 10, sub_w, 28, 14, th->surface);
	shell_draw_rounded_outline(hx, hy + 10, sub_w, 28, 14, 1, th->border);
	shell_font_draw_box_centered(SHELL_FONT_MONO_MEDIUM, 13,
				     hx, hy + 10, sub_w, 28, th->muted, sub_buf);

	/* Filter Tabs on the right-center: All / Videos / Photos */
	char tab_labels[MG_FILTER_COUNT][32];
	snprintf(tab_labels[0], sizeof(tab_labels[0]), "All (%d)", g_mg.all_count);
	snprintf(tab_labels[1], sizeof(tab_labels[1]), "Videos (%d)", g_mg.video_count);
	snprintf(tab_labels[2], sizeof(tab_labels[2]), "Photos (%d)", g_mg.photo_count);

	int tx = MG_SAFE_X + 520;
	for (i = 0; i < MG_FILTER_COUNT; i++) {
		bool active = ((int)g_mg.filter == i);
		bool focused = (g_mg.focus_zone == MG_ZONE_TABS && g_mg.tab_focus_idx == i);
		int tw = shell_font_measure_role(SHELL_FONT_SANS_BOLD, 14, tab_labels[i]) + 32;
		int ty = hy + 7;
		int th_tab = 34;

		if (focused) {
			shell_draw_focus_glow(tx, ty, tw, th_tab, 17,
					      8, th->focus_ring, 60);
		}
		shell_draw_rounded_rect(tx, ty, tw, th_tab, 17,
					active ? th->accent :
					(focused ? th->surface_hi : th->surface));
		shell_draw_rounded_outline(tx, ty, tw, th_tab, 17,
					   focused ? 2 : 1,
					   (active || focused) ? th->accent : th->border);
		shell_font_draw_box_centered(SHELL_FONT_SANS_BOLD, 14,
					     tx, ty, tw, th_tab,
					     active ? 0xfffaf6f0U :
					     (focused ? th->ink : th->muted),
					     tab_labels[i]);
		tx += tw + 10;
	}

	/* Rightmost Page & Item Counter */
	int total_pages = (g_mg.filtered_count + MG_TILES_PER_PAGE - 1) / MG_TILES_PER_PAGE;
	if (total_pages < 1)
		total_pages = 1;
	char pg_buf[64];
	if (g_mg.filtered_count > 0) {
		snprintf(pg_buf, sizeof(pg_buf), "Item %d/%d · Page %d/%d",
			 (g_mg.selected_idx + 1) & 0xff,
			 g_mg.filtered_count & 0xff,
			 (g_mg.current_page + 1) & 0xff,
			 total_pages & 0xff);
	} else {
		snprintf(pg_buf, sizeof(pg_buf), "0 Items");
	}
	int pw = shell_font_measure_role(SHELL_FONT_MONO_SEMIBOLD, 14, pg_buf);
	shell_font_draw_vcentered(SHELL_FONT_MONO_SEMIBOLD, 14,
				  MG_SAFE_X + MG_SAFE_W - pw, hy, MG_HEADER_H,
				  th->muted, pg_buf, pw + 8);
}

static void render_single_tile(const struct shell_theme *th, int slot_idx, bool clear_slot)
{
	int col = slot_idx % MG_TILE_COLS;
	int row = slot_idx / MG_TILE_COLS;
	int x = MG_GRID_X + col * (MG_TILE_W + MG_TILE_GAP_X);
	int y = MG_GRID_Y + row * (MG_TILE_H + MG_TILE_GAP_Y);
	int w = MG_TILE_W;
	int h = MG_TILE_H;
	int pad = 10;
	int item_idx = g_mg.current_page * MG_TILES_PER_PAGE + slot_idx;

	if (clear_slot)
		shell_draw_rect(x - pad, y - pad, w + pad * 2, h + pad * 2, th->bg);

	if (item_idx >= g_mg.filtered_count) {
		/* Empty tile slot on last page */
		shell_draw_rounded_rect(x, y, w, h, MG_TILE_RADIUS, th->surface);
		shell_draw_rounded_outline(x, y, w, h, MG_TILE_RADIUS, 1, th->border);
		return;
	}

	int raw_idx = g_mg.filtered_map[item_idx];
	const struct shell_media_item *it = &g_mg.all_items[raw_idx];
	bool focused = (g_mg.focus_zone == MG_ZONE_GRID && g_mg.selected_idx == item_idx);
	int draw_y = focused ? (y - 3) : y;

	struct giri_media_card mc;
	make_card_from_item(it, raw_idx, &mc);

	if (focused) {
		shell_draw_focus_glow(x, draw_y, w, h, MG_TILE_RADIUS,
				      8, th->focus_ring, 68);
	}
	shell_draw_card_shadow(x, draw_y, w, h, MG_TILE_RADIUS);
	shell_draw_rounded_rect(x, draw_y, w, h, MG_TILE_RADIUS,
				focused ? th->surface_hi : th->surface);

	/*
	 * Hardware-decoded 2x-supersampled 275x164 (16:9 widescreen) thumbnail
	 * from shell_media.c: preserves the full widescreen framing with zero
	 * horizontal cropping and zero dark scrim over the photo.
	 */
	const uint32_t *art = shell_media_get_card_art_fast(&mc, w, MG_TILE_ART_H, NULL);
	if (art) {
		shell_draw_rounded_bitmap(x, draw_y, w, MG_TILE_ART_H,
					  MG_TILE_RADIUS, art, false);
		shell_draw_bitmap_xrgb(x, draw_y + MG_TILE_ART_H - MG_TILE_RADIUS,
				       w, MG_TILE_RADIUS,
				       art + (size_t)(MG_TILE_ART_H - MG_TILE_RADIUS) * w);
	}
	shell_draw_rect(x + 1, draw_y + MG_TILE_ART_H, w - 2, 1, th->border);

	/* Top-left media type badge (VIDEO vs PHOTO) */
	int bx = x + 10;
	int by = draw_y + 10;
	const char *badge_str = it->is_video ? "VIDEO" : "PHOTO";
	int bw = shell_font_measure_role(SHELL_FONT_MONO_SEMIBOLD, 11, badge_str) + 16;
	if (it->is_video) {
		shell_draw_rounded_rect(bx, by, bw, 22, 11, th->accent);
	} else {
		shell_draw_rounded_rect_alpha(bx, by, bw, 22, 11, 0xff14110fU, 210);
	}
	shell_font_draw_box_centered(SHELL_FONT_MONO_SEMIBOLD, 11,
				     bx, by, bw, 22, 0xfffaf6f0U, badge_str);

	/* Top-right resolution / audio pill */
	const char *meta_str = it->is_video ? "720P · PCM" : "16:9 HD";
	int mw = shell_font_measure_role(SHELL_FONT_MONO_MEDIUM, 11, meta_str) + 14;
	int mx = x + w - 10 - mw;
	shell_draw_rounded_rect_alpha(mx, by, mw, 22, 11, 0xff14110fU, 210);
	shell_font_draw_box_centered(SHELL_FONT_MONO_MEDIUM, 11,
				     mx, by, mw, 22, 0xffe7dfd5U, meta_str);

	/* Center Play Icon Overlay on Video Tiles */
	if (it->is_video) {
		int cx = x + w / 2;
		int cy = draw_y + MG_TILE_ART_H / 2;
		shell_draw_circle_alpha(cx, cy, 22,
					focused ? th->accent : 0xff14110fU,
					focused ? 235 : 195);
		shell_draw_symbol(SHELL_SYM_PLAY, cx - 7, cy - 8, 16, 0xfffaf6f0U);
	}

	/* Unobstructed Bottom Caption Panel (draw_y + 164 .. draw_y + 238) */
	shell_font_draw_role(SHELL_FONT_SANS_BOLD, 16,
			     x + 14, draw_y + MG_TILE_ART_H + 13, th->ink,
			     it->title, w - 28);
	shell_font_draw_role(SHELL_FONT_SANS_MEDIUM, 13,
			     x + 14, draw_y + MG_TILE_ART_H + 39, th->muted,
			     it->subtitle, w - 28);

	/* Outline (3px accent when focused, 1px subtle border otherwise) */
	shell_draw_rounded_outline(x, draw_y, w, h, MG_TILE_RADIUS,
				   focused ? 3 : 1,
				   focused ? th->accent : th->border);
}

static void render_footer_bar(const struct shell_theme *th, bool clear_bg)
{
	int fx = MG_SAFE_X;
	int fy = MG_FOOTER_Y;
	int fw = MG_SAFE_W;
	int fh = MG_FOOTER_H;

	if (clear_bg)
		shell_draw_rect(fx - 2, fy - 2, fw + 4, fh + 4, th->bg);

	shell_draw_rounded_rect(fx, fy, fw, fh, 14, th->surface);
	shell_draw_rounded_outline(fx, fy, fw, fh, 14, 1, th->border);

	const char *hints = "Select: Fullscreen · Play: Slideshow · Menu: Exit";
	int hw = shell_font_measure_role(SHELL_FONT_MONO_MEDIUM, 12, hints);

	if (g_mg.filtered_count > 0 && g_mg.selected_idx < g_mg.filtered_count) {
		int raw_idx = g_mg.filtered_map[g_mg.selected_idx];
		const struct shell_media_item *it = &g_mg.all_items[raw_idx];
		const char *bn = strrchr(it->path, '/');
		bn = bn ? (bn + 1) : it->path;

		int lx = fx + 16;
		const char *kind_tag = it->is_video ? "720P VIDEO + AUDIO" : "HIGH-RES PHOTO";
		int kw = shell_font_measure_role(SHELL_FONT_MONO_SEMIBOLD, 11, kind_tag) + 18;
		shell_draw_rounded_rect(lx, fy + 14, kw, 26, 13,
					it->is_video ? th->accent : th->surface_hi);
		shell_font_draw_box_centered(SHELL_FONT_MONO_SEMIBOLD, 11,
					     lx, fy + 14, kw, 26,
					     it->is_video ? 0xfffaf6f0U : th->ink,
					     kind_tag);
		lx += kw + 12;

		int avail_w = (fx + fw - hw - 32) - lx;
		if (avail_w > 80) {
			char raw_info[256];
			char fit_info[128];
			int max_chars = avail_w / 8;
			if (max_chars > 72)
				max_chars = 72;
			snprintf(raw_info, sizeof(raw_info), "%s (%s) · %s",
				 it->title, bn, it->detail[0] ? it->detail : it->subtitle);
			shell_font_fit_clean_phrase(raw_info, fit_info,
						    sizeof(fit_info), max_chars);
			shell_font_draw_vcentered(SHELL_FONT_SANS_MEDIUM, 14,
						  lx, fy, fh, th->ink,
						  fit_info, avail_w);
		}
	}

	shell_font_draw_vcentered(SHELL_FONT_MONO_MEDIUM, 12,
				  fx + fw - hw - 16, fy, fh, th->muted, hints, hw + 8);
}

static void render_grid_view(const struct shell_theme *th)
{
	int i;

	if (!g_mg.bg_valid) {
		shell_draw_rect(0, 0, SHELL_SCREEN_W, SHELL_SCREEN_H, th->bg);
		render_header_bar(th, false);
		for (i = 0; i < MG_TILES_PER_PAGE; i++)
			render_single_tile(th, i, false);
		render_footer_bar(th, false);

		g_mg.bg_valid = true;
		g_mg.header_dirty = false;
		g_mg.footer_dirty = false;
		g_mg.dirty_tiles = 0;
		shell_display_damage_all();
		shell_display_present();
		return;
	}

	if (g_mg.header_dirty) {
		render_header_bar(th, true);
		shell_display_damage(MG_SAFE_X, MG_HEADER_Y - 4,
				     MG_SAFE_W, MG_HEADER_H + 8);
		g_mg.header_dirty = false;
	}

	if (g_mg.dirty_tiles) {
		for (i = 0; i < MG_TILES_PER_PAGE; i++) {
			if (g_mg.dirty_tiles & (1U << i)) {
				int col = i % MG_TILE_COLS;
				int row = i / MG_TILE_COLS;
				int x = MG_GRID_X + col * (MG_TILE_W + MG_TILE_GAP_X);
				int y = MG_GRID_Y + row * (MG_TILE_H + MG_TILE_GAP_Y);

				render_single_tile(th, i, true);
				shell_display_damage(x - 10, y - 10,
						     MG_TILE_W + 20, MG_TILE_H + 20);
			}
		}
		g_mg.dirty_tiles = 0;
	}

	if (g_mg.footer_dirty) {
		render_footer_bar(th, true);
		shell_display_damage(MG_SAFE_X - 2, MG_FOOTER_Y - 2,
				     MG_SAFE_W + 4, MG_FOOTER_H + 4);
		g_mg.footer_dirty = false;
	}

	shell_display_present();
}

/* =========================================================================
 * 2. Edge-to-Edge 1280x720 Fullscreen Video Player & Photo Viewer
 * ========================================================================= */

static void open_player_at(int filtered_idx, bool start_slideshow)
{
	if (g_mg.filtered_count <= 0)
		return;

	filtered_idx = ((filtered_idx % g_mg.filtered_count) + g_mg.filtered_count) %
		       g_mg.filtered_count;
	g_mg.selected_idx = filtered_idx;
	g_mg.current_page = filtered_idx / MG_TILES_PER_PAGE;

	int raw_idx = g_mg.filtered_map[filtered_idx];
	const struct shell_media_item *it = &g_mg.all_items[raw_idx];

	make_card_from_item(it, raw_idx, &g_mg.player_card);
	g_mg.view = MG_VIEW_PLAYER;
	g_mg.player_is_video = it->is_video;
	g_mg.player_playing = it->is_video ? true : start_slideshow;
	g_mg.player_hud_visible = true;
	g_mg.player_hud_dirty = true;
	g_mg.player_fps = 24;
	g_mg.player_total_frames = 360;
	g_mg.player_frame_idx = 0;
	g_mg.player_elapsed_sec = 0;
	g_mg.player_duration_sec = 15;

	shell_media_audio_stop();
	if (it->is_video) {
		shell_media_get_video_info(&g_mg.player_card,
					   &g_mg.player_fps,
					   &g_mg.player_total_frames,
					   &g_mg.player_duration_sec);
		shell_media_audio_start(&g_mg.player_card);
	}
	g_mg.player_last_tick_ms = shell_now_ms();
	g_mg.player_frame_tick_ms = g_mg.player_last_tick_ms;
	g_mg.player_hud_shown_ms = g_mg.player_last_tick_ms;
	g_mg.slideshow_last_ms = g_mg.player_last_tick_ms;
}

static const int g_mg_hud_rects[3][4] = {
	{ 64,  32,  580, 40 },	/* Top-left item pill */
	{ 940, 32,  276, 40 },	/* Top-right playback / timer pill */
	{ 64,  544, 1152, 140 }	/* Bottom dock info & controls */
};

static void render_player_view(const struct shell_theme *th)
{
	bool direct_dmabuf_ok = false;
	int buf_idx = -1;
	int dst_dmabuf_fd = -1;

	if (g_mg.player_is_video) {
		buf_idx = shell_display_prepare_video_backbuf(&dst_dmabuf_fd);
		if (buf_idx >= 0 && dst_dmabuf_fd >= 0) {
			direct_dmabuf_ok = shell_media_render_video_frame_dmabuf(
				&g_mg.player_card, g_mg.player_frame_idx,
				buf_idx, dst_dmabuf_fd);
		}
		if (!direct_dmabuf_ok) {
			const uint32_t *art = shell_media_get_video_frame(
				&g_mg.player_card, g_mg.player_frame_idx);
			if (art)
				shell_draw_bitmap_xrgb(0, 0, SHELL_SCREEN_W, SHELL_SCREEN_H, art);
			else
				shell_draw_rect(0, 0, SHELL_SCREEN_W, SHELL_SCREEN_H, 0xff0c0a09U);
		} else if (!g_mg.player_hud_dirty && g_mg.player_hud_visible) {
			shell_display_present_video_backbuf(buf_idx, g_mg_hud_rects, 3);
			return;
		} else if (!g_mg.player_hud_visible) {
			shell_display_present_video_backbuf(buf_idx, NULL, 0);
			return;
		}
	} else {
		const uint32_t *art = shell_media_get_fullscreen_art(&g_mg.player_card);
		if (art)
			shell_draw_bitmap_xrgb(0, 0, SHELL_SCREEN_W, SHELL_SCREEN_H, art);
		else
			shell_draw_rect(0, 0, SHELL_SCREEN_W, SHELL_SCREEN_H, 0xff0c0a09U);
	}

	g_mg.player_hud_dirty = false;

	if (!g_mg.player_hud_visible) {
		if (direct_dmabuf_ok) {
			shell_display_present_video_backbuf(buf_idx, NULL, 0);
		} else {
			shell_display_damage_all();
			shell_display_present();
		}
		return;
	}

	int raw_idx = g_mg.filtered_map[g_mg.selected_idx];
	const struct shell_media_item *it = &g_mg.all_items[raw_idx];

	/*
	 * 1. Top-Left HUD Box (64, 32, 580, 40) — cleared to opaque dark
	 *    cinema surface first so Grid View never bleeds through!
	 */
	shell_draw_rect(64, 32, 580, 40, 0xff120f0dU);
	shell_draw_rounded_outline(64, 32, 580, 40, 10, 1, 0xff3b3530U);

	const char *b_str = it->is_video ? "VIDEO" : "PHOTO";
	int bw = shell_font_measure_role(SHELL_FONT_MONO_SEMIBOLD, 12, b_str) + 22;
	shell_draw_rounded_rect(70, 37, bw, 30, 8, th->accent);
	shell_font_draw_box_centered(SHELL_FONT_MONO_SEMIBOLD, 12,
				     70, 37, bw, 30, 0xfffaf6f0U, b_str);

	char src_buf[128];
	snprintf(src_buf, sizeof(src_buf), "Item %d of %d · %s",
		 (g_mg.selected_idx + 1) & 0xff,
		 g_mg.filtered_count & 0xff,
		 it->subtitle);
	shell_font_draw_vcentered(SHELL_FONT_SANS_SEMIBOLD, 15,
				  70 + bw + 12, 32, 40, 0xfffaf6f0U,
				  src_buf, 580 - bw - 30);

	/*
	 * 2. Top-Right HUD Box (940, 32, 276, 40) — cleared to opaque dark
	 *    cinema surface first so timer updates never ghost over old text!
	 */
	shell_draw_rect(940, 32, 276, 40, 0xff120f0dU);
	shell_draw_rounded_outline(940, 32, 276, 40, 10, 1,
				   g_mg.player_playing ? th->accent : 0xff3b3530U);

	char status_str[48];
	if (g_mg.player_is_video) {
		int em = (g_mg.player_elapsed_sec / 60) % 100;
		int es = g_mg.player_elapsed_sec % 60;
		int dm = (g_mg.player_duration_sec / 60) % 100;
		int ds = g_mg.player_duration_sec % 60;
		snprintf(status_str, sizeof(status_str), "%s  %02d:%02d / %02d:%02d",
			 g_mg.player_playing ? "PLAYING" : "PAUSED",
			 em, es, dm, ds);
	} else {
		snprintf(status_str, sizeof(status_str), "%s",
			 g_mg.player_playing ? "SLIDESHOW ON (4s)" : "1280×720 HD VIEW");
	}
	shell_font_draw_box_centered(SHELL_FONT_MONO_SEMIBOLD, 14,
				     940, 32, 276, 40, 0xfffaf6f0U, status_str);

	/*
	 * 3. Bottom Dock Panel (64, 544, 1152, 140) — cleared to opaque dark
	 *    cinema surface so video & photo overlays are 100% artifact-free.
	 */
	shell_draw_rect(64, 544, 1152, 140, 0xff120f0dU);
	shell_draw_rounded_outline(64, 544, 1152, 140, 14, 1, 0xff3b3530U);

	shell_font_draw_role(SHELL_FONT_SANS_BOLD, 22,
			     84, 556, 0xfffaf6f0U,
			     it->title, 1112);
	shell_font_draw_role(SHELL_FONT_SANS_MEDIUM, 15,
			     84, 586, 0xffd8cfc4U,
			     it->detail[0] ? it->detail : it->subtitle, 1112);

	/* Video Progress Bar */
	int ctrl_y = 632;
	if (g_mg.player_is_video) {
		int bar_x = 84;
		int bar_y = 616;
		int bar_w = 1112;
		int fill_w = (g_mg.player_duration_sec > 0)
			     ? (bar_w * g_mg.player_elapsed_sec) / g_mg.player_duration_sec
			     : 0;
		if (fill_w > bar_w)
			fill_w = bar_w;
		shell_draw_rect(bar_x, bar_y, bar_w, 5, 0xff352f2bU);
		if (fill_w > 0)
			shell_draw_rect(bar_x, bar_y, fill_w, 5, th->accent);
	}

	/* Control Pills */
	int cx = 84;
	const char *play_lbl = g_mg.player_is_video
			       ? (g_mg.player_playing ? "Pause Video" : "Resume Video")
			       : (g_mg.player_playing ? "Pause Slideshow" : "Start Slideshow");
	int pw = shell_font_measure_role(SHELL_FONT_SANS_BOLD, 14, play_lbl) + 46;
	shell_draw_rounded_rect(cx, ctrl_y, pw, 38, 19, th->accent);
	shell_draw_symbol(SHELL_SYM_PLAY, cx + 14, ctrl_y + 11, 16, 0xfffaf6f0U);
	shell_font_draw_vcentered(SHELL_FONT_SANS_BOLD, 14,
				  cx + 34, ctrl_y, 38, 0xfffaf6f0U, play_lbl, pw - 40);
	cx += pw + 12;

	const char *nav_lbl = "Left / Right: Prev / Next · Up / Down: Hide Info";
	int nw = shell_font_measure_role(SHELL_FONT_SANS_SEMIBOLD, 13, nav_lbl) + 28;
	shell_draw_rounded_rect(cx, ctrl_y, nw, 38, 19, 0xff1e1a17U);
	shell_draw_rounded_outline(cx, ctrl_y, nw, 38, 19, 1, 0xff453e38U);
	shell_font_draw_box_centered(SHELL_FONT_SANS_SEMIBOLD, 13,
				     cx, ctrl_y, nw, 38, 0xfffaf6f0U, nav_lbl);
	cx += nw + 12;

	const char *back_lbl = "Menu: Back to Grid";
	int bkw = shell_font_measure_role(SHELL_FONT_SANS_SEMIBOLD, 13, back_lbl) + 28;
	shell_draw_rounded_rect(cx, ctrl_y, bkw, 38, 19, 0xff1e1a17U);
	shell_draw_rounded_outline(cx, ctrl_y, bkw, 38, 19, 1, 0xff453e38U);
	shell_font_draw_box_centered(SHELL_FONT_SANS_SEMIBOLD, 13,
				     cx, ctrl_y, bkw, 38, 0xfffaf6f0U, back_lbl);

	if (direct_dmabuf_ok) {
		shell_display_present_video_backbuf(buf_idx, g_mg_hud_rects, 3);
	} else {
		shell_display_damage_all();
		shell_display_present();
	}
}

static void render_frame(void)
{
	const struct shell_theme *th = &shell_themes[g_mg.theme_id];

	if (g_mg.view == MG_VIEW_PLAYER)
		render_player_view(th);
	else
		render_grid_view(th);
}

/* =========================================================================
 * 3. Input Handling & Animation Tick
 * ========================================================================= */

static void move_grid_selection(int next_idx)
{
	if (g_mg.filtered_count <= 0)
		return;

	next_idx = ((next_idx % g_mg.filtered_count) + g_mg.filtered_count) %
		   g_mg.filtered_count;
	if (next_idx == g_mg.selected_idx)
		return;

	int old_page = g_mg.current_page;
	int old_slot = g_mg.selected_idx % MG_TILES_PER_PAGE;
	int new_page = next_idx / MG_TILES_PER_PAGE;
	int new_slot = next_idx % MG_TILES_PER_PAGE;

	g_mg.selected_idx = next_idx;
	g_mg.current_page = new_page;
	g_mg.header_dirty = true;
	g_mg.footer_dirty = true;

	if (new_page != old_page) {
		g_mg.dirty_tiles = 0xffU;
	} else {
		g_mg.dirty_tiles |= (uint8_t)((1U << old_slot) | (1U << new_slot));
	}
}

static void handle_key_event(const struct shell_input_event *ev)
{
	if (ev->key == SHELL_KEY_MENU_HOLD) {
		shell_media_audio_stop();
		g_running = 0;
		return;
	}

	/* View 1: Fullscreen Video Player / Photo Viewer */
	if (g_mg.view == MG_VIEW_PLAYER) {
		switch (ev->key) {
		case SHELL_KEY_MENU:
			shell_media_audio_stop();
			g_mg.view = MG_VIEW_GRID;
			mark_grid_full_redraw();
			break;
		case SHELL_KEY_PLAY_PAUSE:
		case SHELL_KEY_SELECT:
			g_mg.player_playing = !g_mg.player_playing;
			g_mg.player_hud_visible = true;
			g_mg.player_hud_dirty = true;
			g_mg.player_hud_shown_ms = shell_now_ms();
			if (g_mg.player_is_video)
				shell_media_audio_set_paused(!g_mg.player_playing);
			g_mg.player_last_tick_ms = g_mg.player_hud_shown_ms;
			g_mg.player_frame_tick_ms = g_mg.player_last_tick_ms;
			g_mg.slideshow_last_ms = g_mg.player_last_tick_ms;
			break;
		case SHELL_KEY_LEFT:
			open_player_at(g_mg.selected_idx - 1,
				       !g_mg.player_is_video && g_mg.player_playing);
			break;
		case SHELL_KEY_RIGHT:
			open_player_at(g_mg.selected_idx + 1,
				       !g_mg.player_is_video && g_mg.player_playing);
			break;
		case SHELL_KEY_UP:
			if (g_mg.player_is_video) {
				g_mg.player_elapsed_sec = (g_mg.player_elapsed_sec + 3) %
							  (g_mg.player_duration_sec > 0 ?
							   g_mg.player_duration_sec : 15);
				g_mg.player_frame_idx = (g_mg.player_elapsed_sec * g_mg.player_fps) %
							(g_mg.player_total_frames > 0 ?
							 g_mg.player_total_frames : 360);
				g_mg.player_hud_visible = true;
				g_mg.player_hud_dirty = true;
				g_mg.player_hud_shown_ms = shell_now_ms();
				shell_media_audio_seek_sec(g_mg.player_elapsed_sec);
			} else {
				g_mg.player_hud_visible = !g_mg.player_hud_visible;
				g_mg.player_hud_dirty = true;
				g_mg.player_hud_shown_ms = shell_now_ms();
			}
			break;
		case SHELL_KEY_DOWN:
			if (g_mg.player_is_video) {
				g_mg.player_elapsed_sec -= 3;
				if (g_mg.player_elapsed_sec < 0)
					g_mg.player_elapsed_sec = 0;
				g_mg.player_frame_idx = (g_mg.player_elapsed_sec * g_mg.player_fps) %
							(g_mg.player_total_frames > 0 ?
							 g_mg.player_total_frames : 360);
				g_mg.player_hud_visible = true;
				g_mg.player_hud_dirty = true;
				g_mg.player_hud_shown_ms = shell_now_ms();
				shell_media_audio_seek_sec(g_mg.player_elapsed_sec);
			} else {
				g_mg.player_hud_visible = !g_mg.player_hud_visible;
				g_mg.player_hud_dirty = true;
				g_mg.player_hud_shown_ms = shell_now_ms();
			}
			break;
		default:
			break;
		}
		return;
	}

	/* View 0: Fullscreen 4x2 Media Grid */
	if (g_mg.focus_zone == MG_ZONE_TABS) {
		switch (ev->key) {
		case SHELL_KEY_MENU:
			g_running = 0;
			break;
		case SHELL_KEY_LEFT:
			g_mg.tab_focus_idx = (g_mg.tab_focus_idx + MG_FILTER_COUNT - 1) %
					     MG_FILTER_COUNT;
			g_mg.header_dirty = true;
			break;
		case SHELL_KEY_RIGHT:
			g_mg.tab_focus_idx = (g_mg.tab_focus_idx + 1) % MG_FILTER_COUNT;
			g_mg.header_dirty = true;
			break;
		case SHELL_KEY_DOWN:
			g_mg.focus_zone = MG_ZONE_GRID;
			g_mg.header_dirty = true;
			g_mg.dirty_tiles |= (uint8_t)(1U << (g_mg.selected_idx % MG_TILES_PER_PAGE));
			break;
		case SHELL_KEY_SELECT:
			apply_filter((enum mg_filter_kind)g_mg.tab_focus_idx);
			break;
		default:
			break;
		}
		return;
	}

	switch (ev->key) {
	case SHELL_KEY_MENU:
		g_running = 0;
		break;
	case SHELL_KEY_LEFT:
		move_grid_selection(g_mg.selected_idx - 1);
		break;
	case SHELL_KEY_RIGHT:
		move_grid_selection(g_mg.selected_idx + 1);
		break;
	case SHELL_KEY_UP: {
		int slot = g_mg.selected_idx % MG_TILES_PER_PAGE;
		if (g_mg.current_page == 0 && slot < MG_TILE_COLS) {
			/* Move up into the Filter Tabs bar */
			g_mg.focus_zone = MG_ZONE_TABS;
			g_mg.tab_focus_idx = (int)g_mg.filter;
			g_mg.header_dirty = true;
			g_mg.dirty_tiles |= (uint8_t)(1U << slot);
		} else if (g_mg.selected_idx >= MG_TILE_COLS) {
			move_grid_selection(g_mg.selected_idx - MG_TILE_COLS);
		} else {
			move_grid_selection(0);
		}
		break;
	}
	case SHELL_KEY_DOWN:
		if (g_mg.selected_idx + MG_TILE_COLS < g_mg.filtered_count)
			move_grid_selection(g_mg.selected_idx + MG_TILE_COLS);
		else if (g_mg.filtered_count > 0)
			move_grid_selection(g_mg.filtered_count - 1);
		break;
	case SHELL_KEY_SELECT:
		open_player_at(g_mg.selected_idx, false);
		break;
	case SHELL_KEY_PLAY_PAUSE:
		open_player_at(g_mg.selected_idx, true);
		break;
	default:
		break;
	}
}

static bool tick_player(uint64_t now_ms)
{
	bool dirty = false;

	if (g_mg.view != MG_VIEW_PLAYER)
		return false;

	/* Auto-hide HUD overlay after 3.5 seconds of inactivity */
	if (g_mg.player_hud_visible &&
	    now_ms >= g_mg.player_hud_shown_ms &&
	    (now_ms - g_mg.player_hud_shown_ms) >= MG_HUD_AUTOHIDE_MS) {
		g_mg.player_hud_visible = false;
		g_mg.player_hud_dirty = true;
		dirty = true;
	}

	if (!g_mg.player_playing)
		return dirty;

	if (g_mg.player_is_video) {
		int fps = g_mg.player_fps > 0 ? g_mg.player_fps : 24;
		int nframes = g_mg.player_total_frames > 0 ? g_mg.player_total_frames : 360;
		int dur = g_mg.player_duration_sec > 0 ? g_mg.player_duration_sec : 15;
		uint64_t frame_ms = 1000ULL / (uint64_t)fps;

		if (now_ms - g_mg.player_frame_tick_ms >= frame_ms) {
			int aud_frame = shell_media_audio_get_frame_idx(fps, nframes);
			int prev_sec = g_mg.player_elapsed_sec;

			if (aud_frame >= 0) {
				g_mg.player_frame_idx = aud_frame;
			} else {
				uint64_t steps = (now_ms - g_mg.player_frame_tick_ms) / frame_ms;
				if (steps > 4)
					steps = 1;
				g_mg.player_frame_idx = (g_mg.player_frame_idx + (int)steps) % nframes;
			}
			g_mg.player_elapsed_sec = (g_mg.player_frame_idx / fps) % (dur + 1);
			if (g_mg.player_elapsed_sec != prev_sec)
				g_mg.player_hud_dirty = true;
			g_mg.player_frame_tick_ms = now_ms;
			return true;
		}
		return dirty;
	}

	/* Photo Slideshow: advance every 4000 ms */
	if (now_ms - g_mg.slideshow_last_ms >= 4000ULL) {
		open_player_at(g_mg.selected_idx + 1, true);
		return true;
	}
	return dirty;
}

static bool preload_next_gallery_thumbnail(void)
{
	int i;

	if (g_mg.preload_done || g_mg.all_count <= 0)
		return false;

	/* First prioritize visible tiles on the current page */
	for (i = 0; i < MG_TILES_PER_PAGE; i++) {
		int f_idx = g_mg.current_page * MG_TILES_PER_PAGE + i;
		if (f_idx >= g_mg.filtered_count)
			break;
		int raw_idx = g_mg.filtered_map[f_idx];
		struct giri_media_card mc;
		make_card_from_item(&g_mg.all_items[raw_idx], raw_idx, &mc);
		if (shell_media_preload_card_one(&mc)) {
			g_mg.dirty_tiles |= (uint8_t)(1U << i);
			return true;
		}
	}

	/* Then pre-warm all remaining items in the background */
	while (g_mg.preload_cursor < g_mg.all_count) {
		int idx = g_mg.preload_cursor++;
		struct giri_media_card mc;
		make_card_from_item(&g_mg.all_items[idx], idx, &mc);
		if (shell_media_preload_card_one(&mc))
			return true;
	}

	g_mg.preload_done = true;
	return false;
}

static void media_gallery_event_loop(void)
{
	while (g_running) {
		struct shell_input_event ev;
		int timeout_ms = (!g_mg.preload_done && g_mg.view == MG_VIEW_GRID) ? 12 : 100;
		bool dirty = false;

		if (g_mg.view == MG_VIEW_PLAYER && g_mg.player_playing) {
			if (g_mg.player_is_video) {
				int fps = g_mg.player_fps > 0 ? g_mg.player_fps : 24;
				uint64_t frame_ms = 1000ULL / (uint64_t)fps;
				uint64_t now = shell_now_ms();
				uint64_t elapsed = (now >= g_mg.player_frame_tick_ms)
						   ? (now - g_mg.player_frame_tick_ms) : 0;
				timeout_ms = (elapsed >= frame_ms) ? 0 : (int)(frame_ms - elapsed);
			} else {
				timeout_ms = 50;
			}
		}

		if (shell_input_poll(&ev, timeout_ms)) {
			handle_key_event(&ev);
			while (g_running && shell_input_poll(&ev, 0))
				handle_key_event(&ev);
			dirty = true;
		}

		if (g_running && tick_player(shell_now_ms()))
			dirty = true;

		if (g_running && !dirty && g_mg.view == MG_VIEW_GRID && !g_mg.preload_done) {
			if (preload_next_gallery_thumbnail() && g_mg.dirty_tiles)
				dirty = true;
		}

		if (g_running && dirty)
			render_frame();
	}
}

/*
 * Run Media Gallery seamlessly inside an already-initialized display/media/input
 * session (zero DRM handover, zero fbcon TUI flash, shared V4L2/thumbnail cache).
 */
int media_gallery_run(enum shell_theme_id theme_id)
{
	int i;

	g_running = 1;
	memset(&g_mg, 0, sizeof(g_mg));
	g_mg.theme_id = theme_id;
	g_mg.view = MG_VIEW_GRID;
	g_mg.focus_zone = MG_ZONE_GRID;
	g_mg.filter = MG_FILTER_ALL;
	mark_grid_full_redraw();

	scan_gallery_folder();

	/* Preload the first page of widescreen 275x164 tiles before first present */
	for (i = 0; i < MG_TILES_PER_PAGE && i < g_mg.filtered_count; i++) {
		int raw_idx = g_mg.filtered_map[i];
		struct giri_media_card mc;
		make_card_from_item(&g_mg.all_items[raw_idx], raw_idx, &mc);
		(void)shell_media_preload_card_one(&mc);
	}

	render_frame();
	media_gallery_event_loop();
	shell_media_audio_stop();
	return 0;
}

#ifdef MEDIA_GALLERY_STANDALONE
int main(int argc, char **argv)
{
	bool headless = false;
	const char *dump_ppm_path = NULL;
	int initial_open_idx = -1;
	int i;

	g_running = 1;
	memset(&g_mg, 0, sizeof(g_mg));
	g_mg.theme_id = SHELL_THEME_PAPER;
	g_mg.view = MG_VIEW_GRID;
	g_mg.focus_zone = MG_ZONE_GRID;
	g_mg.filter = MG_FILTER_ALL;
	mark_grid_full_redraw();

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--headless") == 0) {
			headless = true;
		} else if (strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
			i++;
			if (strcasecmp(argv[i], "signal") == 0)
				g_mg.theme_id = SHELL_THEME_SIGNAL;
			else
				g_mg.theme_id = SHELL_THEME_PAPER;
		} else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
			i++;
			if (strcasecmp(argv[i], "video") == 0 ||
			    strcasecmp(argv[i], "videos") == 0)
				g_mg.filter = MG_FILTER_VIDEOS;
			else if (strcasecmp(argv[i], "photo") == 0 ||
				 strcasecmp(argv[i], "photos") == 0)
				g_mg.filter = MG_FILTER_PHOTOS;
			else
				g_mg.filter = MG_FILTER_ALL;
		} else if (strcmp(argv[i], "--open") == 0 && i + 1 < argc) {
			initial_open_idx = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--dump-ppm") == 0 && i + 1 < argc) {
			dump_ppm_path = argv[++i];
			headless = true;
		}
	}

	signal(SIGINT, handle_sig);
	signal(SIGTERM, handle_sig);

	if (shell_display_init(headless) < 0)
		return 1;
	if (shell_font_init() < 0) {
		shell_display_close();
		return 1;
	}

	shell_media_init();
	shell_input_init(headless);
	scan_gallery_folder();

	/* Preload the first page of thumbnails before presenting the first frame */
	for (i = 0; i < MG_TILES_PER_PAGE && i < g_mg.filtered_count; i++) {
		int raw_idx = g_mg.filtered_map[i];
		struct giri_media_card mc;
		make_card_from_item(&g_mg.all_items[raw_idx], raw_idx, &mc);
		(void)shell_media_preload_card_one(&mc);
	}

	if (initial_open_idx >= 0 && g_mg.filtered_count > 0)
		open_player_at(initial_open_idx, false);

	render_frame();

	if (dump_ppm_path) {
		int rc = shell_display_dump_ppm(dump_ppm_path);
		shell_media_audio_stop();
		shell_input_close();
		shell_media_free();
		shell_font_free();
		shell_display_close();
		return rc == 0 ? 0 : 1;
	}

	media_gallery_event_loop();

	shell_media_audio_stop();
	shell_input_close();
	shell_media_free();
	shell_font_free();
	shell_display_close();
	return 0;
}
#endif /* MEDIA_GALLERY_STANDALONE */
