#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Bluetooth helper for S5L8947X (j33iap): BlueZ bluetoothctl wrapper
#
#   bt scan [SECONDS]     discover nearby Bluetooth devices
#   bt pair MAC           pair, trust and connect a Bluetooth device
#   bt connect MAC        connect an already-paired device
#   bt disconnect MAC     disconnect a device
#   bt status             list paired and connected devices

die() { echo "bt: $*" >&2; exit 1; }

ensure_daemon() {
	command -v bluetoothctl >/dev/null 2>&1 || die "bluetoothctl not installed"
	if ! pidof bluetoothd >/dev/null 2>&1; then
		mkdir -p /run/dbus /var/lib/bluetooth
		dbus-uuidgen --ensure 2>/dev/null || true
		dbus-daemon --system 2>/dev/null || true
		/usr/lib/bluetooth/bluetoothd >/dev/null 2>&1 &
		sleep 1
	fi
	bluetoothctl power on >/dev/null 2>&1 || true
}

case "${1:-}" in
	scan)
		ensure_daemon
		secs="${2:-8}"
		bluetoothctl --timeout "$secs" scan on >/dev/null 2>&1
		bluetoothctl devices 2>/dev/null | sed 's/^Device //'
		;;
	pair)
		[ -n "${2:-}" ] || die "usage: bt pair MAC"
		ensure_daemon
		mac="$2"
		{
			echo "power on"
			echo "agent NoInputNoOutput"
			echo "default-agent"
			echo "pair $mac"
			sleep 4
			echo "trust $mac"
			echo "connect $mac"
			sleep 2
			echo "quit"
		} | bluetoothctl >/dev/null 2>&1
		sync
		bluetoothctl info "$mac" 2>/dev/null | grep -E 'Name:|Paired:|Trusted:|Connected:'
		;;
	connect)
		[ -n "${2:-}" ] || die "usage: bt connect MAC"
		ensure_daemon
		bluetoothctl trust "$2" >/dev/null 2>&1 || true
		bluetoothctl connect "$2"
		;;
	disconnect)
		[ -n "${2:-}" ] || die "usage: bt disconnect MAC"
		ensure_daemon
		bluetoothctl disconnect "$2"
		;;
	status)
		ensure_daemon
		bluetoothctl devices Connected 2>/dev/null | sed 's/^Device /connected\t/'
		bluetoothctl devices Paired 2>/dev/null | sed 's/^Device /paired\t/'
		;;
	*)
		sed -n 's/^#   //p' "$0"
		exit 2
		;;
esac
