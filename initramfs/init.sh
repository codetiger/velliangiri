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
	echo "[velliangiri] $*" >/dev/kmsg 2>/dev/null || true
}

settle() { s_i=0; while [ $s_i -lt 5000 ]; do s_i=$((s_i+1)); done; }
blip()   { b_i=0; while [ $b_i -lt 1700 ]; do b_i=$((b_i+1)); done; }

msg "stage1 starting"

# Seed system clock from build timestamp before starting TLS/SSH services
if [ -r /etc/build-date ]; then
	read -r BUILD_EPOCH < /etc/build-date || true
	if [ -n "$BUILD_EPOCH" ]; then
		date -u -s "@$BUILD_EPOCH" >/dev/null 2>&1 || true
	fi
fi

# Seed kernel CRNG from clocksource jitter (~70 ms)
if [ -x /bin/jitterseed ]; then
	r=$(/bin/jitterseed 2>&1) && msg "entropy: $r" || msg "entropy: jitterseed failed: $r"
fi

# Log early kernel subsystem verdicts to kmsg / HDMI console
dmesg 2>/dev/null | grep -E "AIC-TIMER|apple-wdt|s5l-fe|dwc2" | while read -r line; do
	msg "verdict: $line"
done

hostname velliangiri 2>/dev/null || true
ip link set lo up 2>/dev/null || true

# Force clean USB gadget soft-disconnect/reconnect so the host enumerates
# CDC-ECM (usb0) and CDC-ACM (/dev/ttyGS0) freshly.
if ls /sys/class/udc/*/state >/dev/null 2>&1; then
	msg "forcing USB gadget re-enumeration"
	for u in /sys/class/udc/*/soft_connect; do
		echo disconnect > "$u" 2>/dev/null || true
	done
	settle
	for u in /sys/class/udc/*/soft_connect; do
		echo connect > "$u" 2>/dev/null || true
	done
	n=0
	st=""
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

exec /sbin/velliangiri-stage2
