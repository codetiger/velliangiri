#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Start Weston on the HDMI display through kmsro, and run a client.
#
#   /usr/local/bin/weston-run [seconds] [client...]
#
# The default client is weston-simple-egl (a spinning square rendered on the
# SGX and imported by the compositor as a dma-buf); weston-simple-shm is the
# shared-memory fallback.  Input is not required
# (--continue-without-input); the board has no USB host yet.  The compositor
# keeps the console's mode and exits after the seconds given (default 15),
# then the console is asked to repaint.
set -eu

PREFIX=/usr/local
RUNTIME=${XDG_RUNTIME_DIR:-/tmp/weston-run}
DURATION=${1:-15}
if [ "$#" -gt 0 ]; then shift; fi
if [ "$#" -eq 0 ]; then
	set -- weston-simple-egl
fi
CLIENTS=$*
LOG=/tmp/weston.log

mkdir -p "$RUNTIME"
chmod 700 "$RUNTIME"

export PATH="$PREFIX/bin:$PATH"
export XDG_RUNTIME_DIR=$RUNTIME
export XDG_SEAT=${XDG_SEAT:-seat0}

# libseat in this image has no builtin backend: start seatd for the seat.
# It is a daemon of its own (no udev), and Weston opens card1 through it.
SEATD_PID=
SOCK=${SEATD_SOCK:-/run/seatd.sock}
if [ ! -S "$SOCK" ]; then
	mkdir -p /run
	"$PREFIX/bin/seatd" -g root -l info >/tmp/seatd.log 2>&1 &
	SEATD_PID=$!
	i=0
	while [ ! -S "$SOCK" ] && [ "$i" -lt 50 ]; do
		i=$((i + 1)); sleep 0.1
	done
fi
export SEATD_SOCK=$SOCK
unset LIBSEAT_BACKEND
export LD_LIBRARY_PATH="$PREFIX/lib/sgx-mesa/lib:$PREFIX/lib:$PREFIX/lib/weston:$PREFIX/lib/libweston-14${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export XKB_CONFIG_ROOT="$PREFIX/share/X11/xkb"
export GBM_BACKENDS_PATH="$PREFIX/lib/sgx-mesa/lib/gbm"
export WESTON_DATA_DIR="$PREFIX/share/weston"

# Weston keeps the display's mode.  The console's fbcon keeps its own buffer
# and does not touch the CRTC while Weston is master.
weston --backend=drm --drm-device=card1 --renderer=gl \
	--continue-without-input --shell=kiosk --no-config \
	--socket=wayland-0 --idle-time=0 --log="$LOG" &
WPID=$!
trap 'kill "$WPID" 2>/dev/null || true; if [ -n "$SEATD_PID" ]; then kill "$SEATD_PID" 2>/dev/null || true; fi' EXIT INT TERM

# wait for the Wayland socket, and say why if the compositor died
i=0
while [ ! -S "$RUNTIME/wayland-0" ]; do
	i=$((i + 1))
	if ! kill -0 "$WPID" 2>/dev/null; then
		echo "weston exited; log:" >&2
		tail -20 "$LOG" >&2
		exit 1
	fi
	[ "$i" -lt 100 ] || { echo "weston did not make $RUNTIME/wayland-0" >&2; exit 1; }
	sleep 0.1
done

export WAYLAND_DISPLAY=wayland-0
pids=""
for c in $CLIENTS; do
	# shellcheck disable=SC2086
	timeout "$DURATION" "$c" ${CLIENT_ARGS:-} &
	pids="$pids $!"
done
for p in $pids; do wait "$p" 2>/dev/null || true; done
kill "$WPID" 2>/dev/null || true
wait "$WPID" 2>/dev/null || true

# Put the console's framebuffer back: after Weston drops DRM master the
# console is the one that owns the mode again, and a blank/unblank makes
# fbcon repaint.
if [ -w /sys/class/graphics/fb0/blank ]; then
	echo 1 > /sys/class/graphics/fb0/blank 2>/dev/null || true
	echo 0 > /sys/class/graphics/fb0/blank 2>/dev/null || true
fi
echo "weston-run: done (log: $LOG)"
