#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Stage 1 initramfs /init for S5L8947X (j33iap)

export PATH=/sbin:/usr/sbin:/bin:/usr/bin

mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
mkdir -p /dev/pts /dev/shm /run /var/run /tmp
mount -t devpts -o mode=620,ptmxmode=0666 devpts /dev/pts 2>/dev/null || true
mount -t tmpfs tmpfs /dev/shm 2>/dev/null || true
mount -t tmpfs tmpfs /run 2>/dev/null || true
mount -t tmpfs tmpfs /tmp 2>/dev/null || true

settle() { s_i=0; while [ $s_i -lt 5000 ]; do s_i=$((s_i+1)); done; }
blip()   { b_i=0; while [ $b_i -lt 1700 ]; do b_i=$((b_i+1)); done; }

# Seed system clock from build timestamp before starting TLS/SSH services
if [ -r /etc/build-date ]; then
	read -r BUILD_EPOCH < /etc/build-date || true
	if [ -n "$BUILD_EPOCH" ]; then
		date -u -s "@$BUILD_EPOCH" >/dev/null 2>&1 || true
	fi
fi

# Seed kernel CRNG from clocksource jitter (~70 ms)
if [ -x /bin/jitterseed ]; then
	/bin/jitterseed >/dev/kmsg 2>&1 || true
fi

hostname velliangiri 2>/dev/null || true
ip link set lo up 2>/dev/null || true

# Force clean USB gadget soft-disconnect/reconnect so the host enumerates
# CDC-ECM (usb0) and CDC-ACM (/dev/ttyGS0) freshly.
if ls /sys/class/udc/*/state >/dev/null 2>&1; then
	echo "[velliangiri] forcing USB gadget re-enumeration" >/dev/kmsg 2>/dev/null || true
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
	echo "[velliangiri] UDC state=$st after $n polls" >/dev/kmsg 2>/dev/null || true
fi

exec /sbin/velliangiri-stage2
