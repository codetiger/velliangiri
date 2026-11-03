// SPDX-License-Identifier: GPL-2.0-only
/*
 * Velliangiri 1280x720 TV Shell (/bin/tvshell) - Phase 1 Interface & Supervisor
 *
 * Implements the 1280x720 flat-colour TV shell specified in
 * tools/tvshell/design/{tokens.json,design-system.html}:
 *   - Two themes: Paper (Literata 400/600) and Signal (Atkinson Hyperlegible 400/700)
 *   - First-start theme picker persisted to /mnt/data/linux/etc/shell.conf
 *   - Header (49x40 logo with 500 ms working blink, question, turn counter, rule)
 *   - Answer (<= 8 lines), Sources, Value (64px), Notice, Steps (working progress)
 *   - Item rows (<= 5), Media tiles (4 x 248x140), Full-screen 1280x720 photo viewer
 *   - Choices row with contextual item/tile action prefixing & Ask / More ideas
 *   - Ask line (44px) for Bluetooth keyboards
 *   - Native external app launcher for /bin/doom with 2-second Menu hold return
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "tvshell.h"
#include "shell_assets.h"

enum shell_mode {
	MODE_THEME_PICKER = 0,
	MODE_TURN_VIEW,
	MODE_ASK_LINE,
	MODE_IMAGE_VIEWER,
};

static struct {
	struct shell_config cfg;
	enum shell_mode mode;
	struct shell_turn turns[SHELL_MAX_TURNS];
	int num_turns;
	int cur_turn;

	/* Working / step animation state */
	bool logo_blink_alt;
	uint64_t last_blink_ms;
	uint64_t working_start_ms;

	/* Notice toast */
	char notice_text[64];
	enum shell_symbol_id notice_sym;
	uint64_t notice_until_ms;

	/* Ask line buffer */
	char ask_buf[SHELL_ASK_MAX_LEN];
	int ask_len;

	/* Full-screen Image Viewer state */
	int viewer_tile_idx;
	bool viewer_chip_visible;
	uint64_t viewer_chip_until_ms;
	uint32_t *viewer_pixels;
} app;

static uint64_t monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void mkdir_p(const char *path)
{
	char tmp[256];
	char *p = NULL;
	size_t len;

	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp);
	if (len > 0 && tmp[len - 1] == '/')
		tmp[len - 1] = '\0';
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			(void)mkdir(tmp, 0755);
			*p = '/';
		}
	}
	(void)mkdir(tmp, 0755);
}

static void config_load(struct shell_config *cfg)
{
	FILE *fp;
	char line[256];

	memset(cfg, 0, sizeof(*cfg));
	cfg->theme = SHELL_THEME_PAPER;
	cfg->theme_set = false;
	snprintf(cfg->llm_mode, sizeof(cfg->llm_mode), "auto");

	fp = fopen(SHELL_CONF_PATH, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *eq = strchr(line, '=');
		char *nl;

		if (!eq || line[0] == '#')
			continue;
		*eq++ = '\0';
		nl = strpbrk(eq, "\r\n");
		if (nl)
			*nl = '\0';
		if (!strcmp(line, "THEME")) {
			cfg->theme = (!strcasecmp(eq, "signal")) ?
				     SHELL_THEME_SIGNAL : SHELL_THEME_PAPER;
			cfg->theme_set = true;
		} else if (!strcmp(line, "LLM_MODE")) {
			snprintf(cfg->llm_mode, sizeof(cfg->llm_mode), "%s", eq);
		} else if (!strcmp(line, "LLM_URL")) {
			snprintf(cfg->llm_url, sizeof(cfg->llm_url), "%s", eq);
		} else if (!strcmp(line, "LLM_MODEL_PATH")) {
			snprintf(cfg->llm_model_path, sizeof(cfg->llm_model_path), "%s", eq);
		}
	}
	fclose(fp);
}

static void config_save(const struct shell_config *cfg)
{
	char tmp_path[256];
	FILE *fp;

	mkdir_p("/mnt/data/linux/etc");
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", SHELL_CONF_PATH);
	fp = fopen(tmp_path, "w");
	if (!fp)
		return;
	fprintf(fp, "THEME=%s\n",
		cfg->theme == SHELL_THEME_SIGNAL ? "signal" : "paper");
	fprintf(fp, "LLM_MODE=%s\n", cfg->llm_mode);
	if (cfg->llm_url[0])
		fprintf(fp, "LLM_URL=%s\n", cfg->llm_url);
	if (cfg->llm_model_path[0])
		fprintf(fp, "LLM_MODEL_PATH=%s\n", cfg->llm_model_path);
	fflush(fp);
	fsync(fileno(fp));
	fclose(fp);
	(void)rename(tmp_path, SHELL_CONF_PATH);
}

static void seed_sample_notes(void)
{
	struct stat st;

	mkdir_p(SHELL_NOTES_DIR);
	mkdir_p(SHELL_PHOTOS_DIR);
	mkdir_p(SHELL_THUMBS_DIR);
	mkdir_p("/mnt/data/linux/games/doom/saves");

	if (stat(SHELL_NOTES_DIR "/shopping-list.txt", &st) != 0) {
		FILE *f = fopen(SHELL_NOTES_DIR "/shopping-list.txt", "w");
		if (f) {
			fprintf(f, "Bread flour, whole rye flour, sea salt, olive oil, and parchment paper for the weekend bake.\n");
			fclose(f);
		}
	}
	if (stat(SHELL_NOTES_DIR "/wifi-setup.txt", &st) != 0) {
		FILE *f = fopen(SHELL_NOTES_DIR "/wifi-setup.txt", "w");
		if (f) {
			fprintf(f, "Use '/bin/wifi scan' and '/bin/wifi connect SSID PSK' over SSH or serial. Credentials persist in /mnt/data/linux/etc/wpa_supplicant.\n");
			fclose(f);
		}
	}
}

/* Read live system sensors for the System Status turn */
static void update_system_status_turn(struct shell_turn *t)
{
	int temp_mc = 41000;
	FILE *f;

	f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
	if (!f)
		f = fopen("/sys/class/hwmon/hwmon0/temp1_input", "r");
	if (f) {
		if (fscanf(f, "%d", &temp_mc) != 1)
			temp_mc = 41000;
		fclose(f);
	}
	int temp_c = (temp_mc + 500) / 1000;
	snprintf(t->value_big, sizeof(t->value_big), "%d \xc2\xb0""C", temp_c);
	snprintf(t->value_label, sizeof(t->value_label),
		 "System temperature \xc2\xb7 %s",
		 temp_c < 70 ? "normal" : "warm");

	/* Memory from /proc/meminfo */
	long mem_total_kb = 512 * 1024, mem_avail_kb = 384 * 1024;
	f = fopen("/proc/meminfo", "r");
	if (f) {
		char line[128];
		while (fgets(line, sizeof(line), f)) {
			sscanf(line, "MemTotal: %ld kB", &mem_total_kb);
			sscanf(line, "MemAvailable: %ld kB", &mem_avail_kb);
		}
		fclose(f);
	}
	snprintf(t->items[0].detail, sizeof(t->items[0].detail),
		 "%ld MiB free of %ld MiB", mem_avail_kb / 1024, mem_total_kb / 1024);

	/* Uptime from /proc/uptime */
	double up_sec = 0.0;
	f = fopen("/proc/uptime", "r");
	if (f) {
		(void)fscanf(f, "%lf", &up_sec);
		fclose(f);
	}
	int mins = (int)(up_sec / 60.0);
	int secs = (int)up_sec % 60;
	snprintf(t->items[2].detail, sizeof(t->items[2].detail),
		 "Up %dm %02ds \xc2\xb7 1280\xc3\x97""720@30", mins, secs);
}

static void init_predefined_turns(void)
{
	struct shell_turn *t;
	int i;

	memset(app.turns, 0, sizeof(app.turns));
	app.num_turns = 6;
	app.cur_turn = 0;

	/* Turn 1: Home / Applications */
	t = &app.turns[0];
	snprintf(t->question, sizeof(t->question), "What would you like to do today?");
	snprintf(t->answer, sizeof(t->answer),
		 "Velliangiri is ready. Pick an application below, check system health, or explore media and notes.");
	t->num_items = 3;
	t->items[0].symbol = SHELL_SYM_GAME;
	snprintf(t->items[0].title, sizeof(t->items[0].title), "Doom");
	snprintf(t->items[0].detail, sizeof(t->items[0].detail), "Game \xc2\xb7 1280\xc3\x97""720 \xc2\xb7 HDMI audio");
	snprintf(t->items[0].path, sizeof(t->items[0].path), "/bin/doom \xc2\xb7 saves in /mnt/data/linux/games/doom");
	t->items[0].num_actions = 3;
	t->items[0].actions[0] = (struct shell_choice){ SHELL_SYM_GAME, "Play Doom", SHELL_ACT_LAUNCH_DOOM, 0 };
	t->items[0].actions[1] = (struct shell_choice){ SHELL_SYM_NONE, "Controls guide", SHELL_ACT_GOTO_TURN, 1 };
	t->items[0].actions[2] = (struct shell_choice){ SHELL_SYM_FOLDER, "Show where it is", SHELL_ACT_SHOW_PATH, 0 };

	t->items[1].symbol = SHELL_SYM_PAGE;
	snprintf(t->items[1].title, sizeof(t->items[1].title), "System status & temperature");
	snprintf(t->items[1].detail, sizeof(t->items[1].detail), "Sensors, memory & network");
	snprintf(t->items[1].path, sizeof(t->items[1].path), "/sys/class/hwmon/hwmon0");
	t->items[1].num_actions = 2;
	t->items[1].actions[0] = (struct shell_choice){ SHELL_SYM_NONE, "Open status", SHELL_ACT_GOTO_TURN, 2 };
	t->items[1].actions[1] = (struct shell_choice){ SHELL_SYM_FOLDER, "Show where it is", SHELL_ACT_SHOW_PATH, 1 };

	t->items[2].symbol = SHELL_SYM_FOLDER;
	snprintf(t->items[2].title, sizeof(t->items[2].title), "Media & photo gallery");
	snprintf(t->items[2].detail, sizeof(t->items[2].detail), "4 items \xc2\xb7 hardware scaler");
	snprintf(t->items[2].path, sizeof(t->items[2].path), "/mnt/data/linux/media/photos");
	t->items[2].num_actions = 2;
	t->items[2].actions[0] = (struct shell_choice){ SHELL_SYM_NONE, "Browse media", SHELL_ACT_GOTO_TURN, 3 };
	t->items[2].actions[1] = (struct shell_choice){ SHELL_SYM_FOLDER, "Show where it is", SHELL_ACT_SHOW_PATH, 2 };

	t->num_choices = 3;
	t->choices[0] = (struct shell_choice){ SHELL_SYM_GAME, "Play Doom", SHELL_ACT_LAUNCH_DOOM, 0 };
	t->choices[1] = (struct shell_choice){ SHELL_SYM_NONE, "System status", SHELL_ACT_GOTO_TURN, 2 };
	t->choices[2] = (struct shell_choice){ SHELL_SYM_NONE, "Switch theme", SHELL_ACT_SWITCH_THEME, 0 };
	t->focus_zone = SHELL_FOCUS_CHOICES;
	t->focus_choice_idx = 0;

	/* Turn 2: Play Doom on S5L8947X */
	t = &app.turns[1];
	snprintf(t->question, sizeof(t->question), "Play Doom on S5L8947X");
	snprintf(t->answer, sizeof(t->answer),
		 "Doom runs natively at 1280\xc3\x97""720 with 48 kHz HDMI audio. On the remote: arrows move and turn, OK fires, Play opens doors (double-tap switches weapons), and holding Menu for 2 seconds returns here.");
	snprintf(t->sources, sizeof(t->sources),
		 "Saves kept in /mnt/data/linux/games/doom");
	t->num_choices = 3;
	t->choices[0] = (struct shell_choice){ SHELL_SYM_GAME, "Launch Doom", SHELL_ACT_LAUNCH_DOOM, 0 };
	t->choices[1] = (struct shell_choice){ SHELL_SYM_GAME, "Warp to E1M1", SHELL_ACT_LAUNCH_DOOM, 1 };
	t->choices[2] = (struct shell_choice){ SHELL_SYM_GAME, "Play Widescreen", SHELL_ACT_LAUNCH_DOOM, 2 };
	t->focus_zone = SHELL_FOCUS_CHOICES;
	t->focus_choice_idx = 0;

	/* Turn 3: System Status (64px Value + Item rows) */
	t = &app.turns[2];
	snprintf(t->question, sizeof(t->question), "How is the system running?");
	t->num_items = 3;
	t->items[0].symbol = SHELL_SYM_PAGE;
	snprintf(t->items[0].title, sizeof(t->items[0].title), "Memory & zram swap");
	snprintf(t->items[0].path, sizeof(t->items[0].path), "/proc/meminfo \xc2\xb7 /dev/zram0");
	t->items[0].num_actions = 2;
	t->items[0].actions[0] = (struct shell_choice){ SHELL_SYM_NONE, "Refresh status", SHELL_ACT_REFRESH_STATUS, 0 };
	t->items[0].actions[1] = (struct shell_choice){ SHELL_SYM_FOLDER, "Show where it is", SHELL_ACT_SHOW_PATH, 0 };

	t->items[1].symbol = SHELL_SYM_PAGE;
	snprintf(t->items[1].title, sizeof(t->items[1].title), "Persistent storage");
	snprintf(t->items[1].detail, sizeof(t->items[1].detail), "nand0p2 mounted at /mnt/data/linux");
	snprintf(t->items[1].path, sizeof(t->items[1].path), "/mnt/data/linux");
	t->items[1].num_actions = 2;
	t->items[1].actions[0] = (struct shell_choice){ SHELL_SYM_NONE, "Show notes", SHELL_ACT_GOTO_TURN, 5 };
	t->items[1].actions[1] = (struct shell_choice){ SHELL_SYM_FOLDER, "Show where it is", SHELL_ACT_SHOW_PATH, 1 };

	t->items[2].symbol = SHELL_SYM_PAGE;
	snprintf(t->items[2].title, sizeof(t->items[2].title), "Uptime & display");
	snprintf(t->items[2].path, sizeof(t->items[2].path), "/dev/dri/card0 \xc2\xb7 1280x720@30 VIC 62");
	t->items[2].num_actions = 2;
	t->items[2].actions[0] = (struct shell_choice){ SHELL_SYM_NONE, "Switch theme", SHELL_ACT_SWITCH_THEME, 0 };
	t->items[2].actions[1] = (struct shell_choice){ SHELL_SYM_FOLDER, "Show where it is", SHELL_ACT_SHOW_PATH, 2 };

	update_system_status_turn(t);
	t->num_choices = 3;
	t->choices[0] = (struct shell_choice){ SHELL_SYM_NONE, "Refresh status", SHELL_ACT_REFRESH_STATUS, 0 };
	t->choices[1] = (struct shell_choice){ SHELL_SYM_GAME, "Play Doom", SHELL_ACT_LAUNCH_DOOM, 0 };
	t->choices[2] = (struct shell_choice){ SHELL_SYM_NONE, "Switch theme", SHELL_ACT_SWITCH_THEME, 0 };
	t->focus_zone = SHELL_FOCUS_CHOICES;
	t->focus_choice_idx = 0;

	/* Turn 4: Media Tiles & Full-screen Image Viewer */
	t = &app.turns[3];
	snprintf(t->question, sizeof(t->question), "Show recent photos and videos");
	snprintf(t->answer, sizeof(t->answer),
		 "Select any tile to open the full-screen viewer. Photos are fitted to 1280\xc3\x97""720 on the theme ground.");
	t->num_tiles = 4;

	snprintf(t->tiles[0].title, sizeof(t->tiles[0].title), "Garden time-lapse");
	snprintf(t->tiles[0].detail, sizeof(t->tiles[0].detail), "Today");
	snprintf(t->tiles[0].badge, sizeof(t->tiles[0].badge), "3:12");
	snprintf(t->tiles[0].media_path, sizeof(t->tiles[0].media_path), "/mnt/data/linux/media/videos/garden.mp4");
	t->tiles[0].art_id = 0;

	snprintf(t->tiles[1].title, sizeof(t->tiles[1].title), "Kids\xe2\x80\x99 concert");
	snprintf(t->tiles[1].detail, sizeof(t->tiles[1].detail), "28 Sep");
	snprintf(t->tiles[1].badge, sizeof(t->tiles[1].badge), "12:47");
	snprintf(t->tiles[1].media_path, sizeof(t->tiles[1].media_path), "/mnt/data/linux/media/videos/concert.mp4");
	t->tiles[1].art_id = 1;

	snprintf(t->tiles[2].title, sizeof(t->tiles[2].title), "Beach at sunset");
	snprintf(t->tiles[2].detail, sizeof(t->tiles[2].detail), "Photo \xc2\xb7 3 May");
	snprintf(t->tiles[2].media_path, sizeof(t->tiles[2].media_path), "/mnt/data/linux/media/photos/beach.jpg");
	t->tiles[2].art_id = 2;

	snprintf(t->tiles[3].title, sizeof(t->tiles[3].title), "Velliangiri hills");
	snprintf(t->tiles[3].detail, sizeof(t->tiles[3].detail), "Photo \xc2\xb7 3 Nov");
	snprintf(t->tiles[3].media_path, sizeof(t->tiles[3].media_path), "/mnt/data/linux/media/photos/hills.jpg");
	t->tiles[3].art_id = 3;

	for (i = 0; i < 4; i++) {
		t->tiles[i].thumb_pixels = malloc((size_t)SHELL_THUMB_W * SHELL_THUMB_H * sizeof(uint32_t));
		if (t->tiles[i].thumb_pixels) {
			if (shell_media_load_jpeg_thumb(t->tiles[i].media_path,
							t->tiles[i].thumb_pixels) != 0) {
				shell_media_render_sample_thumb(t->tiles[i].thumb_pixels,
								t->tiles[i].art_id);
			}
		}
		t->tiles[i].num_actions = 2;
		t->tiles[i].actions[0] = (struct shell_choice){
			t->tiles[i].badge[0] ? SHELL_SYM_PLAY : SHELL_SYM_NONE,
			"Open viewer", SHELL_ACT_OPEN_VIEWER, i
		};
		t->tiles[i].actions[1] = (struct shell_choice){
			SHELL_SYM_FOLDER, "Show where it is", SHELL_ACT_SHOW_PATH, i
		};
	}
	t->num_choices = 3;
	t->choices[0] = (struct shell_choice){ SHELL_SYM_NONE, "Open photo viewer", SHELL_ACT_OPEN_VIEWER, 2 };
	t->choices[1] = (struct shell_choice){ SHELL_SYM_NONE, "Show notes", SHELL_ACT_GOTO_TURN, 5 };
	t->choices[2] = (struct shell_choice){ SHELL_SYM_GAME, "Play Doom", SHELL_ACT_LAUNCH_DOOM, 0 };
	t->focus_zone = SHELL_FOCUS_CONTENT;
	t->focus_content_idx = 2; /* Beach at sunset focused */

	/* Turn 5: Sourdough Starter Specimen (from design-system.html) */
	t = &app.turns[4];
	snprintf(t->question, sizeof(t->question), "How do I make a sourdough starter?");
	snprintf(t->answer, sizeof(t->answer),
		 "Mix equal weights of flour and water in a jar and leave it loosely covered at room temperature. Each day, discard half and feed it.");
	snprintf(t->sources, sizeof(t->sources),
		 "Read 2 pages \xc2\xb7 wikipedia.org, seriouseats.com");
	t->num_steps = 2;
	t->steps[0].done = true;
	snprintf(t->steps[0].text, sizeof(t->steps[0].text),
		 "Searched \xe2\x80\x9csourdough starter video\xe2\x80\x9d");
	t->steps[1].done = false;
	snprintf(t->steps[1].text, sizeof(t->steps[1].text),
		 "Checking the length of each one\xe2\x80\xa6");
	t->num_choices = 3;
	t->choices[0] = (struct shell_choice){ SHELL_SYM_NONE, "Feeding schedule", SHELL_ACT_DEMO_WORKING, 4 };
	t->choices[1] = (struct shell_choice){ SHELL_SYM_PLAY, "Find a video", SHELL_ACT_DEMO_WORKING, 3 };
	t->choices[2] = (struct shell_choice){ SHELL_SYM_NONE, "A simple first loaf", SHELL_ACT_GOTO_TURN, 5 };
	t->focus_zone = SHELL_FOCUS_CHOICES;
	t->focus_choice_idx = 1;

	/* Turn 6: Notes & Folders Specimen (from design-system.html) */
	t = &app.turns[5];
	snprintf(t->question, sizeof(t->question), "Show my notes and folders");
	snprintf(t->answer, sizeof(t->answer),
		 "Select a note below to read its contents or reveal where it is kept on Data.");
	t->num_items = 4;

	t->items[0].symbol = SHELL_SYM_PAGE;
	snprintf(t->items[0].title, sizeof(t->items[0].title), "Shopping list");
	snprintf(t->items[0].detail, sizeof(t->items[0].detail), "Note \xc2\xb7 yesterday");
	snprintf(t->items[0].path, sizeof(t->items[0].path), SHELL_NOTES_DIR "/shopping-list.txt");
	snprintf(t->items[0].body, sizeof(t->items[0].body),
		 "Shopping list: Bread flour, whole rye flour, sea salt, olive oil, and parchment paper for the weekend bake.");
	t->items[0].num_actions = 2;
	t->items[0].actions[0] = (struct shell_choice){ SHELL_SYM_NONE, "Read note", SHELL_ACT_READ_NOTE, 0 };
	t->items[0].actions[1] = (struct shell_choice){ SHELL_SYM_FOLDER, "Show where it is", SHELL_ACT_SHOW_PATH, 0 };

	t->items[1].symbol = SHELL_SYM_PAGE;
	snprintf(t->items[1].title, sizeof(t->items[1].title), "Wi-Fi setup notes");
	snprintf(t->items[1].detail, sizeof(t->items[1].detail), "Note \xc2\xb7 2 Oct");
	snprintf(t->items[1].path, sizeof(t->items[1].path), SHELL_NOTES_DIR "/wifi-setup.txt");
	snprintf(t->items[1].body, sizeof(t->items[1].body),
		 "Wi-Fi setup: Run 'wifi scan' and 'wifi connect SSID PSK' to join a network. Credentials persist in /mnt/data/linux/etc/wpa_supplicant.");
	t->items[1].num_actions = 2;
	t->items[1].actions[0] = (struct shell_choice){ SHELL_SYM_NONE, "Read note", SHELL_ACT_READ_NOTE, 1 };
	t->items[1].actions[1] = (struct shell_choice){ SHELL_SYM_FOLDER, "Show where it is", SHELL_ACT_SHOW_PATH, 1 };

	t->items[2].symbol = SHELL_SYM_FOLDER;
	snprintf(t->items[2].title, sizeof(t->items[2].title), "Music");
	snprintf(t->items[2].detail, sizeof(t->items[2].detail), "48 items");
	snprintf(t->items[2].path, sizeof(t->items[2].path), "/mnt/data/linux/media/music");
	t->items[2].num_actions = 2;
	t->items[2].actions[0] = (struct shell_choice){ SHELL_SYM_FOLDER, "Show where it is", SHELL_ACT_SHOW_PATH, 2 };
	t->items[2].actions[1] = (struct shell_choice){ SHELL_SYM_NONE, "Browse media", SHELL_ACT_GOTO_TURN, 3 };

	t->items[3].symbol = SHELL_SYM_GAME;
	snprintf(t->items[3].title, sizeof(t->items[3].title), "Doom");
	snprintf(t->items[3].detail, sizeof(t->items[3].detail), "Ready to play");
	snprintf(t->items[3].path, sizeof(t->items[3].path), "/bin/doom");
	t->items[3].num_actions = 2;
	t->items[3].actions[0] = (struct shell_choice){ SHELL_SYM_GAME, "Play Doom", SHELL_ACT_LAUNCH_DOOM, 0 };
	t->items[3].actions[1] = (struct shell_choice){ SHELL_SYM_FOLDER, "Show where it is", SHELL_ACT_SHOW_PATH, 3 };

	t->num_choices = 3;
	t->choices[0] = (struct shell_choice){ SHELL_SYM_GAME, "Play Doom", SHELL_ACT_LAUNCH_DOOM, 0 };
	t->choices[1] = (struct shell_choice){ SHELL_SYM_NONE, "Browse media", SHELL_ACT_GOTO_TURN, 3 };
	t->choices[2] = (struct shell_choice){ SHELL_SYM_NONE, "Back home", SHELL_ACT_GOTO_TURN, 0 };
	t->focus_zone = SHELL_FOCUS_CONTENT;
	t->focus_content_idx = 1;
}

/* --- Rendering --- */

static const uint32_t *get_logo_bitmap(enum shell_theme_id theme, bool working_outline)
{
	if (theme == SHELL_THEME_SIGNAL)
		return working_outline ? logo_signal_working : logo_signal_ready;
	return working_outline ? logo_paper_working : logo_paper_ready;
}

static void render_header(const struct shell_theme *th, enum shell_theme_id tid,
			  const char *question, int turn_num, int turn_total,
			  bool working_outline)
{
	int x = SHELL_CONTENT_LEFT;
	int y = SHELL_CONTENT_TOP;
	char counter[32];

	shell_draw_bitmap_xrgb(x, y, SHELL_LOGO_W, SHELL_LOGO_H,
			       get_logo_bitmap(tid, working_outline));

	if (turn_total > 0) {
		snprintf(counter, sizeof(counter),
			 tid == SHELL_THEME_SIGNAL ? "%d / %d" : "%d of %d",
			 turn_num, turn_total);
	} else {
		counter[0] = '\0';
	}

	int cw = counter[0] ? shell_font_measure(tid, SHELL_FONT_META, false, counter) : 0;
	int qx = x + SHELL_LOGO_W + SHELL_SPACE_HEADER_GAP;
	int q_max_w = SHELL_SCREEN_W - SHELL_CONTENT_RIGHT - qx - (cw ? cw + 24 : 0);
	int qy = y + (SHELL_LOGO_H - SHELL_FONT_QUESTION) / 2;

	shell_font_draw(tid, SHELL_FONT_QUESTION, false, false,
			qx, qy, th->secondary, question, q_max_w);

	if (cw > 0) {
		int cx = SHELL_SCREEN_W - SHELL_CONTENT_RIGHT - cw;
		int cy = y + (SHELL_LOGO_H - SHELL_FONT_META) / 2;

		shell_font_draw(tid, SHELL_FONT_META, false, false,
				cx, cy, th->secondary, counter, 0);
	}

	if (th->has_rule) {
		int ry = y + SHELL_LOGO_H + SHELL_SPACE_RULE_ABOVE;

		shell_draw_rect(SHELL_CONTENT_LEFT, ry, SHELL_CONTENT_MAX_W, 1, th->rule);
	}
}

static void render_state_line(const struct shell_theme *th, bool working, int progress_pct)
{
	if (!th->has_state_line)
		return;
	if (!working) {
		int h = th->state_line_idle_h;

		shell_draw_rect(0, SHELL_SCREEN_H - h, SHELL_SCREEN_W, h,
				th->state_line_idle);
	} else {
		int h = th->state_line_work_h;
		int fill_w = (SHELL_SCREEN_W * (progress_pct > 0 ? progress_pct : 62)) / 100;

		shell_draw_rect(0, SHELL_SCREEN_H - h, SHELL_SCREEN_W, h,
				th->state_line_track);
		if (fill_w > 0)
			shell_draw_rect(0, SHELL_SCREEN_H - h, fill_w, h, th->accent);
	}
}

static int get_active_choices(const struct shell_turn *t,
			      struct shell_choice out_choices[4],
			      const char **prefix_title)
{
	int n = 0, i;

	*prefix_title = NULL;
	if (t->focus_zone == SHELL_FOCUS_CONTENT) {
		if (t->num_items > 0 && t->focus_content_idx < t->num_items) {
			const struct shell_item *it = &t->items[t->focus_content_idx];

			*prefix_title = it->title;
			for (i = 0; i < it->num_actions && n < 3; i++)
				out_choices[n++] = it->actions[i];
		} else if (t->num_tiles > 0 && t->focus_content_idx < t->num_tiles) {
			const struct shell_tile *tl = &t->tiles[t->focus_content_idx];

			*prefix_title = tl->title;
			for (i = 0; i < tl->num_actions && n < 3; i++)
				out_choices[n++] = tl->actions[i];
		}
	}

	if (n == 0) {
		for (i = 0; i < t->num_choices && n < 3; i++)
			out_choices[n++] = t->choices[i];
	}

	/* 4th choice: Ask when a keyboard is connected, otherwise More ideas */
	if (shell_input_has_keyboard()) {
		out_choices[n++] = (struct shell_choice){
			SHELL_SYM_KEYBOARD, "Ask", SHELL_ACT_OPEN_ASK, 0
		};
	} else {
		out_choices[n++] = (struct shell_choice){
			SHELL_SYM_NONE, "More ideas", SHELL_ACT_NEXT_IDEA, 0
		};
	}
	return n;
}

static void render_choices_row(const struct shell_theme *th, enum shell_theme_id tid,
			       const struct shell_turn *t)
{
	struct shell_choice ch[4];
	const char *prefix = NULL;
	int n = get_active_choices(t, ch, &prefix);
	int bottom_pad = (tid == SHELL_THEME_SIGNAL) ? 56 : SHELL_CONTENT_BOTTOM;
	int box_h = SHELL_FONT_CHOICE + SHELL_SPACE_CHOICE_PAD_Y * 2;
	int y = SHELL_SCREEN_H - bottom_pad - box_h;
	int x = SHELL_CONTENT_LEFT - SHELL_SPACE_CHOICE_PAD_X;
	int i;

	if (prefix && prefix[0]) {
		int pw = shell_font_draw(tid, SHELL_FONT_CHOICE, false, false,
					 SHELL_CONTENT_LEFT,
					 y + SHELL_SPACE_CHOICE_PAD_Y,
					 th->secondary, prefix, 260);
		int dot_w = shell_font_draw(tid, SHELL_FONT_CHOICE, false, false,
					    SHELL_CONTENT_LEFT + pw + 10,
					    y + SHELL_SPACE_CHOICE_PAD_Y,
					    th->secondary, "\xc2\xb7", 0);
		x = SHELL_CONTENT_LEFT + pw + dot_w + 20 - SHELL_SPACE_CHOICE_PAD_X;
	}

	for (i = 0; i < n; i++) {
		bool is_focused = (t->focus_zone == SHELL_FOCUS_CHOICES &&
				   t->focus_choice_idx == i);
		bool is_quiet = (i == n - 1);
		bool bold = is_focused;
		bool italic = (is_quiet && !is_focused && tid == SHELL_THEME_PAPER);
		int sym_sz = (ch[i].symbol == SHELL_SYM_KEYBOARD) ? 26 : 22;
		int sym_w = (ch[i].symbol != SHELL_SYM_NONE) ?
			    (sym_sz + SHELL_SPACE_SYMBOL_GAP) : 0;
		int tw = shell_font_measure(tid, SHELL_FONT_CHOICE, bold, ch[i].label);
		int bw = SHELL_SPACE_CHOICE_PAD_X * 2 + sym_w + tw;

		uint32_t fg = is_focused ? th->on_focus :
			      (is_quiet ? th->secondary : th->choice);

		if (is_focused)
			shell_draw_rounded_rect(x, y, bw, box_h, th->radius, th->focus);

		int inner_x = x + SHELL_SPACE_CHOICE_PAD_X;
		if (ch[i].symbol != SHELL_SYM_NONE) {
			int sym_y = y + (box_h - sym_sz) / 2;

			shell_draw_symbol(ch[i].symbol, inner_x, sym_y, sym_sz, fg);
			inner_x += sym_w;
		}
		shell_font_draw(tid, SHELL_FONT_CHOICE, bold, italic,
				inner_x, y + SHELL_SPACE_CHOICE_PAD_Y, fg,
				ch[i].label, 0);

		x += bw + SHELL_SPACE_CHOICE_GAP;
	}
}

static void render_theme_picker(void)
{
	enum shell_theme_id tid = app.cfg.theme;
	const struct shell_theme *th = &shell_themes[tid];

	shell_draw_rect(0, 0, SHELL_SCREEN_W, SHELL_SCREEN_H, th->ground);
	render_header(th, tid, "Pick a look for your screen", 1, 1, false);

	int body_y = SHELL_CONTENT_TOP + SHELL_LOGO_H +
		     SHELL_SPACE_RULE_ABOVE + 1 + SHELL_SPACE_RULE_BELOW;

	shell_font_draw(tid, SHELL_FONT_DISPLAY, true, false,
			SHELL_CONTENT_LEFT, body_y, th->text,
			tid == SHELL_THEME_SIGNAL ? "Signal" : "Paper", 0);

	body_y += SHELL_FONT_DISPLAY + 20;
	shell_font_draw_wrapped(tid, SHELL_FONT_ANSWER, false,
				SHELL_CONTENT_LEFT, body_y,
				(SHELL_FONT_ANSWER * th->answer_lh_pct) / 100,
				SHELL_TEXT_MAX_W, 4, th->text,
				"Paper is warm and bookish in Literata. Signal is dark and high-contrast in Atkinson Hyperlegible. Use Left and Right to preview, and press OK to start.");

	/* Draw the two theme choices at the bottom */
	int bottom_pad = (tid == SHELL_THEME_SIGNAL) ? 56 : SHELL_CONTENT_BOTTOM;
	int box_h = SHELL_FONT_CHOICE + SHELL_SPACE_CHOICE_PAD_Y * 2;
	int y = SHELL_SCREEN_H - bottom_pad - box_h;
	int x = SHELL_CONTENT_LEFT - SHELL_SPACE_CHOICE_PAD_X;
	const char *labels[2] = { "Paper", "Signal" };
	int i;

	for (i = 0; i < 2; i++) {
		bool is_focused = ((int)tid == i);
		int tw = shell_font_measure(tid, SHELL_FONT_CHOICE, is_focused, labels[i]);
		int bw = SHELL_SPACE_CHOICE_PAD_X * 2 + tw;
		uint32_t fg = is_focused ? th->on_focus : th->choice;

		if (is_focused)
			shell_draw_rounded_rect(x, y, bw, box_h, th->radius, th->focus);
		shell_font_draw(tid, SHELL_FONT_CHOICE, is_focused, false,
				x + SHELL_SPACE_CHOICE_PAD_X,
				y + SHELL_SPACE_CHOICE_PAD_Y,
				fg, labels[i], 0);
		x += bw + SHELL_SPACE_CHOICE_GAP;
	}

	render_state_line(th, false, 0);
	shell_display_present();
}

static void render_turn_screen(void)
{
	enum shell_theme_id tid = app.cfg.theme;
	const struct shell_theme *th = &shell_themes[tid];
	const struct shell_turn *t = &app.turns[app.cur_turn];
	bool working = (t->state == SHELL_TURN_WORKING);

	shell_draw_rect(0, 0, SHELL_SCREEN_W, SHELL_SCREEN_H, th->ground);
	render_header(th, tid, t->question, app.cur_turn + 1, app.num_turns,
		      working && app.logo_blink_alt);

	int y = SHELL_CONTENT_TOP + SHELL_LOGO_H +
		SHELL_SPACE_RULE_ABOVE + 1 + SHELL_SPACE_RULE_BELOW;

	if (working) {
		int i;

		for (i = 0; i < t->num_steps; i++) {
			bool done = t->steps[i].done;
			enum shell_symbol_id sym = done ? SHELL_SYM_CHECK : SHELL_SYM_STEP_DOT;
			uint32_t sym_c = done ? th->secondary : th->accent;
			uint32_t txt_c = done ? th->secondary : th->text;
			bool italic = (!done && tid == SHELL_THEME_PAPER);

			shell_draw_symbol(sym, SHELL_CONTENT_LEFT, y + 2, 24, sym_c);
			shell_font_draw(tid, SHELL_FONT_STEP, false, italic,
					SHELL_CONTENT_LEFT + 24 + 18, y,
					txt_c, t->steps[i].text,
					SHELL_TEXT_MAX_W);
			y += SHELL_FONT_STEP + SHELL_SPACE_STEP_GAP;
		}

		int bottom_pad = (tid == SHELL_THEME_SIGNAL) ? 56 : SHELL_CONTENT_BOTTOM;
		shell_font_draw(tid, SHELL_FONT_META, false, false,
				SHELL_CONTENT_LEFT,
				SHELL_SCREEN_H - bottom_pad - SHELL_FONT_META,
				th->secondary, "Menu to stop", 0);
		render_state_line(th, true, t->progress_pct);
		shell_display_present();
		return;
	}

	/* Notice banner if active */
	if (app.notice_text[0] && monotonic_ms() < app.notice_until_ms) {
		shell_draw_symbol(app.notice_sym, SHELL_CONTENT_LEFT, y + 2, 28, th->text);
		shell_font_draw(tid, SHELL_FONT_ITEM, false, false,
				SHELL_CONTENT_LEFT + 28 + 24, y,
				th->text, app.notice_text, SHELL_TEXT_MAX_W);
		y += SHELL_FONT_ITEM + 28;
	}

	/* Optional 64px Value block */
	if (t->value_big[0]) {
		shell_font_draw(tid, SHELL_FONT_DISPLAY, true, false,
				SHELL_CONTENT_LEFT, y, th->text, t->value_big, 0);
		y += (SHELL_FONT_DISPLAY * 110) / 100 + 4;
		if (t->value_label[0]) {
			shell_font_draw(tid, SHELL_FONT_ITEM_DETAIL, false, false,
					SHELL_CONTENT_LEFT, y, th->secondary,
					t->value_label, SHELL_TEXT_MAX_W);
			y += SHELL_FONT_ITEM_DETAIL + SHELL_SPACE_ANSWER_TO_ITEMS;
		}
	}

	/* Answer text */
	if (t->answer[0]) {
		int lh = (SHELL_FONT_ANSWER * th->answer_lh_pct) / 100;
		int ah = shell_font_draw_wrapped(tid, SHELL_FONT_ANSWER, false,
						 SHELL_CONTENT_LEFT, y, lh,
						 SHELL_TEXT_MAX_W,
						 SHELL_LIMIT_ANSWER_LINES,
						 th->text, t->answer);
		y += ah;
	}

	/* Optional sources line */
	if (t->sources[0]) {
		y += SHELL_SPACE_ANSWER_TO_SOURCES;
		shell_draw_symbol(SHELL_SYM_PAGE, SHELL_CONTENT_LEFT, y, 20, th->secondary);
		shell_font_draw(tid, SHELL_FONT_META, false, false,
				SHELL_CONTENT_LEFT + 20 + 10, y,
				th->secondary, t->sources, SHELL_TEXT_MAX_W);
		y += SHELL_FONT_META;
	}

	/* Optional Item rows (<= 5) */
	if (t->num_items > 0) {
		int i;
		int row_h = SHELL_FONT_ITEM + SHELL_SPACE_ITEM_PAD_Y * 2;
		int row_x = SHELL_CONTENT_LEFT - SHELL_SPACE_ITEM_PAD_X;
		int row_w = 1060;

		if (t->answer[0] || t->sources[0])
			y += SHELL_SPACE_ANSWER_TO_ITEMS;

		for (i = 0; i < t->num_items && i < SHELL_LIMIT_ITEMS; i++) {
			const struct shell_item *it = &t->items[i];
			bool is_on = (t->focus_zone == SHELL_FOCUS_CONTENT &&
				      t->focus_content_idx == i);
			uint32_t fg = is_on ? th->on_focus : th->item;
			uint32_t dfg = is_on ? th->on_focus_secondary : th->secondary;

			if (is_on)
				shell_draw_rounded_rect(row_x, y, row_w, row_h,
							th->radius, th->focus);

			int tx = SHELL_CONTENT_LEFT;
			if (it->symbol != SHELL_SYM_NONE) {
				shell_draw_symbol(it->symbol, tx,
						  y + (row_h - 24) / 2, 24, fg);
				tx += 24 + 16;
			}
			shell_font_draw(tid, SHELL_FONT_ITEM, is_on, false,
					tx, y + SHELL_SPACE_ITEM_PAD_Y,
					fg, it->title, 620);

			if (it->detail[0]) {
				int dw = shell_font_measure(tid, SHELL_FONT_ITEM_DETAIL,
							    false, it->detail);
				int dx = row_x + row_w - SHELL_SPACE_ITEM_PAD_X - dw;
				int dy = y + (row_h - SHELL_FONT_ITEM_DETAIL) / 2;

				shell_font_draw(tid, SHELL_FONT_ITEM_DETAIL, false, false,
						dx, dy, dfg, it->detail, 0);
			}
			y += row_h;
		}
	}

	/* Optional Media tiles (4 x 248x140) */
	if (t->num_tiles > 0) {
		int i;

		y += SHELL_SPACE_ANSWER_TO_TILES;
		for (i = 0; i < t->num_tiles && i < SHELL_LIMIT_TILES; i++) {
			const struct shell_tile *tl = &t->tiles[i];
			bool is_on = (t->focus_zone == SHELL_FOCUS_CONTENT &&
				      t->focus_content_idx == i);
			int tx = SHELL_CONTENT_LEFT +
				 i * (SHELL_THUMB_W + SHELL_SPACE_TILE_GAP);

			if (tl->thumb_pixels) {
				shell_draw_rounded_bitmap(tx, y,
							  SHELL_THUMB_W, SHELL_THUMB_H,
							  th->radius, tl->thumb_pixels);
			}
			if (is_on) {
				int off = SHELL_THUMB_FOCUS_OFFSET;
				int fw = SHELL_THUMB_FOCUS_FRAME;

				shell_draw_rounded_outline(tx - off - fw, y - off - fw,
							   SHELL_THUMB_W + (off + fw) * 2,
							   SHELL_THUMB_H + (off + fw) * 2,
							   th->radius + off, fw, th->focus);
			}

			/* Video duration badge at bottom-right (8px inset) */
			if (tl->badge[0]) {
				uint32_t b_bg = is_on ? th->focus : th->badge;
				uint32_t b_fg = is_on ? th->on_focus : th->on_badge;
				int tw = shell_font_measure(tid, SHELL_FONT_BADGE, false, tl->badge);
				int bw = 8 + 12 + 6 + tw + 8;
				int bh = SHELL_FONT_BADGE + 6;
				int bx = tx + SHELL_THUMB_W - 8 - bw;
				int by = y + SHELL_THUMB_H - 8 - bh;

				shell_draw_rounded_rect(bx, by, bw, bh, 4, b_bg);
				shell_draw_symbol(SHELL_SYM_PLAY, bx + 8,
						  by + (bh - 12) / 2, 12, b_fg);
				shell_font_draw(tid, SHELL_FONT_BADGE, false, false,
						bx + 8 + 12 + 6, by + 3,
						b_fg, tl->badge, 0);
			}

			int ty = y + SHELL_THUMB_H + 12;
			shell_font_draw(tid, SHELL_FONT_TILE_TITLE, is_on, false,
					tx, ty,
					is_on ? th->text : th->item,
					tl->title, SHELL_THUMB_W);
			ty += SHELL_FONT_TILE_TITLE + 6;
			shell_font_draw(tid, SHELL_FONT_TILE_DETAIL, false, false,
					tx, ty, th->secondary,
					tl->detail, SHELL_THUMB_W);
		}
	}

	render_choices_row(th, tid, t);
	render_state_line(th, false, 0);
	shell_display_present();
}

static void render_ask_screen(void)
{
	enum shell_theme_id tid = app.cfg.theme;
	const struct shell_theme *th = &shell_themes[tid];
	const struct shell_turn *t = &app.turns[app.cur_turn];

	shell_draw_rect(0, 0, SHELL_SCREEN_W, SHELL_SCREEN_H, th->ground);
	render_header(th, tid, t->question, app.cur_turn + 1, app.num_turns, false);

	int y = 240;

	shell_draw_symbol(SHELL_SYM_KEYBOARD, SHELL_CONTENT_LEFT, y, 26, th->secondary);
	shell_font_draw(tid, SHELL_FONT_ITEM_DETAIL, false, false,
			SHELL_CONTENT_LEFT + 26 + 12, y + 2,
			th->secondary, "Ask anything", 0);

	y += 26 + 18;
	int tw = shell_font_draw(tid, SHELL_FONT_ASK_INPUT, false, false,
				 SHELL_CONTENT_LEFT, y, th->text,
				 app.ask_buf, SHELL_CONTENT_MAX_W - 12);

	/* Caret (3px in Paper text, 4px in Signal accent, 50px high) */
	int caret_w = (tid == SHELL_THEME_SIGNAL) ? 4 : 3;
	uint32_t caret_c = (tid == SHELL_THEME_SIGNAL) ? th->accent : th->text;

	shell_draw_rect(SHELL_CONTENT_LEFT + tw + 2, y - 2, caret_w, 50, caret_c);

	if (tid == SHELL_THEME_PAPER) {
		shell_draw_rect(SHELL_CONTENT_LEFT, y + 50 + 14,
				SHELL_CONTENT_MAX_W, 2, th->text);
	}

	int bottom_pad = (tid == SHELL_THEME_SIGNAL) ? 56 : SHELL_CONTENT_BOTTOM;
	shell_font_draw(tid, SHELL_FONT_META, false, false,
			SHELL_CONTENT_LEFT,
			SHELL_SCREEN_H - bottom_pad - SHELL_FONT_META,
			th->secondary, "Enter to ask \xc2\xb7 Esc to cancel", 0);

	render_state_line(th, false, 0);
	shell_display_present();
}

static void open_image_viewer(int tile_idx)
{
	enum shell_theme_id tid = app.cfg.theme;
	const struct shell_theme *th = &shell_themes[tid];
	const struct shell_turn *t = &app.turns[3];

	if (tile_idx < 0 || tile_idx >= t->num_tiles)
		tile_idx = 0;
	app.viewer_tile_idx = tile_idx;
	app.mode = MODE_IMAGE_VIEWER;
	app.viewer_chip_visible = true;
	app.viewer_chip_until_ms = monotonic_ms() + SHELL_TIMING_VIEWER_HIDE_MS;

	if (!app.viewer_pixels) {
		app.viewer_pixels = malloc((size_t)SHELL_SCREEN_W *
					   SHELL_SCREEN_H * sizeof(uint32_t));
	}
	if (app.viewer_pixels) {
		const struct shell_tile *tl = &t->tiles[tile_idx];

		if (shell_media_load_jpeg_viewer(tl->media_path,
						 app.viewer_pixels,
						 th->ground) != 0) {
			shell_media_render_sample_viewer(app.viewer_pixels,
							 tl->art_id,
							 th->ground);
		}
	}
}

static void render_image_viewer(void)
{
	enum shell_theme_id tid = app.cfg.theme;
	const struct shell_theme *th = &shell_themes[tid];
	const struct shell_turn *t = &app.turns[3];
	const struct shell_tile *tl = &t->tiles[app.viewer_tile_idx];

	if (app.viewer_pixels) {
		shell_draw_bitmap_xrgb(0, 0, SHELL_SCREEN_W, SHELL_SCREEN_H,
				       app.viewer_pixels);
	} else {
		shell_draw_rect(0, 0, SHELL_SCREEN_W, SHELL_SCREEN_H, th->ground);
	}

	if (app.viewer_chip_visible) {
		char sub[64];

		snprintf(sub, sizeof(sub),
			 tid == SHELL_THEME_SIGNAL ?
			 "%d / %d \xc2\xb7 \xe2\x86\x90 \xe2\x86\x92 \xc2\xb7 Menu back" :
			 "%d of %d \xc2\xb7 \xe2\x86\x90 \xe2\x86\x92 \xc2\xb7 Menu back",
			 app.viewer_tile_idx + 1, t->num_tiles);

		int tw1 = shell_font_measure(tid, SHELL_FONT_ITEM_DETAIL, false, tl->title);
		int tw2 = shell_font_measure(tid, SHELL_FONT_BADGE, false, sub);
		int max_tw = tw1 > tw2 ? tw1 : tw2;
		int chip_w = 18 + SHELL_LOGO_W + 16 + max_tw + 18;
		int chip_h = 68;

		shell_draw_rounded_rect(32, 20, chip_w, chip_h, th->radius, th->ground);
		shell_draw_bitmap_xrgb(32 + 18, 20 + (chip_h - SHELL_LOGO_H) / 2,
				       SHELL_LOGO_W, SHELL_LOGO_H,
				       get_logo_bitmap(tid, false));
		shell_font_draw(tid, SHELL_FONT_ITEM_DETAIL, false, false,
				32 + 18 + SHELL_LOGO_W + 16, 20 + 12,
				th->text, tl->title, 0);
		shell_font_draw(tid, SHELL_FONT_BADGE, false, false,
				32 + 18 + SHELL_LOGO_W + 16, 20 + 38,
				th->secondary, sub, 0);
	}
	shell_display_present();
}

static void redraw_current_screen(void)
{
	switch (app.mode) {
	case MODE_THEME_PICKER:
		render_theme_picker();
		break;
	case MODE_ASK_LINE:
		render_ask_screen();
		break;
	case MODE_IMAGE_VIEWER:
		render_image_viewer();
		break;
	case MODE_TURN_VIEW:
	default:
		render_turn_screen();
		break;
	}
}

/* --- Actions & Navigation --- */

static void launch_doom_app(int mode)
{
	const char *bin = "/bin/doom";
	const char *argv[8];
	struct stat st;
	int n = 0;

	if (stat("/bin/doom", &st) != 0 && stat("/usr/local/bin/doom", &st) == 0)
		bin = "/usr/local/bin/doom";

	argv[n++] = bin;
	if (mode == 1) {
		argv[n++] = "-skill";
		argv[n++] = "3";
		argv[n++] = "-warp";
		argv[n++] = "1";
		argv[n++] = "1";
	} else if (mode == 2) {
		argv[n++] = "-widescreen";
	}
	argv[n] = NULL;

	shell_app_run(argv, true);
	redraw_current_screen();
}

static void execute_action(enum shell_action_id act, int arg)
{
	struct shell_turn *t = &app.turns[app.cur_turn];

	switch (act) {
	case SHELL_ACT_GOTO_TURN:
		if (arg >= 0 && arg < app.num_turns) {
			app.cur_turn = arg;
			if (app.cur_turn == 2)
				update_system_status_turn(&app.turns[2]);
		}
		break;

	case SHELL_ACT_NEXT_IDEA:
		app.cur_turn = (app.cur_turn + 1) % app.num_turns;
		if (app.cur_turn == 2)
			update_system_status_turn(&app.turns[2]);
		break;

	case SHELL_ACT_OPEN_ASK:
		app.mode = MODE_ASK_LINE;
		app.ask_len = 0;
		app.ask_buf[0] = '\0';
		break;

	case SHELL_ACT_LAUNCH_DOOM:
		launch_doom_app(arg);
		return;

	case SHELL_ACT_SWITCH_THEME:
		app.cfg.theme = (app.cfg.theme == SHELL_THEME_PAPER) ?
				SHELL_THEME_SIGNAL : SHELL_THEME_PAPER;
		app.cfg.theme_set = true;
		config_save(&app.cfg);
		break;

	case SHELL_ACT_REFRESH_STATUS:
		update_system_status_turn(&app.turns[2]);
		break;

	case SHELL_ACT_OPEN_VIEWER:
		app.cur_turn = 3;
		open_image_viewer(arg);
		break;

	case SHELL_ACT_SHOW_PATH:
		if (t->num_items > 0 && arg >= 0 && arg < t->num_items) {
			size_t n = strlen(t->items[arg].path);
			if (n >= sizeof(t->sources))
				n = sizeof(t->sources) - 1;
			memmove(t->sources, t->items[arg].path, n);
			t->sources[n] = '\0';
		} else if (t->num_tiles > 0 && arg >= 0 && arg < t->num_tiles) {
			size_t n = strlen(t->tiles[arg].media_path);
			if (n >= sizeof(t->sources))
				n = sizeof(t->sources) - 1;
			memmove(t->sources, t->tiles[arg].media_path, n);
			t->sources[n] = '\0';
		}
		break;

	case SHELL_ACT_READ_NOTE:
		if (arg >= 0 && arg < t->num_items && t->items[arg].body[0]) {
			size_t n = strlen(t->items[arg].body);
			if (n >= sizeof(t->answer))
				n = sizeof(t->answer) - 1;
			memmove(t->answer, t->items[arg].body, n);
			t->answer[n] = '\0';
		}
		break;

	case SHELL_ACT_DEMO_WORKING:
		t->state = SHELL_TURN_WORKING;
		t->progress_pct = 35;
		t->demo_target_turn = arg;
		app.working_start_ms = monotonic_ms();
		app.last_blink_ms = app.working_start_ms;
		app.logo_blink_alt = true;
		break;

	case SHELL_ACT_NONE:
	default:
		break;
	}
	redraw_current_screen();
}

static void handle_ask_submit(const char *query)
{
	app.mode = MODE_TURN_VIEW;
	if (!query || !query[0]) {
		redraw_current_screen();
		return;
	}

	if (strcasestr(query, "doom") || strcasestr(query, "game") ||
	    strcasestr(query, "play")) {
		app.cur_turn = 1;
	} else if (strcasestr(query, "temp") || strcasestr(query, "status") ||
		   strcasestr(query, "system") || strcasestr(query, "mem")) {
		update_system_status_turn(&app.turns[2]);
		app.cur_turn = 2;
	} else if (strcasestr(query, "photo") || strcasestr(query, "video") ||
		   strcasestr(query, "media") || strcasestr(query, "beach")) {
		app.cur_turn = 3;
	} else if (strcasestr(query, "sourdough") || strcasestr(query, "bread") ||
		   strcasestr(query, "loaf")) {
		app.cur_turn = 4;
	} else if (strcasestr(query, "note") || strcasestr(query, "shop") ||
		   strcasestr(query, "wifi") || strcasestr(query, "music")) {
		app.cur_turn = 5;
	} else if (strcasestr(query, "signal")) {
		app.cfg.theme = SHELL_THEME_SIGNAL;
		config_save(&app.cfg);
	} else if (strcasestr(query, "paper")) {
		app.cfg.theme = SHELL_THEME_PAPER;
		config_save(&app.cfg);
	} else {
		app.cur_turn = 0;
		snprintf(app.turns[0].question, sizeof(app.turns[0].question),
			 "%s", query);
	}
	redraw_current_screen();
}

static void handle_turn_event(const struct shell_event *ev)
{
	struct shell_turn *t = &app.turns[app.cur_turn];
	struct shell_choice active_ch[4];
	const char *prefix = NULL;
	int n_choices = get_active_choices(t, active_ch, &prefix);
	int n_content = t->num_items > 0 ? t->num_items : t->num_tiles;

	if (t->state == SHELL_TURN_WORKING) {
		if (ev->type == SHELL_EVT_KEY_MENU || ev->type == SHELL_EVT_TEXT_ESC) {
			t->state = SHELL_TURN_READY;
			redraw_current_screen();
		}
		return;
	}

	/* Typing any printable character on a Bluetooth keyboard opens the Ask line */
	if (ev->type == SHELL_EVT_TEXT_CHAR) {
		app.mode = MODE_ASK_LINE;
		app.ask_buf[0] = (char)ev->codepoint;
		app.ask_buf[1] = '\0';
		app.ask_len = 1;
		redraw_current_screen();
		return;
	}

	switch (ev->type) {
	case SHELL_EVT_KEY_LEFT:
		if (t->focus_zone == SHELL_FOCUS_CONTENT && t->num_tiles > 0) {
			if (t->focus_content_idx > 0)
				t->focus_content_idx--;
		} else {
			t->focus_zone = SHELL_FOCUS_CHOICES;
			if (t->focus_choice_idx > 0)
				t->focus_choice_idx--;
		}
		redraw_current_screen();
		break;

	case SHELL_EVT_KEY_RIGHT:
		if (t->focus_zone == SHELL_FOCUS_CONTENT && t->num_tiles > 0) {
			if (t->focus_content_idx + 1 < t->num_tiles)
				t->focus_content_idx++;
		} else {
			t->focus_zone = SHELL_FOCUS_CHOICES;
			if (t->focus_choice_idx + 1 < n_choices)
				t->focus_choice_idx++;
		}
		redraw_current_screen();
		break;

	case SHELL_EVT_KEY_UP:
		if (t->focus_zone == SHELL_FOCUS_CHOICES && n_content > 0) {
			t->focus_zone = SHELL_FOCUS_CONTENT;
			if (t->focus_content_idx >= n_content)
				t->focus_content_idx = n_content - 1;
		} else if (t->focus_zone == SHELL_FOCUS_CONTENT && t->num_items > 0 &&
			   t->focus_content_idx > 0) {
			t->focus_content_idx--;
		} else if (app.cur_turn > 0) {
			app.cur_turn--;
			if (app.cur_turn == 2)
				update_system_status_turn(&app.turns[2]);
		}
		redraw_current_screen();
		break;

	case SHELL_EVT_KEY_DOWN:
		if (t->focus_zone == SHELL_FOCUS_CONTENT) {
			if (t->num_items > 0 && t->focus_content_idx + 1 < t->num_items) {
				t->focus_content_idx++;
			} else {
				t->focus_zone = SHELL_FOCUS_CHOICES;
				t->focus_choice_idx = 0;
			}
		} else if (app.cur_turn + 1 < app.num_turns) {
			app.cur_turn++;
			if (app.cur_turn == 2)
				update_system_status_turn(&app.turns[2]);
		}
		redraw_current_screen();
		break;

	case SHELL_EVT_KEY_OK:
	case SHELL_EVT_TEXT_ENTER:
		if (t->focus_zone == SHELL_FOCUS_CONTENT) {
			if (t->num_items > 0 && t->focus_content_idx < t->num_items) {
				const struct shell_item *it = &t->items[t->focus_content_idx];
				if (it->num_actions > 0)
					execute_action(it->actions[0].action,
						       it->actions[0].arg);
			} else if (t->num_tiles > 0 && t->focus_content_idx < t->num_tiles) {
				const struct shell_tile *tl = &t->tiles[t->focus_content_idx];
				if (tl->num_actions > 0)
					execute_action(tl->actions[0].action,
						       tl->actions[0].arg);
			}
		} else {
			if (t->focus_choice_idx >= 0 && t->focus_choice_idx < n_choices) {
				execute_action(active_ch[t->focus_choice_idx].action,
					       active_ch[t->focus_choice_idx].arg);
			}
		}
		break;

	case SHELL_EVT_KEY_MENU:
	case SHELL_EVT_TEXT_ESC:
		if (t->focus_zone == SHELL_FOCUS_CONTENT) {
			t->focus_zone = SHELL_FOCUS_CHOICES;
			t->focus_choice_idx = 0;
		} else if (app.cur_turn > 0) {
			app.cur_turn = 0;
		}
		redraw_current_screen();
		break;

	case SHELL_EVT_KEY_PLAY:
		if (shell_input_has_keyboard()) {
			execute_action(SHELL_ACT_OPEN_ASK, 0);
		} else {
			launch_doom_app(0);
		}
		break;

	default:
		break;
	}
}

int main(int argc, char **argv)
{
	const char *dump_ppm = NULL;
	const char *force_theme = NULL;
	int force_turn = -1;
	bool force_viewer = false;
	bool force_working = false;
	bool force_ask = false;
	int opt;

	while ((opt = getopt(argc, argv, "d:t:n:vwah")) != -1) {
		switch (opt) {
		case 'd':
			dump_ppm = optarg;
			break;
		case 't':
			force_theme = optarg;
			break;
		case 'n':
			force_turn = atoi(optarg);
			break;
		case 'v':
			force_viewer = true;
			break;
		case 'w':
			force_working = true;
			break;
		case 'a':
			force_ask = true;
			break;
		default:
			fprintf(stderr,
				"usage: %s [-d out.ppm] [-t paper|signal] [-n turn] [-v] [-w] [-a]\n",
				argv[0]);
			return opt == 'h' ? 0 : 2;
		}
	}

	seed_sample_notes();
	config_load(&app.cfg);

	if (force_theme) {
		app.cfg.theme = (!strcasecmp(force_theme, "signal")) ?
				SHELL_THEME_SIGNAL : SHELL_THEME_PAPER;
		app.cfg.theme_set = true;
	}

	if (shell_display_init(dump_ppm != NULL) < 0)
		return 1;
	if (shell_font_init() < 0)
		return 1;
	if (!dump_ppm)
		shell_input_init();

	init_predefined_turns();

	if (!app.cfg.theme_set && !dump_ppm)
		app.mode = MODE_THEME_PICKER;
	else
		app.mode = MODE_TURN_VIEW;

	if (force_turn >= 0 && force_turn < app.num_turns)
		app.cur_turn = force_turn;
	if (force_working) {
		app.turns[app.cur_turn].state = SHELL_TURN_WORKING;
		app.turns[app.cur_turn].progress_pct = 62;
	}
	if (force_ask) {
		app.mode = MODE_ASK_LINE;
		snprintf(app.ask_buf, sizeof(app.ask_buf),
			 "Find a sourdough video under ten min");
		app.ask_len = (int)strlen(app.ask_buf);
	}
	if (force_viewer) {
		app.cur_turn = 3;
		open_image_viewer(2);
	}

	redraw_current_screen();

	if (dump_ppm) {
		int r = shell_display_dump_ppm(dump_ppm);

		shell_font_free();
		shell_display_close();
		return r < 0 ? 1 : 0;
	}

	for (;;) {
		struct shell_event ev;
		int timeout_ms = 250;
		uint64_t now = monotonic_ms();

		/* Handle 500ms logo blink and simulated progress during working state */
		struct shell_turn *ct = &app.turns[app.cur_turn];
		if (app.mode == MODE_TURN_VIEW && ct->state == SHELL_TURN_WORKING) {
			timeout_ms = 100;
			if (now - app.last_blink_ms >= SHELL_TIMING_LOGO_BLINK_MS) {
				app.logo_blink_alt = !app.logo_blink_alt;
				app.last_blink_ms = now;
				ct->progress_pct += 22;
				if (ct->progress_pct >= 100) {
					ct->state = SHELL_TURN_READY;
					if (ct->demo_target_turn >= 0 &&
					    ct->demo_target_turn < app.num_turns)
						app.cur_turn = ct->demo_target_turn;
				}
				redraw_current_screen();
			}
		}

		/* Auto-hide image viewer overlay chip after 2 seconds */
		if (app.mode == MODE_IMAGE_VIEWER && app.viewer_chip_visible &&
		    now >= app.viewer_chip_until_ms) {
			app.viewer_chip_visible = false;
			redraw_current_screen();
		}

		/* Clear notice banner when expired */
		if (app.notice_text[0] && now >= app.notice_until_ms) {
			app.notice_text[0] = '\0';
			redraw_current_screen();
		}

		if (shell_input_poll(&ev, timeout_ms) <= 0)
			continue;

		if (ev.type == SHELL_EVT_KB_ATTACHED) {
			snprintf(app.notice_text, sizeof(app.notice_text),
				 "Keyboard connected");
			app.notice_sym = SHELL_SYM_KEYBOARD;
			app.notice_until_ms = monotonic_ms() + 2500;
			redraw_current_screen();
			continue;
		} else if (ev.type == SHELL_EVT_KB_DETACHED) {
			app.notice_text[0] = '\0';
			redraw_current_screen();
			continue;
		}

		if (app.mode == MODE_THEME_PICKER) {
			if (ev.type == SHELL_EVT_KEY_LEFT ||
			    ev.type == SHELL_EVT_KEY_RIGHT) {
				app.cfg.theme = (app.cfg.theme == SHELL_THEME_PAPER) ?
						SHELL_THEME_SIGNAL : SHELL_THEME_PAPER;
				redraw_current_screen();
			} else if (ev.type == SHELL_EVT_KEY_OK ||
				   ev.type == SHELL_EVT_TEXT_ENTER) {
				app.cfg.theme_set = true;
				config_save(&app.cfg);
				app.mode = MODE_TURN_VIEW;
				redraw_current_screen();
			}
			continue;
		}

		if (app.mode == MODE_IMAGE_VIEWER) {
			if (ev.type == SHELL_EVT_KEY_MENU ||
			    ev.type == SHELL_EVT_TEXT_ESC ||
			    ev.type == SHELL_EVT_MENU_HOLD) {
				app.mode = MODE_TURN_VIEW;
				app.turns[3].focus_zone = SHELL_FOCUS_CONTENT;
				app.turns[3].focus_content_idx = app.viewer_tile_idx;
				redraw_current_screen();
			} else if (ev.type == SHELL_EVT_KEY_LEFT) {
				int prev = (app.viewer_tile_idx + 3) % 4;
				open_image_viewer(prev);
				redraw_current_screen();
			} else if (ev.type == SHELL_EVT_KEY_RIGHT) {
				int next = (app.viewer_tile_idx + 1) % 4;
				open_image_viewer(next);
				redraw_current_screen();
			} else if (ev.type == SHELL_EVT_KEY_OK ||
				   ev.type == SHELL_EVT_TEXT_ENTER) {
				app.viewer_chip_visible = !app.viewer_chip_visible;
				if (app.viewer_chip_visible)
					app.viewer_chip_until_ms = monotonic_ms() +
								   SHELL_TIMING_VIEWER_HIDE_MS;
				redraw_current_screen();
			}
			continue;
		}

		if (app.mode == MODE_ASK_LINE) {
			if (ev.type == SHELL_EVT_TEXT_ESC ||
			    ev.type == SHELL_EVT_KEY_MENU) {
				app.mode = MODE_TURN_VIEW;
				redraw_current_screen();
			} else if (ev.type == SHELL_EVT_TEXT_ENTER ||
				   ev.type == SHELL_EVT_KEY_OK) {
				handle_ask_submit(app.ask_buf);
			} else if (ev.type == SHELL_EVT_TEXT_BACKSPACE) {
				if (app.ask_len > 0) {
					app.ask_buf[--app.ask_len] = '\0';
					redraw_current_screen();
				}
			} else if (ev.type == SHELL_EVT_TEXT_CHAR) {
				if (app.ask_len + 1 < SHELL_ASK_MAX_LEN) {
					app.ask_buf[app.ask_len++] = (char)ev.codepoint;
					app.ask_buf[app.ask_len] = '\0';
					redraw_current_screen();
				}
			}
			continue;
		}

		handle_turn_event(&ev);
	}
}
