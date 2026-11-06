/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Giri on Velliangiri OS — 1280x720 TV Shell (Design System & Schema v1.2)
 */

#ifndef TVSHELL_H
#define TVSHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "shell_tokens.h"

/* Typography roles (Plus Jakarta Sans 500/600/700 + JetBrains Mono 500/600) */
enum shell_font_role {
	SHELL_FONT_SANS_MEDIUM = 0,
	SHELL_FONT_SANS_SEMIBOLD,
	SHELL_FONT_SANS_BOLD,
	SHELL_FONT_MONO_MEDIUM,
	SHELL_FONT_MONO_SEMIBOLD,
	SHELL_FONT_ROLE_COUNT
};

/* 8 Built-in vector/procedural artwork presets from prototype.html */
enum giri_preset_art {
	GIRI_ART_DAWN_RIDGE = 0,
	GIRI_ART_WEATHER_SKY,
	GIRI_ART_MONSOON_VIDEO,
	GIRI_ART_WATERFALL_VIDEO,
	GIRI_ART_SHOLA_MIST,
	GIRI_ART_DOOM_GAME,
	GIRI_ART_APP_CHESS,
	GIRI_ART_APP_RADIO,
	GIRI_ART_COUNT
};

/* Media card visual kind */
enum giri_card_kind {
	GIRI_CARD_PAGE_IMAGE = 0,
	GIRI_CARD_PAGE_VIDEO,
	GIRI_CARD_LOCAL_PHOTO,
	GIRI_CARD_LOCAL_VIDEO,
	GIRI_CARD_APP_ICON,
};

/* OS Capability Action (tool.json v1.0 & screen.schema.json v1.2) */
struct giri_action {
	char type[24];		/* ask_giri, open_media, launch_app, show_screen, open_prompt_box, open_settings */
	char capability[36];	/* internet.search_and_extract, media.get_library, apps.launch, apps.list_and_install, ui.open_prompt, system.open_settings */
	char query[96];
	char kind[24];
	char app_id[32];
};

/* Slot 2: Editorial Summary Box (1152 x 148 px) */
struct giri_summary {
	char title[96];
	char text[320];
	char source_caption[64];
};

/* Slot 3: Full-Bleed Visual Media Card (274.5 x 312 px) */
struct giri_media_card {
	char id[40];
	char title[48];
	char subtitle[64];
	char badge[16];
	char meta_pill[16];
	char action_label[24];
	enum giri_card_kind kind;
	enum giri_preset_art preset_art;
	char uri[256];
	char detail_text[384];
	struct giri_action action;
};

/* Slot 4: Bottom Bar Button (4 buttons: 188px 1fr 1fr 188px) */
struct giri_footer_link {
	char id[40];
	char label[64];
	enum shell_symbol_id symbol;
	struct giri_action action;
};

/* Complete v1.2 Screen Payload */
struct giri_screen {
	char schema_version[8];
	char screen_id[48];
	char page_type[24];
	char prompt_title[80];
	struct giri_summary summary;
	struct giri_media_card media_cards[SHELL_CARD_COUNT];
	struct giri_footer_link footer_links[SHELL_FOOTER_COUNT];
};

/* On-device AI Model Catalog Entry (supported_models.json) */
#define GIRI_MAX_MODELS		8

struct giri_model_info {
	char id[32];
	char name[40];
	char family[32];
	char params[16];
	char quantization[16];
	char size_label[24];
	char description[96];
	bool default_model;
	bool installed;
	bool downloading;
	int progress;
};

/* On-device Application Catalog Entry (apps.json) */
#define GIRI_MAX_APPS		8

struct giri_app_info {
	char id[32];
	char name[40];
	char subtitle[64];
	char badge[16];
	char meta_pill[16];
	char version[16];
	char size_label[24];
	char binary_path[128];
	enum giri_preset_art preset_art;
	bool installed;
	bool installing;
	int progress;
	char description[280];
};

/* 4-Window Composited UI System */
enum giri_window_id {
	GIRI_WIN_MAIN = 0,		/* Window 0: 4-Slot Main TV Screen (.tv-screen) */
	GIRI_WIN_MEDIA_OVERLAY,		/* Window 1: 1280x720 Fullscreen Media / Detail Viewer (.media-overlay) */
	GIRI_WIN_ASK_MODAL,		/* Window 2: Ask Giri Prompt Modal (#inputModal) */
	GIRI_WIN_SETTINGS_MODAL,	/* Window 3: Velliangiri OS Settings Modal (#settingsModal) */
};

/* Main screen focus zone */
enum giri_focus_zone {
	GIRI_ZONE_CARDS = 0,		/* Slot 3: 4 Media Cards (0..3) */
	GIRI_ZONE_FOOTER = 1,		/* Slot 4: 4 Bottom Bar Buttons (0..3) */
};

/* Remote & Keyboard Input Events */
enum shell_key {
	SHELL_KEY_NONE = 0,
	SHELL_KEY_UP,
	SHELL_KEY_DOWN,
	SHELL_KEY_LEFT,
	SHELL_KEY_RIGHT,
	SHELL_KEY_SELECT,
	SHELL_KEY_MENU,
	SHELL_KEY_MENU_HOLD,
	SHELL_KEY_PLAY_PAUSE,
	SHELL_KEY_BACKSPACE,
	SHELL_KEY_CHAR,
};

struct shell_input_event {
	enum shell_key key;
	char ch;
};

/* shell_display.c */
int shell_display_init(bool headless);
void shell_display_close(void);
uint32_t *shell_display_backbuffer(void);
void shell_display_damage(int x, int y, int w, int h);
void shell_display_damage_all(void);
void shell_display_present(void);
int shell_display_get_video_dmabuf(int buf_idx);
int shell_display_prepare_video_backbuf(int *out_dmabuf_fd);
void shell_display_present_video_backbuf(int buf_idx,
					 const int (*hud_rects)[4],
					 int num_hud_rects);
void shell_display_release(void);
int shell_display_acquire(void);
int shell_display_dump_ppm(const char *path);

void shell_draw_rect(int x, int y, int w, int h, uint32_t color);
void shell_draw_rect_alpha(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
void shell_draw_rounded_rect(int x, int y, int w, int h, int radius, uint32_t color);
void shell_draw_rounded_rect_alpha(int x, int y, int w, int h, int radius,
				   uint32_t color, uint8_t alpha);
void shell_draw_rounded_outline(int x, int y, int w, int h, int radius,
				int thickness, uint32_t color);
void shell_draw_focus_glow(int x, int y, int w, int h, int radius,
			   int spread, uint32_t glow_color, uint8_t max_alpha);
void shell_draw_card_shadow(int x, int y, int w, int h, int radius);
void shell_draw_card_bottom_scrim(int card_x, int card_y, int card_w, int card_h,
				  int radius, int scrim_h);
void shell_draw_top_scrim(int w, int h);
void shell_draw_bottom_scrim(int y0, int w, int h);
void shell_draw_shimmer_bar(int x, int y, int w, int h, int radius,
			    uint32_t base_color, uint32_t highlight_color, int phase_pct);
void shell_draw_bitmap_xrgb(int x, int y, int w, int h, const uint32_t *pixels);
void shell_draw_rounded_bitmap(int x, int y, int w, int h, int radius,
			       const uint32_t *pixels, bool zoom_slightly);
void shell_draw_rounded_bitmap_stride(int x, int y, int w, int h, int stride,
				      int radius, const uint32_t *pixels,
				      bool zoom_slightly);
void shell_draw_circle_alpha(int cx, int cy, int r, uint32_t color, uint8_t alpha);
void shell_draw_symbol(enum shell_symbol_id sym, int x, int y, int size, uint32_t fg);

/* shell_font.c */
int shell_font_init(void);
void shell_font_free(void);
int shell_font_cap_height(enum shell_font_role role, int size_px);
int shell_font_measure_role(enum shell_font_role role, int size_px, const char *utf8);
int shell_font_draw_role(enum shell_font_role role, int size_px,
			 int x, int y, uint32_t fg, const char *utf8, int max_width);
int shell_font_draw_vcentered(enum shell_font_role role, int size_px,
			      int x, int box_y, int box_h,
			      uint32_t fg, const char *utf8, int max_width);
int shell_font_draw_box_centered(enum shell_font_role role, int size_px,
				 int box_x, int box_y, int box_w, int box_h,
				 uint32_t fg, const char *utf8);
int shell_font_draw_wrapped_role(enum shell_font_role role, int size_px,
				 int x, int y, int line_height, int max_width,
				 int max_lines, uint32_t fg, const char *utf8);
int shell_font_draw_summary_body(int x, int y, int line_height, int max_width,
				 int max_lines, uint32_t fg, uint32_t highlight_color,
				 uint8_t highlight_alpha, const char *utf8,
				 int active_sentence_idx, int *total_sentences_out);
void shell_font_fit_clean_phrase(const char *src, char *dst, size_t dst_sz, int max_chars);
void shell_font_fit_short_summary(const char *src, char *dst, size_t dst_sz, int max_chars);

/* shell_media.c */
#define SHELL_MAX_GALLERY_ITEMS	64

struct shell_media_item {
	char path[256];
	char title[48];
	char subtitle[64];
	char badge[16];
	char meta_pill[16];
	char detail[256];
	bool is_video;
};

void shell_media_init(void);
void shell_media_free(void);
const uint32_t *shell_media_get_card_art(const struct giri_media_card *card,
					 int w, int h);
const uint32_t *shell_media_get_card_art_fast(const struct giri_media_card *card,
					      int w, int h, bool *is_final_out);
bool shell_media_preload_card_one(const struct giri_media_card *card);
bool shell_media_preload_next_card(const struct giri_screen *scr);
void shell_media_release_hw(void);
const uint32_t *shell_media_get_fullscreen_art(const struct giri_media_card *card);
void shell_media_get_video_info(const struct giri_media_card *card,
				int *out_fps, int *out_frame_count,
				int *out_duration_sec);
const uint32_t *shell_media_get_video_frame(const struct giri_media_card *card,
					    int frame_idx);
bool shell_media_render_video_frame_dmabuf(const struct giri_media_card *card,
					   int frame_idx, int buf_idx,
					   int dst_dmabuf_fd);
void shell_media_audio_start(const struct giri_media_card *card);
void shell_media_audio_stop(void);
void shell_media_audio_set_paused(bool paused);
void shell_media_audio_seek_sec(int elapsed_sec);
int shell_media_audio_get_frame_idx(int fps, int total_frames);
bool shell_media_poll_async(void);
/* "Licence · Wikimedia Commons" for a downloaded commons: card image. */
bool shell_media_get_card_credit(const struct giri_media_card *card, char *buf, size_t sz);
int shell_media_scan_device(struct shell_media_item *items, int max_items, bool video_only);

/* shell_input.c */
int shell_input_init(bool headless);
void shell_input_close(void);
bool shell_input_poll(struct shell_input_event *ev, int timeout_ms);
int shell_app_run(const char *const argv[], bool game_keys);
uint64_t shell_now_ms(void);

/* media_gallery.c */
int media_gallery_run(enum shell_theme_id theme_id);
int media_gallery_main(int argc, char **argv);

/* ------------------------------------------------------------------
 * shell_llm.c: Giri LLM backend (shell.conf LLM_* keys)
 * ------------------------------------------------------------------ */
enum shell_llm_mode {
	SHELL_LLM_DEVICE = 0,	/* on-device model (built-in responses for now) */
	SHELL_LLM_REMOTE,	/* OpenAI-compatible server on the LAN */
};

struct shell_llm_config {
	enum shell_llm_mode mode;
	char url[160];		/* http://host[:port][/prefix] */
	char model[64];
	char api_key[128];
	int timeout_s;
	bool json_schema;	/* send response_format json_schema */
	char device_model[160];
};

#define SHELL_LLM_CARDS	4

struct shell_llm_card {
	char title[64];
	char subtitle[96];
	char badge[24];
	char image[96];		/* image search phrase */
};

struct shell_llm_result {
	bool ok;
	char title[160];
	char summary[400];
	char next[2][96];
	struct shell_llm_card cards[SHELL_LLM_CARDS];
	char error[160];
	int elapsed_ms;
};

const char *shell_llm_config_path(void);
void shell_llm_config_load(struct shell_llm_config *cfg);
void shell_llm_describe(const struct shell_llm_config *cfg, char *buf, size_t sz);
/* Start a request on a worker thread; a new request supersedes the old one. */
int shell_llm_start(const struct shell_llm_config *cfg, const char *query);
/* Non-blocking: true once the current request has finished (ok or error). */
bool shell_llm_poll(struct shell_llm_result *out);
void shell_llm_cancel(void);

#endif /* TVSHELL_H */
