#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Stage 1 initramfs /init for S5L8947X (j33iap)

export PATH=/sbin:/usr/sbin:/bin:/usr/bin

mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
mkdir -p /dev/pts /dev/shm /run /var/run /tmp
mount -t devpts -o mode=620,ptmxmode=0666 devpts /dev/pts 2>/dev/null || true
mount -t tmpfs tmpfs /run 2>/dev/null || true
mount -t tmpfs tmpfs /tmp 2>/dev/null || true

# Seed system clock from build timestamp before starting TLS/SSH services
if [ -r /etc/build-date ]; then
	read -r BUILD_EPOCH < /etc/build-date || true
	if [ -n "$BUILD_EPOCH" ]; then
		date -u -s "@$BUILD_EPOCH" >/dev/null 2>&1 || true
	fi
fi

# Seed kernel CRNG from clocksource jitter (~70 ms)
if [ -x /bin/jitterseed ]; then
	/bin/jitterseed || true
fi

hostname velliangiri 2>/dev/null || true
ip link set lo up 2>/dev/null || true

exec /sbin/velliangiri-stage2
