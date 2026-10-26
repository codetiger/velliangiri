#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Stage 2 service bring-up for S5L8947X (j33iap)

export PATH=/sbin:/usr/sbin:/bin:/usr/bin

log_msg() {
	echo "[velliangiri] $*" >/dev/kmsg 2>/dev/null || echo "[velliangiri] $*"
}

log_msg "stage2 starting on $(uname -sr)"

# Start hardware watchdog supervision (/dev/watchdog)
if [ -c /dev/watchdog ]; then
	watchdog -T 60 -t 15 /dev/watchdog 2>/dev/null || true
	log_msg "hardware watchdog daemon active (60s bite, 15s feed)"
fi

# Bring up USB CDC-ECM + CDC-ACM gadget if UDC is active
if [ -d /sys/class/net/usb0 ] || ip link show usb0 >/dev/null 2>&1; then
	ip addr add 10.55.0.2/24 dev usb0 2>/dev/null || true
	ip link set usb0 up 2>/dev/null || true
	log_msg "usb0 configured at 10.55.0.2/24"
fi

# Bring up Fast Ethernet (eth0) via DHCP in background
if ip link show eth0 >/dev/null 2>&1; then
	ip link set eth0 up 2>/dev/null || true
	udhcpc -i eth0 -b -s /etc/udhcpc/default.script -p /run/udhcpc.eth0.pid >/dev/null 2>&1 &
	log_msg "eth0 link up; udhcpc started in background"
fi

# Start Dropbear SSH server on port 22
mkdir -p /etc/dropbear
if [ -x /usr/sbin/dropbear ]; then
	/usr/sbin/dropbear -R -B -E -p 22 2>/dev/kmsg || true
	log_msg "dropbear SSH daemon listening on port 22 (root login permitted)"
fi

# Spawn interactive shells on available consoles
if [ -c /dev/ttyGS0 ]; then
	(
		while true; do
			setsid /bin/sh </dev/ttyGS0 >/dev/ttyGS0 2>&1
			sleep 1
		done
	) &
fi

if [ -c /dev/tty0 ]; then
	(
		while true; do
			setsid /bin/sh </dev/tty0 >/dev/tty0 2>&1
			sleep 1
		done
	) &
fi

log_msg "system ready"

# Keep PID 1 alive and supervise UART0 console
while true; do
	if [ -c /dev/ttySAC0 ]; then
		setsid /bin/sh </dev/ttySAC0 >/dev/ttySAC0 2>&1 || sleep 2
	else
		sleep 60
	fi
done
