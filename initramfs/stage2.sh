#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Stage 2 service bring-up for S5L8947X (j33iap)

trap 'sync; reboot -f' TERM
trap 'sync; halt -f' USR1
trap 'sync; poweroff -f' USR2

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export HOME=/root USER=root LOGNAME=root SHELL=/bin/sh

log_msg() {
	echo "[velliangiri] $*" >/dev/kmsg 2>/dev/null || echo "[velliangiri] $*"
}

log_msg "stage2 starting on $(uname -sr)"
hostname -F /etc/hostname 2>/dev/null || hostname velliangiri 2>/dev/null || true

# Start hardware watchdog supervision (/dev/watchdog) first so nothing can delay it
if [ -c /dev/watchdog ]; then
	watchdog -T 60 -t 15 /dev/watchdog
	log_msg "hardware watchdog daemon active (60s bite, 15s feed)"
fi

# Bring up USB CDC-ECM + CDC-ACM gadget if UDC is active
if [ -d /sys/class/net/usb0 ] || ip link show usb0 >/dev/null 2>&1; then
	ip link set usb0 up 2>/dev/null || true
	ip addr add 10.55.0.2/24 dev usb0 2>/dev/null || true
	log_msg "usb0 configured at 10.55.0.2/24"
fi

# Spawn interactive getty/shell on USB CDC-ACM (/dev/ttyGS0)
(
	while true; do
		if [ -c /dev/ttyGS0 ]; then
			/sbin/getty -i -n -l /bin/sh 115200 ttyGS0 2>/dev/null || sleep 1
		else
			sleep 1
		fi
	done
) &
log_msg "CDC-ACM console getty active on /dev/ttyGS0"

# Start Dropbear SSH server on port 22 in background
mkdir -p /etc/dropbear
if [ -x /usr/sbin/dropbear ]; then
	/usr/sbin/dropbear -R -E -p 22 2>/dev/kmsg &
	log_msg "dropbear SSH daemon listening on port 22 (root login permitted)"
fi

# Bring up Fast Ethernet (eth0) via DHCP in background
if [ -d /sys/class/net/eth0 ] || ip link show eth0 >/dev/null 2>&1; then
	ip link set eth0 up 2>/dev/null || true
	udhcpc -i eth0 -b -s /etc/udhcpc/default.script -p /run/udhcpc.eth0.pid >/dev/null 2>&1 &
	log_msg "eth0 link up; udhcpc started in background"
fi

log_msg "system ready"

# Keep PID 1 alive on tty0 / fallback loop and reap orphans via wait
cd "$HOME" 2>/dev/null || cd /
while true; do
	if [ -c /dev/tty0 ]; then
		/bin/sh -l </dev/tty0 >/dev/tty0 2>&1 &
		wait $!
		sleep 1
	else
		sleep 60 &
		wait $!
	fi
done
