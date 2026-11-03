/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Velliangiri 1280x720 TV Shell - Core Interfaces & Data Structures
 */

#ifndef TVSHELL_H
#define TVSHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "shell_tokens.h"

#define SHELL_CONF_PATH		"/mnt/data/linux/etc/shell.conf"
#define SHELL_NOTES_DIR		"/mnt/data/linux/notes"
#define SHELL_PHOTOS_DIR	"/mnt/data/linux/media/photos"
#define SHELL_THUMBS_DIR	"/mnt/data/linux/cache/thumbs"

#define SHELL_MAX_TURNS		12
#define SHELL_MAX_STEPS		4
#define SHELL_ASK_MAX_LEN	128

enum shell_turn_state {
	SHELL_TURN_READY = 0,
	SHELL_TURN_WORKING = 1,
};

enum shell_focus_zone {
	SHELL_FOCUS_CONTENT = 0,	/* Focused on an item row or media tile */
	SHELL_FOCUS_CHOICES = 1,	/* Focused on the bottom choices row */
};

enum shell_action_id {
	SHELL_ACT_NONE = 0,
	SHELL_ACT_GOTO_TURN,
	SHELL_ACT_NEXT_IDEA,
	SHELL_ACT_OPEN_ASK,
	SHELL_ACT_LAUNCH_DOOM,
	SHELL_ACT_SWITCH_THEME,
	SHELL_ACT_REFRESH_STATUS,
	SHELL_ACT_OPEN_VIEWER,
	SHELL_ACT_SHOW_PATH,
	SHELL_ACT_READ_NOTE,
	SHELL_ACT_DEMO_WORKING,
};

struct shell_choice {
	enum shell_symbol_id symbol;
	char label[32];
	enum shell_action_id action;
	int arg;
};

struct shell_item {
	enum shell_symbol_id symbol;
	char title[64];
	char detail[48];
	char path[128];
	char body[384];
	struct shell_choice actions[3];
	int num_actions;
};

struct shell_tile {
	char title[48];
	char detail[32];
	char badge[16];		/* Non-empty for video duration (e.g. "3:12") */
	char media_path[128];
	int art_id;		/* Built-in procedural specimen artwork ID */
	uint32_t *thumb_pixels;	/* 248x140 XRGB8888 */
	struct shell_choice actions[2];
	int num_actions;
};

struct shell_step {
	bool done;
	char text[80];
};

struct shell_turn {
	char question[96];
	enum shell_turn_state state;
	int progress_pct;

	/* Answer & optional sources */
	char answer[512];
	char sources[96];

	/* Optional Value display (64px bold + 22px label) */
	char value_big[32];
	char value_label[64];

	/* Optional Working steps */
	struct shell_step steps[SHELL_MAX_STEPS];
	int num_steps;
	int demo_target_turn;

	/* Optional Item rows (<= 5) or Media tiles (<= 4) */
	struct shell_item items[SHELL_LIMIT_ITEMS];
	int num_items;

	struct shell_tile tiles[SHELL_LIMIT_TILES];
	int num_tiles;

	/* Up to 3 predefined choices (4th is automatic Ask / More ideas) */
	struct shell_choice choices[SHELL_LIMIT_LLM_CHOICES];
	int num_choices;

	/* Per-turn remembered focus */
	enum shell_focus_zone focus_zone;
	int focus_content_idx;
	int focus_choice_idx;
};

struct shell_config {
	enum shell_theme_id theme;
	bool theme_set;
	char llm_mode[16];
	char llm_url[128];
	char llm_model_path[128];
};

enum shell_event_type {
	SHELL_EVT_NONE = 0,
	SHELL_EVT_KEY_LEFT,
	SHELL_EVT_KEY_RIGHT,
	SHELL_EVT_KEY_UP,
	SHELL_EVT_KEY_DOWN,
	SHELL_EVT_KEY_OK,
	SHELL_EVT_KEY_MENU,
	SHELL_EVT_KEY_PLAY,
	SHELL_EVT_MENU_HOLD,
	SHELL_EVT_KB_ATTACHED,
	SHELL_EVT_KB_DETACHED,
	SHELL_EVT_TEXT_CHAR,
	SHELL_EVT_TEXT_BACKSPACE,
	SHELL_EVT_TEXT_ENTER,
	SHELL_EVT_TEXT_ESC,
};

struct shell_event {
	enum shell_event_type type;
	uint32_t codepoint;
};

/* shell_display.c */
int shell_display_init(bool headless);
void shell_display_close(void);
uint32_t *shell_display_backbuffer(void);
void shell_display_damage(int x, int y, int w, int h);
void shell_display_damage_all(void);
void shell_display_present(void);
void shell_display_release(void);
int shell_display_acquire(void);
int shell_display_dump_ppm(const char *path);

void shell_draw_rect(int x, int y, int w, int h, uint32_t color);
void shell_draw_rounded_rect(int x, int y, int w, int h, int radius, uint32_t color);
void shell_draw_rounded_outline(int x, int y, int w, int h, int radius, int thickness, uint32_t color);
void shell_draw_bitmap_xrgb(int x, int y, int w, int h, const uint32_t *pixels);
void shell_draw_rounded_bitmap(int x, int y, int w, int h, int radius, const uint32_t *pixels);
void shell_draw_symbol(enum shell_symbol_id sym, int x, int y, int size, uint32_t fg);

/* shell_font.c */
int shell_font_init(void);
void shell_font_free(void);
int shell_font_measure(enum shell_theme_id theme, int size_px, bool bold, const char *utf8);
int shell_font_draw(enum shell_theme_id theme, int size_px, bool bold, bool italic,
		    int x, int y, uint32_t fg, const char *utf8, int max_width);
int shell_font_draw_wrapped(enum shell_theme_id theme, int size_px, bool bold,
			    int x, int y, int line_height, int max_width, int max_lines,
			    uint32_t fg, const char *utf8);

/* shell_input.c */
int shell_input_init(void);
void shell_input_close(void);
bool shell_input_has_keyboard(void);
int shell_input_poll(struct shell_event *ev, int timeout_ms);
int shell_app_run(const char *const argv[], bool game_keys);
void shell_input_set_game_map(bool enable);

/* shell_media.c */
void shell_media_render_sample_thumb(uint32_t *dst_248x140, int art_id);
void shell_media_render_sample_viewer(uint32_t *dst_1280x720, int art_id, uint32_t ground);
int shell_media_load_jpeg_thumb(const char *jpeg_path, uint32_t *dst_248x140);
int shell_media_load_jpeg_viewer(const char *jpeg_path, uint32_t *dst_1280x720, uint32_t ground);

#endif /* TVSHELL_H */
