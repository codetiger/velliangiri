#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Stage 1 initramfs /init for S5L8947X (j33iap)

export PATH=/sbin:/usr/sbin:/bin:/usr/bin

mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true

# Ensure PID 1 has open stdin/stdout/stderr attached to the active console
if [ -c /dev/console ]; then
	exec </dev/console >/dev/console 2>&1
elif [ -c /dev/null ]; then
	exec </dev/null >/dev/null 2>&1
fi

mkdir -p /dev/pts /dev/shm /run /var/run /tmp
mount -t devpts -o mode=620,ptmxmode=0666 devpts /dev/pts 2>/dev/null || true
mount -t tmpfs tmpfs /dev/shm 2>/dev/null || true
mount -t tmpfs tmpfs /run 2>/dev/null || true
mount -t tmpfs tmpfs /tmp 2>/dev/null || true

msg() {
	read -r up _ < /proc/uptime 2>/dev/null || up="?"
	echo "[velliangiri +${up}s] $*" >/dev/kmsg 2>/dev/null || true
}

settle() { s_i=0; while [ $s_i -lt 5000 ]; do s_i=$((s_i+1)); done; }
blip()   { b_i=0; while [ $b_i -lt 1700 ]; do b_i=$((b_i+1)); done; }

msg "stage1 starting"

# The kernel sets the clock from the PMU RTC when that has been set since
# power-up; otherwise, or if it is older than the image, use the build
# timestamp so TLS/SSH services start with a plausible date. An RTC older
# than the image is wrong, so it gets the new time too.
if [ -r /etc/build-date ]; then
	read -r BUILD_EPOCH < /etc/build-date || true
	if [ -n "$BUILD_EPOCH" ] && [ "$(date -u +%s)" -lt "$BUILD_EPOCH" ]; then
		date -u -s "@$BUILD_EPOCH" >/dev/null 2>&1 || true
		[ -e /dev/rtc0 ] && hwclock -w -u -f /dev/rtc0 >/dev/null 2>&1 || true
	fi
fi

# Seed kernel CRNG from clocksource jitter asynchronously (~9 ms)
if [ -x /bin/jitterseed ]; then
	( r=$(/bin/jitterseed 2>&1) && msg "entropy: $r" || msg "entropy: jitterseed failed: $r" ) &
fi

hostname velliangiri 2>/dev/null || true
ip link set lo up 2>/dev/null || true

# Ensure USB gadget (CDC-ECM usb0 + CDC-ACM /dev/ttyGS0) is enumerated.
# If the built-in gadget already reached 'configured' during kernel init,
# avoid tearing down the link with a redundant soft_connect cycle.
if ls /sys/class/udc/*/state >/dev/null 2>&1; then
	st=""
	for u in /sys/class/udc/*/state; do
		st=$(cat "$u" 2>/dev/null)
	done
	if [ "$st" = "configured" ]; then
		msg "UDC already configured"
	else
		msg "forcing USB gadget re-enumeration (was ${st:-unknown})"
		for u in /sys/class/udc/*/soft_connect; do
			echo disconnect > "$u" 2>/dev/null || true
		done
		settle
		for u in /sys/class/udc/*/soft_connect; do
			echo connect > "$u" 2>/dev/null || true
		done
		n=0
		while [ $n -lt 150 ]; do
			n=$((n+1))
			for u in /sys/class/udc/*/state; do
				st=$(cat "$u" 2>/dev/null)
			done
			[ "$st" = "configured" ] && break
			blip
		done
		msg "UDC state=$st after $n polls"
	fi
fi

exec /sbin/velliangiri-stage2
