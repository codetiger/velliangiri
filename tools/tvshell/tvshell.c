// SPDX-License-Identifier: GPL-2.0-only
/*
 * Giri on Velliangiri OS — 1280x720 TV Shell & 4-Window Compositor (Schema v1.2)
 *
 * Implements the exact pixel-by-pixel v1.2 TV Design System:
 *   - Window 0: 4-Slot Fixed-Length Main TV Screen (.tv-screen)
 *   - Window 1: 1280x720 Fullscreen Edge-to-Edge Media / Detail Viewer (.media-overlay)
 *   - Window 2: Ask Giri Prompt & Suggested Topics Modal (#inputModal)
 *   - Window 3: Velliangiri OS Settings & On-Device AI Models Modal (#settingsModal)
 *
 * Loads on-device JSON catalogs (screen.json, supported_models.json, apps.json)
 * from /etc/tvshell/, /usr/share/tvshell/, or tools/tvshell/design/ with
 * compiled-in fallbacks so the shell works completely standalone offline.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

#include "tvshell.h"
#include "shell_assets.h"

#define HISTORY_MAX		12
#define QUICK_PROMPTS_COUNT	7

/* Ping-pong 5-frame logo animation steps at 65ms per step */
static const int logo_pingpong_steps[8] = { 0, 1, 2, 3, 4, 3, 2, 1 };

struct quick_prompt {
	const char *label;
	const char *query;
};

static const struct quick_prompt quick_prompts[QUICK_PROMPTS_COUNT] = {
	{ "What is top in the news on BBC?", "What is top in the news on BBC?" },
	{ "BBC Science & Space News", "What is top in BBC Science news?" },
	{ "What is the weather right now?", "What is the weather right now?" },
	{ "Search widescreen videos", "Search widescreen videos and streams" },
	{ "Tell me about the Western Ghats", "Tell me about the Western Ghats" },
	{ "Open Media Gallery", "Open Media Gallery" },
	{ "Play Doom", "Play Doom" },
};

/* Global shell state */
static struct {
	enum shell_theme_id theme_id;
	enum giri_window_id active_win;
	enum giri_focus_zone focus_zone;
	int card_focus_idx;
	int footer_focus_idx;

	/* Screen history stack */
	struct giri_screen history[HISTORY_MAX];
	int history_len;
	int history_idx;

	/* Working / Stage-1 progressive shimmer state */
	bool is_working;
	uint64_t working_start_ms;
	uint64_t last_logo_step_ms;
	int logo_step_cursor;
	int logo_frame_idx;
	char pending_query[96];
	bool has_pending_screen;
	struct giri_screen pending_screen;

	/* Incremental damage & non-blocking card art preload state */
	bool allow_async_art;
	bool cards_need_preload;
	bool main_bg_valid;
	uint8_t dirty_slots;	/* bit 0: Slot 1 header, bit 1: Slot 2 summary */
	uint8_t dirty_cards;	/* bits 0..3: Slot 3 cards 0..3 */
	uint8_t dirty_footer;	/* bits 0..3: Slot 4 buttons 0..3 */
	int backbuf_modal_win;
	int backbuf_modal_theme;
	int backbuf_modal_hist_idx;

	/* Summary read-aloud (Play/Pause button) state */
	bool tts_speaking;
	int tts_sentence_idx;
	int tts_total_sentences;
	uint64_t tts_last_step_ms;

	/* Window 1: Fullscreen Media / Detail Overlay state */
	char overlay_badge[24];
	char overlay_source[64];
	char overlay_title[96];
	char overlay_text[384];
	struct giri_media_card overlay_card;
	bool overlay_is_video;
	bool overlay_playing;
	bool overlay_hud_dirty;
	int overlay_fps;
	int overlay_total_frames;
	int overlay_elapsed_sec;
	int overlay_duration_sec;
	int overlay_frame_idx;
	int overlay_gallery_idx;
	uint64_t overlay_last_tick_ms;
	uint64_t overlay_frame_tick_ms;

	/* On-device Gallery Media state (research/gallery) */
	struct shell_media_item gallery_items[SHELL_MAX_GALLERY_ITEMS];
	int gallery_count;
	int gallery_page;

	/* Window 2: Ask Giri Modal state */
	int ask_focus_idx;	/* 0 = input/submit, 1..7 = chips, 8 = Close */
	char ask_input[96];
	int ask_input_len;

	/* Window 3: Velliangiri OS Settings Modal state */
	int settings_focus_idx;	/* 0..2 = prefs, 3..8 = models, 9 = Close */
	bool auto_read_aloud;
	bool nav_sound_enabled;
	struct giri_model_info models[GIRI_MAX_MODELS];
	int model_count;
	int active_model_idx;
	uint64_t model_dl_tick_ms;

	/* On-device App Catalog (apps.json) */
	struct giri_app_info apps[GIRI_MAX_APPS];
	int app_count;

	/* Live system metrics for Settings strip */
	char sys_temp[24];
	char sys_storage[28];
	char sys_memory[24];
	char sys_network[24];
	char sys_display[24];
} g_ui;

static volatile sig_atomic_t g_running = 1;

static void handle_sig(int sig)
{
	(void)sig;
	g_running = 0;
}

/* =========================================================================
 * 1. Minimal Clean-Room JSON Helpers & On-Device Catalog Loaders
 * ========================================================================= */

static char *read_file_text(const char *path)
{
	FILE *fp = fopen(path, "rb");
	long len;
	char *buf;

	if (!fp)
		return NULL;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return NULL;
	}
	len = ftell(fp);
	if (len <= 0 || len > 256 * 1024) {
		fclose(fp);
		return NULL;
	}
	rewind(fp);
	buf = malloc((size_t)len + 1);
	if (!buf) {
		fclose(fp);
		return NULL;
	}
	if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
		free(buf);
		fclose(fp);
		return NULL;
	}
	buf[len] = '\0';
	fclose(fp);
	return buf;
}

static char *read_catalog_file(const char *filename)
{
	char path[256];
	char *txt;

	snprintf(path, sizeof(path), "/etc/tvshell/%s", filename);
	txt = read_file_text(path);
	if (txt)
		return txt;

	snprintf(path, sizeof(path), "/usr/share/tvshell/%s", filename);
	txt = read_file_text(path);
	if (txt)
		return txt;

	snprintf(path, sizeof(path), "tools/tvshell/design/%s", filename);
	return read_file_text(path);
}

/* Extract a JSON string value for `"key": "value"` within [start, end) */
static bool json_extract_str(const char *start, const char *end,
			     const char *key, char *out, size_t out_sz)
{
	char pat[64];
	const char *p = start;
	size_t pat_len;

	if (!start || !end || !key || !out || out_sz == 0)
		return false;
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	pat_len = strlen(pat);

	while (p < end) {
		const char *hit = strstr(p, pat);

		if (!hit || hit >= end)
			return false;
		p = hit + pat_len;
		while (p < end && isspace((unsigned char)*p))
			p++;
		if (p >= end || *p != ':')
			continue;
		p++;
		while (p < end && isspace((unsigned char)*p))
			p++;
		if (p >= end || *p != '"')
			continue;
		p++;

		size_t w = 0;
		while (p < end && *p != '"') {
			if (*p == '\\' && p + 1 < end) {
				p++;
				if (*p == 'n' || *p == 'r' || *p == 't') {
					if (w + 1 < out_sz)
						out[w++] = ' ';
					p++;
					continue;
				}
			}
			if (w + 1 < out_sz)
				out[w++] = *p;
			p++;
		}
		out[w] = '\0';
		return true;
	}
	return false;
}

static bool json_extract_bool(const char *start, const char *end,
			      const char *key, bool *out)
{
	char pat[64];
	const char *p = start;
	size_t pat_len;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	pat_len = strlen(pat);
	while (p < end) {
		const char *hit = strstr(p, pat);

		if (!hit || hit >= end)
			return false;
		p = hit + pat_len;
		while (p < end && isspace((unsigned char)*p))
			p++;
		if (p >= end || *p != ':')
			continue;
		p++;
		while (p < end && isspace((unsigned char)*p))
			p++;
		if (end - p >= 4 && strncmp(p, "true", 4) == 0) {
			*out = true;
			return true;
		}
		if (end - p >= 5 && strncmp(p, "false", 5) == 0) {
			*out = false;
			return true;
		}
	}
	return false;
}

/* Find matching closing brace/bracket starting at open_pos */
static const char *json_find_matching(const char *open_pos, const char *limit,
				      char open_ch, char close_ch)
{
	int depth = 0;
	bool in_str = false;
	const char *p;

	for (p = open_pos; p < limit && *p; p++) {
		if (*p == '"' && (p == open_pos || p[-1] != '\\'))
			in_str = !in_str;
		else if (!in_str) {
			if (*p == open_ch)
				depth++;
			else if (*p == close_ch) {
				depth--;
				if (depth == 0)
					return p;
			}
		}
	}
	return NULL;
}

static enum giri_preset_art parse_preset_art(const char *name)
{
	if (!name || !name[0])
		return GIRI_ART_DAWN_RIDGE;
	if (strcmp(name, "weather_sky") == 0)
		return GIRI_ART_WEATHER_SKY;
	if (strcmp(name, "monsoon_video") == 0)
		return GIRI_ART_MONSOON_VIDEO;
	if (strcmp(name, "waterfall_video") == 0)
		return GIRI_ART_WATERFALL_VIDEO;
	if (strcmp(name, "shola_mist") == 0)
		return GIRI_ART_SHOLA_MIST;
	if (strcmp(name, "doom_game") == 0)
		return GIRI_ART_DOOM_GAME;
	if (strcmp(name, "app_chess") == 0)
		return GIRI_ART_APP_CHESS;
	if (strcmp(name, "app_radio") == 0)
		return GIRI_ART_APP_RADIO;
	return GIRI_ART_DAWN_RIDGE;
}

static enum giri_card_kind parse_card_kind(const char *name)
{
	if (!name)
		return GIRI_CARD_PAGE_IMAGE;
	if (strcmp(name, "page_video") == 0)
		return GIRI_CARD_PAGE_VIDEO;
	if (strcmp(name, "local_photo") == 0)
		return GIRI_CARD_LOCAL_PHOTO;
	if (strcmp(name, "local_video") == 0)
		return GIRI_CARD_LOCAL_VIDEO;
	if (strcmp(name, "app_icon") == 0)
		return GIRI_CARD_APP_ICON;
	return GIRI_CARD_PAGE_IMAGE;
}

static enum shell_symbol_id parse_symbol_id(const char *name)
{
	if (!name)
		return SHELL_SYM_GLOBE;
	if (strcmp(name, "check") == 0)
		return SHELL_SYM_CHECK;
	if (strcmp(name, "folder") == 0)
		return SHELL_SYM_FOLDER;
	if (strcmp(name, "game") == 0)
		return SHELL_SYM_GAME;
	if (strcmp(name, "globe") == 0)
		return SHELL_SYM_GLOBE;
	if (strcmp(name, "image") == 0)
		return SHELL_SYM_IMAGE;
	if (strcmp(name, "keyboard") == 0)
		return SHELL_SYM_KEYBOARD;
	if (strcmp(name, "page") == 0)
		return SHELL_SYM_PAGE;
	if (strcmp(name, "play") == 0)
		return SHELL_SYM_PLAY;
	if (strcmp(name, "settings") == 0)
		return SHELL_SYM_SETTINGS;
	if (strcmp(name, "spark") == 0)
		return SHELL_SYM_SPARK;
	if (strcmp(name, "stream") == 0)
		return SHELL_SYM_STREAM;
	return SHELL_SYM_GLOBE;
}

static void derive_card_action_label(struct giri_media_card *mc)
{
	if (strcmp(mc->action.type, "play_video") == 0 ||
	    mc->kind == GIRI_CARD_PAGE_VIDEO ||
	    mc->kind == GIRI_CARD_LOCAL_VIDEO)
		snprintf(mc->action_label, sizeof(mc->action_label), "Play Video");
	else if (strcmp(mc->action.type, "open_image") == 0 ||
		 mc->kind == GIRI_CARD_LOCAL_PHOTO)
		snprintf(mc->action_label, sizeof(mc->action_label), "View Photo");
	else if (strcmp(mc->action.type, "launch_app") == 0)
		snprintf(mc->action_label, sizeof(mc->action_label), "Launch App");
	else if (strcmp(mc->action.type, "install_app") == 0)
		snprintf(mc->action_label, sizeof(mc->action_label), "Install App");
	else
		snprintf(mc->action_label, sizeof(mc->action_label), "Open");
}

/* Parse a screen.json v1.2 payload into struct giri_screen */
static bool parse_screen_json(const char *json, struct giri_screen *scr)
{
	const char *end;
	const char *arr_pos;

	if (!json || !scr)
		return false;
	end = json + strlen(json);
	memset(scr, 0, sizeof(*scr));

	(void)json_extract_str(json, end, "schema_version",
			       scr->schema_version, sizeof(scr->schema_version));
	(void)json_extract_str(json, end, "screen_id",
			       scr->screen_id, sizeof(scr->screen_id));
	(void)json_extract_str(json, end, "page_type",
			       scr->page_type, sizeof(scr->page_type));
	(void)json_extract_str(json, end, "prompt_title",
			       scr->prompt_title, sizeof(scr->prompt_title));

	/* Summary object */
	const char *sum_key = strstr(json, "\"summary\"");
	if (sum_key) {
		const char *b0 = strchr(sum_key, '{');
		const char *b1 = b0 ? json_find_matching(b0, end, '{', '}') : NULL;

		if (b0 && b1) {
			(void)json_extract_str(b0, b1, "title",
					       scr->summary.title, sizeof(scr->summary.title));
			(void)json_extract_str(b0, b1, "text",
					       scr->summary.text, sizeof(scr->summary.text));
			(void)json_extract_str(b0, b1, "source_caption",
					       scr->summary.source_caption,
					       sizeof(scr->summary.source_caption));
		}
	}

	/* Media cards array (4 cards) */
	arr_pos = strstr(json, "\"media_cards\"");
	if (arr_pos) {
		const char *a0 = strchr(arr_pos, '[');
		const char *a1 = a0 ? json_find_matching(a0, end, '[', ']') : NULL;

		if (a0 && a1) {
			const char *cur = a0 + 1;
			int idx = 0;

			while (cur < a1 && idx < SHELL_CARD_COUNT) {
				const char *c0 = strchr(cur, '{');
				if (!c0 || c0 >= a1)
					break;
				const char *c1 = json_find_matching(c0, a1, '{', '}');
				if (!c1)
					break;

				struct giri_media_card *mc = &scr->media_cards[idx++];
				char kind_str[24] = "";
				char preset_str[24] = "";

				(void)json_extract_str(c0, c1, "id", mc->id, sizeof(mc->id));
				(void)json_extract_str(c0, c1, "title", mc->title, sizeof(mc->title));
				(void)json_extract_str(c0, c1, "subtitle", mc->subtitle, sizeof(mc->subtitle));
				(void)json_extract_str(c0, c1, "badge", mc->badge, sizeof(mc->badge));
				(void)json_extract_str(c0, c1, "meta_pill", mc->meta_pill, sizeof(mc->meta_pill));
				(void)json_extract_str(c0, c1, "kind", kind_str, sizeof(kind_str));
				(void)json_extract_str(c0, c1, "uri", mc->uri, sizeof(mc->uri));
				(void)json_extract_str(c0, c1, "preset_art", preset_str, sizeof(preset_str));
				mc->kind = parse_card_kind(kind_str);
				mc->preset_art = parse_preset_art(preset_str);

				const char *act_key = strstr(c0, "\"action\"");
				if (act_key && act_key < c1) {
					const char *ac0 = strchr(act_key, '{');
					const char *ac1 = ac0 ? json_find_matching(ac0, c1, '{', '}') : NULL;

					if (ac0 && ac1) {
						(void)json_extract_str(ac0, ac1, "type",
								       mc->action.type, sizeof(mc->action.type));
						(void)json_extract_str(ac0, ac1, "capability",
								       mc->action.capability, sizeof(mc->action.capability));
						(void)json_extract_str(ac0, ac1, "query",
								       mc->action.query, sizeof(mc->action.query));
						(void)json_extract_str(ac0, ac1, "kind",
								       mc->action.kind, sizeof(mc->action.kind));
						(void)json_extract_str(ac0, ac1, "app_id",
								       mc->action.app_id, sizeof(mc->action.app_id));
					}
				}
				derive_card_action_label(mc);
				cur = c1 + 1;
			}
		}
	}

	/* Footer links array (4 buttons) */
	arr_pos = strstr(json, "\"footer_links\"");
	if (arr_pos) {
		const char *a0 = strchr(arr_pos, '[');
		const char *a1 = a0 ? json_find_matching(a0, end, '[', ']') : NULL;

		if (a0 && a1) {
			const char *cur = a0 + 1;
			int idx = 0;

			while (cur < a1 && idx < SHELL_FOOTER_COUNT) {
				const char *c0 = strchr(cur, '{');
				if (!c0 || c0 >= a1)
					break;
				const char *c1 = json_find_matching(c0, a1, '{', '}');
				if (!c1)
					break;

				struct giri_footer_link *fl = &scr->footer_links[idx++];
				char sym_str[24] = "";

				(void)json_extract_str(c0, c1, "id", fl->id, sizeof(fl->id));
				(void)json_extract_str(c0, c1, "label", fl->label, sizeof(fl->label));
				(void)json_extract_str(c0, c1, "symbol", sym_str, sizeof(sym_str));
				fl->symbol = parse_symbol_id(sym_str);

				const char *act_key = strstr(c0, "\"action\"");
				if (act_key && act_key < c1) {
					const char *ac0 = strchr(act_key, '{');
					const char *ac1 = ac0 ? json_find_matching(ac0, c1, '{', '}') : NULL;

					if (ac0 && ac1) {
						(void)json_extract_str(ac0, ac1, "type",
								       fl->action.type, sizeof(fl->action.type));
						(void)json_extract_str(ac0, ac1, "capability",
								       fl->action.capability, sizeof(fl->action.capability));
						(void)json_extract_str(ac0, ac1, "query",
								       fl->action.query, sizeof(fl->action.query));
						(void)json_extract_str(ac0, ac1, "kind",
								       fl->action.kind, sizeof(fl->action.kind));
					}
				}
				cur = c1 + 1;
			}
		}
	}

	return scr->summary.title[0] != '\0';
}

/* Load supported_models.json into g_ui.models */
static void load_supported_models(void)
{
	static const struct giri_model_info fallback_models[6] = {
		{
			.id = "gemma4:e4b",
			.name = "Gemma 4 E4B",
			.family = "Gemma 4",
			.params = "4B",
			.quantization = "Q4_K_M",
			.size_label = "3.1 GB",
			.description = "Default Velliangiri OS edge model for fast TV summaries",
			.default_model = true,
			.installed = true,
			.progress = 100,
		},
		{
			.id = "gemma3:12b",
			.name = "Gemma 3 12B",
			.family = "Gemma 3",
			.params = "12B",
			.quantization = "Q4_K_M",
			.size_label = "8.1 GB",
			.description = "High-capacity model for deep editorial summarization",
			.default_model = false,
			.installed = true,
			.progress = 100,
		},
		{
			.id = "gemma3:4b",
			.name = "Gemma 3 4B",
			.family = "Gemma 3",
			.params = "4B",
			.quantization = "Q4_K_M",
			.size_label = "3.3 GB",
			.description = "Compact multimodal-ready model with low latency",
			.default_model = false,
			.installed = false,
			.progress = 0,
		},
		{
			.id = "qwen2.5:3b",
			.name = "Qwen 2.5 3B",
			.family = "Qwen 2.5",
			.params = "3B",
			.quantization = "Q4_K_M",
			.size_label = "1.9 GB",
			.description = "Fast instruction model tuned for strict JSON output",
			.default_model = false,
			.installed = false,
			.progress = 0,
		},
		{
			.id = "llama3.2:3b",
			.name = "Llama 3.2 3B",
			.family = "Llama 3.2",
			.params = "3B",
			.quantization = "Q4_K_M",
			.size_label = "2.0 GB",
			.description = "Balanced compact model for news and topic overviews",
			.default_model = false,
			.installed = false,
			.progress = 0,
		},
		{
			.id = "gemma3:1b",
			.name = "Gemma 3 1B",
			.family = "Gemma 3",
			.params = "1B",
			.quantization = "Q4_K_M",
			.size_label = "815 MB",
			.description = "Ultra-lightweight 1B model for instant responses",
			.default_model = false,
			.installed = false,
			.progress = 0,
		},
	};

	memcpy(g_ui.models, fallback_models, sizeof(fallback_models));
	g_ui.model_count = 6;
	g_ui.active_model_idx = 0;

	char *json = read_catalog_file("supported_models.json");
	if (!json)
		return;

	const char *end = json + strlen(json);
	const char *arr = strstr(json, "\"models\"");
	if (arr) {
		const char *a0 = strchr(arr, '[');
		const char *a1 = a0 ? json_find_matching(a0, end, '[', ']') : NULL;

		if (a0 && a1) {
			const char *cur = a0 + 1;
			int idx = 0;

			while (cur < a1 && idx < GIRI_MAX_MODELS) {
				const char *c0 = strchr(cur, '{');
				if (!c0 || c0 >= a1)
					break;
				const char *c1 = json_find_matching(c0, a1, '{', '}');
				if (!c1)
					break;

				struct giri_model_info *m = &g_ui.models[idx];
				bool rec = false;

				memset(m, 0, sizeof(*m));
				(void)json_extract_str(c0, c1, "id", m->id, sizeof(m->id));
				(void)json_extract_str(c0, c1, "name", m->name, sizeof(m->name));
				(void)json_extract_str(c0, c1, "family", m->family, sizeof(m->family));
				(void)json_extract_str(c0, c1, "params", m->params, sizeof(m->params));
				(void)json_extract_str(c0, c1, "quantization", m->quantization, sizeof(m->quantization));
				(void)json_extract_str(c0, c1, "size", m->size_label, sizeof(m->size_label));
				(void)json_extract_str(c0, c1, "description", m->description, sizeof(m->description));
				(void)json_extract_bool(c0, c1, "recommended", &rec);

				m->default_model = rec;
				/* First two models (gemma4:e4b, gemma3:12b) installed by default */
				m->installed = (idx < 2) || rec;
				m->progress = m->installed ? 100 : 0;
				idx++;
				cur = c1 + 1;
			}
			if (idx > 0)
				g_ui.model_count = idx;
		}
	}
	free(json);
}

/* Load apps.json into g_ui.apps */
static void load_apps_catalog(void)
{
	static const struct giri_app_info fallback_apps[4] = {
		{
			.id = "gallery",
			.name = "Media Gallery",
			.subtitle = "Photos & 720p Videos on Device",
			.badge = "APP",
			.meta_pill = "OPEN",
			.version = "1.2",
			.size_label = "640 KB",
			.binary_path = "/bin/media-gallery",
			.preset_art = GIRI_ART_MONSOON_VIDEO,
			.installed = true,
			.progress = 100,
			.description = "Dedicated fullscreen 1280x720 Media Gallery for browsing and playing all photos and 720p videos stored in the device gallery folder.",
		},
		{
			.id = "doom",
			.name = "Doom (1993)",
			.subtitle = "Installed · Direct Fullscreen Launch",
			.badge = "GAME",
			.meta_pill = "PLAY",
			.version = "1.10",
			.size_label = "4.2 MB",
			.binary_path = "/bin/doom",
			.preset_art = GIRI_ART_DOOM_GAME,
			.installed = true,
			.progress = 100,
			.description = "Classic id Software Doom (E1M1 Knee-Deep in the Dead) running natively on Velliangiri OS with 6-button remote game mapping.",
		},
		{
			.id = "chess",
			.name = "Temple Chess",
			.subtitle = "Grandmaster AI · 6-Button Remote",
			.badge = "BOARD GAME",
			.meta_pill = "1.4 MB",
			.version = "1.2",
			.size_label = "1.4 MB",
			.binary_path = "/usr/bin/temple-chess",
			.preset_art = GIRI_ART_APP_CHESS,
			.installed = true,
			.progress = 100,
			.description = "Offline living-room chess engine tuned for 10-foot viewing and D-pad navigation.",
		},
		{
			.id = "fm_radio",
			.name = "Western Ghats FM",
			.subtitle = "Coimbatore & Nilgiri Community Radio",
			.badge = "AUDIO APP",
			.meta_pill = "2.1 MB",
			.version = "1.0",
			.size_label = "2.1 MB",
			.binary_path = "/usr/bin/ghats-fm",
			.preset_art = GIRI_ART_APP_RADIO,
			.installed = false,
			.progress = 0,
			.description = "Regional Tamil and English public radio streams, classical concerts, and hill weather bulletins.",
		},
	};

	memcpy(g_ui.apps, fallback_apps, sizeof(fallback_apps));
	g_ui.app_count = 4;

	char *json = read_catalog_file("apps.json");
	if (!json)
		return;

	const char *end = json + strlen(json);
	const char *arr = strstr(json, "\"apps\"");
	if (arr) {
		const char *a0 = strchr(arr, '[');
		const char *a1 = a0 ? json_find_matching(a0, end, '[', ']') : NULL;

		if (a0 && a1) {
			const char *cur = a0 + 1;
			int idx = 0;

			while (cur < a1 && idx < GIRI_MAX_APPS) {
				const char *c0 = strchr(cur, '{');
				if (!c0 || c0 >= a1)
					break;
				const char *c1 = json_find_matching(c0, a1, '{', '}');
				if (!c1)
					break;

				struct giri_app_info *ap = &g_ui.apps[idx];
				char preset_str[24] = "";

				memset(ap, 0, sizeof(*ap));
				(void)json_extract_str(c0, c1, "id", ap->id, sizeof(ap->id));
				(void)json_extract_str(c0, c1, "name", ap->name, sizeof(ap->name));
				(void)json_extract_str(c0, c1, "subtitle", ap->subtitle, sizeof(ap->subtitle));
				(void)json_extract_str(c0, c1, "badge", ap->badge, sizeof(ap->badge));
				(void)json_extract_str(c0, c1, "meta_pill", ap->meta_pill, sizeof(ap->meta_pill));
				(void)json_extract_str(c0, c1, "version", ap->version, sizeof(ap->version));
				(void)json_extract_str(c0, c1, "size", ap->size_label, sizeof(ap->size_label));
				(void)json_extract_str(c0, c1, "binary_path", ap->binary_path, sizeof(ap->binary_path));
				(void)json_extract_str(c0, c1, "preset_art", preset_str, sizeof(preset_str));
				(void)json_extract_str(c0, c1, "description", ap->description, sizeof(ap->description));
				(void)json_extract_bool(c0, c1, "installed", &ap->installed);
				ap->preset_art = parse_preset_art(preset_str);
				ap->progress = ap->installed ? 100 : 0;
				idx++;
				cur = c1 + 1;
			}
			if (idx > 0)
				g_ui.app_count = idx;
		}
	}
	free(json);
}

/* =========================================================================
 * 2. Built-In Topic & Capability Screens (Schema v1.2)
 * ========================================================================= */

static void set_standard_footer(struct giri_screen *scr,
				const char *ctx1_label, enum shell_symbol_id ctx1_sym,
				const char *ctx1_type, const char *ctx1_cap, const char *ctx1_query,
				const char *ctx2_label, enum shell_symbol_id ctx2_sym,
				const char *ctx2_type, const char *ctx2_cap, const char *ctx2_query)
{
	/* [0] Fixed Bottom-Left: Ask Giri */
	snprintf(scr->footer_links[0].id, sizeof(scr->footer_links[0].id), "btn_ask_giri");
	snprintf(scr->footer_links[0].label, sizeof(scr->footer_links[0].label), "Ask Giri");
	scr->footer_links[0].symbol = SHELL_SYM_KEYBOARD;
	snprintf(scr->footer_links[0].action.type, sizeof(scr->footer_links[0].action.type), "open_prompt_box");
	snprintf(scr->footer_links[0].action.capability, sizeof(scr->footer_links[0].action.capability), "ui.open_prompt");

	/* [1] Contextual Follow-Up 1 */
	snprintf(scr->footer_links[1].id, sizeof(scr->footer_links[1].id), "btn_ctx_1");
	shell_font_fit_clean_phrase(ctx1_label, scr->footer_links[1].label,
				    sizeof(scr->footer_links[1].label), SHELL_LIMIT_FOOTER_LABEL);
	scr->footer_links[1].symbol = ctx1_sym;
	snprintf(scr->footer_links[1].action.type, sizeof(scr->footer_links[1].action.type), "%s", ctx1_type);
	snprintf(scr->footer_links[1].action.capability, sizeof(scr->footer_links[1].action.capability), "%s", ctx1_cap);
	if (strcmp(ctx1_type, "launch_app") == 0)
		snprintf(scr->footer_links[1].action.app_id, sizeof(scr->footer_links[1].action.app_id), "%s", ctx1_query);
	else
		snprintf(scr->footer_links[1].action.query, sizeof(scr->footer_links[1].action.query), "%s", ctx1_query);

	/* [2] Contextual Follow-Up 2 */
	snprintf(scr->footer_links[2].id, sizeof(scr->footer_links[2].id), "btn_ctx_2");
	shell_font_fit_clean_phrase(ctx2_label, scr->footer_links[2].label,
				    sizeof(scr->footer_links[2].label), SHELL_LIMIT_FOOTER_LABEL);
	scr->footer_links[2].symbol = ctx2_sym;
	snprintf(scr->footer_links[2].action.type, sizeof(scr->footer_links[2].action.type), "%s", ctx2_type);
	snprintf(scr->footer_links[2].action.capability, sizeof(scr->footer_links[2].action.capability), "%s", ctx2_cap);
	if (strcmp(ctx2_type, "launch_app") == 0)
		snprintf(scr->footer_links[2].action.app_id, sizeof(scr->footer_links[2].action.app_id), "%s", ctx2_query);
	else
		snprintf(scr->footer_links[2].action.query, sizeof(scr->footer_links[2].action.query), "%s", ctx2_query);

	/* [3] Fixed Bottom-Right: Settings */
	snprintf(scr->footer_links[3].id, sizeof(scr->footer_links[3].id), "btn_settings");
	snprintf(scr->footer_links[3].label, sizeof(scr->footer_links[3].label), "Settings");
	scr->footer_links[3].symbol = SHELL_SYM_SETTINGS;
	snprintf(scr->footer_links[3].action.type, sizeof(scr->footer_links[3].action.type), "open_settings");
	snprintf(scr->footer_links[3].action.capability, sizeof(scr->footer_links[3].action.capability), "system.open_settings");
}

static void build_home_screen(struct giri_screen *scr)
{
	char *json = read_catalog_file("screen.json");
	if (json) {
		if (parse_screen_json(json, scr)) {
			free(json);
			return;
		}
		free(json);
	}

	memset(scr, 0, sizeof(*scr));
	snprintf(scr->schema_version, sizeof(scr->schema_version), "1.2");
	snprintf(scr->screen_id, sizeof(scr->screen_id), "screen_home_welcome");
	snprintf(scr->page_type, sizeof(scr->page_type), "home");
	snprintf(scr->prompt_title, sizeof(scr->prompt_title), "Welcome to Velliangiri OS");

	snprintf(scr->summary.title, sizeof(scr->summary.title),
		 "Hi, I am Giri, your living room assistant part of Velliangiri OS");
	snprintf(scr->summary.text, sizeof(scr->summary.text),
		 "Do you know Velliangiri is a mountain range in southern India's Western Ghats near Coimbatore? Rising 1,778 meters across seven hills through shola forests, its 14 km trail is one of the region's most revered high-altitude trekking spots.");
	snprintf(scr->summary.source_caption, sizeof(scr->summary.source_caption),
		 "Velliangiri Hills · Western Ghats");

	/* Card 0: Explore Top News */
	snprintf(scr->media_cards[0].id, sizeof(scr->media_cards[0].id), "card_home_news");
	snprintf(scr->media_cards[0].title, sizeof(scr->media_cards[0].title), "Explore Top News");
	snprintf(scr->media_cards[0].subtitle, sizeof(scr->media_cards[0].subtitle), "BBC Global & Regional Headlines");
	snprintf(scr->media_cards[0].badge, sizeof(scr->media_cards[0].badge), "LIVE NEWS");
	snprintf(scr->media_cards[0].meta_pill, sizeof(scr->media_cards[0].meta_pill), "BBC");
	scr->media_cards[0].kind = GIRI_CARD_PAGE_IMAGE;
	scr->media_cards[0].preset_art = GIRI_ART_DAWN_RIDGE;
	snprintf(scr->media_cards[0].action.type, sizeof(scr->media_cards[0].action.type), "ask_giri");
	snprintf(scr->media_cards[0].action.capability, sizeof(scr->media_cards[0].action.capability), "internet.search_and_extract");
	snprintf(scr->media_cards[0].action.query, sizeof(scr->media_cards[0].action.query), "What is top in the news on BBC?");
	derive_card_action_label(&scr->media_cards[0]);

	/* Card 1: Check Live Weather */
	snprintf(scr->media_cards[1].id, sizeof(scr->media_cards[1].id), "card_home_weather");
	snprintf(scr->media_cards[1].title, sizeof(scr->media_cards[1].title), "Check Live Weather");
	snprintf(scr->media_cards[1].subtitle, sizeof(scr->media_cards[1].subtitle), "Foothills Forecast & Sky Radar");
	snprintf(scr->media_cards[1].badge, sizeof(scr->media_cards[1].badge), "WEATHER");
	snprintf(scr->media_cards[1].meta_pill, sizeof(scr->media_cards[1].meta_pill), "LIVE");
	scr->media_cards[1].kind = GIRI_CARD_PAGE_IMAGE;
	scr->media_cards[1].preset_art = GIRI_ART_WEATHER_SKY;
	snprintf(scr->media_cards[1].action.type, sizeof(scr->media_cards[1].action.type), "ask_giri");
	snprintf(scr->media_cards[1].action.capability, sizeof(scr->media_cards[1].action.capability), "internet.search_and_extract");
	snprintf(scr->media_cards[1].action.query, sizeof(scr->media_cards[1].action.query), "What is the weather right now?");
	derive_card_action_label(&scr->media_cards[1]);

	/* Card 2: Media Gallery (Separate Fullscreen App) */
	snprintf(scr->media_cards[2].id, sizeof(scr->media_cards[2].id), "card_home_gallery");
	snprintf(scr->media_cards[2].title, sizeof(scr->media_cards[2].title), "Media Gallery");
	snprintf(scr->media_cards[2].subtitle, sizeof(scr->media_cards[2].subtitle), "Photos & 720p Videos on Device");
	snprintf(scr->media_cards[2].badge, sizeof(scr->media_cards[2].badge), "GALLERY");
	snprintf(scr->media_cards[2].meta_pill, sizeof(scr->media_cards[2].meta_pill), "OPEN");
	scr->media_cards[2].kind = GIRI_CARD_APP_ICON;
	scr->media_cards[2].preset_art = GIRI_ART_MONSOON_VIDEO;
	snprintf(scr->media_cards[2].uri, sizeof(scr->media_cards[2].uri), "GX016305.mov");
	snprintf(scr->media_cards[2].action.type, sizeof(scr->media_cards[2].action.type), "launch_app");
	snprintf(scr->media_cards[2].action.capability, sizeof(scr->media_cards[2].action.capability), "apps.launch");
	snprintf(scr->media_cards[2].action.app_id, sizeof(scr->media_cards[2].action.app_id), "gallery");
	derive_card_action_label(&scr->media_cards[2]);

	/* Card 3: Play Doom */
	snprintf(scr->media_cards[3].id, sizeof(scr->media_cards[3].id), "card_home_doom");
	snprintf(scr->media_cards[3].title, sizeof(scr->media_cards[3].title), "Play Doom");
	snprintf(scr->media_cards[3].subtitle, sizeof(scr->media_cards[3].subtitle), "Installed · Direct Fullscreen Launch");
	snprintf(scr->media_cards[3].badge, sizeof(scr->media_cards[3].badge), "GAME");
	snprintf(scr->media_cards[3].meta_pill, sizeof(scr->media_cards[3].meta_pill), "PLAY");
	scr->media_cards[3].kind = GIRI_CARD_APP_ICON;
	scr->media_cards[3].preset_art = GIRI_ART_DOOM_GAME;
	snprintf(scr->media_cards[3].action.type, sizeof(scr->media_cards[3].action.type), "launch_app");
	snprintf(scr->media_cards[3].action.capability, sizeof(scr->media_cards[3].action.capability), "apps.launch");
	snprintf(scr->media_cards[3].action.app_id, sizeof(scr->media_cards[3].action.app_id), "doom");
	derive_card_action_label(&scr->media_cards[3]);

	set_standard_footer(scr,
		"Open Media Gallery on this device", SHELL_SYM_IMAGE,
		"launch_app", "apps.launch", "gallery",
		"Explore science and space discoveries", SHELL_SYM_GLOBE,
		"ask_giri", "internet.search_and_extract", "What is top in BBC Science news?");
}

static void fill_card_uri(struct giri_media_card *mc, const char *id,
			  const char *title, const char *subtitle,
			  const char *badge, const char *meta_pill,
			  enum giri_card_kind kind, enum giri_preset_art preset,
			  const char *uri,
			  const char *act_type, const char *act_cap,
			  const char *query_or_app, const char *detail_text)
{
	memset(mc, 0, sizeof(*mc));
	snprintf(mc->id, sizeof(mc->id), "%s", id);
	shell_font_fit_clean_phrase(title, mc->title, sizeof(mc->title), SHELL_LIMIT_CARD_TITLE);
	shell_font_fit_clean_phrase(subtitle, mc->subtitle, sizeof(mc->subtitle), SHELL_LIMIT_CARD_SUB);
	snprintf(mc->badge, sizeof(mc->badge), "%s", badge);
	snprintf(mc->meta_pill, sizeof(mc->meta_pill), "%s", meta_pill);
	mc->kind = kind;
	mc->preset_art = preset;
	if (uri && uri[0])
		snprintf(mc->uri, sizeof(mc->uri), "%s", uri);
	snprintf(mc->action.type, sizeof(mc->action.type), "%s", act_type);
	snprintf(mc->action.capability, sizeof(mc->action.capability), "%s", act_cap);
	if (strcmp(act_type, "launch_app") == 0 || strcmp(act_type, "install_app") == 0)
		snprintf(mc->action.app_id, sizeof(mc->action.app_id), "%s", query_or_app);
	else
		snprintf(mc->action.query, sizeof(mc->action.query), "%s", query_or_app);
	if (detail_text)
		snprintf(mc->detail_text, sizeof(mc->detail_text), "%s", detail_text);
	derive_card_action_label(mc);
}

static void populate_gallery_screen(struct giri_screen *scr, int page)
{
	int i;

	if (g_ui.gallery_count <= 0) {
		g_ui.gallery_count = shell_media_scan_device(g_ui.gallery_items,
							     SHELL_MAX_GALLERY_ITEMS,
							     false);
	}

	int total_items = g_ui.gallery_count;
	int total_pages = (total_items + 3) / 4;
	if (total_pages < 1)
		total_pages = 1;
	page = ((page % total_pages) + total_pages) % total_pages;
	g_ui.gallery_page = page;

	memset(scr, 0, sizeof(*scr));
	snprintf(scr->schema_version, sizeof(scr->schema_version), "1.2");
	snprintf(scr->screen_id, sizeof(scr->screen_id), "screen_media_library");
	snprintf(scr->page_type, sizeof(scr->page_type), "media");
	snprintf(scr->prompt_title, sizeof(scr->prompt_title),
		 "Photos & Videos on This Device");

	if (total_items > 0) {
		int first_num = page * 4 + 1;
		int last_num = page * 4 + 4;
		if (last_num > total_items)
			last_num = total_items;

		snprintf(scr->summary.title, sizeof(scr->summary.title),
			 "On-Device Gallery · %d Photos & 720p Videos on Velliangiri OS",
			 total_items);
		snprintf(scr->summary.text, sizeof(scr->summary.text),
			 "Select any photo or video card below for 1280×720 fullscreen viewing. Inside the fullscreen viewer, press Left or Right on your remote to browse all %d gallery items, or use Next 4 below.",
			 total_items);
		snprintf(scr->summary.source_caption, sizeof(scr->summary.source_caption),
			 "Showing %d–%d of %d · Page %d/%d",
			 first_num & 0xff, last_num & 0xff, total_items & 0xff,
			 (page + 1) & 0xff, total_pages & 0xff);

		for (i = 0; i < SHELL_CARD_COUNT; i++) {
			int g_idx = (page * 4 + i) % total_items;
			const struct shell_media_item *it = &g_ui.gallery_items[g_idx];
			char id_buf[32];

			snprintf(id_buf, sizeof(id_buf), "gal_%d", g_idx);
			fill_card_uri(&scr->media_cards[i], id_buf,
				      it->title, it->subtitle,
				      it->badge, it->meta_pill,
				      it->is_video ? GIRI_CARD_LOCAL_VIDEO : GIRI_CARD_LOCAL_PHOTO,
				      it->is_video ? GIRI_ART_MONSOON_VIDEO : GIRI_ART_DAWN_RIDGE,
				      it->path,
				      it->is_video ? "play_video" : "open_image",
				      it->is_video ? "media.play_video" : "media.open_image",
				      it->title, it->detail[0] ? it->detail : it->subtitle);
		}
	} else {
		snprintf(scr->summary.title, sizeof(scr->summary.title),
			 "1280×720 Widescreen Streams, Nature Clips & On-Device Media");
		snprintf(scr->summary.text, sizeof(scr->summary.text),
			 "Select any card below to open the edge-to-edge 1280×720 fullscreen viewer. Use the Play/Pause button on your remote to control playback and press Menu at any time to return to Giri.");
		snprintf(scr->summary.source_caption, sizeof(scr->summary.source_caption),
			 "Velliangiri OS · Media Library");

		fill_card_uri(&scr->media_cards[0], "vid_1", "Dawn Over Seventh Hill",
			      "Golden Hour Ridge Time-Lapse", "VIDEO", "00:15",
			      GIRI_CARD_PAGE_VIDEO, GIRI_ART_DAWN_RIDGE, "dawn_ridge.jpg",
			      "play_video", "media.play_video", "Dawn Over Seventh Hill",
			      "Widescreen 720p time-lapse capturing first light breaking across the seven hills of Velliangiri.");
		fill_card_uri(&scr->media_cards[1], "vid_2", "Siruvani Gorge Cascade",
			      "High-Altitude Forest Waterfall", "VIDEO", "00:18",
			      GIRI_CARD_PAGE_VIDEO, GIRI_ART_WATERFALL_VIDEO, "waterfall_video.jpg",
			      "play_video", "media.play_video", "Siruvani Gorge Cascade",
			      "Crystal-clear mountain streams plunging down twin granite cliffs in the Western Ghats.");
		fill_card_uri(&scr->media_cards[2], "vid_3", "Monsoon Cloud Sweep",
			      "Palghat Gap Atmospheric Flow", "STREAM", "00:15",
			      GIRI_CARD_PAGE_VIDEO, GIRI_ART_MONSOON_VIDEO, "monsoon_video.jpg",
			      "play_video", "media.play_video", "Monsoon Cloud Sweep",
			      "Dramatic southwest monsoon clouds rolling across the shola canopy and foothill valleys.");
		fill_card_uri(&scr->media_cards[3], "vid_4", "Shola Valley Morning",
			      "Emerald Mist & Birdsong Panorama", "PHOTO", "HD",
			      GIRI_CARD_LOCAL_PHOTO, GIRI_ART_SHOLA_MIST, "shola_mist.jpg",
			      "open_image", "media.open_image", "Shola Valley Morning",
			      "High-dynamic-range still photograph of morning mist suspended above the shola evergreen canopy.");
	}

	char next_lbl[64];
	snprintf(next_lbl, sizeof(next_lbl),
		 "Next 4 photos & videos (Page %d/%d)",
		 (((page + 1) % total_pages) + 1) & 0xff, total_pages & 0xff);

	set_standard_footer(scr,
		next_lbl, SHELL_SYM_IMAGE,
		"gallery_next_page", "media.get_library", "Watch photos and videos on this device",
		"Explore apps and Play Doom on TV", SHELL_SYM_GAME,
		"ask_giri", "apps.list_and_install", "Play Doom");
}

static void build_topic_screen(const char *query, struct giri_screen *scr)
{
	memset(scr, 0, sizeof(*scr));
	snprintf(scr->schema_version, sizeof(scr->schema_version), "1.2");

	if (strcasestr(query, "science") || strcasestr(query, "space")) {
		snprintf(scr->screen_id, sizeof(scr->screen_id), "screen_bbc_science");
		snprintf(scr->page_type, sizeof(scr->page_type), "news");
		snprintf(scr->prompt_title, sizeof(scr->prompt_title), "BBC Science & Space Discoveries");
		snprintf(scr->summary.title, sizeof(scr->summary.title),
			 "Deep-Space Telescopes & Renewable Grid Milestones Lead Science");
		snprintf(scr->summary.text, sizeof(scr->summary.text),
			 "Astronomers mapped water-vapor plumes across outer exoplanet atmospheres while southern Indian solar-wind hybrid corridors set a new clean-energy dispatch record. Marine biologists also documented resilient coral reefs in the Indian Ocean.");
		snprintf(scr->summary.source_caption, sizeof(scr->summary.source_caption),
			 "BBC Science & Environment · Verified");

		fill_card_uri(&scr->media_cards[0], "sci_1", "Exoplanet Water Plumes",
			      "James Webb Infrared Spectroscopy", "SPACE", "JWST",
			      GIRI_CARD_PAGE_IMAGE, GIRI_ART_DAWN_RIDGE, "sci_space.jpg",
			      "open_image", "media.open_image", "Exoplanet Water Plumes",
			      "High-resolution near-infrared spectroscopy confirmed water vapor and silicate cloud layers in a temperate sub-Neptune atmosphere.");
		fill_card_uri(&scr->media_cards[1], "sci_2", "Southern Grid Record",
			      "Wind & Solar Storage Dispatch", "ENERGY", "GRID",
			      GIRI_CARD_PAGE_IMAGE, GIRI_ART_WEATHER_SKY, "sci_energy.jpg",
			      "open_image", "media.open_image", "Southern Grid Record",
			      "Coimbatore and Tirunelveli wind-solar corridors supplied peak afternoon grid demand with zero thermal curtailment.");
		fill_card_uri(&scr->media_cards[2], "sci_3", "Shola Canopy Ecology",
			      "High-Altitude Cloud Forest Study", "NATURE", "720P",
			      GIRI_CARD_PAGE_VIDEO, GIRI_ART_SHOLA_MIST, "sci_forest.jpg",
			      "play_video", "media.play_video", "Shola Canopy Ecology",
			      "Field researchers in the Nilgiri Biosphere Reserve tracked montane cloud-water capture across sky-island catchments.");
		fill_card_uri(&scr->media_cards[3], "sci_4", "Monsoon Ocean Buoys",
			      "Bay & Arabian Sea Autonomous Fleet", "OCEAN", "LIVE",
			      GIRI_CARD_PAGE_VIDEO, GIRI_ART_MONSOON_VIDEO, "sci_ocean.jpg",
			      "play_video", "media.play_video", "Monsoon Ocean Buoys",
			      "Deep-ocean gliders transmitted real-time salinity and thermocline profiles ahead of the southwest monsoon onset.");

		set_standard_footer(scr,
			"What is top in the news on BBC?", SHELL_SYM_GLOBE,
			"ask_giri", "internet.search_and_extract", "What is top in the news on BBC?",
			"Tell me about the Western Ghats", SHELL_SYM_SPARK,
			"ask_giri", "internet.search_and_extract", "Tell me about the Western Ghats");
		return;
	}

	if (strcasestr(query, "weather") || strcasestr(query, "forecast")) {
		snprintf(scr->screen_id, sizeof(scr->screen_id), "screen_weather_live");
		snprintf(scr->page_type, sizeof(scr->page_type), "weather");
		snprintf(scr->prompt_title, sizeof(scr->prompt_title), "Live Foothills Weather & Radar");
		snprintf(scr->summary.title, sizeof(scr->summary.title),
			 "24°C Pleasant Breeze in Poondi Foothills · Clear Evening Ahead");
		snprintf(scr->summary.text, sizeof(scr->summary.text),
			 "Light southwest winds at 14 km/h carry cool mountain air across Poondi and Coimbatore. Humidity stands at 62%% with high visibility along the seven hills and a brief passing mist expected over the 1,778m summit before dawn.");
		snprintf(scr->summary.source_caption, sizeof(scr->summary.source_caption),
			 "Velliangiri Foothills · Live Telemetry");

		fill_card_uri(&scr->media_cards[0], "wx_1", "Poondi Foothills · 24°C",
			      "Clear Skies · Wind SW 14 km/h", "NOW", "24°C",
			      GIRI_CARD_PAGE_IMAGE, GIRI_ART_WEATHER_SKY, "IMG_6397.jpeg",
			      "open_image", "media.open_image", "Poondi Foothills · 24°C",
			      "Current foothill station telemetry shows 24°C, 62% relative humidity, barometric pressure 1013 hPa, and clear visibility.");
		fill_card_uri(&scr->media_cards[1], "wx_2", "Seventh Hill Summit · 15°C",
			      "Crisp Alpine Breeze · Light Mist", "1,778M", "15°C",
			      GIRI_CARD_PAGE_IMAGE, GIRI_ART_DAWN_RIDGE, "IMG_6418.jpeg",
			      "open_image", "media.open_image", "Seventh Hill Summit",
			      "High-altitude ridge conditions remain cool at 15°C with broken orographic cloud bands drifting across the fifth and seventh hills.");
		fill_card_uri(&scr->media_cards[2], "wx_3", "Palghat Gap Wind Radar",
			      "Southwest Monsoon Corridor Flow", "RADAR", "720P",
			      GIRI_CARD_PAGE_VIDEO, GIRI_ART_MONSOON_VIDEO, "weather_sky.jpg",
			      "play_video", "media.play_video", "Palghat Gap Wind Radar",
			      "Doppler wind sweep across the Palghat Gap shows steady moisture transport and evening thermal cooling.");
		fill_card_uri(&scr->media_cards[3], "wx_4", "Siruvani Catchment",
			      "Reservoir Inflow & Forest Humidity", "BASIN", "LIVE",
			      GIRI_CARD_PAGE_IMAGE, GIRI_ART_SHOLA_MIST, "waterfall_video.jpg",
			      "open_image", "media.open_image", "Siruvani Catchment",
			      "Siruvani and Noyyal headwater catchments report steady perennial stream flow and crystal-clear water quality.");

		set_standard_footer(scr,
			"Tell me about the Western Ghats", SHELL_SYM_GLOBE,
			"ask_giri", "internet.search_and_extract", "Tell me about the Western Ghats",
			"What is top in the news on BBC?", SHELL_SYM_PAGE,
			"ask_giri", "internet.search_and_extract", "What is top in the news on BBC?");
		return;
	}

	if (strcasestr(query, "ghats") || strcasestr(query, "velliangiri")) {
		snprintf(scr->screen_id, sizeof(scr->screen_id), "screen_western_ghats");
		snprintf(scr->page_type, sizeof(scr->page_type), "topic");
		snprintf(scr->prompt_title, sizeof(scr->prompt_title), "Western Ghats & Velliangiri Hills");
		snprintf(scr->summary.title, sizeof(scr->summary.title),
			 "Velliangiri Seven Hills & the UNESCO Western Ghats Biodiversity");
		snprintf(scr->summary.text, sizeof(scr->summary.text),
			 "Older than the Himalayas, the Western Ghats form a 1,600 km mountain chain along peninsular India. The Velliangiri hills—known as Thenkailayam—span seven sculpted ridges rising to 1,778m above the Noyyal river valley.");
		snprintf(scr->summary.source_caption, sizeof(scr->summary.source_caption),
			 "Velliangiri Encyclopedia · Western Ghats");

		fill_card_uri(&scr->media_cards[0], "vg_1", "Seven Hills Trail",
			      "Poondi to 1,778m Peak · 14 km", "TREK", "1,778M",
			      GIRI_CARD_PAGE_IMAGE, GIRI_ART_DAWN_RIDGE, "IMG_6452.jpeg",
			      "open_image", "media.open_image", "Seven Hills Trail",
			      "The sacred 14 km trail climbs through bamboo groves, Vellai Vinayagar temple, Paambatti Sunai spring, and breezy alpine meadows.");
		fill_card_uri(&scr->media_cards[1], "vg_2", "Shola Sky Islands",
			      "Montane Grasslands & Cloud Forests", "ECOLOGY", "UNESCO",
			      GIRI_CARD_PAGE_IMAGE, GIRI_ART_SHOLA_MIST, "IMG_6455.jpeg",
			      "open_image", "media.open_image", "Shola Sky Islands",
			      "Stunted evergreen shola forests nestled in high valleys act as natural sponges feeding the Noyyal and Bhavani rivers year-round.");
		fill_card_uri(&scr->media_cards[2], "vg_3", "Siruvani Cascades",
			      "Twin Granite Cliffs & Pure Waters", "STREAM", "720P",
			      GIRI_CARD_PAGE_VIDEO, GIRI_ART_WATERFALL_VIDEO, "waterfall_video.jpg",
			      "play_video", "media.play_video", "Siruvani Cascades",
			      "Fed by high-altitude forest springs, the Siruvani waterfalls cascade through granite gorges west of Coimbatore.");
		fill_card_uri(&scr->media_cards[3], "vg_4", "Poondi Temple Trailhead",
			      "Bamboo Staffs & Foothills Start", "CULTURE", "HD",
			      GIRI_CARD_PAGE_IMAGE, GIRI_ART_MONSOON_VIDEO, "IMG_6402.jpeg",
			      "open_image", "media.open_image", "Poondi Temple Trailhead",
			      "Trekkers gather at the historic Poondi foothills temple with bamboo walking sticks before ascending the seven hills.");

		set_standard_footer(scr,
			"Open Media Gallery on this device", SHELL_SYM_IMAGE,
			"launch_app", "apps.launch", "gallery",
			"What is the weather right now?", SHELL_SYM_SPARK,
			"ask_giri", "internet.search_and_extract", "What is the weather right now?");
		return;
	}

	if (strcasestr(query, "video") || strcasestr(query, "stream")) {
		snprintf(scr->screen_id, sizeof(scr->screen_id), "screen_video_streams");
		snprintf(scr->page_type, sizeof(scr->page_type), "media");
		snprintf(scr->prompt_title, sizeof(scr->prompt_title), "Widescreen Videos & Nature Streams");
		snprintf(scr->summary.title, sizeof(scr->summary.title),
			 "1280×720 Widescreen Nature Streams & Atmospheric Time-Lapses");
		snprintf(scr->summary.text, sizeof(scr->summary.text),
			 "Explore curated 1280×720 nature time-lapses from the Western Ghats, or open the dedicated Media Gallery app below to browse and play all photos and 720p videos stored on this device.");
		snprintf(scr->summary.source_caption, sizeof(scr->summary.source_caption),
			 "Velliangiri OS · Widescreen Streams");

		fill_card_uri(&scr->media_cards[0], "vid_1", "Dawn Over Seventh Hill",
			      "Golden Hour Ridge Time-Lapse", "VIDEO", "00:15",
			      GIRI_CARD_PAGE_VIDEO, GIRI_ART_DAWN_RIDGE, "dawn_ridge.jpg",
			      "play_video", "media.play_video", "Dawn Over Seventh Hill",
			      "Widescreen 720p time-lapse capturing first light breaking across the seven hills of Velliangiri.");
		fill_card_uri(&scr->media_cards[1], "vid_2", "Siruvani Gorge Cascade",
			      "High-Altitude Forest Waterfall", "VIDEO", "00:18",
			      GIRI_CARD_PAGE_VIDEO, GIRI_ART_WATERFALL_VIDEO, "waterfall_video.jpg",
			      "play_video", "media.play_video", "Siruvani Gorge Cascade",
			      "Crystal-clear mountain streams plunging down twin granite cliffs in the Western Ghats.");
		fill_card_uri(&scr->media_cards[2], "vid_3", "Monsoon Cloud Sweep",
			      "Palghat Gap Atmospheric Flow", "STREAM", "00:15",
			      GIRI_CARD_PAGE_VIDEO, GIRI_ART_MONSOON_VIDEO, "monsoon_video.jpg",
			      "play_video", "media.play_video", "Monsoon Cloud Sweep",
			      "Dramatic southwest monsoon clouds rolling across the shola canopy and foothill valleys.");
		fill_card_uri(&scr->media_cards[3], "card_open_gallery", "Media Gallery",
			      "Photos & 720p Videos on Device", "GALLERY", "OPEN",
			      GIRI_CARD_APP_ICON, GIRI_ART_MONSOON_VIDEO, "GX016305.mov",
			      "launch_app", "apps.launch", "gallery",
			      "Launch the standalone fullscreen Media Gallery app to browse all device photos and 720p videos.");

		set_standard_footer(scr,
			"Open Media Gallery on this device", SHELL_SYM_IMAGE,
			"launch_app", "apps.launch", "gallery",
			"Explore apps and Play Doom on TV", SHELL_SYM_GAME,
			"ask_giri", "apps.list_and_install", "Play Doom");
		return;
	}

	if (strcasestr(query, "doom") || strcasestr(query, "app") ||
	    strcasestr(query, "game") || strcasestr(query, "gallery")) {
		int i;

		snprintf(scr->screen_id, sizeof(scr->screen_id), "screen_apps_catalog");
		snprintf(scr->page_type, sizeof(scr->page_type), "apps");
		snprintf(scr->prompt_title, sizeof(scr->prompt_title), "Apps & Games on Velliangiri OS");
		snprintf(scr->summary.title, sizeof(scr->summary.title),
			 "On-Device TV Apps · Media Gallery & Fullscreen Doom (apps.json)");
		snprintf(scr->summary.text, sizeof(scr->summary.text),
			 "Launch installed native applications directly in 1280×720 fullscreen. Select Media Gallery to browse all on-device photos and 720p videos, or hold Menu for 2 seconds inside any app to return to Giri.");
		snprintf(scr->summary.source_caption, sizeof(scr->summary.source_caption),
			 "Velliangiri OS · On-Device App Catalog");

		for (i = 0; i < SHELL_CARD_COUNT && i < g_ui.app_count; i++) {
			const struct giri_app_info *ap = &g_ui.apps[i];

			fill_card_uri(&scr->media_cards[i], ap->id,
				      ap->name, ap->subtitle,
				      ap->badge, ap->installed ? "READY" : ap->size_label,
				      GIRI_CARD_APP_ICON, ap->preset_art, NULL,
				      ap->installed ? "launch_app" : "install_app",
				      ap->installed ? "apps.launch" : "apps.list_and_install",
				      ap->id, ap->description);
		}

		set_standard_footer(scr,
			"Open Media Gallery on this device", SHELL_SYM_IMAGE,
			"launch_app", "apps.launch", "gallery",
			"What is top in the news on BBC?", SHELL_SYM_GLOBE,
			"ask_giri", "internet.search_and_extract", "What is top in the news on BBC?");
		return;
	}

	/* Default: BBC Top News Briefing */
	snprintf(scr->screen_id, sizeof(scr->screen_id), "screen_bbc_top_news");
	snprintf(scr->page_type, sizeof(scr->page_type), "news");
	shell_font_fit_clean_phrase(query[0] ? query : "BBC Global & Regional News",
				    scr->prompt_title, sizeof(scr->prompt_title),
				    SHELL_LIMIT_PROMPT_TITLE);
	snprintf(scr->summary.title, sizeof(scr->summary.title),
		 "Global Semiconductor Pacts, Clean Transit & Monsoon Preparedness");
	snprintf(scr->summary.text, sizeof(scr->summary.text),
		 "International trade ministers finalized resilient chip-supply corridors today while Indian Railways expanded high-speed regional electric service across southern corridors. Forecasters also issued an early favorable outlook for reservoir inflows.");
	snprintf(scr->summary.source_caption, sizeof(scr->summary.source_caption),
		 "BBC News · Top Headlines");

	fill_card_uri(&scr->media_cards[0], "news_1", "Chip Supply Corridors",
		      "Global Fabrication & Packaging Pact", "WORLD", "BBC",
		      GIRI_CARD_PAGE_IMAGE, GIRI_ART_DAWN_RIDGE, "news_world.jpg",
		      "open_image", "media.open_image", "Chip Supply Corridors",
		      "New multilateral agreements expand domestic wafer testing and open-source RISC-V and ARM edge systems across Asia and Europe.");
	fill_card_uri(&scr->media_cards[1], "news_2", "Southern Rail Corridor",
		      "Coimbatore–Chennai Express Upgrade", "INDIA", "TRANSIT",
		      GIRI_CARD_PAGE_IMAGE, GIRI_ART_WEATHER_SKY, "news_transit.jpg",
		      "open_image", "media.open_image", "Southern Rail Corridor",
		      "Upgraded signalling and solar-fed traction substations cut intercity travel times across Tamil Nadu and Kerala.");
	fill_card_uri(&scr->media_cards[2], "news_3", "Western Ghats Canopy",
		      "UNESCO Corridor Telemetry Report", "CLIMATE", "720P",
		      GIRI_CARD_PAGE_VIDEO, GIRI_ART_SHOLA_MIST, "news_climate.jpg",
		      "play_video", "media.play_video", "Western Ghats Canopy",
		      "Satellite canopy monitoring shows strong regeneration across protected shola-grassland sky islands.");
	fill_card_uri(&scr->media_cards[3], "news_4", "Reservoir Inflow Briefing",
		      "Siruvani & Bhavani Basin Outlook", "REGIONAL", "720P",
		      GIRI_CARD_PAGE_VIDEO, GIRI_ART_WATERFALL_VIDEO, "news_water.jpg",
		      "play_video", "media.play_video", "Reservoir Inflow Briefing",
		      "Hydrologists report healthy storage levels across western Tamil Nadu catchments ahead of the seasonal rains.");

	set_standard_footer(scr,
		"Explore science and space discoveries", SHELL_SYM_SPARK,
		"ask_giri", "internet.search_and_extract", "What is top in BBC Science news?",
		"Check live foothills weather right now", SHELL_SYM_GLOBE,
		"ask_giri", "internet.search_and_extract", "What is the weather right now?");
}

/* =========================================================================
 * 3. Live System Metrics Probe (for Settings Modal Read-Only Strip)
 * ========================================================================= */

static void refresh_system_metrics(void)
{
	snprintf(g_ui.sys_temp, sizeof(g_ui.sys_temp), "43°C · Cool");
	snprintf(g_ui.sys_storage, sizeof(g_ui.sys_storage), "6.4 GB Free (80%%)");
	snprintf(g_ui.sys_memory, sizeof(g_ui.sys_memory), "88%% Available");
	snprintf(g_ui.sys_network, sizeof(g_ui.sys_network), "On-Device Ready");
	snprintf(g_ui.sys_display, sizeof(g_ui.sys_display), "1280×720 · 60Hz");

	/* Probe thermal zone if present */
	FILE *fp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
	if (fp) {
		long millideg = 0;

		if (fscanf(fp, "%ld", &millideg) == 1 && millideg > 1000) {
			int c = (int)(millideg / 1000);
			snprintf(g_ui.sys_temp, sizeof(g_ui.sys_temp),
				 "%d°C · %s", c, c < 65 ? "Cool" : "Warm");
		}
		fclose(fp);
	}

	/* Probe rootfs free space */
	struct statvfs vfs;
	if (statvfs("/", &vfs) == 0 && vfs.f_blocks > 0) {
		double free_gb = ((double)vfs.f_bavail * vfs.f_frsize) / (1024.0 * 1024.0 * 1024.0);
		int pct = (int)((vfs.f_bavail * 100ULL) / vfs.f_blocks);

		if (free_gb >= 1.0)
			snprintf(g_ui.sys_storage, sizeof(g_ui.sys_storage),
				 "%.1f GB Free (%d%%)", free_gb, pct);
		else
			snprintf(g_ui.sys_storage, sizeof(g_ui.sys_storage),
				 "%.0f MB Free (%d%%)", free_gb * 1024.0, pct);
	}

	/* Probe /proc/meminfo */
	fp = fopen("/proc/meminfo", "r");
	if (fp) {
		char line[128];
		unsigned long total_kb = 0, avail_kb = 0;

		while (fgets(line, sizeof(line), fp)) {
			if (sscanf(line, "MemTotal: %lu kB", &total_kb) == 1)
				continue;
			if (sscanf(line, "MemAvailable: %lu kB", &avail_kb) == 1)
				break;
		}
		fclose(fp);
		if (total_kb > 0 && avail_kb > 0) {
			int pct = (int)((avail_kb * 100UL) / total_kb);
			snprintf(g_ui.sys_memory, sizeof(g_ui.sys_memory),
				 "%d%% Available", pct);
		}
	}
}

/* =========================================================================
 * 4. Pixel-Exact Compositor for All 4 Windows (Schema v1.2)
 * ========================================================================= */

static const struct giri_screen *current_screen(void)
{
	return &g_ui.history[g_ui.history_idx];
}

static void mark_full_redraw(void)
{
	g_ui.main_bg_valid = false;
	g_ui.backbuf_modal_win = -1;
	g_ui.dirty_slots = 0x03;
	g_ui.dirty_cards = 0x0f;
	g_ui.dirty_footer = 0x0f;
}

/* Draw the DOOM / E1M1 vector typography inside preset_art == GIRI_ART_DOOM_GAME */
static void draw_doom_art_labels(int x, int y, int w, int h)
{
#ifdef HAVE_DOOM_POSTER_ASSET
	(void)x;
	(void)y;
	(void)w;
	(void)h;
#else
	int title_px = (w >= 640) ? 72 : 42;
	int sub_px = (w >= 640) ? 18 : 11;
	int tw = shell_font_measure_role(SHELL_FONT_MONO_SEMIBOLD, title_px, "DOOM");
	int sw = shell_font_measure_role(SHELL_FONT_SANS_BOLD, sub_px, "DIRECT LAUNCH · E1M1");
	int ty = y + (h * 112) / 312;
	int sy = ty + title_px + 4;

	shell_font_draw_role(SHELL_FONT_MONO_SEMIBOLD, title_px,
			     x + (w - tw) / 2, ty, 0xfff59e0bU, "DOOM", w - 16);
	shell_font_draw_role(SHELL_FONT_SANS_BOLD, sub_px,
			     x + (w - sw) / 2, sy, 0xfffca5a5U, "DIRECT LAUNCH · E1M1", w - 16);
#endif
}

static void render_slot1_header(const struct shell_theme *th, bool clear_bg)
{
	const struct giri_screen *scr = current_screen();
	int hx = SHELL_SAFE_LEFT;
	int hy = SHELL_SLOT1_Y;
	int frame = g_ui.is_working ? g_ui.logo_frame_idx : 0;
	const uint32_t *logo_px = (g_ui.theme_id == SHELL_THEME_SIGNAL)
				  ? logo_signal_frames[frame]
				  : logo_paper_frames[frame];

	if (clear_bg)
		shell_draw_rect(SHELL_SAFE_LEFT, SHELL_SLOT1_Y,
				SHELL_USABLE_W, SHELL_HEADER_H, th->bg);

	shell_draw_bitmap_xrgb(hx, hy + 2, SHELL_LOGO_W, SHELL_LOGO_H, logo_px);
	hx += SHELL_LOGO_W + 14;

	hx += shell_font_draw_vcentered(SHELL_FONT_SANS_BOLD, 22,
					hx, hy, SHELL_HEADER_H, th->ink, "Giri", 120) + 14;
	hx += shell_font_draw_vcentered(SHELL_FONT_SANS_BOLD, 20,
					hx, hy, SHELL_HEADER_H, th->accent, "—", 40) + 14;

	const char *ptitle = g_ui.is_working && g_ui.pending_query[0]
			     ? g_ui.pending_query : scr->prompt_title;
	shell_font_draw_vcentered(SHELL_FONT_SANS_SEMIBOLD, 20,
				  hx, hy, SHELL_HEADER_H, th->muted, ptitle,
				  SHELL_SAFE_LEFT + SHELL_USABLE_W - hx);
}

static void render_slot2_summary(const struct shell_theme *th, bool clear_corners)
{
	const struct giri_screen *scr = current_screen();
	int sx = SHELL_SAFE_LEFT;
	int sy = SHELL_SLOT2_Y;
	int sw = SHELL_USABLE_W;
	int sh = SHELL_SUMMARY_H;

	if (clear_corners) {
		shell_draw_rect(sx, sy, 12, 12, th->bg);
		shell_draw_rect(sx + sw - 12, sy, 12, 12, th->bg);
		shell_draw_rect(sx, sy + sh - 12, 12, 12, th->bg);
		shell_draw_rect(sx + sw - 12, sy + sh - 12, 12, 12, th->bg);
	}
	shell_draw_rounded_rect(sx, sy, sw, sh, 12, th->surface);

	int pad_x = sx + 22;
	int top_y = sy + 14;
	int src_w = shell_font_measure_role(SHELL_FONT_MONO_MEDIUM, 13,
					    scr->summary.source_caption);
	int src_x = sx + sw - 22 - src_w;

	if (scr->summary.source_caption[0]) {
		shell_font_draw_role(SHELL_FONT_MONO_MEDIUM, 13,
				     src_x, top_y + 5, th->muted,
				     scr->summary.source_caption, src_w + 4);
	}

	int max_title_w = (src_x - pad_x - 16);
	if (max_title_w < 200)
		max_title_w = sw - 44;
	shell_font_draw_role(SHELL_FONT_SANS_BOLD, 21,
			     pad_x, top_y, th->ink,
			     scr->summary.title, max_title_w);

	int body_y = top_y + 34;
	int body_w = sw - 44;

	if (g_ui.is_working) {
		int phase = (int)(((shell_now_ms() - g_ui.working_start_ms) / 8ULL) % 100ULL);
		uint32_t base_c = (g_ui.theme_id == SHELL_THEME_PAPER) ? 0xffdfd5c3U : 0xff1e2430U;

		shell_draw_shimmer_bar(pad_x, body_y + 6, (body_w * 96) / 100, 16, 4,
				       base_c, th->accent, phase);
		shell_draw_shimmer_bar(pad_x, body_y + 32, (body_w * 78) / 100, 16, 4,
				       base_c, th->accent, (phase + 35) % 100);
	} else {
		int total_sents = 0;
		int active_sent = g_ui.tts_speaking ? g_ui.tts_sentence_idx : -1;

		shell_font_draw_summary_body(pad_x, body_y, 26, body_w,
					     SHELL_LIMIT_SUMMARY_LINES,
					     th->ink, 0xfff59e0bU, 66,
					     scr->summary.text,
					     active_sent, &total_sents);
		g_ui.tts_total_sentences = total_sents;
	}
}

static void render_slot3_card(const struct shell_theme *th, int i, bool clear_bg)
{
	static const int card_xs[4] = { 64, 356, 649, 942 };
	static const int card_ws[4] = { 274, 275, 275, 274 };
	const struct giri_screen *scr = current_screen();
	const struct giri_media_card *mc = &scr->media_cards[i];
	bool focused = (g_ui.active_win == GIRI_WIN_MAIN &&
			g_ui.focus_zone == GIRI_ZONE_CARDS &&
			g_ui.card_focus_idx == i);
	int cx = card_xs[i];
	int cw = card_ws[i];
	int ch = SHELL_CARD_H;
	int cy = SHELL_SLOT3_Y - (focused ? 4 : 0);

	if (clear_bg)
		shell_draw_rect(cx - 6, SHELL_SLOT3_Y - 8, cw + 12, ch + 24, th->bg);

	if (focused) {
		shell_draw_card_shadow(cx, cy, cw, ch, SHELL_CARD_RADIUS);
		shell_draw_focus_glow(cx, cy, cw, ch, SHELL_CARD_RADIUS,
				      3, 0xfff59e0bU, 77);
	}

	const uint32_t *art;
	if (g_ui.allow_async_art) {
		bool is_final = true;
		art = shell_media_get_card_art_fast(mc, cw, ch, &is_final);
		if (!is_final)
			g_ui.cards_need_preload = true;
	} else {
		art = shell_media_get_card_art(mc, cw, ch);
	}

	if (art)
		shell_draw_rounded_bitmap_stride(cx, cy, cw, ch, 275,
						 SHELL_CARD_RADIUS, art, focused);
	else
		shell_draw_rounded_rect(cx, cy, cw, ch, SHELL_CARD_RADIUS, th->card_bg);

	if (mc->preset_art == GIRI_ART_DOOM_GAME)
		draw_doom_art_labels(cx, cy, cw, ch);

	/* Video center 46x46 play overlay circle */
	bool is_video = (mc->kind == GIRI_CARD_PAGE_VIDEO ||
			 mc->kind == GIRI_CARD_LOCAL_VIDEO ||
			 strcmp(mc->action.type, "play_video") == 0);
	if (is_video) {
		int pcx = cx + cw / 2;
		int pcy = cy + 128;

		shell_draw_circle_alpha(pcx, pcy, 23, 0xff0c0d10U, 217);
		shell_draw_symbol(SHELL_SYM_PLAY, pcx - 8, pcy - 9, 18, 0xfff59e0bU);
	}

	/* Top-left badge (border: none, rx=5, rgba(10,12,16,0.82)) */
	if (mc->badge[0]) {
		int bw = shell_font_measure_role(SHELL_FONT_MONO_SEMIBOLD, 11, mc->badge) + 16;

		shell_draw_rounded_rect_alpha(cx + 12, cy + 12, bw, 22, 5, 0xff0a0c10U, 209);
		shell_font_draw_box_centered(SHELL_FONT_MONO_SEMIBOLD, 11,
					     cx + 12, cy + 12, bw, 22,
					     0xfff2efe9U, mc->badge);
	}

	/* Top-right meta pill (border: none, rx=5, rgba(10,12,16,0.84), #F59E0B) */
	if (mc->meta_pill[0]) {
		int pw = shell_font_measure_role(SHELL_FONT_MONO_SEMIBOLD, 11, mc->meta_pill) + 16;
		int px0 = cx + cw - 12 - pw;

		shell_draw_rounded_rect_alpha(px0, cy + 12, pw, 22, 5, 0xff0a0c10U, 214);
		shell_font_draw_box_centered(SHELL_FONT_MONO_SEMIBOLD, 11,
					     px0, cy + 12, pw, 22,
					     0xfff59e0bU, mc->meta_pill);
	}

	/* Bottom gradient scrim (148px tall) + overlaid title, subtitle, action row */
	shell_draw_card_bottom_scrim(cx, cy, cw, ch, SHELL_CARD_RADIUS, 148);

	int text_x = cx + 15;
	int text_w = cw - 30;
	int act_y = cy + ch - 27;
	int sub_y = act_y - 22;
	int title_y = sub_y - 24;

	shell_font_draw_role(SHELL_FONT_SANS_BOLD, 17,
			     text_x, title_y, 0xfff8fafcU, mc->title, text_w);
	shell_font_draw_role(SHELL_FONT_SANS_MEDIUM, 13,
			     text_x, sub_y, 0xffe2e8f0U, mc->subtitle, text_w);

	uint32_t act_col = focused ? 0xfffbbf24U : 0xffcbd5e1U;
	shell_font_draw_role(SHELL_FONT_MONO_SEMIBOLD, 12,
			     text_x, act_y, act_col,
			     mc->action_label[0] ? mc->action_label : "Open",
			     text_w - 72);

	if (focused) {
		int sel_w = shell_font_measure_role(SHELL_FONT_MONO_SEMIBOLD, 12, "Select ↵");
		shell_font_draw_role(SHELL_FONT_MONO_SEMIBOLD, 12,
				     cx + cw - 15 - sel_w, act_y,
				     0xfffbbf24U, "Select ↵", sel_w + 4);
	}

	/* Selectable card border: 3px focus_ring when focused, 2px border otherwise */
	shell_draw_rounded_outline(cx, cy, cw, ch, SHELL_CARD_RADIUS,
				   focused ? SHELL_CARD_FOCUS_BORDER : SHELL_CARD_BORDER,
				   focused ? th->focus_ring : th->border);
}

static void render_slot4_footer_btn(const struct shell_theme *th, int i, bool clear_bg)
{
	static const int btn_xs[4] = { 64, 268, 648, 1028 };
	static const int btn_ws[4] = { 188, 364, 364, 188 };
	const struct giri_screen *scr = current_screen();
	const struct giri_footer_link *fl = &scr->footer_links[i];
	bool focused = (g_ui.active_win == GIRI_WIN_MAIN &&
			g_ui.focus_zone == GIRI_ZONE_FOOTER &&
			g_ui.footer_focus_idx == i);
	int bx = btn_xs[i];
	int bw = btn_ws[i];
	int bh = SHELL_FOOTER_H;
	int by = SHELL_SLOT4_Y - (focused ? 2 : 0);

	if (clear_bg)
		shell_draw_rect(bx - 4, SHELL_SLOT4_Y - 6, bw + 8, bh + 10, th->bg);

	if (focused) {
		shell_draw_focus_glow(bx, by, bw, bh, SHELL_FOOTER_RADIUS,
				      3, 0xfff59e0bU, 71);
	}

	shell_draw_rounded_rect(bx, by, bw, bh, SHELL_FOOTER_RADIUS,
				focused ? th->btn_focus_bg : th->btn_bg);
	shell_draw_rounded_outline(bx, by, bw, bh, SHELL_FOOTER_RADIUS,
				   focused ? SHELL_FOOTER_FOCUS_BORDER : SHELL_FOOTER_BORDER,
				   focused ? th->focus_ring : th->border);

	/* 30x30 internal icon box (border: none, rx=8) */
	int ibx = bx + 12;
	int iby = by + (bh - 30) / 2;
	if (focused) {
		shell_draw_rounded_rect(ibx, iby, 30, 30, 8, th->accent);
		shell_draw_symbol(fl->symbol, ibx + 6, iby + 6, 18, 0xffffffffU);
	} else {
		shell_draw_rounded_rect_alpha(ibx, iby, 30, 30, 8, th->accent, 36);
		shell_draw_symbol(fl->symbol, ibx + 6, iby + 6, 18, th->accent);
	}

	/* Right keycap badge "↵" (border: none, rx=6, 24x22) */
	int kw = 24, kh = 22;
	int kx = bx + bw - 12 - kw;
	int ky = by + (bh - kh) / 2;
	shell_draw_rounded_rect(kx, ky, kw, kh, 6,
				focused ? th->accent : th->surface);
	shell_font_draw_box_centered(SHELL_FONT_MONO_SEMIBOLD, 12,
				     kx, ky, kw, kh,
				     focused ? 0xffffffffU : th->muted, "↵");

	/* Button label vertically centered on cap-height */
	int lx = ibx + 30 + 10;
	int lw = kx - lx - 8;
	shell_font_draw_vcentered(SHELL_FONT_SANS_SEMIBOLD, 14,
				  lx, by, bh, th->ink,
				  fl->label, lw);
}

/*
 * Window 0: 4-Slot Main TV Screen (.tv-screen)
 *   Slot 1 (y=36,  h=44):  Header (49x40 logo + "Giri" + "—" + prompt_title)
 *   Slot 2 (y=108, h=148): Editorial Summary Box (title + source + <=250c body)
 *   Slot 3 (y=284, h=312): 4 Selectable Full-Bleed Visual Cards (gap 18px)
 *   Slot 4 (y=624, h=58):  4 Selectable Bottom Bar Buttons (188px 364px 364px 188px)
 */
static void render_window_main(const struct shell_theme *th)
{
	int i;

	g_ui.backbuf_modal_win = -1;
	shell_draw_rect(0, 0, SHELL_SCREEN_W, SHELL_SCREEN_H, th->bg);
	render_slot1_header(th, false);
	render_slot2_summary(th, false);
	for (i = 0; i < SHELL_CARD_COUNT; i++)
		render_slot3_card(th, i, false);
	for (i = 0; i < SHELL_FOOTER_COUNT; i++)
		render_slot4_footer_btn(th, i, false);

	g_ui.main_bg_valid = (g_ui.active_win == GIRI_WIN_MAIN);
	g_ui.dirty_slots = 0;
	g_ui.dirty_cards = 0;
	g_ui.dirty_footer = 0;
}

static void render_window_main_incremental(const struct shell_theme *th)
{
	int i;

	if (!g_ui.main_bg_valid || g_ui.backbuf_modal_win != -1) {
		render_window_main(th);
		return;
	}
	if (g_ui.dirty_slots & 0x01)
		render_slot1_header(th, true);
	if (g_ui.dirty_slots & 0x02)
		render_slot2_summary(th, true);
	for (i = 0; i < SHELL_CARD_COUNT; i++) {
		if (g_ui.dirty_cards & (1U << i))
			render_slot3_card(th, i, true);
	}
	for (i = 0; i < SHELL_FOOTER_COUNT; i++) {
		if (g_ui.dirty_footer & (1U << i))
			render_slot4_footer_btn(th, i, true);
	}
	g_ui.dirty_slots = 0;
	g_ui.dirty_cards = 0;
	g_ui.dirty_footer = 0;
}

/*
 * Window 1: Fullscreen 1280x720 Edge-to-Edge Media / Detail Viewer (.media-overlay)
 */
static void render_video_hud_dock(const struct shell_theme *th,
				  int (*hud_rects)[4])
{
	int bx = 56, by = 28;
	int badge_w = shell_font_measure_role(SHELL_FONT_MONO_SEMIBOLD, 12,
					      g_ui.overlay_badge) + 22;
	int src_w = shell_font_measure_role(SHELL_FONT_MONO_MEDIUM, 13,
					    g_ui.overlay_source);
	const char *hint = "Menu / Esc = Back";
	int hw = shell_font_measure_role(SHELL_FONT_MONO_SEMIBOLD, 13, hint) + 32;
	int hx = SHELL_SCREEN_W - 56 - hw;
	int top_w;
	int dx = 44, dy = SHELL_SCREEN_H - 202, dw = SHELL_SCREEN_W - 88, dh = 166;

	if (src_w > 560)
		src_w = 560;
	top_w = badge_w + 14 + src_w + 24;
	if (bx - 8 + top_w > hx - 12)
		top_w = (hx - 12) - (bx - 8);

	hud_rects[0][0] = bx - 8;
	hud_rects[0][1] = by - 4;
	hud_rects[0][2] = top_w;
	hud_rects[0][3] = 36;

	hud_rects[1][0] = hx;
	hud_rects[1][1] = by - 2;
	hud_rects[1][2] = hw;
	hud_rects[1][3] = 32;

	hud_rects[2][0] = dx;
	hud_rects[2][1] = dy;
	hud_rects[2][2] = dw;
	hud_rects[2][3] = dh;

	if (!g_ui.overlay_hud_dirty)
		return;
	g_ui.overlay_hud_dirty = false;

	/* 1. Top-left badge + source pill */
	shell_draw_rect(bx - 8, by - 4, top_w, 36, 0xff0c0f16U);
	shell_draw_rounded_outline(bx - 8, by - 4, top_w, 36, 8, 1, 0xff263044U);
	shell_draw_rounded_rect(bx, by, badge_w, 28, 6, th->accent);
	shell_font_draw_box_centered(SHELL_FONT_MONO_SEMIBOLD, 12,
				     bx, by, badge_w, 28,
				     0xffffffffU, g_ui.overlay_badge);
	shell_font_draw_vcentered(SHELL_FONT_MONO_MEDIUM, 13,
				  bx + badge_w + 14, by, 28, 0xffe2e8f0U,
				  g_ui.overlay_source, top_w - badge_w - 26);

	/* 2. Top-right Back pill */
	shell_draw_rect(hx, by - 2, hw, 32, 0xff0f1218U);
	shell_draw_rounded_outline(hx, by - 2, hw, 32, 8, 2, th->focus_ring);
	shell_font_draw_box_centered(SHELL_FONT_MONO_SEMIBOLD, 13,
				     hx, by - 2, hw, 32,
				     0xfff8fafcU, hint);

	/* 3. Bottom cinema player dock */
	shell_draw_rect(dx, dy, dw, dh, 0xff0b0e14U);
	shell_draw_rounded_outline(dx, dy, dw, dh, 12, 1, 0xff263044U);

	{
		int px = 56, py = SHELL_SCREEN_H - 190, pw = SHELL_SCREEN_W - 112, ph = 42;
		const char *st_txt = g_ui.overlay_playing ? "PLAYING" : "PAUSED";
		int st_w = shell_font_measure_role(SHELL_FONT_MONO_SEMIBOLD, 13, st_txt) + 20;
		char time_buf[32];
		int tw, tx, tr_w, ty, fill_w;
		int sy = SHELL_SCREEN_H - 132;

		shell_draw_rounded_rect(px, py, pw, ph, 8, 0xff131822U);
		shell_draw_rounded_rect(px + 16, py + 8, st_w, 26, 5, 0xfff59e0bU);
		shell_font_draw_box_centered(SHELL_FONT_MONO_SEMIBOLD, 13,
					     px + 16, py + 8, st_w, 26,
					     0xff161513U, st_txt);

		snprintf(time_buf, sizeof(time_buf), "00:%02d / 00:%02d",
			 g_ui.overlay_elapsed_sec, g_ui.overlay_duration_sec);
		tw = shell_font_measure_role(SHELL_FONT_MONO_MEDIUM, 14, time_buf);
		shell_font_draw_vcentered(SHELL_FONT_MONO_MEDIUM, 14,
					  px + pw - 16 - tw, py, ph,
					  0xffe2dfd7U, time_buf, tw + 4);

		tx = px + 16 + st_w + 16;
		tr_w = (px + pw - 16 - tw - 16) - tx;
		ty = py + (ph - 8) / 2;
		shell_draw_rounded_rect(tx, ty, tr_w, 8, 4, 0xff2b3548U);
		fill_w = (tr_w * g_ui.overlay_elapsed_sec) /
			 (g_ui.overlay_duration_sec > 0 ? g_ui.overlay_duration_sec : 15);
		if (fill_w > tr_w)
			fill_w = tr_w;
		if (fill_w > 0)
			shell_draw_rounded_rect(tx, ty, fill_w, 8, 4, 0xfff59e0bU);

		shell_font_draw_role(SHELL_FONT_SANS_BOLD, 28,
				     56, sy, 0xffffffffU,
				     g_ui.overlay_title, SHELL_SCREEN_W - 112);
		shell_font_draw_wrapped_role(SHELL_FONT_SANS_MEDIUM, 18,
					     56, sy + 38, 26, 1120, 2,
					     0xfff8fafcU, g_ui.overlay_text);
	}
}

static void render_window_media_overlay(const struct shell_theme *th)
{
	g_ui.main_bg_valid = false;
	g_ui.backbuf_modal_win = -1;

	/*
	 * Zero-copy hardware DMABUF video scanout fast path:
	 * s5l8947x-jpeg (/dev/video2) -> NV12 DMABUF -> s5l8947x-m2sc (/dev/video1)
	 * -> back DRM dumb buffer DMABUF, then composite only the 3 HUD pills.
	 */
	if (g_ui.overlay_is_video) {
		int vdmabuf_fd = -1;
		int vbuf_idx = shell_display_prepare_video_backbuf(&vdmabuf_fd);

		if (vbuf_idx >= 0 &&
		    shell_media_render_video_frame_dmabuf(&g_ui.overlay_card,
							  g_ui.overlay_frame_idx,
							  vbuf_idx, vdmabuf_fd)) {
			int hud_rects[3][4];

			render_video_hud_dock(th, hud_rects);
			shell_display_present_video_backbuf(vbuf_idx, hud_rects, 3);
			return;
		}
	}

	const uint32_t *fs_art = g_ui.overlay_is_video
				 ? shell_media_get_video_frame(&g_ui.overlay_card,
							       g_ui.overlay_frame_idx)
				 : shell_media_get_fullscreen_art(&g_ui.overlay_card);

	if (fs_art)
		shell_draw_bitmap_xrgb(0, 0, SHELL_SCREEN_W, SHELL_SCREEN_H, fs_art);
	else
		shell_draw_rect(0, 0, SHELL_SCREEN_W, SHELL_SCREEN_H, 0xff060709U);

	if (g_ui.overlay_card.preset_art == GIRI_ART_DOOM_GAME)
		draw_doom_art_labels(0, 0, SHELL_SCREEN_W, SHELL_SCREEN_H);

	/* Top gradient bar */
	shell_draw_top_scrim(SHELL_SCREEN_W, 96);

	int bx = 56, by = 28;
	int badge_w = shell_font_measure_role(SHELL_FONT_MONO_SEMIBOLD, 12, g_ui.overlay_badge) + 22;
	shell_draw_rounded_rect(bx, by, badge_w, 28, 6, th->accent);
	shell_font_draw_box_centered(SHELL_FONT_MONO_SEMIBOLD, 12,
				     bx, by, badge_w, 28,
				     0xffffffffU, g_ui.overlay_badge);

	shell_font_draw_vcentered(SHELL_FONT_MONO_MEDIUM, 13,
				  bx + badge_w + 14, by, 28, 0xffe2e8f0U,
				  g_ui.overlay_source, 640);

	/* Selectable right Back pill ("Menu / Esc = Back") -> 2px focus_ring border */
	const char *hint = (g_ui.overlay_gallery_idx >= 0 && !g_ui.overlay_is_video)
			   ? "Left/Right = Browse · Menu = Back"
			   : "Menu / Esc = Back";
	int hw = shell_font_measure_role(SHELL_FONT_MONO_SEMIBOLD, 13, hint) + 32;
	int hx = SHELL_SCREEN_W - 56 - hw;
	shell_draw_rounded_rect_alpha(hx, by - 2, hw, 32, 8, 0xff0f1218U, 210);
	shell_draw_rounded_outline(hx, by - 2, hw, 32, 8, 2, th->focus_ring);
	shell_font_draw_box_centered(SHELL_FONT_MONO_SEMIBOLD, 13,
				     hx, by - 2, hw, 32,
				     0xfff8fafcU, hint);

	/* Bottom gradient scrim & summary box */
	shell_draw_bottom_scrim(SHELL_SCREEN_H - 240, SHELL_SCREEN_W, 240);

	int sy = SHELL_SCREEN_H - 132;
	if (g_ui.overlay_is_video) {
		/* Video player bar */
		int px = 56, py = SHELL_SCREEN_H - 190, pw = SHELL_SCREEN_W - 112, ph = 42;
		shell_draw_rounded_rect_alpha(px, py, pw, ph, 8, 0xff0c0d10U, 210);

		const char *st_txt = g_ui.overlay_playing ? "PLAYING" : "PAUSED";
		int st_w = shell_font_measure_role(SHELL_FONT_MONO_SEMIBOLD, 13, st_txt) + 20;
		shell_draw_rounded_rect(px + 16, py + 8, st_w, 26, 5, 0xfff59e0bU);
		shell_font_draw_box_centered(SHELL_FONT_MONO_SEMIBOLD, 13,
					     px + 16, py + 8, st_w, 26,
					     0xff161513U, st_txt);

		char time_buf[32];
		snprintf(time_buf, sizeof(time_buf), "00:%02d / 00:%02d",
			 g_ui.overlay_elapsed_sec, g_ui.overlay_duration_sec);
		int tw = shell_font_measure_role(SHELL_FONT_MONO_MEDIUM, 14, time_buf);
		shell_font_draw_vcentered(SHELL_FONT_MONO_MEDIUM, 14,
					  px + pw - 16 - tw, py, ph,
					  0xffe2dfd7U, time_buf, tw + 4);

		int tx = px + 16 + st_w + 16;
		int tr_w = (px + pw - 16 - tw - 16) - tx;
		int ty = py + (ph - 8) / 2;
		shell_draw_rounded_rect_alpha(tx, ty, tr_w, 8, 4, 0xffffffffU, 56);
		int fill_w = (tr_w * g_ui.overlay_elapsed_sec) /
			     (g_ui.overlay_duration_sec > 0 ? g_ui.overlay_duration_sec : 15);
		if (fill_w > 0)
			shell_draw_rounded_rect(tx, ty, fill_w, 8, 4, 0xfff59e0bU);
	}

	shell_font_draw_role(SHELL_FONT_SANS_BOLD, 28,
			     56, sy, 0xffffffffU, g_ui.overlay_title, SHELL_SCREEN_W - 112);
	shell_font_draw_wrapped_role(SHELL_FONT_SANS_MEDIUM, 18,
				     56, sy + 38, 26, 1120, 2,
				     0xfff8fafcU, g_ui.overlay_text);
}

/*
 * Shared Modal Overlay Frame & Surface Palette for Windows 2 and 3.
 * Ensures dark ("Signal") theme modals clearly stand out as elevated overlay
 * dialogs with deep backdrop dimming, ambient halo, elevated slate surfaces,
 * and crisp double-rimmed borders.
 */
struct modal_palette {
	uint32_t dlg_bg;
	uint32_t dlg_border;
	uint32_t dlg_inner_rim;
	uint32_t strip_bg;
	uint32_t strip_border;
	uint32_t surf_bg;
	uint32_t focus_bg;
	uint32_t row_border;
	uint32_t pill_bg;
};

static void get_modal_palette(const struct shell_theme *th, struct modal_palette *mp)
{
	if (g_ui.theme_id == SHELL_THEME_SIGNAL) {
		mp->dlg_bg        = 0xff131722U;
		mp->dlg_border    = 0xff4b5975U;
		mp->dlg_inner_rim = 0xff263044U;
		mp->strip_bg      = 0xff191f2dU;
		mp->strip_border  = 0xff2b3548U;
		mp->surf_bg       = 0xff1c2230U;
		mp->focus_bg      = 0xff262f42U;
		mp->row_border    = 0xff364156U;
		mp->pill_bg       = 0xff10141dU;
	} else {
		mp->dlg_bg        = th->bg;
		mp->dlg_border    = 0xffc6bdb0U;
		mp->dlg_inner_rim = 0xfffaf8f3U;
		mp->strip_bg      = th->surface;
		mp->strip_border  = th->border;
		mp->surf_bg       = th->surface;
		mp->focus_bg      = th->btn_focus_bg;
		mp->row_border    = th->border;
		mp->pill_bg       = th->bg;
	}
}

static void draw_modal_backdrop_and_frame(const struct modal_palette *mp,
					  int dx, int dy, int dw, int dh)
{
	uint32_t dim_c = (g_ui.theme_id == SHELL_THEME_SIGNAL) ? 0xff020305U : 0xff08090cU;
	uint8_t dim_a = (g_ui.theme_id == SHELL_THEME_SIGNAL) ? 226 : 214;
	int ix0 = dx + 14;
	int iy0 = dy + 14;
	int ix1 = dx + dw - 14;
	int iy1 = dy + dh - 14;

	/*
	 * Dim the 4 visible strips of Window 0 around the opaque dialog body
	 * instead of dimming the ~600,000 pixels underneath the dialog.
	 */
	shell_draw_rect_alpha(0, 0, SHELL_SCREEN_W, iy0, dim_c, dim_a);
	shell_draw_rect_alpha(0, iy1, SHELL_SCREEN_W, SHELL_SCREEN_H - iy1, dim_c, dim_a);
	shell_draw_rect_alpha(0, iy0, ix0, iy1 - iy0, dim_c, dim_a);
	shell_draw_rect_alpha(ix1, iy0, SHELL_SCREEN_W - ix1, iy1 - iy0, dim_c, dim_a);

	/* Multi-layer elevation shadow + subtle outer halo */
	shell_draw_card_shadow(dx, dy + 2, dw, dh, 16);
	if (g_ui.theme_id == SHELL_THEME_SIGNAL)
		shell_draw_focus_glow(dx, dy, dw, dh, 16, 5, 0xff64748bU, 34);

	/* Elevated dialog surface + inner specular rim + 2px outer border */
	shell_draw_rounded_rect(dx, dy, dw, dh, 16, mp->dlg_bg);
	shell_draw_rounded_outline(dx + 2, dy + 2, dw - 4, dh - 4, 14, 1, mp->dlg_inner_rim);
	shell_draw_rounded_outline(dx, dy, dw, dh, 16, 2, mp->dlg_border);
}

/*
 * Window 2: Ask Giri Prompt & Suggested Topics Modal (#inputModal)
 */
static void render_window_ask_modal(const struct shell_theme *th)
{
	struct modal_palette mp;
	int dx = 180, dy = 152, dw = 920, dh = 416;

	get_modal_palette(th, &mp);
	if (g_ui.backbuf_modal_win != GIRI_WIN_ASK_MODAL ||
	    g_ui.backbuf_modal_theme != (int)g_ui.theme_id ||
	    g_ui.backbuf_modal_hist_idx != g_ui.history_idx) {
		if (!g_ui.main_bg_valid)
			render_window_main(th);
		draw_modal_backdrop_and_frame(&mp, dx, dy, dw, dh);
		g_ui.main_bg_valid = false;
		g_ui.backbuf_modal_win = GIRI_WIN_ASK_MODAL;
		g_ui.backbuf_modal_theme = (int)g_ui.theme_id;
		g_ui.backbuf_modal_hist_idx = g_ui.history_idx;
	} else {
		shell_draw_rounded_rect(dx + 4, dy + 4, dw - 8, dh - 8, 12, mp.dlg_bg);
	}

	int ix = dx + 34;
	int iy = dy + 28;
	int iw = dw - 68;

	shell_font_draw_role(SHELL_FONT_SANS_BOLD, 24, ix, iy, th->ink, "Ask Giri", iw);
	iy += 46;

	/* Input field + Submit button row (focus index 0) */
	bool inp_focused = (g_ui.ask_focus_idx == 0);
	int sub_w = 136;
	int fld_w = iw - sub_w - 12;

	if (inp_focused) {
		shell_draw_focus_glow(ix, iy, fld_w, 56, 10, 3, 0xfff59e0bU, 64);
		shell_draw_focus_glow(ix + fld_w + 12, iy, sub_w, 56, 10, 3, 0xfff59e0bU, 89);
	}

	shell_draw_rounded_rect(ix, iy, fld_w, 56, 10, mp.surf_bg);
	shell_draw_rounded_outline(ix, iy, fld_w, 56, 10,
				   inp_focused ? 3 : 2,
				   inp_focused ? th->focus_ring : mp.row_border);

	if (g_ui.ask_input_len > 0) {
		shell_font_draw_vcentered(SHELL_FONT_SANS_SEMIBOLD, 20,
					  ix + 18, iy, 56, th->ink,
					  g_ui.ask_input, fld_w - 36);
	} else {
		shell_font_draw_vcentered(SHELL_FONT_SANS_SEMIBOLD, 19,
					  ix + 18, iy, 56, th->muted,
					  "Ask any question, news, weather, topic, media, or Doom…",
					  fld_w - 36);
	}

	int sbx = ix + fld_w + 12;
	shell_draw_rounded_rect(sbx, iy, sub_w, 56, 10, th->accent);
	shell_draw_rounded_outline(sbx, iy, sub_w, 56, 10,
				   inp_focused ? 3 : 2,
				   inp_focused ? th->focus_ring : th->accent);
	shell_font_draw_box_centered(SHELL_FONT_SANS_BOLD, 18,
				     sbx, iy, sub_w, 56, 0xffffffffU,
				     "Ask Giri");

	iy += 56 + 22;
	shell_font_draw_role(SHELL_FONT_MONO_SEMIBOLD, 11,
			     ix + 2, iy, th->muted, "SUGGESTED TOPICS", iw);
	iy += 22;

	/* 7 Quick-Prompt Chips (focus indices 1..7) */
	int cx = ix;
	int cy = iy;
	int i;

	for (i = 0; i < QUICK_PROMPTS_COUNT; i++) {
		int idx = i + 1;
		bool c_focused = (g_ui.ask_focus_idx == idx);
		int cw = shell_font_measure_role(SHELL_FONT_SANS_SEMIBOLD, 14,
						 quick_prompts[i].label) + 28;
		int ch = 38;

		if (cx + cw > ix + iw) {
			cx = ix;
			cy += ch + 10;
		}
		if (c_focused)
			shell_draw_focus_glow(cx, cy, cw, ch, 8, 3, 0xfff59e0bU, 64);

		shell_draw_rounded_rect(cx, cy, cw, ch, 8,
					c_focused ? mp.focus_bg : mp.surf_bg);
		shell_draw_rounded_outline(cx, cy, cw, ch, 8,
					   c_focused ? 3 : 2,
					   c_focused ? th->focus_ring : mp.row_border);
		shell_font_draw_box_centered(SHELL_FONT_SANS_SEMIBOLD, 14,
					     cx, cy, cw, ch, th->ink,
					     quick_prompts[i].label);
		cx += cw + 10;
	}

	/* Close row at bottom of dialog (focus index 8) */
	int close_idx = QUICK_PROMPTS_COUNT + 1;
	bool close_focused = (g_ui.ask_focus_idx == close_idx);
	int c_y = dy + dh - 28 - 44;

	if (close_focused)
		shell_draw_focus_glow(ix, c_y, iw, 44, 10, 3, 0xfff59e0bU, 64);
	shell_draw_rounded_rect(ix, c_y, iw, 44, 10,
				close_focused ? mp.focus_bg : mp.surf_bg);
	shell_draw_rounded_outline(ix, c_y, iw, 44, 10,
				   close_focused ? 3 : 2,
				   close_focused ? th->focus_ring : mp.row_border);

	int close_lbl_w = shell_font_measure_role(SHELL_FONT_SANS_BOLD, 15, "Close");
	int dw_pill = shell_font_measure_role(SHELL_FONT_MONO_SEMIBOLD, 12, "Done ↵") + 20;
	int total_cw = close_lbl_w + 12 + dw_pill;
	int cl_x = ix + (iw - total_cw) / 2;
	int dpx = cl_x + close_lbl_w + 12;
	int dpy = c_y + (44 - 24) / 2;

	shell_font_draw_vcentered(SHELL_FONT_SANS_BOLD, 15,
				  cl_x, c_y, 44, th->ink, "Close", close_lbl_w + 8);
	shell_draw_rounded_rect(dpx, dpy, dw_pill, 24, 6,
				close_focused ? th->accent : mp.pill_bg);
	shell_font_draw_box_centered(SHELL_FONT_MONO_SEMIBOLD, 12,
				     dpx, dpy, dw_pill, 24,
				     close_focused ? 0xffffffffU : th->accent, "Done ↵");
}

/* Helper to draw a selectable .setting-row in Window 3 */
static void draw_setting_row(const struct shell_theme *th,
			     const struct modal_palette *mp,
			     int x, int y, int w, int h,
			     const char *title, const char *meta_tag,
			     const char *desc, const char *val_badge,
			     bool focused, bool is_active_badge,
			     bool is_downloading, int progress)
{
	if (focused)
		shell_draw_focus_glow(x, y, w, h, 10, 3, 0xfff59e0bU, 64);

	shell_draw_rounded_rect(x, y, w, h, 10,
				focused ? mp->focus_bg : mp->surf_bg);

	int vw = shell_font_measure_role(SHELL_FONT_MONO_SEMIBOLD, 12, val_badge) + 20;
	int vx = x + w - 14 - vw;
	int vy = y + (h - 24) / 2;
	bool pill_accent = focused || is_active_badge;

	shell_draw_rounded_rect(vx, vy, vw, 24, 6, pill_accent ? th->accent : mp->pill_bg);
	shell_font_draw_box_centered(SHELL_FONT_MONO_SEMIBOLD, 12,
				     vx, vy, vw, 24,
				     pill_accent ? 0xffffffffU : th->accent, val_badge);

	int tx = x + 14;
	int max_tw = vx - tx - 8;
	int tw = shell_font_draw_role(SHELL_FONT_SANS_BOLD, 15,
				      tx, y + 13, th->ink, title, max_tw);
	if (meta_tag && meta_tag[0] && tw + 14 < max_tw) {
		shell_font_draw_role(SHELL_FONT_MONO_MEDIUM, 11,
				     tx + tw + 7, y + 16, th->muted,
				     meta_tag, max_tw - tw - 7);
	}
	if (desc && desc[0]) {
		shell_font_draw_role(SHELL_FONT_SANS_MEDIUM, 11,
				     tx, y + 34, th->muted, desc, max_tw);
	}

	if (is_downloading) {
		shell_draw_rect_alpha(x + 2, y + h - 6, w - 4, 4, 0xfff59e0bU, 46);
		int fw = ((w - 4) * progress) / 100;
		if (fw > 0)
			shell_draw_rect(x + 2, y + h - 6, fw, 4, 0xfff59e0bU);
	}

	shell_draw_rounded_outline(x, y, w, h, 10,
				   focused ? 3 : 2,
				   focused ? th->focus_ring : mp->row_border);
}

/*
 * Window 3: Velliangiri OS Settings Modal (#settingsModal)
 *   - Read-only 5-column System Status Strip
 *   - 3-column OS Preferences grid (indices 0..2)
 *   - 2x3 Supported AI Models grid from supported_models.json (indices 3..8)
 *   - Close Settings row (index 9)
 */
static void render_window_settings_modal(const struct shell_theme *th)
{
	struct modal_palette mp;
	int dx = 160, dy = 42, dw = 960, dh = 636;

	get_modal_palette(th, &mp);
	if (g_ui.backbuf_modal_win != GIRI_WIN_SETTINGS_MODAL ||
	    g_ui.backbuf_modal_theme != (int)g_ui.theme_id ||
	    g_ui.backbuf_modal_hist_idx != g_ui.history_idx) {
		if (!g_ui.main_bg_valid)
			render_window_main(th);
		draw_modal_backdrop_and_frame(&mp, dx, dy, dw, dh);
		g_ui.main_bg_valid = false;
		g_ui.backbuf_modal_win = GIRI_WIN_SETTINGS_MODAL;
		g_ui.backbuf_modal_theme = (int)g_ui.theme_id;
		g_ui.backbuf_modal_hist_idx = g_ui.history_idx;
	} else {
		shell_draw_rounded_rect(dx + 4, dy + 4, dw - 8, dh - 8, 12, mp.dlg_bg);
	}

	int ix = dx + 28;
	int iy = dy + 22;
	int iw = dw - 56;	/* 904px */

	shell_font_draw_role(SHELL_FONT_SANS_BOLD, 24,
			     ix, iy, th->ink, "Velliangiri OS Settings", iw);
	iy += 38;

	/* Non-selectable 5-column System Status Strip (rx=10) */
	shell_draw_rounded_rect(ix, iy, iw, 54, 10, mp.strip_bg);
	if (g_ui.theme_id == SHELL_THEME_SIGNAL)
		shell_draw_rounded_outline(ix, iy, iw, 54, 10, 1, mp.strip_border);
	const char *labels[5] = { "TEMPERATURE", "STORAGE", "MEMORY", "NETWORK", "DISPLAY" };
	const char *vals[5] = {
		g_ui.sys_temp, g_ui.sys_storage, g_ui.sys_memory,
		g_ui.sys_network, g_ui.sys_display
	};
	int col_w = (iw - 32) / 5;
	int i;

	for (i = 0; i < 5; i++) {
		int mx = ix + 16 + i * col_w;
		shell_font_draw_role(SHELL_FONT_MONO_SEMIBOLD, 11,
				     mx, iy + 11, th->muted, labels[i], col_w - 8);
		shell_font_draw_role(SHELL_FONT_SANS_BOLD, 15,
				     mx, iy + 28, th->ink, vals[i], col_w - 8);
	}
	iy += 54 + 14;

	/* Section 1: OS PREFERENCES (3 columns, indices 0..2) */
	shell_font_draw_role(SHELL_FONT_MONO_SEMIBOLD, 11,
			     ix + 2, iy, th->muted, "OS PREFERENCES", iw);
	iy += 18;

	int pref_w = (iw - 20) / 3;	/* 294px each */
	draw_setting_row(th, &mp, ix, iy, pref_w, 58,
			 "Display Theme", NULL, "Paper (Daylight) or Signal (Night)",
			 g_ui.theme_id == SHELL_THEME_PAPER ? "Paper" : "Signal",
			 g_ui.settings_focus_idx == 0, false, false, 0);
	draw_setting_row(th, &mp, ix + pref_w + 10, iy, pref_w, 58,
			 "Auto Read Aloud", NULL, "Speak summary automatically on load",
			 g_ui.auto_read_aloud ? "On" : "Off",
			 g_ui.settings_focus_idx == 1, false, false, 0);
	draw_setting_row(th, &mp, ix + (pref_w + 10) * 2, iy, iw - (pref_w + 10) * 2, 58,
			 "Navigation Sound", NULL, "Remote D-pad acoustic micro-feedback",
			 g_ui.nav_sound_enabled ? "On" : "Muted",
			 g_ui.settings_focus_idx == 2, false, false, 0);
	iy += 58 + 14;

	/* Section 2: SUPPORTED AI MODELS (2x3 grid, indices 3..8) */
	shell_font_draw_role(SHELL_FONT_MONO_SEMIBOLD, 11,
			     ix + 2, iy, th->muted,
			     "SUPPORTED AI MODELS (SUPPORTED_MODELS.JSON)", iw);
	iy += 18;

	int mod_w = (iw - 10) / 2;	/* 447px each */
	for (i = 0; i < g_ui.model_count && i < 6; i++) {
		const struct giri_model_info *m = &g_ui.models[i];
		int col = i & 1;
		int row = i >> 1;
		int mx = ix + col * (mod_w + 10);
		int my = iy + row * (58 + 8);
		int focus_idx = 3 + i;
		bool is_active = m->installed && (i == g_ui.active_model_idx);
		char meta[48];
		char val_badge[32];
		char dl_desc[96];

		snprintf(meta, sizeof(meta), "%s · %s", m->id, m->size_label);
		if (is_active)
			snprintf(val_badge, sizeof(val_badge), "✓ Active");
		else if (m->installed)
			snprintf(val_badge, sizeof(val_badge), "Select ↵");
		else if (m->downloading)
			snprintf(val_badge, sizeof(val_badge), "%d%%", m->progress);
		else
			snprintf(val_badge, sizeof(val_badge), "⬇ %s", m->size_label);

		if (m->downloading)
			snprintf(dl_desc, sizeof(dl_desc), "Downloading %d%%…", m->progress);

		draw_setting_row(th, &mp, mx, my, mod_w, 58,
				 m->name, meta,
				 m->downloading ? dl_desc : m->description,
				 val_badge,
				 g_ui.settings_focus_idx == focus_idx,
				 is_active, m->downloading, m->progress);
	}

	iy += 3 * (58 + 8) + 6;

	/* Close Settings row (index 3 + model_count) */
	int close_idx = 3 + g_ui.model_count;
	bool close_focused = (g_ui.settings_focus_idx == close_idx);

	if (close_focused)
		shell_draw_focus_glow(ix, iy, iw, 44, 10, 3, 0xfff59e0bU, 64);
	shell_draw_rounded_rect(ix, iy, iw, 44, 10,
				close_focused ? mp.focus_bg : mp.surf_bg);
	shell_draw_rounded_outline(ix, iy, iw, 44, 10,
				   close_focused ? 3 : 2,
				   close_focused ? th->focus_ring : mp.row_border);

	int close_lbl_w = shell_font_measure_role(SHELL_FONT_SANS_BOLD, 15, "Close Settings");
	int dw_pill = shell_font_measure_role(SHELL_FONT_MONO_SEMIBOLD, 12, "Done ↵") + 20;
	int total_cw = close_lbl_w + 12 + dw_pill;
	int cl_x = ix + (iw - total_cw) / 2;
	int dpx = cl_x + close_lbl_w + 12;
	int dpy = iy + (44 - 24) / 2;

	shell_font_draw_vcentered(SHELL_FONT_SANS_BOLD, 15,
				  cl_x, iy, 44, th->ink, "Close Settings", close_lbl_w + 8);
	shell_draw_rounded_rect(dpx, dpy, dw_pill, 24, 6,
				close_focused ? th->accent : mp.pill_bg);
	shell_font_draw_box_centered(SHELL_FONT_MONO_SEMIBOLD, 12,
				     dpx, dpy, dw_pill, 24,
				     close_focused ? 0xffffffffU : th->accent, "Done ↵");
}

static void render_frame(void)
{
	const struct shell_theme *th = &shell_themes[g_ui.theme_id];

	switch (g_ui.active_win) {
	case GIRI_WIN_MAIN:
	default:
		render_window_main_incremental(th);
		break;
	case GIRI_WIN_MEDIA_OVERLAY:
		render_window_media_overlay(th);
		break;
	case GIRI_WIN_ASK_MODAL:
		render_window_ask_modal(th);
		break;
	case GIRI_WIN_SETTINGS_MODAL:
		render_window_settings_modal(th);
		break;
	}
	shell_display_present();
}

/* =========================================================================
 * 5. Action Execution & 6-Button Remote Navigation State Machine
 * ========================================================================= */

static bool launch_media_gallery_app(void)
{
	g_ui.tts_speaking = false;
	g_ui.is_working = false;
	g_ui.has_pending_screen = false;
	g_ui.main_bg_valid = false;
	g_ui.backbuf_modal_win = -1;

	/*
	 * Run the Media Gallery app directly on the active DRM/V4L2/FreeType
	 * session so there is zero DRM master drop and zero fbcon TUI flash.
	 */
	media_gallery_run(g_ui.theme_id);
	mark_full_redraw();
	return true;
}

static void start_topic_transition(const char *query)
{
	bool was_main = (g_ui.active_win == GIRI_WIN_MAIN);

	if (query && (strcasestr(query, "gallery") ||
		      strcasestr(query, "on this device") ||
		      strcasestr(query, "photo"))) {
		if (launch_media_gallery_app())
			return;
	}

	g_ui.active_win = GIRI_WIN_MAIN;
	g_ui.tts_speaking = false;
	g_ui.is_working = true;
	g_ui.working_start_ms = shell_now_ms();
	g_ui.last_logo_step_ms = g_ui.working_start_ms;
	g_ui.logo_step_cursor = 0;
	g_ui.logo_frame_idx = 0;
	shell_font_fit_clean_phrase(query, g_ui.pending_query,
				    sizeof(g_ui.pending_query), SHELL_LIMIT_PROMPT_TITLE);

	/* Build target screen structure immediately (<0.05ms) for background preload */
	build_topic_screen(g_ui.pending_query, &g_ui.pending_screen);
	g_ui.has_pending_screen = true;

	if (!was_main || !g_ui.main_bg_valid)
		mark_full_redraw();
	else
		g_ui.dirty_slots |= 0x03;
}

static void complete_topic_transition(void)
{
	if (!g_ui.has_pending_screen)
		build_topic_screen(g_ui.pending_query, &g_ui.pending_screen);

	if (g_ui.history_idx + 1 < HISTORY_MAX) {
		g_ui.history_idx++;
		g_ui.history_len = g_ui.history_idx + 1;
	}
	g_ui.history[g_ui.history_idx] = g_ui.pending_screen;
	g_ui.has_pending_screen = false;
	g_ui.is_working = false;
	g_ui.logo_frame_idx = 0;
	g_ui.focus_zone = GIRI_ZONE_CARDS;
	g_ui.card_focus_idx = 0;
	g_ui.cards_need_preload = true;
	mark_full_redraw();

	if (g_ui.auto_read_aloud) {
		g_ui.tts_speaking = true;
		g_ui.tts_sentence_idx = 0;
		g_ui.tts_last_step_ms = shell_now_ms();
	}
}

static void open_gallery_item_overlay(int g_idx)
{
	if (g_ui.gallery_count <= 0)
		return;
	g_idx = ((g_idx % g_ui.gallery_count) + g_ui.gallery_count) % g_ui.gallery_count;
	g_ui.overlay_gallery_idx = g_idx;

	const struct shell_media_item *it = &g_ui.gallery_items[g_idx];
	struct giri_media_card mc;
	char id_buf[32];

	snprintf(id_buf, sizeof(id_buf), "gal_%d", g_idx);
	fill_card_uri(&mc, id_buf, it->title, it->subtitle,
		      it->badge, it->meta_pill,
		      it->is_video ? GIRI_CARD_LOCAL_VIDEO : GIRI_CARD_LOCAL_PHOTO,
		      it->is_video ? GIRI_ART_MONSOON_VIDEO : GIRI_ART_DAWN_RIDGE,
		      it->path,
		      it->is_video ? "play_video" : "open_image",
		      it->is_video ? "media.play_video" : "media.open_image",
		      it->title, it->detail[0] ? it->detail : it->subtitle);

	g_ui.tts_speaking = false;
	g_ui.active_win = GIRI_WIN_MEDIA_OVERLAY;
	snprintf(g_ui.overlay_badge, sizeof(g_ui.overlay_badge),
		 "%s", it->is_video ? "VIDEO STREAM" : "PHOTO VIEW");
	snprintf(g_ui.overlay_source, sizeof(g_ui.overlay_source),
		 "%s · Item %d of %d on Device",
		 it->subtitle, g_idx + 1, g_ui.gallery_count);
	snprintf(g_ui.overlay_title, sizeof(g_ui.overlay_title), "%s", mc.title);
	snprintf(g_ui.overlay_text, sizeof(g_ui.overlay_text),
		 "%s", mc.detail_text[0] ? mc.detail_text : mc.subtitle);
	g_ui.overlay_card = mc;
	g_ui.overlay_is_video = it->is_video;
	g_ui.overlay_playing = it->is_video;
	g_ui.overlay_hud_dirty = true;
	g_ui.overlay_fps = 24;
	g_ui.overlay_total_frames = 360;
	g_ui.overlay_elapsed_sec = 0;
	g_ui.overlay_duration_sec = 15;
	shell_media_audio_stop();
	if (it->is_video) {
		shell_media_get_video_info(&g_ui.overlay_card,
					   &g_ui.overlay_fps,
					   &g_ui.overlay_total_frames,
					   &g_ui.overlay_duration_sec);
		shell_media_audio_start(&g_ui.overlay_card);
	}
	g_ui.overlay_frame_idx = 0;
	g_ui.overlay_last_tick_ms = shell_now_ms();
	g_ui.overlay_frame_tick_ms = g_ui.overlay_last_tick_ms;
}

static void open_media_overlay(const char *badge, const struct giri_media_card *mc, bool is_video)
{
	int g_idx = -1;

	if (mc && strncmp(mc->id, "gal_", 4) == 0 && g_ui.gallery_count > 0) {
		g_idx = atoi(mc->id + 4);
		open_gallery_item_overlay(g_idx);
		return;
	}

	g_ui.overlay_gallery_idx = -1;
	g_ui.tts_speaking = false;
	g_ui.active_win = GIRI_WIN_MEDIA_OVERLAY;
	snprintf(g_ui.overlay_badge, sizeof(g_ui.overlay_badge), "%s", badge);
	snprintf(g_ui.overlay_source, sizeof(g_ui.overlay_source),
		 "%s", current_screen()->summary.source_caption[0]
		       ? current_screen()->summary.source_caption
		       : "Velliangiri OS · Standardized Detail View");
	snprintf(g_ui.overlay_title, sizeof(g_ui.overlay_title), "%s", mc->title);
	snprintf(g_ui.overlay_text, sizeof(g_ui.overlay_text),
		 "%s", mc->detail_text[0] ? mc->detail_text : mc->subtitle);
	g_ui.overlay_card = *mc;
	g_ui.overlay_is_video = is_video;
	g_ui.overlay_playing = is_video;
	g_ui.overlay_hud_dirty = true;
	g_ui.overlay_fps = 24;
	g_ui.overlay_total_frames = 360;
	g_ui.overlay_elapsed_sec = 0;
	g_ui.overlay_duration_sec = 15;
	shell_media_audio_stop();
	if (is_video) {
		shell_media_get_video_info(&g_ui.overlay_card,
					   &g_ui.overlay_fps,
					   &g_ui.overlay_total_frames,
					   &g_ui.overlay_duration_sec);
		shell_media_audio_start(&g_ui.overlay_card);
	}
	g_ui.overlay_frame_idx = 0;
	g_ui.overlay_last_tick_ms = shell_now_ms();
	g_ui.overlay_frame_tick_ms = g_ui.overlay_last_tick_ms;
}

static void execute_action(const struct giri_action *act, const struct giri_media_card *card)
{
	if (!act)
		return;
	if (g_ui.is_working)
		complete_topic_transition();

	if (strcmp(act->type, "gallery_next_page") == 0) {
		populate_gallery_screen(&g_ui.history[g_ui.history_idx],
					g_ui.gallery_page + 1);
		g_ui.cards_need_preload = true;
		g_ui.dirty_slots |= 0x02;
		g_ui.dirty_cards = 0x0f;
		g_ui.dirty_footer |= (1U << 1);
		return;
	}

	if (strcmp(act->type, "open_prompt_box") == 0 ||
	    strcmp(act->capability, "ui.open_prompt") == 0) {
		g_ui.tts_speaking = false;
		g_ui.active_win = GIRI_WIN_ASK_MODAL;
		g_ui.ask_focus_idx = 0;
		g_ui.ask_input[0] = '\0';
		g_ui.ask_input_len = 0;
		return;
	}

	if (strcmp(act->type, "open_settings") == 0 ||
	    strcmp(act->capability, "system.open_settings") == 0) {
		g_ui.tts_speaking = false;
		refresh_system_metrics();
		g_ui.active_win = GIRI_WIN_SETTINGS_MODAL;
		g_ui.settings_focus_idx = 0;
		return;
	}

	if (strcmp(act->capability, "media.get_library") == 0) {
		if (launch_media_gallery_app())
			return;
	}

	if (strcmp(act->type, "launch_app") == 0 ||
	    strcmp(act->capability, "apps.launch") == 0) {
		if ((strcmp(act->app_id, "gallery") == 0 ||
		     strcmp(act->app_id, "media_gallery") == 0 ||
		     (card && strcasestr(card->title, "Gallery") != NULL)) &&
		    launch_media_gallery_app()) {
			return;
		}
		/* If /bin/doom exists on target TV, launch it directly with game_map=1 */
		if (strcmp(act->app_id, "doom") == 0 && access("/bin/doom", X_OK) == 0) {
			const char *const argv[] = { "/bin/doom", NULL };
			shell_app_run(argv, true);
			mark_full_redraw();
			return;
		}
		if (card) {
			struct giri_media_card app_card = *card;
			snprintf(app_card.detail_text, sizeof(app_card.detail_text),
				 "%.40s is running directly in 1280×720 fullscreen mode on Velliangiri OS with 6-button remote mapping active. Press Menu or Escape at any time to return to Giri.",
				 card->title);
			open_media_overlay("APP RUNNING", &app_card, false);
			snprintf(g_ui.overlay_title, sizeof(g_ui.overlay_title),
				 "%.40s — Fullscreen Game Session", card->title);
		}
		return;
	}

	if (strcmp(act->type, "open_image") == 0 ||
	    strcmp(act->capability, "media.open_image") == 0) {
		if (card)
			open_media_overlay("PHOTO VIEW", card, false);
		return;
	}

	if (strcmp(act->type, "play_video") == 0 ||
	    strcmp(act->capability, "media.play_video") == 0) {
		if (card)
			open_media_overlay("VIDEO STREAM", card, true);
		return;
	}

	const char *q = act->query[0] ? act->query
			: (card && card->title[0] ? card->title : "What is top in the news on BBC?");
	start_topic_transition(q);
}

static void handle_key_event(const struct shell_input_event *ev)
{
	/*
	 * If a topic shimmer transition is in flight:
	 *   - Pressing Menu/Back cancels the loading immediately in <0.5ms.
	 *   - Pressing any navigation/select key completes the transition
	 *     immediately (using non-blocking card art) and applies the key
	 *     to the new screen without dropping user input.
	 */
	if (g_ui.is_working) {
		if (ev->key == SHELL_KEY_MENU) {
			g_ui.is_working = false;
			g_ui.has_pending_screen = false;
			g_ui.logo_frame_idx = 0;
			g_ui.dirty_slots |= 0x03;
			return;
		}
		complete_topic_transition();
	}

	/* Global 2-second Menu hold returns to Home Screen from any state */
	if (ev->key == SHELL_KEY_MENU_HOLD) {
		g_ui.active_win = GIRI_WIN_MAIN;
		g_ui.history_idx = 0;
		g_ui.focus_zone = GIRI_ZONE_CARDS;
		g_ui.card_focus_idx = 0;
		g_ui.tts_speaking = false;
		mark_full_redraw();
		return;
	}

	/* Window 2: Ask Giri Modal navigation */
	if (g_ui.active_win == GIRI_WIN_ASK_MODAL) {
		int total = QUICK_PROMPTS_COUNT + 2; /* 0 = input, 1..7 = chips, 8 = Close */

		if (ev->key == SHELL_KEY_CHAR && g_ui.ask_focus_idx == 0) {
			if (g_ui.ask_input_len + 1 < (int)sizeof(g_ui.ask_input)) {
				g_ui.ask_input[g_ui.ask_input_len++] = ev->ch;
				g_ui.ask_input[g_ui.ask_input_len] = '\0';
			}
			return;
		}
		if (ev->key == SHELL_KEY_BACKSPACE && g_ui.ask_focus_idx == 0 && g_ui.ask_input_len > 0) {
			g_ui.ask_input[--g_ui.ask_input_len] = '\0';
			return;
		}

		switch (ev->key) {
		case SHELL_KEY_MENU:
			g_ui.active_win = GIRI_WIN_MAIN;
			mark_full_redraw();
			break;
		case SHELL_KEY_DOWN:
		case SHELL_KEY_RIGHT:
			g_ui.ask_focus_idx = (g_ui.ask_focus_idx + 1) % total;
			break;
		case SHELL_KEY_UP:
		case SHELL_KEY_LEFT:
			g_ui.ask_focus_idx = (g_ui.ask_focus_idx - 1 + total) % total;
			break;
		case SHELL_KEY_SELECT:
			if (g_ui.ask_focus_idx == 0) {
				const char *q = g_ui.ask_input_len > 0
						? g_ui.ask_input
						: quick_prompts[0].query;
				start_topic_transition(q);
			} else if (g_ui.ask_focus_idx >= 1 &&
				   g_ui.ask_focus_idx <= QUICK_PROMPTS_COUNT) {
				start_topic_transition(quick_prompts[g_ui.ask_focus_idx - 1].query);
			} else {
				g_ui.active_win = GIRI_WIN_MAIN;
				mark_full_redraw();
			}
			break;
		default:
			break;
		}
		return;
	}

	/* Window 3: Settings Modal navigation (matches prototype.html 2D grid rules) */
	if (g_ui.active_win == GIRI_WIN_SETTINGS_MODAL) {
		int total = 3 + g_ui.model_count + 1;
		int last_idx = total - 1;

		switch (ev->key) {
		case SHELL_KEY_MENU:
			g_ui.active_win = GIRI_WIN_MAIN;
			mark_full_redraw();
			break;
		case SHELL_KEY_LEFT:
			g_ui.settings_focus_idx = (g_ui.settings_focus_idx - 1 + total) % total;
			break;
		case SHELL_KEY_RIGHT:
			g_ui.settings_focus_idx = (g_ui.settings_focus_idx + 1) % total;
			break;
		case SHELL_KEY_DOWN:
			if (g_ui.settings_focus_idx <= 2) {
				g_ui.settings_focus_idx = (g_ui.settings_focus_idx == 0)
							  ? 3 : (4 < last_idx ? 4 : last_idx);
			} else if (g_ui.settings_focus_idx < last_idx) {
				int next = g_ui.settings_focus_idx + 2;
				g_ui.settings_focus_idx = (next < last_idx) ? next : last_idx;
			} else {
				g_ui.settings_focus_idx = 0;
			}
			break;
		case SHELL_KEY_UP:
			if (g_ui.settings_focus_idx == last_idx) {
				g_ui.settings_focus_idx = (last_idx - 2 >= 3) ? (last_idx - 2) : 3;
			} else if (g_ui.settings_focus_idx >= 3) {
				int off = g_ui.settings_focus_idx - 3;
				if (off < 2)
					g_ui.settings_focus_idx = (off == 0) ? 0 : 2;
				else
					g_ui.settings_focus_idx -= 2;
			} else {
				g_ui.settings_focus_idx = last_idx;
			}
			break;
		case SHELL_KEY_SELECT:
			if (g_ui.settings_focus_idx == 0) {
				g_ui.theme_id = (g_ui.theme_id == SHELL_THEME_PAPER)
						? SHELL_THEME_SIGNAL : SHELL_THEME_PAPER;
			} else if (g_ui.settings_focus_idx == 1) {
				g_ui.auto_read_aloud = !g_ui.auto_read_aloud;
			} else if (g_ui.settings_focus_idx == 2) {
				g_ui.nav_sound_enabled = !g_ui.nav_sound_enabled;
			} else if (g_ui.settings_focus_idx >= 3 &&
				   g_ui.settings_focus_idx < last_idx) {
				int m_idx = g_ui.settings_focus_idx - 3;
				struct giri_model_info *m = &g_ui.models[m_idx];

				if (m->installed) {
					g_ui.active_model_idx = m_idx;
				} else if (!m->downloading) {
					m->downloading = true;
					m->progress = 16;
					g_ui.model_dl_tick_ms = shell_now_ms();
				}
			} else if (g_ui.settings_focus_idx == last_idx) {
				g_ui.active_win = GIRI_WIN_MAIN;
				mark_full_redraw();
			}
			break;
		default:
			break;
		}
		return;
	}

	/* Window 1: Fullscreen Media Overlay navigation */
	if (g_ui.active_win == GIRI_WIN_MEDIA_OVERLAY) {
		switch (ev->key) {
		case SHELL_KEY_MENU:
		case SHELL_KEY_SELECT:
			shell_media_audio_stop();
			g_ui.active_win = GIRI_WIN_MAIN;
			mark_full_redraw();
			break;
		case SHELL_KEY_PLAY_PAUSE:
			if (g_ui.overlay_is_video) {
				g_ui.overlay_playing = !g_ui.overlay_playing;
				shell_media_audio_set_paused(!g_ui.overlay_playing);
				g_ui.overlay_hud_dirty = true;
				g_ui.overlay_frame_tick_ms = shell_now_ms();
			}
			break;
		case SHELL_KEY_UP:
			if (g_ui.overlay_gallery_idx >= 0 && g_ui.gallery_count > 0)
				open_gallery_item_overlay(g_ui.overlay_gallery_idx - 1);
			break;
		case SHELL_KEY_DOWN:
			if (g_ui.overlay_gallery_idx >= 0 && g_ui.gallery_count > 0)
				open_gallery_item_overlay(g_ui.overlay_gallery_idx + 1);
			break;
		case SHELL_KEY_LEFT:
			if (g_ui.overlay_is_video) {
				int fps = g_ui.overlay_fps > 0 ? g_ui.overlay_fps : 24;
				int tot = g_ui.overlay_total_frames > 0 ? g_ui.overlay_total_frames : 360;
				g_ui.overlay_elapsed_sec -= 3;
				if (g_ui.overlay_elapsed_sec < 0)
					g_ui.overlay_elapsed_sec = 0;
				g_ui.overlay_frame_idx = (g_ui.overlay_elapsed_sec * fps) % tot;
				shell_media_audio_seek_sec(g_ui.overlay_elapsed_sec);
				g_ui.overlay_hud_dirty = true;
				g_ui.overlay_frame_tick_ms = shell_now_ms();
			} else if (g_ui.overlay_gallery_idx >= 0 && g_ui.gallery_count > 0) {
				open_gallery_item_overlay(g_ui.overlay_gallery_idx - 1);
			}
			break;
		case SHELL_KEY_RIGHT:
			if (g_ui.overlay_is_video) {
				int fps = g_ui.overlay_fps > 0 ? g_ui.overlay_fps : 24;
				int tot = g_ui.overlay_total_frames > 0 ? g_ui.overlay_total_frames : 360;
				g_ui.overlay_elapsed_sec += 3;
				if (g_ui.overlay_elapsed_sec >= g_ui.overlay_duration_sec)
					g_ui.overlay_elapsed_sec = 0;
				g_ui.overlay_frame_idx = (g_ui.overlay_elapsed_sec * fps) % tot;
				shell_media_audio_seek_sec(g_ui.overlay_elapsed_sec);
				g_ui.overlay_hud_dirty = true;
				g_ui.overlay_frame_tick_ms = shell_now_ms();
			} else if (g_ui.overlay_gallery_idx >= 0 && g_ui.gallery_count > 0) {
				open_gallery_item_overlay(g_ui.overlay_gallery_idx + 1);
			}
			break;
		default:
			break;
		}
		return;
	}

	/* Window 0: Main 4-Slot Screen navigation */
	const struct giri_screen *scr = current_screen();
	int prev_idx;

	switch (ev->key) {
	case SHELL_KEY_LEFT:
		if (g_ui.focus_zone == GIRI_ZONE_CARDS) {
			prev_idx = g_ui.card_focus_idx;
			g_ui.card_focus_idx = (g_ui.card_focus_idx + SHELL_CARD_COUNT - 1) % SHELL_CARD_COUNT;
			g_ui.dirty_cards |= (uint8_t)((1U << prev_idx) | (1U << g_ui.card_focus_idx));
		} else {
			prev_idx = g_ui.footer_focus_idx;
			g_ui.footer_focus_idx = (g_ui.footer_focus_idx + SHELL_FOOTER_COUNT - 1) % SHELL_FOOTER_COUNT;
			g_ui.dirty_footer |= (uint8_t)((1U << prev_idx) | (1U << g_ui.footer_focus_idx));
		}
		break;
	case SHELL_KEY_RIGHT:
		if (g_ui.focus_zone == GIRI_ZONE_CARDS) {
			prev_idx = g_ui.card_focus_idx;
			g_ui.card_focus_idx = (g_ui.card_focus_idx + 1) % SHELL_CARD_COUNT;
			g_ui.dirty_cards |= (uint8_t)((1U << prev_idx) | (1U << g_ui.card_focus_idx));
		} else {
			prev_idx = g_ui.footer_focus_idx;
			g_ui.footer_focus_idx = (g_ui.footer_focus_idx + 1) % SHELL_FOOTER_COUNT;
			g_ui.dirty_footer |= (uint8_t)((1U << prev_idx) | (1U << g_ui.footer_focus_idx));
		}
		break;
	case SHELL_KEY_DOWN:
		if (g_ui.focus_zone == GIRI_ZONE_CARDS) {
			g_ui.focus_zone = GIRI_ZONE_FOOTER;
			g_ui.dirty_cards |= (uint8_t)(1U << g_ui.card_focus_idx);
			g_ui.dirty_footer |= (uint8_t)(1U << g_ui.footer_focus_idx);
		}
		break;
	case SHELL_KEY_UP:
		if (g_ui.focus_zone == GIRI_ZONE_FOOTER) {
			g_ui.focus_zone = GIRI_ZONE_CARDS;
			g_ui.dirty_cards |= (uint8_t)(1U << g_ui.card_focus_idx);
			g_ui.dirty_footer |= (uint8_t)(1U << g_ui.footer_focus_idx);
		}
		break;
	case SHELL_KEY_PLAY_PAUSE:
		if (g_ui.tts_speaking) {
			g_ui.tts_speaking = false;
		} else {
			g_ui.tts_speaking = true;
			g_ui.tts_sentence_idx = 0;
			g_ui.tts_last_step_ms = shell_now_ms();
		}
		g_ui.dirty_slots |= 0x02;
		break;
	case SHELL_KEY_SELECT:
		if (g_ui.focus_zone == GIRI_ZONE_CARDS) {
			const struct giri_media_card *mc = &scr->media_cards[g_ui.card_focus_idx];
			execute_action(&mc->action, mc);
		} else {
			const struct giri_footer_link *fl = &scr->footer_links[g_ui.footer_focus_idx];
			execute_action(&fl->action, NULL);
		}
		break;
	case SHELL_KEY_MENU:
		if (g_ui.tts_speaking) {
			g_ui.tts_speaking = false;
			g_ui.dirty_slots |= 0x02;
		} else if (g_ui.history_idx > 0) {
			g_ui.history_idx--;
			g_ui.focus_zone = GIRI_ZONE_CARDS;
			g_ui.card_focus_idx = 0;
			g_ui.cards_need_preload = true;
			mark_full_redraw();
		}
		break;
	default:
		break;
	}
}

/* Advance animations (65ms ping-pong logo, Stage-1 shimmer, TTS sentence highlight, model download) */
static bool tick_animations(uint64_t now)
{
	bool dirty = false;
	int i;

	if (g_ui.is_working) {
		g_ui.dirty_slots |= 0x02; /* Shimmer bar animates in Slot 2 only */
		dirty = true;
		if (now - g_ui.last_logo_step_ms >= SHELL_LOGO_FRAME_MS) {
			g_ui.last_logo_step_ms = now;
			g_ui.logo_step_cursor = (g_ui.logo_step_cursor + 1) & 7;
			g_ui.logo_frame_idx = logo_pingpong_steps[g_ui.logo_step_cursor];
			g_ui.dirty_slots |= 0x01;
		}
		if (now - g_ui.working_start_ms >= 260) {
			complete_topic_transition();
			dirty = true;
		}
	}

	if (g_ui.tts_speaking && now - g_ui.tts_last_step_ms >= 2200) {
		g_ui.tts_last_step_ms = now;
		g_ui.tts_sentence_idx++;
		if (g_ui.tts_sentence_idx >= g_ui.tts_total_sentences)
			g_ui.tts_speaking = false;
		g_ui.dirty_slots |= 0x02;
		dirty = true;
	}

	if (g_ui.active_win == GIRI_WIN_MEDIA_OVERLAY &&
	    g_ui.overlay_is_video && g_ui.overlay_playing) {
		int fps = g_ui.overlay_fps > 0 ? g_ui.overlay_fps : 24;
		int tot = g_ui.overlay_total_frames > 0 ? g_ui.overlay_total_frames : 360;
		uint64_t frame_ms = 1000ULL / (uint64_t)fps;
		int aud_fidx = shell_media_audio_get_frame_idx(fps, tot);

		if (aud_fidx >= 0) {
			if (aud_fidx != g_ui.overlay_frame_idx) {
				int new_sec;

				g_ui.overlay_frame_idx = aud_fidx;
				g_ui.overlay_frame_tick_ms = now;
				new_sec = g_ui.overlay_frame_idx / fps;
				if (new_sec != g_ui.overlay_elapsed_sec) {
					g_ui.overlay_elapsed_sec = new_sec;
					g_ui.overlay_hud_dirty = true;
				}
				dirty = true;
			}
		} else if (now - g_ui.overlay_frame_tick_ms >= frame_ms) {
			uint64_t steps = (now - g_ui.overlay_frame_tick_ms) / frame_ms;
			int new_sec;

			if (steps > 4) {
				steps = 1;
				g_ui.overlay_frame_tick_ms = now;
			} else {
				g_ui.overlay_frame_tick_ms += steps * frame_ms;
			}
			g_ui.overlay_frame_idx = (g_ui.overlay_frame_idx + (int)steps) % tot;
			new_sec = g_ui.overlay_frame_idx / fps;
			if (new_sec != g_ui.overlay_elapsed_sec) {
				g_ui.overlay_elapsed_sec = new_sec;
				g_ui.overlay_hud_dirty = true;
			}
			dirty = true;
		}
	}

	for (i = 0; i < g_ui.model_count; i++) {
		if (g_ui.models[i].downloading && now - g_ui.model_dl_tick_ms >= 320) {
			g_ui.model_dl_tick_ms = now;
			g_ui.models[i].progress += 28;
			if (g_ui.models[i].progress >= 100) {
				g_ui.models[i].progress = 100;
				g_ui.models[i].downloading = false;
				g_ui.models[i].installed = true;
				g_ui.active_model_idx = i;
			}
			dirty = true;
		}
	}

	return dirty;
}

/* Ensure only a single /bin/tvshell instance owns /dev/input and /dev/fb0 */
static int acquire_single_instance_lock(void)
{
	int fd = open("/tmp/tvshell.lock", O_CREAT | O_RDWR, 0644);
	char buf[32];

	if (fd < 0)
		return -1;

	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = '\0';
			long old_pid = strtol(buf, NULL, 10);
			if (old_pid > 1 && old_pid != (long)getpid()) {
				kill((pid_t)old_pid, SIGTERM);
				usleep(80000);
				kill((pid_t)old_pid, SIGKILL);
				usleep(40000);
			}
		}
		(void)flock(fd, LOCK_EX);
	}

	(void)ftruncate(fd, 0);
	(void)lseek(fd, 0, SEEK_SET);
	int len = snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
	if (len > 0)
		(void)write(fd, buf, (size_t)len);
	return fd;
}

static uint64_t bench_now_us(void)
{
	struct timeval tv;

	if (gettimeofday(&tv, NULL) == 0)
		return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
	return shell_now_ms() * 1000ULL;
}

static void run_benchmark(uint64_t init_disp_us, uint64_t init_font_us, uint64_t init_media_us)
{
	uint64_t t0, t1;
	int i;
	const int N = 30;

	printf("=== tvshell on-device benchmark (%d iterations per test) ===\n", N);
	printf("init_display            : %8.2f ms\n", init_disp_us / 1000.0);
	printf("init_font               : %8.2f ms\n", init_font_us / 1000.0);
	printf("init_media              : %8.2f ms\n", init_media_us / 1000.0);

	/* 1. Full 1280x720 Main Window Redraw */
	g_ui.active_win = GIRI_WIN_MAIN;
	g_ui.is_working = false;
	t0 = bench_now_us();
	for (i = 0; i < N; i++) {
		mark_full_redraw();
		render_frame();
	}
	t1 = bench_now_us();
	printf("main_full_redraw        : %8.2f ms (%6.1f FPS)\n",
	       (t1 - t0) / (1000.0 * N), (1000000.0 * N) / (double)(t1 - t0));

	/* 2. Incremental Card Focus Move (2 cards redrawn) */
	g_ui.focus_zone = GIRI_ZONE_CARDS;
	t0 = bench_now_us();
	for (i = 0; i < N; i++) {
		int prev = g_ui.card_focus_idx;
		int next = (prev + 1) & 3;
		g_ui.card_focus_idx = next;
		g_ui.dirty_cards = (uint8_t)((1u << prev) | (1u << next));
		render_frame();
	}
	t1 = bench_now_us();
	printf("card_focus_move (inc)   : %8.2f ms (%6.1f FPS)\n",
	       (t1 - t0) / (1000.0 * N), (1000000.0 * N) / (double)(t1 - t0));

	/* 3. Incremental Footer Button Focus Move (2 buttons redrawn) */
	g_ui.focus_zone = GIRI_ZONE_FOOTER;
	t0 = bench_now_us();
	for (i = 0; i < N; i++) {
		int prev = g_ui.footer_focus_idx;
		int next = (prev + 1) & 3;
		g_ui.footer_focus_idx = next;
		g_ui.dirty_footer = (uint8_t)((1u << prev) | (1u << next));
		render_frame();
	}
	t1 = bench_now_us();
	printf("footer_focus_move (inc) : %8.2f ms (%6.1f FPS)\n",
	       (t1 - t0) / (1000.0 * N), (1000000.0 * N) / (double)(t1 - t0));

	/* 4. Working / Loading Shimmer Tick (Slots 1 + 2 only) */
	g_ui.is_working = true;
	t0 = bench_now_us();
	for (i = 0; i < N; i++) {
		g_ui.logo_frame_idx = i % 5;
		g_ui.dirty_slots = 0x03;
		render_frame();
	}
	t1 = bench_now_us();
	g_ui.is_working = false;
	printf("working_shimmer_tick    : %8.2f ms (%6.1f FPS)\n",
	       (t1 - t0) / (1000.0 * N), (1000000.0 * N) / (double)(t1 - t0));

	/* 5. Ask Giri Modal Open & D-Pad Navigation */
	g_ui.active_win = GIRI_WIN_MAIN;
	mark_full_redraw();
	render_frame();
	g_ui.active_win = GIRI_WIN_ASK_MODAL;
	g_ui.backbuf_modal_win = -1;
	t0 = bench_now_us();
	render_frame();
	t1 = bench_now_us();
	printf("ask_modal_open          : %8.2f ms (%6.1f FPS)\n",
	       (t1 - t0) / 1000.0, 1000000.0 / (double)(t1 - t0));

	t0 = bench_now_us();
	for (i = 0; i < N; i++) {
		g_ui.ask_focus_idx = 1 + (i % 7);
		render_frame();
	}
	t1 = bench_now_us();
	printf("ask_modal_nav (inc)     : %8.2f ms (%6.1f FPS)\n",
	       (t1 - t0) / (1000.0 * N), (1000000.0 * N) / (double)(t1 - t0));

	/* 6. Settings Modal Open & D-Pad Navigation */
	g_ui.active_win = GIRI_WIN_MAIN;
	mark_full_redraw();
	render_frame();
	g_ui.active_win = GIRI_WIN_SETTINGS_MODAL;
	g_ui.backbuf_modal_win = -1;
	t0 = bench_now_us();
	render_frame();
	t1 = bench_now_us();
	printf("settings_modal_open     : %8.2f ms (%6.1f FPS)\n",
	       (t1 - t0) / 1000.0, 1000000.0 / (double)(t1 - t0));

	t0 = bench_now_us();
	for (i = 0; i < N; i++) {
		g_ui.settings_focus_idx = 3 + (i % 6);
		render_frame();
	}
	t1 = bench_now_us();
	printf("settings_modal_nav (inc): %8.2f ms (%6.1f FPS)\n",
	       (t1 - t0) / (1000.0 * N), (1000000.0 * N) / (double)(t1 - t0));

	/* 7. Non-blocking Topic Transition + Progressive Card Preload */
	g_ui.active_win = GIRI_WIN_MAIN;
	g_ui.allow_async_art = true;
	mark_full_redraw();
	render_frame();
	t0 = bench_now_us();
	start_topic_transition("Tell me about the Western Ghats");
	render_frame();
	t1 = bench_now_us();
	printf("topic_transition_start  : %8.2f ms (%6.1f FPS)\n",
	       (t1 - t0) / 1000.0, 1000000.0 / (double)(t1 - t0));

	/* 8. Hardware V4L2 JPEG + M2SC Card & Video Frame Offload */
	t0 = bench_now_us();
	int preloaded = 0;
	while (shell_media_preload_next_card(&g_ui.pending_screen))
		preloaded++;
	t1 = bench_now_us();
	if (preloaded > 0) {
		printf("hw_card_jpeg_scale (%d)  : %8.2f ms/card\n",
		       preloaded, (t1 - t0) / (1000.0 * preloaded));
	}

	{
		struct giri_media_card vcard;
		const int VN = 60;

		memset(&vcard, 0, sizeof(vcard));
		snprintf(vcard.uri, sizeof(vcard.uri), "GX016305.mov");
		snprintf(vcard.title, sizeof(vcard.title), "Lagoon Speedboat Clip");
		snprintf(vcard.subtitle, sizeof(vcard.subtitle), "Turquoise Reef · 720p Video");
		vcard.kind = GIRI_CARD_LOCAL_VIDEO;
		vcard.preset_art = GIRI_ART_MONSOON_VIDEO;
		t0 = bench_now_us();
		for (i = 0; i < VN; i++)
			(void)shell_media_get_video_frame(&vcard, i);
		t1 = bench_now_us();
		printf("hw_video_720p_decode    : %8.2f ms/frame (%6.1f FPS)\n",
		       (t1 - t0) / (1000.0 * VN), (1000000.0 * VN) / (double)(t1 - t0));

		open_media_overlay("VIDEO STREAM", &vcard, true);
		shell_media_audio_stop();
		render_frame();
		t0 = bench_now_us();
		for (i = 1; i <= VN; i++) {
			g_ui.overlay_frame_idx = i;
			if ((i % 24) == 0) {
				g_ui.overlay_elapsed_sec = i / 24;
				g_ui.overlay_hud_dirty = true;
			}
			render_frame();
		}
		t1 = bench_now_us();
		printf("video_overlay_playback  : %8.2f ms/frame (%6.1f FPS)\n",
		       (t1 - t0) / (1000.0 * VN), (1000000.0 * VN) / (double)(t1 - t0));
	}
}

int main(int argc, char **argv)
{
	bool headless = false;
	bool bench_mode = false;
	const char *dump_ppm_path = NULL;
	const char *initial_topic = NULL;
	uint64_t t0, init_disp_us = 0, init_font_us = 0, init_media_us = 0;
	int lock_fd = -1;
	int i;

	memset(&g_ui, 0, sizeof(g_ui));
	g_ui.theme_id = SHELL_THEME_PAPER;
	g_ui.active_win = GIRI_WIN_MAIN;
	g_ui.focus_zone = GIRI_ZONE_CARDS;
	g_ui.nav_sound_enabled = true;
	g_ui.overlay_gallery_idx = -1;
	mark_full_redraw();

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--headless") == 0) {
			headless = true;
		} else if (strcmp(argv[i], "--bench") == 0) {
			bench_mode = true;
			headless = true;
		} else if (strcmp(argv[i], "--bench-display") == 0) {
			bench_mode = true;
			headless = false;
		} else if (strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
			i++;
			if (strcasecmp(argv[i], "signal") == 0)
				g_ui.theme_id = SHELL_THEME_SIGNAL;
			else
				g_ui.theme_id = SHELL_THEME_PAPER;
		} else if (strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
			i++;
			if (strcasecmp(argv[i], "overlay") == 0)
				g_ui.active_win = GIRI_WIN_MEDIA_OVERLAY;
			else if (strcasecmp(argv[i], "ask") == 0)
				g_ui.active_win = GIRI_WIN_ASK_MODAL;
			else if (strcasecmp(argv[i], "settings") == 0)
				g_ui.active_win = GIRI_WIN_SETTINGS_MODAL;
			else
				g_ui.active_win = GIRI_WIN_MAIN;
		} else if (strcmp(argv[i], "--topic") == 0 && i + 1 < argc) {
			initial_topic = argv[++i];
		} else if (strcmp(argv[i], "--focus-footer") == 0 && i + 1 < argc) {
			g_ui.focus_zone = GIRI_ZONE_FOOTER;
			g_ui.footer_focus_idx = atoi(argv[++i]) & 3;
		} else if (strcmp(argv[i], "--focus-card") == 0 && i + 1 < argc) {
			g_ui.focus_zone = GIRI_ZONE_CARDS;
			g_ui.card_focus_idx = atoi(argv[++i]) & 3;
		} else if (strcmp(argv[i], "--tts-demo") == 0) {
			g_ui.tts_speaking = true;
			g_ui.tts_sentence_idx = 0;
		} else if (strcmp(argv[i], "--dump-ppm") == 0 && i + 1 < argc) {
			dump_ppm_path = argv[++i];
			headless = true;
		}
	}

	signal(SIGINT, handle_sig);
	signal(SIGTERM, handle_sig);

	if (!headless)
		lock_fd = acquire_single_instance_lock();

	t0 = bench_now_us();
	if (shell_display_init(headless) < 0)
		return 1;
	init_disp_us = bench_now_us() - t0;

	t0 = bench_now_us();
	if (shell_font_init() < 0) {
		shell_display_close();
		return 1;
	}
	init_font_us = bench_now_us() - t0;

	t0 = bench_now_us();
	shell_media_init();
	init_media_us = bench_now_us() - t0;
	shell_input_init(headless);

	load_supported_models();
	load_apps_catalog();
	refresh_system_metrics();

	if (initial_topic)
		build_topic_screen(initial_topic, &g_ui.history[0]);
	else
		build_home_screen(&g_ui.history[0]);
	g_ui.history_len = 1;
	g_ui.history_idx = 0;

	if (g_ui.active_win == GIRI_WIN_MEDIA_OVERLAY) {
		open_media_overlay("VIDEO STREAM", &g_ui.history[0].media_cards[0],
				   g_ui.history[0].media_cards[0].kind == GIRI_CARD_PAGE_VIDEO ||
				   g_ui.history[0].media_cards[0].kind == GIRI_CARD_LOCAL_VIDEO);
	}

	render_frame();

	if (bench_mode) {
		run_benchmark(init_disp_us, init_font_us, init_media_us);
		shell_input_close();
		shell_media_free();
		shell_font_free();
		shell_display_close();
		if (lock_fd >= 0)
			close(lock_fd);
		return 0;
	}

	if (dump_ppm_path) {
		int rc = shell_display_dump_ppm(dump_ppm_path);
		shell_input_close();
		shell_media_free();
		shell_font_free();
		shell_display_close();
		return rc == 0 ? 0 : 1;
	}

	g_ui.allow_async_art = true;
	while (g_running) {
		struct shell_input_event ev;
		int timeout_ms = (g_ui.is_working || g_ui.cards_need_preload) ? 16 :
				 (g_ui.tts_speaking ||
				  g_ui.active_win == GIRI_WIN_SETTINGS_MODAL) ? 30 : 250;
		bool dirty = false;

		if (g_ui.active_win == GIRI_WIN_MEDIA_OVERLAY &&
		    g_ui.overlay_is_video && g_ui.overlay_playing) {
			int fps = g_ui.overlay_fps > 0 ? g_ui.overlay_fps : 24;
			uint64_t frame_ms = 1000ULL / (uint64_t)fps;
			uint64_t now = shell_now_ms();
			uint64_t elapsed = (now >= g_ui.overlay_frame_tick_ms)
					   ? (now - g_ui.overlay_frame_tick_ms) : 0;
			timeout_ms = (elapsed >= frame_ms) ? 0 : (int)(frame_ms - elapsed);
		}

		if (shell_input_poll(&ev, timeout_ms)) {
			handle_key_event(&ev);
			while (shell_input_poll(&ev, 0))
				handle_key_event(&ev);
			dirty = true;
		}
		if (shell_media_poll_async()) {
			g_ui.dirty_cards = 0x0f;
			dirty = true;
		}
		if (tick_animations(shell_now_ms()))
			dirty = true;

		/*
		 * Progressively decode at most 1 uncached card JPEG per idle
		 * tick so button presses are never blocked by batch decoding.
		 */
		if (!dirty && g_ui.is_working && g_ui.has_pending_screen) {
			(void)shell_media_preload_next_card(&g_ui.pending_screen);
		} else if (!dirty && g_ui.cards_need_preload &&
			   g_ui.active_win == GIRI_WIN_MAIN) {
			if (shell_media_preload_next_card(current_screen())) {
				g_ui.dirty_cards = 0x0f;
				dirty = true;
			} else {
				g_ui.cards_need_preload = false;
			}
		}

		if (dirty)
			render_frame();
	}

	shell_input_close();
	shell_media_free();
	shell_font_free();
	shell_display_close();
	if (lock_fd >= 0)
		close(lock_fd);
	return 0;
}
