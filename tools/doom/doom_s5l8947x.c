// SPDX-License-Identifier: GPL-2.0-only
/*
 * doom_s5l8947x.c - Native S5L8947X (j33iap) platform backend for doomgeneric
 *
 * Features:
 *   - 1280x720@30 XRGB8888 output via /dev/fb0 (or /dev/dri/card0 dumb buffer)
 *     with ARMv7 NEON 4:3 pillarboxed (960x720) and 16:9 widescreen (1280x720)
 *     upscaling from Doom's 320x200 8-bit paletted framebuffer.
 *   - Direct kernel ALSA ioctl (/dev/snd/pcmC0D0p) 48 kHz 16-bit stereo mixer
 *     supporting 16 simultaneous WAD DS* PCM sound effect channels and a
 *     real-time polyphonic MUS\x1a score synthesizer (zero alsa-lib/SDL deps).
 *   - Multi-device Linux evdev input (/dev/input/event*) with hotplug rescan,
 *     automatic MCU game_map sysfs toggle, context-aware 7-button IR remote
 *     mapping (Menu/Title/Gameplay/Weapon cycle + 2-second Menu hold to exit
 *     back to tvshell), and full Bluetooth/USB keyboard support.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/fb.h>
#include <linux/input.h>
#include <sound/asound.h>
#endif

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#include "doomgeneric.h"
#include "doomstat.h"
#include "d_player.h"
#include "i_sound.h"
#include "i_system.h"
#include "i_video.h"
#include "m_argv.h"
#include "m_config.h"
#include "r_defs.h"
#include "r_main.h"
#include "r_state.h"
#include "w_wad.h"
#include "z_zone.h"

/*
 * Doom internal key codes (from doomkeys.h), prefixed with DOOM_KEY_ so they
 * do not collide with <linux/input-event-codes.h> KEY_* macros.
 */
#define DOOM_KEY_RIGHTARROW	0xae
#define DOOM_KEY_LEFTARROW	0xac
#define DOOM_KEY_UPARROW	0xad
#define DOOM_KEY_DOWNARROW	0xaf
#define DOOM_KEY_STRAFE_L	0xa0
#define DOOM_KEY_STRAFE_R	0xa1
#define DOOM_KEY_USE		0xa2
#define DOOM_KEY_FIRE		0xa3
#define DOOM_KEY_ESCAPE		27
#define DOOM_KEY_ENTER		13
#define DOOM_KEY_TAB		9
#define DOOM_KEY_F1		(0x80 + 0x3b)
#define DOOM_KEY_F2		(0x80 + 0x3c)
#define DOOM_KEY_F3		(0x80 + 0x3d)
#define DOOM_KEY_F4		(0x80 + 0x3e)
#define DOOM_KEY_F5		(0x80 + 0x3f)
#define DOOM_KEY_F6		(0x80 + 0x40)
#define DOOM_KEY_F7		(0x80 + 0x41)
#define DOOM_KEY_F8		(0x80 + 0x42)
#define DOOM_KEY_F9		(0x80 + 0x43)
#define DOOM_KEY_F10		(0x80 + 0x44)
#define DOOM_KEY_F11		(0x80 + 0x57)
#define DOOM_KEY_F12		(0x80 + 0x58)
#define DOOM_KEY_BACKSPACE	0x7f
#define DOOM_KEY_PAUSE		0xff
#define DOOM_KEY_EQUALS		0x3d
#define DOOM_KEY_MINUS		0x2d
#define DOOM_KEY_RSHIFT		(0x80 + 0x36)
#define DOOM_KEY_RCTRL		(0x80 + 0x1d)
#define DOOM_KEY_RALT		(0x80 + 0x38)

/* External Doom menu state for smart 7-button IR remote context switching */
extern int messageToPrint;
extern boolean messageNeedsInput;
extern int saveStringEnter;

/* Required by i_sound.c when compiled with -DFEATURE_SOUND */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

/* --------------------------------------------------------------------------
 * Display state (1280x720 XRGB8888)
 * -------------------------------------------------------------------------- */
#define FB_TARGET_W	1280
#define FB_TARGET_H	720

static int fb_fd = -1;
static uint32_t *fb_mem;
static size_t fb_mmap_len;
static uint32_t fb_width = FB_TARGET_W;
static uint32_t fb_height = FB_TARGET_H;
static uint32_t fb_stride_px = FB_TARGET_W;
static bool opt_widescreen;
static const char *opt_dump_ppm;
static int opt_max_tics;
static int rendered_frames;

static uint32_t pal32[256];
static uint32_t row_Staging[FB_TARGET_W];
static uint32_t *headless_fb;

static void update_palette32(void)
{
	int i;

	for (i = 0; i < 256; i++) {
		pal32[i] = 0xFF000000U |
			   ((uint32_t)colors[i].r << 16) |
			   ((uint32_t)colors[i].g << 8) |
			   (uint32_t)colors[i].b;
	}
	palette_changed = false;
}

static void save_ppm_frame(const char *path, const uint32_t *pixels,
			   uint32_t w, uint32_t h, uint32_t stride_px)
{
	FILE *fp = fopen(path, "wb");
	uint8_t row[FB_TARGET_W * 3];
	uint32_t x, y;

	if (!fp)
		return;
	fprintf(fp, "P6\n%u %u\n255\n", w, h);
	for (y = 0; y < h; y++) {
		const uint32_t *src = pixels + y * stride_px;
		for (x = 0; x < w && x < FB_TARGET_W; x++) {
			uint32_t c = src[x];
			row[x * 3 + 0] = (c >> 16) & 0xFF;
			row[x * 3 + 1] = (c >> 8) & 0xFF;
			row[x * 3 + 2] = c & 0xFF;
		}
		fwrite(row, 1, w * 3, fp);
	}
	fclose(fp);
}

/* --------------------------------------------------------------------------
 * MCU IR Remote game_map sysfs helper
 * -------------------------------------------------------------------------- */
static void set_mcu_game_map(int enable)
{
	static const char *const paths[] = {
		"/sys/bus/serial-base/devices/serial0-0/game_map",
		"/sys/devices/platform/soc@30000000/32600000.serial/32600000.serial:0/32600000.serial:0.0/serial0/serial0-0/game_map",
		"/sys/bus/serdev/devices/serial0-0/game_map",
	};
	const char *val = enable ? "1\n" : "0\n";
	size_t i;

	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		int fd = open(paths[i], O_WRONLY | O_CLOEXEC);

		if (fd >= 0) {
			ssize_t r = write(fd, val, 2);
			(void)r;
			close(fd);
			return;
		}
	}
}

/* --------------------------------------------------------------------------
 * Input queue & Linux evdev multiplexer
 * -------------------------------------------------------------------------- */
#define KEYQUEUE_SIZE	64

static struct {
	unsigned char key;
	int pressed;
} key_queue[KEYQUEUE_SIZE];
static unsigned int kq_head;
static unsigned int kq_tail;

static void enqueue_key(int pressed, unsigned char key)
{
	unsigned int next = (kq_head + 1) % KEYQUEUE_SIZE;

	if (next == kq_tail)
		return;
	key_queue[kq_head].pressed = pressed;
	key_queue[kq_head].key = key;
	kq_head = next;
}

#ifdef __linux__
#define MAX_EVDEVS	16

struct evdev_slot {
	int fd;
	bool is_remote;
};

static struct evdev_slot evdevs[MAX_EVDEVS];
static int num_evdevs;
static uint32_t last_evdev_scan_ms;

/* 2-second Menu hold state & IR remote weapon cycle state */
static bool menu_held;
static uint32_t menu_down_ms;
static unsigned char ir_select_active_key;
static uint32_t last_use_press_ms;
static unsigned char ir_cycled_weapon_key;

static bool test_bit(int bit, const unsigned long *arr)
{
	return (arr[bit / (sizeof(unsigned long) * 8)] >>
		(bit % (sizeof(unsigned long) * 8))) & 1UL;
}

static void rescan_evdevs(void)
{
	DIR *dir;
	struct dirent *de;
	int i;

	for (i = 0; i < num_evdevs; i++) {
		if (evdevs[i].fd >= 0)
			close(evdevs[i].fd);
	}
	num_evdevs = 0;

	dir = opendir("/dev/input");
	if (!dir)
		return;

	while ((de = readdir(dir)) != NULL && num_evdevs < MAX_EVDEVS) {
		char path[300];
		char name[64] = "";
		unsigned long evbits[2] = { 0 };
		unsigned long keybits[KEY_MAX / (sizeof(unsigned long) * 8) + 2];
		int fd;

		if (strncmp(de->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;

		if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0 ||
		    !test_bit(EV_KEY, evbits)) {
			close(fd);
			continue;
		}
		memset(keybits, 0, sizeof(keybits));
		ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
		ioctl(fd, EVIOCGNAME(sizeof(name)), name);

		/*
		 * Full keyboards advertise alphabetic keys (KEY_A, KEY_Q, KEY_Z);
		 * the 7-button S5L8947X MCU IR remote does not.
		 */
		bool is_kb = test_bit(KEY_A, keybits) &&
			     test_bit(KEY_Q, keybits) &&
			     test_bit(KEY_Z, keybits);
		evdevs[num_evdevs].fd = fd;
		evdevs[num_evdevs].is_remote = !is_kb;
		num_evdevs++;
	}
	closedir(dir);
}

static unsigned char pick_ir_select_key(void)
{
	if (messageToPrint && messageNeedsInput)
		return 'y';
	if (menuactive || saveStringEnter || gamestate != GS_LEVEL)
		return DOOM_KEY_ENTER;
	if (playeringame[consoleplayer] &&
	    players[consoleplayer].playerstate == PST_DEAD)
		return DOOM_KEY_USE;
	return DOOM_KEY_FIRE;
}

static unsigned char pick_next_weapon_key(void)
{
	player_t *plr;
	int cur, step;

	if (!playeringame[consoleplayer])
		return '2';
	plr = &players[consoleplayer];
	cur = (int)plr->readyweapon;

	for (step = 1; step <= NUMWEAPONS; step++) {
		int next = (cur + step) % NUMWEAPONS;
		if (!plr->weaponowned[next])
			continue;
		switch (next) {
		case wp_fist:
		case wp_chainsaw:
			return '1';
		case wp_pistol:
			if (plr->ammo[am_clip] > 0)
				return '2';
			break;
		case wp_shotgun:
		case wp_supershotgun:
			if (plr->ammo[am_shell] > 0)
				return '3';
			break;
		case wp_chaingun:
			if (plr->ammo[am_clip] > 0)
				return '4';
			break;
		case wp_missile:
			if (plr->ammo[am_misl] > 0)
				return '5';
			break;
		case wp_plasma:
			if (plr->ammo[am_cell] > 0)
				return '6';
			break;
		case wp_bfg:
			if (plr->ammo[am_cell] >= 40)
				return '7';
			break;
		default:
			break;
		}
	}
	return '1';
}

static unsigned char translate_kb_code(uint16_t code)
{
	switch (code) {
	case KEY_UP:
	case KEY_W:
		return DOOM_KEY_UPARROW;
	case KEY_DOWN:
	case KEY_S:
		return DOOM_KEY_DOWNARROW;
	case KEY_LEFT:
		return DOOM_KEY_LEFTARROW;
	case KEY_RIGHT:
		return DOOM_KEY_RIGHTARROW;
	case KEY_A:
	case KEY_COMMA:
		return DOOM_KEY_STRAFE_L;
	case KEY_D:
	case KEY_DOT:
		return DOOM_KEY_STRAFE_R;
	case KEY_LEFTCTRL:
	case KEY_RIGHTCTRL:
		return DOOM_KEY_FIRE;
	case KEY_SPACE:
	case KEY_E:
		return DOOM_KEY_USE;
	case KEY_LEFTSHIFT:
	case KEY_RIGHTSHIFT:
		return DOOM_KEY_RSHIFT;
	case KEY_LEFTALT:
	case KEY_RIGHTALT:
		return DOOM_KEY_RALT;
	case KEY_ENTER:
	case KEY_KPENTER:
		return DOOM_KEY_ENTER;
	case KEY_ESC:
		return DOOM_KEY_ESCAPE;
	case KEY_TAB:
		return DOOM_KEY_TAB;
	case KEY_BACKSPACE:
		return DOOM_KEY_BACKSPACE;
	case KEY_PAUSE:
		return DOOM_KEY_PAUSE;
	case KEY_EQUAL:
	case KEY_KPEQUAL:
		return DOOM_KEY_EQUALS;
	case KEY_MINUS:
	case KEY_KPMINUS:
		return DOOM_KEY_MINUS;
	case KEY_1: return '1';
	case KEY_2: return '2';
	case KEY_3: return '3';
	case KEY_4: return '4';
	case KEY_5: return '5';
	case KEY_6: return '6';
	case KEY_7: return '7';
	case KEY_8: return '8';
	case KEY_9: return '9';
	case KEY_0: return '0';
	case KEY_Y: return 'y';
	case KEY_N: return 'n';
	case KEY_I: return 'i';
	case KEY_K: return 'k';
	case KEY_F: return 'f';
	case KEY_Q: return 'q';
	case KEY_C: return 'c';
	case KEY_L: return 'l';
	case KEY_P: return 'p';
	case KEY_R: return 'r';
	case KEY_T: return 't';
	case KEY_U: return 'u';
	case KEY_V: return 'v';
	case KEY_X: return 'x';
	case KEY_Z: return 'z';
	case KEY_B: return 'b';
	case KEY_G: return 'g';
	case KEY_H: return 'h';
	case KEY_J: return 'j';
	case KEY_M: return 'm';
	case KEY_O: return 'o';
	case KEY_F1: return DOOM_KEY_F1;
	case KEY_F2: return DOOM_KEY_F2;
	case KEY_F3: return DOOM_KEY_F3;
	case KEY_F4: return DOOM_KEY_F4;
	case KEY_F5: return DOOM_KEY_F5;
	case KEY_F6: return DOOM_KEY_F6;
	case KEY_F7: return DOOM_KEY_F7;
	case KEY_F8: return DOOM_KEY_F8;
	case KEY_F9: return DOOM_KEY_F9;
	case KEY_F10: return DOOM_KEY_F10;
	case KEY_F11: return DOOM_KEY_F11;
	case KEY_F12: return DOOM_KEY_F12;
	default:
		return 0;
	}
}

static void poll_linux_input(void)
{
	uint32_t now = DG_GetTicksMs();
	int i;

	if (now - last_evdev_scan_ms >= 3000) {
		last_evdev_scan_ms = now;
		if (num_evdevs == 0)
			rescan_evdevs();
	}

	if (menu_held && (now - menu_down_ms) >= 2000) {
		menu_held = false;
		I_Quit();
	}

	if (ir_cycled_weapon_key && (now - last_use_press_ms) >= 120) {
		enqueue_key(0, ir_cycled_weapon_key);
		ir_cycled_weapon_key = 0;
	}

	for (i = 0; i < num_evdevs; i++) {
		struct input_event ev;
		while (read(evdevs[i].fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
			if (ev.type != EV_KEY || ev.value == 2)
				continue;

			int pressed = (ev.value == 1) ? 1 : 0;

			/* 2-second Menu hold detection (on both remote & KB) */
			if (ev.code == KEY_MENU || ev.code == KEY_HOMEPAGE ||
			    (evdevs[i].is_remote && ev.code == KEY_ESC)) {
				if (pressed) {
					menu_held = true;
					menu_down_ms = now;
				} else if (menu_held) {
					menu_held = false;
					if ((now - menu_down_ms) >= 2000) {
						I_Quit();
					} else {
						unsigned char mk = (messageToPrint && messageNeedsInput)
							? 'n' : DOOM_KEY_ESCAPE;
						enqueue_key(1, mk);
						enqueue_key(0, mk);
					}
				}
				continue;
			}

			if (evdevs[i].is_remote) {
				switch (ev.code) {
				case KEY_UP:
					enqueue_key(pressed, DOOM_KEY_UPARROW);
					break;
				case KEY_DOWN:
					enqueue_key(pressed, DOOM_KEY_DOWNARROW);
					break;
				case KEY_LEFT:
					enqueue_key(pressed, DOOM_KEY_LEFTARROW);
					break;
				case KEY_RIGHT:
					enqueue_key(pressed, DOOM_KEY_RIGHTARROW);
					break;
				case KEY_LEFTCTRL:
				case KEY_ENTER:
				case KEY_OK:
					if (pressed) {
						ir_select_active_key = pick_ir_select_key();
						enqueue_key(1, ir_select_active_key);
					} else if (ir_select_active_key) {
						enqueue_key(0, ir_select_active_key);
						ir_select_active_key = 0;
					}
					break;
				case KEY_SPACE:
				case KEY_PLAYPAUSE:
					enqueue_key(pressed, DOOM_KEY_USE);
					if (pressed && gamestate == GS_LEVEL && !menuactive) {
						if (now - last_use_press_ms <= 350) {
							ir_cycled_weapon_key = pick_next_weapon_key();
							enqueue_key(1, ir_cycled_weapon_key);
						}
						last_use_press_ms = now;
					}
					break;
				default:
					break;
				}
			} else {
				if (pressed && ev.code == KEY_F11)
					opt_widescreen = !opt_widescreen;
				unsigned char dk = translate_kb_code(ev.code);
				if (dk != 0)
					enqueue_key(pressed, dk);
			}
		}
	}
}
#endif /* __linux__ */

/* --------------------------------------------------------------------------
 * S5L8947X Direct Kernel ALSA Audio Engine (16-ch SFX + MUS Synthesizer)
 * -------------------------------------------------------------------------- */
#define MIX_CHANNELS		16
#define PCM_PERIOD_FRAMES	512
#define PCM_BUFFER_FRAMES	4096
#define DEFAULT_PCM_DEV		"/dev/snd/pcmC0D0p"

struct sfx_voice {
	const uint8_t *samples;
	uint32_t length;
	uint64_t pos_fp;
	uint64_t step_fp;
	int left_gain;	/* 0..256 */
	int right_gain;	/* 0..256 */
	int lumpnum;
	bool active;
};

/* Polyphonic MUS\x1a score player state */
#define MUS_VOICES	8

struct mus_voice {
	bool active;
	uint8_t channel;
	uint8_t note;
	uint8_t volume;		/* 0..127 */
	uint32_t phase;
	uint32_t phase_inc;
	uint32_t env_decay;	/* 0..65536 */
};

struct mus_song {
	const uint8_t *data;
	int len;
	uint16_t score_start;
	uint16_t score_len;
};

static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sfx_voice sfx_voices[MIX_CHANNELS];
static bool sfx_use_prefix = true;
static unsigned int pcm_rate = 48000;
static int pcm_fd = -1;
static pthread_t audio_thread;
static volatile bool audio_running;

/* MUS player state */
static struct mus_song *active_song;
static const uint8_t *mus_ptr;
static const uint8_t *mus_end;
static bool mus_looping;
static bool mus_paused_flag;
static int mus_master_vol = 64; /* 0..127 */
static uint32_t mus_delay_ticks;
static uint32_t mus_samples_until_tick;
static uint8_t mus_chan_vol[16];
static int16_t mus_chan_bend[16];
static struct mus_voice mus_voices[MUS_VOICES];
static uint32_t mus_note_freq_fp[128]; /* phase increment per sample */

static void init_mus_freq_table(unsigned int rate)
{
	/* MIDI note 0 (C-1) in Hz, multiplied by 2^(1/12) per semitone */
	double hz = 8.175798915643707;
	const double semi = 1.0594630943592953;
	int n;

	if (rate == 0)
		rate = 48000;
	for (n = 0; n < 128; n++) {
		double inc = (hz / (double)rate) * 4294967296.0;
		mus_note_freq_fp[n] = (uint32_t)inc;
		hz *= semi;
	}
}

static void mus_reset_voices(void)
{
	int i;

	memset(mus_voices, 0, sizeof(mus_voices));
	for (i = 0; i < 16; i++) {
		mus_chan_vol[i] = 100;
		mus_chan_bend[i] = 64;
	}
}

static void mus_note_on(uint8_t ch, uint8_t note, uint8_t vol)
{
	int i, slot = 0;
	uint32_t min_env = 0xFFFFFFFFU;

	for (i = 0; i < MUS_VOICES; i++) {
		if (!mus_voices[i].active ||
		    (mus_voices[i].channel == ch && mus_voices[i].note == note)) {
			slot = i;
			break;
		}
		if (mus_voices[i].env_decay < min_env) {
			min_env = mus_voices[i].env_decay;
			slot = i;
		}
	}

	mus_voices[slot].active = true;
	mus_voices[slot].channel = ch;
	mus_voices[slot].note = note & 0x7F;
	mus_voices[slot].volume = vol & 0x7F;
	mus_voices[slot].phase = 0;
	mus_voices[slot].phase_inc = mus_note_freq_fp[note & 0x7F];
	mus_voices[slot].env_decay = (ch == 15) ? 24000 : 65536;
}

static void mus_note_off(uint8_t ch, uint8_t note)
{
	int i;

	for (i = 0; i < MUS_VOICES; i++) {
		if (mus_voices[i].active &&
		    mus_voices[i].channel == ch &&
		    mus_voices[i].note == (note & 0x7F)) {
			mus_voices[i].active = false;
		}
	}
}

static void mus_step_tick(void)
{
	if (!active_song || !mus_ptr || mus_paused_flag)
		return;

	if (mus_delay_ticks > 0) {
		mus_delay_ticks--;
		return;
	}

	while (mus_ptr < mus_end && mus_delay_ticks == 0) {
		uint8_t desc = *mus_ptr++;
		uint8_t ch = desc & 0x0F;
		uint8_t ev = (desc >> 4) & 0x07;
		bool last = (desc & 0x80) != 0;

		switch (ev) {
		case 0: /* Release note */
			if (mus_ptr < mus_end)
				mus_note_off(ch, *mus_ptr++ & 0x7F);
			break;
		case 1: /* Play note */
			if (mus_ptr < mus_end) {
				uint8_t b = *mus_ptr++;
				uint8_t note = b & 0x7F;
				if (b & 0x80) {
					if (mus_ptr < mus_end)
						mus_chan_vol[ch] = *mus_ptr++ & 0x7F;
				}
				mus_note_on(ch, note, mus_chan_vol[ch]);
			}
			break;
		case 2: /* Pitch bend */
			if (mus_ptr < mus_end)
				mus_chan_bend[ch] = *mus_ptr++;
			break;
		case 3: /* System event */
			if (mus_ptr < mus_end) {
				uint8_t sys = *mus_ptr++ & 0x7F;
				if (sys >= 10 && sys <= 14)
					mus_reset_voices();
			}
			break;
		case 4: /* Controller */
			if (mus_ptr + 1 < mus_end) {
				uint8_t ctrl = *mus_ptr++ & 0x7F;
				uint8_t val = *mus_ptr++ & 0x7F;
				if (ctrl == 3)
					mus_chan_vol[ch] = val;
			} else {
				mus_ptr = mus_end;
			}
			break;
		case 6: /* End of score */
			if (mus_looping) {
				mus_ptr = active_song->data + active_song->score_start;
				mus_reset_voices();
			} else {
				mus_ptr = NULL;
				mus_reset_voices();
			}
			return;
		default:
			break;
		}

		if (last) {
			uint32_t d = 0;
			while (mus_ptr < mus_end) {
				uint8_t b = *mus_ptr++;
				d = (d << 7) | (b & 0x7F);
				if (!(b & 0x80))
					break;
			}
			mus_delay_ticks = d;
		}
	}

	if (mus_ptr >= mus_end) {
		if (mus_looping && active_song) {
			mus_ptr = active_song->data + active_song->score_start;
			mus_reset_voices();
		} else {
			mus_ptr = NULL;
		}
	}
}

static void mix_audio_frames(int16_t *out, size_t frames)
{
	size_t f;
	int c, v;
	uint32_t samples_per_mus_tick = pcm_rate / 140;

	pthread_mutex_lock(&audio_lock);

	for (f = 0; f < frames; f++) {
		int32_t acc_l = 0;
		int32_t acc_r = 0;

		/* Step 140 Hz MUS score clock */
		if (active_song && mus_ptr && !mus_paused_flag && mus_master_vol > 0) {
			if (mus_samples_until_tick == 0) {
				mus_step_tick();
				mus_samples_until_tick = samples_per_mus_tick;
			}
			mus_samples_until_tick--;

			for (v = 0; v < MUS_VOICES; v++) {
				struct mus_voice *mv = &mus_voices[v];
				if (!mv->active)
					continue;
				mv->phase += mv->phase_inc;
				int32_t wave;
				if (mv->channel == 15) {
					/* Percussion noise + punch */
					uint32_t n = mv->phase ^ (mv->phase >> 13);
					wave = (int16_t)(n & 0x1FFF) - 4096;
					if (mv->env_decay > 12)
						mv->env_decay -= 12;
					else
						mv->active = false;
				} else {
					/* Warm asymmetric pulse + sub-octave harmonic */
					int32_t p1 = (mv->phase & 0x80000000U) ? 2800 : -2800;
					int32_t p2 = ((mv->phase << 1) & 0xC0000000U) ? 1400 : -1400;
					wave = p1 + p2;
					if (mv->env_decay > 1)
						mv->env_decay -= 1;
				}
				int32_t gain = (mv->volume * mus_master_vol * (int32_t)(mv->env_decay >> 8)) >> 14;
				int32_t sample = (wave * gain) >> 8;
				acc_l += sample;
				acc_r += sample;
			}
		}

		/* Mix 16 digital SFX channels */
		for (c = 0; c < MIX_CHANNELS; c++) {
			struct sfx_voice *sv = &sfx_voices[c];
			if (!sv->active || !sv->samples)
				continue;

			uint32_t idx = (uint32_t)(sv->pos_fp >> 32);
			if (idx >= sv->length) {
				sv->active = false;
				continue;
			}
			int32_t s0 = ((int32_t)sv->samples[idx] - 128) << 8;
			int32_t s1 = (idx + 1 < sv->length)
				? (((int32_t)sv->samples[idx + 1] - 128) << 8)
				: s0;
			int32_t frac = (int32_t)((sv->pos_fp >> 16) & 0xFFFF);
			int32_t sample = s0 + (((s1 - s0) * frac) >> 16);

			acc_l += (sample * sv->left_gain) >> 8;
			acc_r += (sample * sv->right_gain) >> 8;
			sv->pos_fp += sv->step_fp;
		}

		if (acc_l > 32767)
			acc_l = 32767;
		else if (acc_l < -32768)
			acc_l = -32768;
		if (acc_r > 32767)
			acc_r = 32767;
		else if (acc_r < -32768)
			acc_r = -32768;

		out[f * 2 + 0] = (int16_t)acc_l;
		out[f * 2 + 1] = (int16_t)acc_r;
	}

	pthread_mutex_unlock(&audio_lock);
}

#ifdef __linux__
static void hw_mask_set(struct snd_pcm_hw_params *p, int param, unsigned int bit)
{
	struct snd_mask *mask = &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];

	memset(mask, 0, sizeof(*mask));
	mask->bits[bit / 32] |= 1U << (bit % 32);
}

static void hw_interval_set(struct snd_pcm_hw_params *p, int param, unsigned int val)
{
	struct snd_interval *iv =
		&p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];

	memset(iv, 0, sizeof(*iv));
	iv->min = val;
	iv->max = val;
	iv->integer = 1;
}

static int pcm_configure(int fd, unsigned int rate)
{
	struct snd_pcm_hw_params hw;
	int i;

	memset(&hw, 0, sizeof(hw));
	for (i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; i++)
		memset(&hw.masks[i], 0xff, sizeof(hw.masks[i]));
	for (i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i++)
		hw.intervals[i].max = ~0U;

	hw_mask_set(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	hw_mask_set(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	hw_mask_set(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	hw_interval_set(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
	hw_interval_set(&hw, SNDRV_PCM_HW_PARAM_RATE, rate);
	hw_interval_set(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PCM_PERIOD_FRAMES);
	hw_interval_set(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, PCM_BUFFER_FRAMES);
	hw.rmask = ~0U;

	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0)
		return -1;
	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0)
		return -1;
	return 0;
}

static void *alsa_audio_worker(void *arg)
{
	int16_t period_buf[PCM_PERIOD_FRAMES * 2];

	(void)arg;
	while (audio_running) {
		mix_audio_frames(period_buf, PCM_PERIOD_FRAMES);

		struct snd_xferi xfer = {
			.buf = period_buf,
			.frames = PCM_PERIOD_FRAMES,
		};
		while (audio_running &&
		       ioctl(pcm_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer) < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EPIPE) {
				ioctl(pcm_fd, SNDRV_PCM_IOCTL_PREPARE);
				continue;
			}
			usleep(5000);
			break;
		}
	}
	return NULL;
}
#endif /* __linux__ */

static boolean s5l_sfx_init(boolean use_sfx_prefix)
{
	sfx_use_prefix = use_sfx_prefix;
	memset(sfx_voices, 0, sizeof(sfx_voices));
	init_mus_freq_table(48000);

#ifdef __linux__
	const unsigned int rates[] = { 48000, 44100, 32000, 46875 };
	size_t i;

	pcm_fd = open(DEFAULT_PCM_DEV, O_WRONLY | O_CLOEXEC);
	if (pcm_fd >= 0) {
		for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
			if (pcm_configure(pcm_fd, rates[i]) == 0) {
				pcm_rate = rates[i];
				break;
			}
		}
		init_mus_freq_table(pcm_rate);
		audio_running = true;
		if (pthread_create(&audio_thread, NULL, alsa_audio_worker, NULL) != 0) {
			audio_running = false;
			close(pcm_fd);
			pcm_fd = -1;
		}
	}
#endif
	return true;
}

static void s5l_sfx_shutdown(void)
{
#ifdef __linux__
	if (audio_running) {
		audio_running = false;
		pthread_join(audio_thread, NULL);
	}
	if (pcm_fd >= 0) {
		close(pcm_fd);
		pcm_fd = -1;
	}
#endif
}

static int s5l_sfx_get_lump_num(sfxinfo_t *sfx)
{
	char namebuf[9];
	int lump;

	if (sfx->link != NULL)
		sfx = sfx->link;
	if (sfx_use_prefix)
		snprintf(namebuf, sizeof(namebuf), "ds%s", sfx->name);
	else
		snprintf(namebuf, sizeof(namebuf), "%s", sfx->name);

	lump = W_CheckNumForName(namebuf);
	if (lump < 0)
		lump = W_GetNumForName("dspistol");
	return lump;
}

static void s5l_sfx_update(void)
{
}

static void compute_stereo_gains(int vol, int sep, int *out_l, int *out_r)
{
	/* vol: 0..127, sep: 0..254 (128 = center) */
	*out_l = (vol * (254 - sep)) / 127;
	*out_r = (vol * sep) / 127;
}

static void s5l_sfx_update_params(int channel, int vol, int sep)
{
	if (channel < 0 || channel >= MIX_CHANNELS)
		return;
	pthread_mutex_lock(&audio_lock);
	compute_stereo_gains(vol, sep, &sfx_voices[channel].left_gain,
			     &sfx_voices[channel].right_gain);
	pthread_mutex_unlock(&audio_lock);
}

static int s5l_sfx_start(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
	const uint8_t *data;
	int lump_len;
	uint16_t fmt, rate;
	uint32_t samples_len;

	if (channel < 0 || channel >= MIX_CHANNELS || sfxinfo->lumpnum < 0)
		return -1;

	lump_len = W_LumpLength(sfxinfo->lumpnum);
	if (lump_len <= 8)
		return -1;

	data = (const uint8_t *)W_CacheLumpNum(sfxinfo->lumpnum, PU_STATIC);
	if (!data)
		return -1;

	fmt = (uint16_t)(data[0] | (data[1] << 8));
	rate = (uint16_t)(data[2] | (data[3] << 8));
	samples_len = (uint32_t)data[4] |
		      ((uint32_t)data[5] << 8) |
		      ((uint32_t)data[6] << 16) |
		      ((uint32_t)data[7] << 24);

	if (fmt != 3 || rate == 0)
		return -1;
	if (samples_len > (uint32_t)(lump_len - 8))
		samples_len = (uint32_t)(lump_len - 8);

	const uint8_t *pcm = data + 8;
	if (samples_len > 32) {
		pcm += 16;
		samples_len -= 32;
	}

	pthread_mutex_lock(&audio_lock);
	sfx_voices[channel].samples = pcm;
	sfx_voices[channel].length = samples_len;
	sfx_voices[channel].pos_fp = 0;
	sfx_voices[channel].step_fp = ((uint64_t)rate << 32) / pcm_rate;
	compute_stereo_gains(vol, sep, &sfx_voices[channel].left_gain,
			     &sfx_voices[channel].right_gain);
	sfx_voices[channel].lumpnum = sfxinfo->lumpnum;
	sfx_voices[channel].active = true;
	pthread_mutex_unlock(&audio_lock);

	return channel;
}

static void s5l_sfx_stop(int channel)
{
	if (channel < 0 || channel >= MIX_CHANNELS)
		return;
	pthread_mutex_lock(&audio_lock);
	sfx_voices[channel].active = false;
	pthread_mutex_unlock(&audio_lock);
}

static boolean s5l_sfx_is_playing(int channel)
{
	boolean active;

	if (channel < 0 || channel >= MIX_CHANNELS)
		return false;
	pthread_mutex_lock(&audio_lock);
	active = sfx_voices[channel].active ? true : false;
	pthread_mutex_unlock(&audio_lock);
	return active;
}

static void s5l_sfx_cache(sfxinfo_t *sounds, int num_sounds)
{
	int i;

	for (i = 1; i < num_sounds; i++) {
		if (sounds[i].link == NULL) {
			sounds[i].lumpnum = s5l_sfx_get_lump_num(&sounds[i]);
		}
	}
}

static snddevice_t s5l_sound_devices[] = {
	SNDDEVICE_SB,
	SNDDEVICE_PAS,
	SNDDEVICE_GUS,
	SNDDEVICE_WAVEBLASTER,
	SNDDEVICE_SOUNDCANVAS,
	SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
	s5l_sound_devices,
	sizeof(s5l_sound_devices) / sizeof(s5l_sound_devices[0]),
	s5l_sfx_init,
	s5l_sfx_shutdown,
	s5l_sfx_get_lump_num,
	s5l_sfx_update,
	s5l_sfx_update_params,
	s5l_sfx_start,
	s5l_sfx_stop,
	s5l_sfx_is_playing,
	s5l_sfx_cache,
};

/* Music module callbacks */
static boolean s5l_mus_init(void)
{
	mus_reset_voices();
	return true;
}

static void s5l_mus_shutdown(void)
{
	pthread_mutex_lock(&audio_lock);
	active_song = NULL;
	mus_ptr = NULL;
	mus_reset_voices();
	pthread_mutex_unlock(&audio_lock);
}

static void s5l_mus_set_volume(int volume)
{
	pthread_mutex_lock(&audio_lock);
	mus_master_vol = (volume < 0) ? 0 : (volume > 127 ? 127 : volume);
	pthread_mutex_unlock(&audio_lock);
}

static void s5l_mus_pause(void)
{
	pthread_mutex_lock(&audio_lock);
	mus_paused_flag = true;
	mus_reset_voices();
	pthread_mutex_unlock(&audio_lock);
}

static void s5l_mus_resume(void)
{
	pthread_mutex_lock(&audio_lock);
	mus_paused_flag = false;
	pthread_mutex_unlock(&audio_lock);
}

static void *s5l_mus_register(void *data, int len)
{
	const uint8_t *b = (const uint8_t *)data;
	struct mus_song *song;

	if (!b || len < 16 || memcmp(b, "MUS\x1a", 4) != 0)
		return NULL;

	song = calloc(1, sizeof(*song));
	if (!song)
		return NULL;
	song->data = b;
	song->len = len;
	song->score_len = (uint16_t)(b[4] | (b[5] << 8));
	song->score_start = (uint16_t)(b[6] | (b[7] << 8));
	if (song->score_start >= (uint16_t)len) {
		free(song);
		return NULL;
	}
	return song;
}

static void s5l_mus_unregister(void *handle)
{
	struct mus_song *song = (struct mus_song *)handle;

	if (!song)
		return;
	pthread_mutex_lock(&audio_lock);
	if (active_song == song) {
		active_song = NULL;
		mus_ptr = NULL;
		mus_reset_voices();
	}
	pthread_mutex_unlock(&audio_lock);
	free(song);
}

static void s5l_mus_play(void *handle, boolean looping)
{
	struct mus_song *song = (struct mus_song *)handle;

	if (!song)
		return;
	pthread_mutex_lock(&audio_lock);
	active_song = song;
	mus_ptr = song->data + song->score_start;
	mus_end = song->data + song->len;
	mus_looping = (looping != 0);
	mus_paused_flag = false;
	mus_delay_ticks = 0;
	mus_samples_until_tick = 0;
	mus_reset_voices();
	pthread_mutex_unlock(&audio_lock);
}

static void s5l_mus_stop(void)
{
	pthread_mutex_lock(&audio_lock);
	active_song = NULL;
	mus_ptr = NULL;
	mus_reset_voices();
	pthread_mutex_unlock(&audio_lock);
}

static boolean s5l_mus_is_playing(void)
{
	boolean playing;

	pthread_mutex_lock(&audio_lock);
	playing = (active_song != NULL && mus_ptr != NULL) ? true : false;
	pthread_mutex_unlock(&audio_lock);
	return playing;
}

static void s5l_mus_poll(void)
{
}

static snddevice_t s5l_music_devices[] = {
	SNDDEVICE_SB,
	SNDDEVICE_ADLIB,
	SNDDEVICE_GUS,
	SNDDEVICE_GENMIDI,
	SNDDEVICE_AWE32,
};

music_module_t DG_music_module = {
	s5l_music_devices,
	sizeof(s5l_music_devices) / sizeof(s5l_music_devices[0]),
	s5l_mus_init,
	s5l_mus_shutdown,
	s5l_mus_set_volume,
	s5l_mus_pause,
	s5l_mus_resume,
	s5l_mus_register,
	s5l_mus_unregister,
	s5l_mus_play,
	s5l_mus_stop,
	s5l_mus_is_playing,
	s5l_mus_poll,
};

/* --------------------------------------------------------------------------
 * doomgeneric Platform Callbacks (DG_*)
 * -------------------------------------------------------------------------- */
static uint64_t start_epoch_ms;

static uint64_t monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void cleanup_platform(void)
{
	set_mcu_game_map(0);
	s5l_sfx_shutdown();
	if (fb_mem && fb_mem != MAP_FAILED) {
		memset(fb_mem, 0, fb_mmap_len);
		munmap(fb_mem, fb_mmap_len);
		fb_mem = NULL;
	}
	if (fb_fd >= 0) {
		close(fb_fd);
		fb_fd = -1;
	}
}

static void on_term_signal(int sig)
{
	(void)sig;
	cleanup_platform();
	_exit(0);
}

void DG_Init(void)
{
	start_epoch_ms = monotonic_ms();
	atexit(cleanup_platform);
	signal(SIGINT, on_term_signal);
	signal(SIGTERM, on_term_signal);
	signal(SIGHUP, on_term_signal);

	set_mcu_game_map(1);

#ifdef __linux__
	rescan_evdevs();
	fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fb_fd >= 0) {
		struct fb_var_screeninfo vinfo;
		struct fb_fix_screeninfo finfo;

		if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0 &&
		    ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) == 0) {
			fb_width = vinfo.xres ? vinfo.xres : FB_TARGET_W;
			fb_height = vinfo.yres ? vinfo.yres : FB_TARGET_H;
			fb_stride_px = (finfo.line_length >= fb_width * 4)
				? (finfo.line_length / 4) : fb_width;
			fb_mmap_len = finfo.smem_len ? finfo.smem_len
				: (size_t)fb_stride_px * fb_height * 4;
			fb_mem = mmap(NULL, fb_mmap_len, PROT_READ | PROT_WRITE,
				      MAP_SHARED, fb_fd, 0);
			if (fb_mem != MAP_FAILED)
				memset(fb_mem, 0, fb_mmap_len);
			else
				fb_mem = NULL;
		}
	}
#endif

	if (!fb_mem) {
		headless_fb = calloc((size_t)FB_TARGET_W * FB_TARGET_H, sizeof(uint32_t));
	}
}

void DG_DrawFrame(void)
{
	uint32_t *dst_base = fb_mem ? fb_mem : headless_fb;
	const uint8_t *src = (const uint8_t *)I_VideoBuffer;
	uint32_t out_w, out_h, scale, view_w, view_h, x_off, y_off;
	uint32_t y_src, x, s;

	if (!dst_base || !src)
		return;

	if (palette_changed)
		update_palette32();

#ifdef __linux__
	poll_linux_input();
#endif

	/*
	 * Uniform integer upscaler from Doom's 320x200 paletted framebuffer
	 * into the 1280x720 CEA-861 Title Safe area:
	 *   - Default: exact 3x3 integer scale (960x600) centered at
	 *     x_off=160, y_off=60 (60 px top/bottom and 160 px left/right
	 *     margins prevent TV overscan from clipping the top HUD/menu
	 *     and eliminate fractional 3.6x vertical row distortion).
	 *   - Widescreen (-widescreen): 16:9 (1066x600) centered at
	 *     x_off=107, y_off=60 inside the Title Safe area.
	 */
	out_w = (fb_width < FB_TARGET_W) ? fb_width : FB_TARGET_W;
	out_h = (fb_height < FB_TARGET_H) ? fb_height : FB_TARGET_H;

	scale = out_h / SCREENHEIGHT;
	if (scale < 1)
		scale = 1;
	if (out_w / SCREENWIDTH < scale)
		scale = out_w / SCREENWIDTH;

	view_h = SCREENHEIGHT * scale;
	view_w = opt_widescreen ? ((view_h * 16U) / 9U) : (SCREENWIDTH * scale);
	if (view_w > out_w)
		view_w = out_w;

	x_off = (out_w - view_w) / 2U;
	y_off = (out_h - view_h) / 2U;

	if (rendered_frames == 0) {
		uint32_t y;

		for (y = 0; y < y_off; y++)
			memset(dst_base + y * fb_stride_px, 0, out_w * sizeof(uint32_t));
		for (y = y_off + view_h; y < out_h; y++)
			memset(dst_base + y * fb_stride_px, 0, out_w * sizeof(uint32_t));
	}

	memset(row_Staging, 0, x_off * sizeof(uint32_t));
	if (x_off + view_w < out_w) {
		memset(row_Staging + x_off + view_w, 0,
		       (out_w - (x_off + view_w)) * sizeof(uint32_t));
	}

	if (I_HiresViewActive && scale == HIRES_SCALE &&
	    (uint32_t)I_HiresWidth <= out_w) {
		uint32_t y_hr;
		uint32_t vx0 = (uint32_t)viewwindowx;
		uint32_t vx1 = (uint32_t)(viewwindowx + scaledviewwidth);
		uint32_t vy0 = (uint32_t)viewwindowy;
		uint32_t vy1 = (uint32_t)(viewwindowy + viewheight);

		for (y_hr = 0; y_hr < view_h; y_hr++) {
			uint32_t y_dst = y_off + y_hr;
			uint32_t y2d = y_hr / HIRES_SCALE;
			const uint8_t *srow = src + y2d * SCREENWIDTH;
			uint32_t *d = row_Staging + x_off;

			if (y_dst >= out_h)
				break;

			if (y_hr >= vy0 && y_hr < vy1) {
				const uint8_t *hrow = (const uint8_t *)I_HiresBuffer +
						      y_hr * view_w;
				const uint8_t *mrow = (const uint8_t *)I_UiOverlayMask +
						      y2d * SCREENWIDTH;

				if (!opt_widescreen && vx0 == 0 && vx1 == view_w) {
					for (x = 0; x < SCREENWIDTH; x++) {
						if (mrow[x]) {
							uint32_t c = pal32[srow[x]];

							d[0] = c;
							d[1] = c;
							d[2] = c;
						} else {
							d[0] = pal32[hrow[0]];
							d[1] = pal32[hrow[1]];
							d[2] = pal32[hrow[2]];
						}
						hrow += 3;
						d += 3;
					}
				} else {
					for (x = 0; x < view_w; x++) {
						uint32_t x2d = (x * SCREENWIDTH) / view_w;

						if (x >= vx0 && x < vx1 && !mrow[x2d])
							d[x] = pal32[hrow[x]];
						else
							d[x] = pal32[srow[x2d]];
					}
				}
			} else {
				if (!opt_widescreen) {
					for (x = 0; x < SCREENWIDTH; x++) {
						uint32_t c = pal32[srow[x]];

						d[0] = c;
						d[1] = c;
						d[2] = c;
						d += 3;
					}
				} else {
					for (x = 0; x < view_w; x++)
						d[x] = pal32[srow[(x * SCREENWIDTH) / view_w]];
				}
			}

			memcpy(dst_base + y_dst * fb_stride_px, row_Staging,
			       out_w * sizeof(uint32_t));
		}
	} else {
		for (y_src = 0; y_src < SCREENHEIGHT; y_src++) {
			const uint8_t *srow = src + y_src * SCREENWIDTH;
			uint32_t *d = row_Staging + x_off;

			if (!opt_widescreen && scale == 3) {
				for (x = 0; x < SCREENWIDTH; x++) {
					uint32_t c = pal32[srow[x]];

					d[0] = c;
					d[1] = c;
					d[2] = c;
					d += 3;
				}
			} else {
				for (x = 0; x < view_w; x++)
					d[x] = pal32[srow[(x * SCREENWIDTH) / view_w]];
			}

			for (s = 0; s < scale; s++) {
				uint32_t y_dst = y_off + y_src * scale + s;

				if (y_dst < out_h) {
					memcpy(dst_base + y_dst * fb_stride_px, row_Staging,
					       out_w * sizeof(uint32_t));
				}
			}
		}
	}

	rendered_frames++;
	if (opt_max_tics > 0 && rendered_frames >= opt_max_tics) {
		if (opt_dump_ppm)
			save_ppm_frame(opt_dump_ppm, dst_base, out_w, out_h, fb_stride_px);
		I_Quit();
	}
}

void DG_SleepMs(uint32_t ms)
{
	usleep((useconds_t)ms * 1000U);
}

uint32_t DG_GetTicksMs(void)
{
	return (uint32_t)(monotonic_ms() - start_epoch_ms);
}

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
#ifdef __linux__
	poll_linux_input();
#endif
	if (kq_tail == kq_head)
		return 0;

	*pressed = key_queue[kq_tail].pressed;
	*doomKey = key_queue[kq_tail].key;
	kq_tail = (kq_tail + 1) % KEYQUEUE_SIZE;
	return 1;
}

void DG_SetWindowTitle(const char *title)
{
	(void)title;
}

/* --------------------------------------------------------------------------
 * Main entry point with automatic WAD & persistent savegame discovery
 * -------------------------------------------------------------------------- */
static const char *const wad_candidates[] = {
	"/mnt/data/linux/games/doom/doom1.wad",
	"/mnt/data/linux/games/doom/doom.wad",
	"/mnt/data/linux/games/doom/doom2.wad",
	"/mnt/data/linux/games/doom/plutonia.wad",
	"/mnt/data/linux/games/doom/tnt.wad",
	"/mnt/data/linux/games/doom/freedoom1.wad",
	"/mnt/data/linux/games/doom/freedoom2.wad",
	"/usr/local/share/games/doom/doom1.wad",
	"/usr/local/share/games/doom/freedoom1.wad",
	"/usr/share/games/doom/doom1.wad",
	"/usr/share/games/doom/doom.wad",
	"/usr/share/games/doom/doom2.wad",
	"/usr/share/games/doom/freedoom1.wad",
	"/usr/share/games/doom/freedoom2.wad",
	"build/dl/doom/freedoom1.wad",
	"freedoom1.wad",
	NULL
};

static void mkdir_p(const char *dir)
{
	char tmp[256];
	char *p;

	snprintf(tmp, sizeof(tmp), "%s", dir);
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

int main(int argc, char **argv)
{
	char **new_argv;
	char iwad_abs[PATH_MAX];
	int new_argc = 0;
	bool has_iwad = false;
	int i;

	new_argv = calloc((size_t)argc + 8, sizeof(char *));
	if (!new_argv)
		return 1;

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-iwad") == 0 && i + 1 < argc) {
			has_iwad = true;
			new_argv[new_argc++] = argv[i++];
			if (realpath(argv[i], iwad_abs))
				new_argv[new_argc++] = strdup(iwad_abs);
			else
				new_argv[new_argc++] = argv[i];
			continue;
		} else if (strcmp(argv[i], "-widescreen") == 0) {
			opt_widescreen = true;
			continue;
		} else if (strcmp(argv[i], "-dump-ppm") == 0 && i + 1 < argc) {
			opt_dump_ppm = argv[++i];
			continue;
		} else if (strcmp(argv[i], "-tics") == 0 && i + 1 < argc) {
			opt_max_tics = atoi(argv[++i]);
			continue;
		}
		new_argv[new_argc++] = argv[i];
	}

	if (!has_iwad) {
		for (i = 0; wad_candidates[i] != NULL; i++) {
			if (access(wad_candidates[i], R_OK) == 0) {
				const char *resolved = realpath(wad_candidates[i], iwad_abs)
					? strdup(iwad_abs) : wad_candidates[i];
				new_argv[new_argc++] = "-iwad";
				new_argv[new_argc++] = (char *)resolved;
				printf("[doom] Selected IWAD: %s\n", resolved);
				break;
			}
		}
	}

	/*
	 * Ensure persistent config & savegame directory exists and chdir() into
	 * it before D_DoomMain() calls M_SetConfigDir(NULL) (which uses ".").
	 */
	if (access("/mnt/data/linux", W_OK) == 0) {
		mkdir_p("/mnt/data/linux/games/doom/saves");
		if (chdir("/mnt/data/linux/games/doom") == 0)
			M_SetConfigDir("/mnt/data/linux/games/doom/");
	} else {
		mkdir_p("/tmp/doom-saves");
		if (chdir("/tmp/doom-saves") == 0)
			M_SetConfigDir("/tmp/doom-saves/");
	}

	I_HiresWidth = opt_widescreen
		? ((SCREENHEIGHT * HIRES_SCALE * 16) / 9)
		: (SCREENWIDTH * HIRES_SCALE);

	doomgeneric_Create(new_argc, new_argv);

	for (;;)
		doomgeneric_Tick();

	return 0;
}
