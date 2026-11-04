// SPDX-License-Identifier: GPL-2.0-only
/*
 * Giri on Velliangiri OS — TV Input Multiplexer & App Supervisor
 *
 * Monitors /dev/input/event* and /dev/input (via inotify):
 *   - 7-button IR remote (Up, Down, Left, Right, Select/OK, Menu/Back, Play/Pause)
 *   - Optional USB / Bluetooth keyboard for Ask Giri text input
 *   - Toggles /sys/bus/serdev/devices/serial0-0/game_map between 0 (shell) and 1 (games)
 *   - Tracks 2-second Menu hold (2000 ms) to exit external apps or return home
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#ifdef __linux__
#include <linux/input.h>
#include <sys/inotify.h>
#endif
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "tvshell.h"

#define MAX_EVDEVS		16
#define SHELL_MENU_HOLD_MS	2000ULL
#define SHELL_DPAD_REPEAT_DELAY	260ULL
#define SHELL_DPAD_REPEAT_RATE	110ULL

struct evdev_slot {
	int fd;
	bool is_keyboard;
	char name[64];
};

static struct {
	struct evdev_slot devs[MAX_EVDEVS];
	int num_devs;
	int inotify_fd;
	bool headless;
	bool has_keyboard;
	bool shift_down;
	bool menu_down;
	bool menu_hold_fired;
	uint64_t menu_down_ms;
	uint16_t down_code;
	uint64_t down_ms;
	uint64_t last_repeat_ms;
	bool pending_ir_press;
	uint8_t last_ir_scan;
} inp = {
	.inotify_fd = -1,
};

uint64_t shell_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void shell_input_set_game_map(bool enable)
{
	static const char *const paths[] = {
		"/sys/bus/serial-base/devices/serial0-0/game_map",
		"/sys/devices/platform/soc@30000000/32600000.serial/32600000.serial:0/32600000.serial:0.0/serial0/serial0-0/game_map",
		"/sys/bus/serdev/devices/serial0-0/game_map",
	};
	const char *val = enable ? "1\n" : "0\n";
	size_t i;

	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		int fd = open(paths[i], O_WRONLY);

		if (fd >= 0) {
			(void)write(fd, val, 2);
			close(fd);
			return;
		}
	}
}

#ifdef __linux__
static bool test_bit(int bit, const unsigned long *array)
{
	return (array[bit / (sizeof(unsigned long) * 8)] >>
		(bit % (sizeof(unsigned long) * 8))) & 1UL;
}

static bool check_is_keyboard(int fd)
{
	unsigned long keybits[(KEY_MAX + sizeof(unsigned long) * 8) /
			      (sizeof(unsigned long) * 8)];

	memset(keybits, 0, sizeof(keybits));
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0)
		return false;
	return test_bit(KEY_Q, keybits) && test_bit(KEY_W, keybits) &&
	       test_bit(KEY_E, keybits) && test_bit(KEY_A, keybits);
}

static void rescan_evdevs(void)
{
	DIR *d;
	struct dirent *de;
	bool kb_found = false;
	int i;

	for (i = 0; i < inp.num_devs; i++) {
		if (inp.devs[i].fd >= 0)
			close(inp.devs[i].fd);
	}
	inp.num_devs = 0;

	d = opendir("/dev/input");
	if (!d)
		return;

	while ((de = readdir(d)) != NULL && inp.num_devs < MAX_EVDEVS) {
		char path[300];
		int fd;

		if (strncmp(de->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;

		struct evdev_slot *s = &inp.devs[inp.num_devs++];
		s->fd = fd;
		s->name[0] = '\0';
		(void)ioctl(fd, EVIOCGNAME(sizeof(s->name) - 1), s->name);
		s->is_keyboard = check_is_keyboard(fd);
		if (s->is_keyboard)
			kb_found = true;
	}
	closedir(d);
	inp.has_keyboard = kb_found;
}
#endif

int shell_input_init(bool headless)
{
	inp.headless = headless;
	if (headless)
		return 0;

	shell_input_set_game_map(false);
#ifdef __linux__
	inp.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inp.inotify_fd >= 0)
		(void)inotify_add_watch(inp.inotify_fd, "/dev/input",
					IN_CREATE | IN_DELETE);
	rescan_evdevs();
#endif
	return 0;
}

void shell_input_close(void)
{
	int i;

	for (i = 0; i < inp.num_devs; i++) {
		if (inp.devs[i].fd >= 0)
			close(inp.devs[i].fd);
	}
	inp.num_devs = 0;
	if (inp.inotify_fd >= 0) {
		close(inp.inotify_fd);
		inp.inotify_fd = -1;
	}
}

#ifdef __linux__
static char keycode_to_ascii(uint16_t code, bool shift)
{
	switch (code) {
	case KEY_A: return shift ? 'A' : 'a';
	case KEY_B: return shift ? 'B' : 'b';
	case KEY_C: return shift ? 'C' : 'c';
	case KEY_D: return shift ? 'D' : 'd';
	case KEY_E: return shift ? 'E' : 'e';
	case KEY_F: return shift ? 'F' : 'f';
	case KEY_G: return shift ? 'G' : 'g';
	case KEY_H: return shift ? 'H' : 'h';
	case KEY_I: return shift ? 'I' : 'i';
	case KEY_J: return shift ? 'J' : 'j';
	case KEY_K: return shift ? 'K' : 'k';
	case KEY_L: return shift ? 'L' : 'l';
	case KEY_M: return shift ? 'M' : 'm';
	case KEY_N: return shift ? 'N' : 'n';
	case KEY_O: return shift ? 'O' : 'o';
	case KEY_P: return shift ? 'P' : 'p';
	case KEY_Q: return shift ? 'Q' : 'q';
	case KEY_R: return shift ? 'R' : 'r';
	case KEY_S: return shift ? 'S' : 's';
	case KEY_T: return shift ? 'T' : 't';
	case KEY_U: return shift ? 'U' : 'u';
	case KEY_V: return shift ? 'V' : 'v';
	case KEY_W: return shift ? 'W' : 'w';
	case KEY_X: return shift ? 'X' : 'x';
	case KEY_Y: return shift ? 'Y' : 'y';
	case KEY_Z: return shift ? 'Z' : 'z';
	case KEY_1: return shift ? '!' : '1';
	case KEY_2: return shift ? '@' : '2';
	case KEY_3: return shift ? '#' : '3';
	case KEY_4: return shift ? '$' : '4';
	case KEY_5: return shift ? '%' : '5';
	case KEY_6: return shift ? '^' : '6';
	case KEY_7: return shift ? '&' : '7';
	case KEY_8: return shift ? '*' : '8';
	case KEY_9: return shift ? '(' : '9';
	case KEY_0: return shift ? ')' : '0';
	case KEY_SPACE: return ' ';
	case KEY_MINUS: return shift ? '_' : '-';
	case KEY_EQUAL: return shift ? '+' : '=';
	case KEY_DOT: return shift ? '>' : '.';
	case KEY_COMMA: return shift ? '<' : ',';
	case KEY_SLASH: return shift ? '?' : '/';
	case KEY_SEMICOLON: return shift ? ':' : ';';
	case KEY_APOSTROPHE: return shift ? '"' : '\'';
	default: return 0;
	}
}

static bool translate_ev_key(const struct evdev_slot *slot,
			     const struct input_event *ie,
			     struct shell_input_event *out)
{
	/*
	 * Track MCU_EVT_IR_KEY (0x10) scan frames via EV_MSC MSC_SCAN.
	 * Unlike 0x11 NEC repeat pulses, 0x10 frames always emit MSC_SCAN,
	 * allowing us to distinguish a rapid re-press of the same button
	 * (< 200ms after the previous tap) from a held-button repeat pulse.
	 */
	if (ie->type == EV_MSC && ie->code == MSC_SCAN) {
		uint8_t ir_code = (uint8_t)(ie->value & 0xfeU);

		if (ir_code == 0x04 &&
		    (inp.last_ir_scan == 0x5c || inp.last_ir_scan == 0x5e)) {
			return false;
		}
		inp.pending_ir_press = true;
		inp.last_ir_scan = ir_code;
		return false;
	}

	if (ie->type != EV_KEY)
		return false;

	bool is_new_ir_press = inp.pending_ir_press;
	inp.pending_ir_press = false;

	if (ie->code == KEY_LEFTSHIFT || ie->code == KEY_RIGHTSHIFT) {
		inp.shift_down = (ie->value != 0);
		return false;
	}

	uint64_t now = shell_now_ms();

	/*
	 * Emit SHELL_KEY_MENU immediately on key-down (0ms latency for closing
	 * modals or navigating back), while tracking a continuous 2-second hold
	 * across repeats (value == 2) to emit SHELL_KEY_MENU_HOLD (returns to
	 * Home Screen or terminates external apps in shell_app_run).
	 */
	if (ie->code == KEY_MENU || (!slot->is_keyboard && ie->code == KEY_ESC)) {
		if (ie->value == 1 || (ie->value == 2 && is_new_ir_press)) {
			inp.menu_down = true;
			inp.menu_hold_fired = false;
			inp.menu_down_ms = now;
			out->key = SHELL_KEY_MENU;
			return true;
		} else if (ie->value == 2 && inp.menu_down) {
			if (!inp.menu_hold_fired &&
			    now - inp.menu_down_ms >= SHELL_MENU_HOLD_MS) {
				inp.menu_hold_fired = true;
				out->key = SHELL_KEY_MENU_HOLD;
				return true;
			}
			return false;
		} else if (ie->value == 0) {
			inp.menu_down = false;
			inp.menu_hold_fired = false;
			return false;
		}
		return false;
	}

	/*
	 * Register navigation and action buttons immediately on key-down
	 * (value == 1) and on rapid physical IR re-taps (value == 2 with
	 * MSC_SCAN), while providing smooth controlled auto-repeat when a
	 * directional key is held down.
	 */
	if (ie->value == 0) {
		if (inp.down_code == ie->code)
			inp.down_code = 0;
		return false;
	}

	if (ie->value == 1 || (ie->value == 2 && is_new_ir_press)) {
		inp.down_code = ie->code;
		inp.down_ms = now;
		inp.last_repeat_ms = now;
	} else if (ie->value == 2) {
		bool can_repeat = (ie->code == KEY_LEFT || ie->code == KEY_RIGHT ||
				   ie->code == KEY_UP || ie->code == KEY_DOWN ||
				   ie->code == KEY_BACKSPACE);
		if (!can_repeat || inp.down_code != ie->code)
			return false;
		if (now - inp.down_ms < SHELL_DPAD_REPEAT_DELAY ||
		    now - inp.last_repeat_ms < SHELL_DPAD_REPEAT_RATE)
			return false;
		inp.last_repeat_ms = now;
	} else {
		return false;
	}

	switch (ie->code) {
	case KEY_LEFT:
		out->key = SHELL_KEY_LEFT;
		return true;
	case KEY_RIGHT:
		out->key = SHELL_KEY_RIGHT;
		return true;
	case KEY_UP:
		out->key = SHELL_KEY_UP;
		return true;
	case KEY_DOWN:
		out->key = SHELL_KEY_DOWN;
		return true;
	case KEY_ENTER:
	case KEY_KPENTER:
		out->key = SHELL_KEY_SELECT;
		return true;
	case KEY_PLAYPAUSE:
	case KEY_PLAY:
		out->key = SHELL_KEY_PLAY_PAUSE;
		return true;
	case KEY_ESC:
		out->key = SHELL_KEY_MENU;
		return true;
	case KEY_BACKSPACE:
		out->key = SHELL_KEY_BACKSPACE;
		return true;
	default:
		break;
	}

	if (!slot->is_keyboard) {
		if (ie->code == KEY_LEFTCTRL) {
			out->key = SHELL_KEY_SELECT;
			return true;
		}
		if (ie->code == KEY_SPACE) {
			out->key = SHELL_KEY_PLAY_PAUSE;
			return true;
		}
		return false;
	}

	char ch = keycode_to_ascii(ie->code, inp.shift_down);
	if (ch != 0) {
		out->key = SHELL_KEY_CHAR;
		out->ch = ch;
		return true;
	}
	return false;
}
#endif

bool shell_input_poll(struct shell_input_event *ev, int timeout_ms)
{
	memset(ev, 0, sizeof(*ev));
	if (inp.headless) {
		if (timeout_ms > 0)
			usleep((useconds_t)timeout_ms * 1000U);
		return false;
	}

#ifdef __linux__
	struct pollfd pfds[MAX_EVDEVS + 1];
	int nfds = 0, i, ret;

	/* Check if an active Menu press has crossed the 2-second hold threshold */
	if (inp.menu_down && !inp.menu_hold_fired) {
		uint64_t elapsed = shell_now_ms() - inp.menu_down_ms;

		if (elapsed >= SHELL_MENU_HOLD_MS) {
			inp.menu_hold_fired = true;
			ev->key = SHELL_KEY_MENU_HOLD;
			return true;
		}
		int rem = (int)(SHELL_MENU_HOLD_MS - elapsed);
		if (timeout_ms < 0 || rem < timeout_ms)
			timeout_ms = rem;
	}

	/* Drain buffered events before blocking in poll() */
	for (i = 0; i < inp.num_devs; i++) {
		struct input_event ie;

		while (inp.devs[i].fd >= 0 &&
		       read(inp.devs[i].fd, &ie, sizeof(ie)) == (ssize_t)sizeof(ie)) {
			if (translate_ev_key(&inp.devs[i], &ie, ev))
				return true;
		}
	}

	for (i = 0; i < inp.num_devs; i++) {
		pfds[nfds].fd = inp.devs[i].fd;
		pfds[nfds].events = POLLIN;
		pfds[nfds].revents = 0;
		nfds++;
	}
	if (inp.inotify_fd >= 0) {
		pfds[nfds].fd = inp.inotify_fd;
		pfds[nfds].events = POLLIN;
		pfds[nfds].revents = 0;
		nfds++;
	}

	ret = poll(pfds, (nfds_t)nfds, timeout_ms);
	if (ret <= 0) {
		if (inp.menu_down && !inp.menu_hold_fired &&
		    shell_now_ms() - inp.menu_down_ms >= SHELL_MENU_HOLD_MS) {
			inp.menu_hold_fired = true;
			ev->key = SHELL_KEY_MENU_HOLD;
			return true;
		}
		return false;
	}

	if (inp.inotify_fd >= 0 && (pfds[nfds - 1].revents & POLLIN)) {
		char ibuf[256];

		while (read(inp.inotify_fd, ibuf, sizeof(ibuf)) > 0)
			;
		rescan_evdevs();
	}

	for (i = 0; i < inp.num_devs; i++) {
		struct input_event ie;

		if (inp.devs[i].fd < 0)
			continue;
		while (read(inp.devs[i].fd, &ie, sizeof(ie)) == (ssize_t)sizeof(ie)) {
			if (translate_ev_key(&inp.devs[i], &ie, ev))
				return true;
		}
	}
#else
	if (timeout_ms > 0)
		usleep((useconds_t)timeout_ms * 1000U);
#endif
	return false;
}

/*
 * Run an external application (such as /bin/doom), releasing DRM master so
 * the app owns the display and monitoring the IR remote for a 2-second Menu
 * hold (2000 ms) to return to the shell at any time.
 */
int shell_app_run(const char *const argv[], bool game_keys)
{
	pid_t pid;
	int status = 0;

	if (!argv || !argv[0])
		return -EINVAL;

	shell_display_release();
	if (game_keys)
		shell_input_set_game_map(true);

	pid = fork();
	if (pid < 0) {
		shell_input_set_game_map(false);
		shell_display_acquire();
		return -errno;
	}
	if (pid == 0) {
		setpgid(0, 0);
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	setpgid(pid, pid);

	inp.menu_down = false;
	inp.menu_hold_fired = false;
	inp.down_code = 0;

	for (;;) {
		pid_t w = waitpid(pid, &status, WNOHANG);
		if (w == pid || (w < 0 && errno != EINTR))
			break;

		struct shell_input_event ev;
		if (shell_input_poll(&ev, 50) &&
		    ev.key == SHELL_KEY_MENU_HOLD) {
			/* 2-second Menu hold: terminate external app and return */
			kill(-pid, SIGTERM);
			usleep(150000);
			if (waitpid(pid, &status, WNOHANG) == 0) {
				kill(-pid, SIGKILL);
				(void)waitpid(pid, &status, 0);
			}
			break;
		}
	}

	shell_input_set_game_map(false);
	inp.menu_down = false;
	inp.menu_hold_fired = false;
	inp.down_code = 0;

#ifdef __linux__
	usleep(100000);
	int i;
	for (i = 0; i < inp.num_devs; i++) {
		struct input_event ie;
		while (inp.devs[i].fd >= 0 &&
		       read(inp.devs[i].fd, &ie, sizeof(ie)) == (ssize_t)sizeof(ie))
			;
	}
#endif

	shell_display_acquire();
	return status;
}
