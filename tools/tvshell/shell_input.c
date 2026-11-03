// SPDX-License-Identifier: GPL-2.0-only
/*
 * Velliangiri 1280x720 TV Shell - Input Multiplexer, Keyboard Hotplug & App Supervisor
 *
 * Monitors /dev/input/event* and /dev/input (via inotify):
 *   - Distinguishes the 7-button IR remote from Bluetooth keyboards via EVIOCGBIT
 *   - Toggles /sys/bus/serdev/devices/serial0-0/game_map between 0 (shell) and 1 (games)
 *   - Tracks 2-second Menu hold (SHELL_TIMING_MENU_HOLD_MS = 2000) to exit external apps
 *   - Translates keyboard events into printable ASCII for the Ask line
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

#define MAX_EVDEVS	16

struct evdev_slot {
	int fd;
	bool is_keyboard;
	char name[64];
};

static struct {
	struct evdev_slot devs[MAX_EVDEVS];
	int num_devs;
	int inotify_fd;
	bool has_keyboard;
	bool shift_down;
	bool menu_down;
	bool menu_hold_fired;
	uint64_t menu_down_ms;
} inp = {
	.inotify_fd = -1,
};

#ifdef __linux__
static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}
#endif

void shell_input_set_game_map(bool enable)
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

static bool rescan_evdevs(void)
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
		return false;

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

	bool changed = (kb_found != inp.has_keyboard);
	inp.has_keyboard = kb_found;
	return changed;
}
#endif

int shell_input_init(void)
{
	shell_input_set_game_map(false);
#ifdef __linux__
	inp.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inp.inotify_fd >= 0)
		(void)inotify_add_watch(inp.inotify_fd, "/dev/input",
					IN_CREATE | IN_DELETE);
	(void)rescan_evdevs();
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

bool shell_input_has_keyboard(void)
{
	return inp.has_keyboard;
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
			     struct shell_event *out)
{
	if (ie->type != EV_KEY)
		return false;

	if (ie->code == KEY_LEFTSHIFT || ie->code == KEY_RIGHTSHIFT) {
		inp.shift_down = (ie->value != 0);
		return false;
	}

	/* Track 2-second Menu hold across press (1), repeat (2), and release (0) */
	if (ie->code == KEY_MENU || (!slot->is_keyboard && ie->code == KEY_ESC)) {
		if (ie->value == 1) {
			inp.menu_down = true;
			inp.menu_hold_fired = false;
			inp.menu_down_ms = now_ms();
			return false;
		} else if (ie->value == 2 && inp.menu_down) {
			if (!inp.menu_hold_fired &&
			    now_ms() - inp.menu_down_ms >= SHELL_TIMING_MENU_HOLD_MS) {
				inp.menu_hold_fired = true;
				out->type = SHELL_EVT_MENU_HOLD;
				return true;
			}
			return false;
		} else if (ie->value == 0 && inp.menu_down) {
			bool emit_click = !inp.menu_hold_fired;
			inp.menu_down = false;
			inp.menu_hold_fired = false;
			if (emit_click) {
				out->type = SHELL_EVT_KEY_MENU;
				return true;
			}
			return false;
		}
	}

	/* Only act on key press (1) or repeat (2) for navigation and typing */
	if (ie->value == 0)
		return false;

	switch (ie->code) {
	case KEY_LEFT:
		out->type = SHELL_EVT_KEY_LEFT;
		return true;
	case KEY_RIGHT:
		out->type = SHELL_EVT_KEY_RIGHT;
		return true;
	case KEY_UP:
		out->type = SHELL_EVT_KEY_UP;
		return true;
	case KEY_DOWN:
		out->type = SHELL_EVT_KEY_DOWN;
		return true;
	case KEY_ENTER:
	case KEY_KPENTER:
		out->type = slot->is_keyboard ? SHELL_EVT_TEXT_ENTER : SHELL_EVT_KEY_OK;
		return true;
	case KEY_PLAYPAUSE:
	case KEY_PLAY:
		out->type = SHELL_EVT_KEY_PLAY;
		return true;
	case KEY_ESC:
		out->type = slot->is_keyboard ? SHELL_EVT_TEXT_ESC : SHELL_EVT_KEY_MENU;
		return true;
	case KEY_BACKSPACE:
		out->type = SHELL_EVT_TEXT_BACKSPACE;
		return true;
	default:
		break;
	}

	if (!slot->is_keyboard) {
		if (ie->code == KEY_LEFTCTRL) {
			out->type = SHELL_EVT_KEY_OK;
			return true;
		}
		if (ie->code == KEY_SPACE) {
			out->type = SHELL_EVT_KEY_PLAY;
			return true;
		}
		return false;
	}

	char ch = keycode_to_ascii(ie->code, inp.shift_down);
	if (ch != 0) {
		out->type = SHELL_EVT_TEXT_CHAR;
		out->codepoint = (uint8_t)ch;
		return true;
	}
	return false;
}
#endif

int shell_input_poll(struct shell_event *ev, int timeout_ms)
{
	memset(ev, 0, sizeof(*ev));

#ifdef __linux__
	struct pollfd pfds[MAX_EVDEVS + 1];
	int nfds = 0, i, ret;

	/* Check if an active Menu press has crossed the 2-second hold threshold */
	if (inp.menu_down && !inp.menu_hold_fired) {
		uint64_t elapsed = now_ms() - inp.menu_down_ms;

		if (elapsed >= SHELL_TIMING_MENU_HOLD_MS) {
			inp.menu_hold_fired = true;
			ev->type = SHELL_EVT_MENU_HOLD;
			return 1;
		}
		int rem = (int)(SHELL_TIMING_MENU_HOLD_MS - elapsed);
		if (timeout_ms < 0 || rem < timeout_ms)
			timeout_ms = rem;
	}

	/* First drain any buffered events before blocking in poll() */
	for (i = 0; i < inp.num_devs; i++) {
		struct input_event ie;

		while (inp.devs[i].fd >= 0 &&
		       read(inp.devs[i].fd, &ie, sizeof(ie)) == (ssize_t)sizeof(ie)) {
			if (translate_ev_key(&inp.devs[i], &ie, ev))
				return 1;
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
		    now_ms() - inp.menu_down_ms >= SHELL_TIMING_MENU_HOLD_MS) {
			inp.menu_hold_fired = true;
			ev->type = SHELL_EVT_MENU_HOLD;
			return 1;
		}
		return 0;
	}

	if (inp.inotify_fd >= 0 && (pfds[nfds - 1].revents & POLLIN)) {
		char ibuf[256];

		while (read(inp.inotify_fd, ibuf, sizeof(ibuf)) > 0)
			;
		if (rescan_evdevs()) {
			ev->type = inp.has_keyboard ? SHELL_EVT_KB_ATTACHED
						    : SHELL_EVT_KB_DETACHED;
			return 1;
		}
	}

	for (i = 0; i < inp.num_devs; i++) {
		struct input_event ie;

		if (inp.devs[i].fd < 0)
			continue;
		while (read(inp.devs[i].fd, &ie, sizeof(ie)) == (ssize_t)sizeof(ie)) {
			if (translate_ev_key(&inp.devs[i], &ie, ev))
				return 1;
		}
	}
#else
	if (timeout_ms > 0)
		usleep((useconds_t)timeout_ms * 1000U);
#endif
	return 0;
}

/*
 * Run an external application (such as /bin/doom), releasing DRM master so
 * the app owns the display and monitoring the IR remote for a 2-second Menu
 * hold (menuHoldMs: 2000) to return to the shell at any time.
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

	for (;;) {
		pid_t w = waitpid(pid, &status, WNOHANG);
		if (w == pid || (w < 0 && errno != EINTR))
			break;

		struct shell_event ev;
		if (shell_input_poll(&ev, 50) > 0 &&
		    ev.type == SHELL_EVT_MENU_HOLD) {
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

#ifdef __linux__
	/* Drain any residual button release events before returning to the shell */
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
