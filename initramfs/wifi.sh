#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Wi-Fi helper for S5L8947X (j33iap): wpa_supplicant + udhcpc on wlan0
#
#   wifi scan                 list nearby networks
#   wifi connect SSID [PSK]   join a network (open if no PSK) and get a lease
#   wifi up                   start with the existing configuration
#   wifi down                 stop wpa_supplicant and DHCP
#   wifi status               association and address

IFACE=${WIFI_IFACE:-wlan0}
CONF=/etc/wpa_supplicant/wpa_supplicant.conf
CTRL=/run/wpa_supplicant
WPA_PID=/run/wpa_supplicant.$IFACE.pid
DHCP_PID=/run/udhcpc.$IFACE.pid

die() { echo "wifi: $*" >&2; exit 1; }

# The interface registers once the firmware is up, shortly after boot
wait_iface() {
	n=0
	while [ ! -d "/sys/class/net/$IFACE" ]; do
		n=$((n + 1))
		[ "$n" -gt "${1:-20}" ] && die "$IFACE not present (firmware not loaded?)"
		sleep 1
	done
}

stop() {
	for f in "$DHCP_PID" "$WPA_PID"; do
		[ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null
		rm -f "$f"
	done
	ip addr flush dev "$IFACE" 2>/dev/null
}

start() {
	[ -f "$CONF" ] || die "no $CONF; use 'wifi connect SSID [PSK]'"
	wait_iface
	stop
	ip link set "$IFACE" up || die "cannot bring $IFACE up"
	wpa_supplicant -B -i "$IFACE" -c "$CONF" -P "$WPA_PID" ||
		die "wpa_supplicant failed to start"
	udhcpc -i "$IFACE" -b -s /etc/udhcpc/default.script -p "$DHCP_PID" \
		>/dev/null 2>&1
}

case "$1" in
	scan)
		wait_iface
		if ! ip link show "$IFACE" | grep -q '<.*UP'; then
			ip link set "$IFACE" up
			# A scan issued while the interface is still coming up can spin
			sleep 2
		fi
		{ timeout 20 iw dev "$IFACE" scan 2>/dev/null ||
			{ sleep 2; timeout 20 iw dev "$IFACE" scan 2>/dev/null; }; } |
			awk '/^BSS/ { if (ssid != "") printf "%-32s %s dBm  %s MHz\n", ssid, sig, freq; ssid = ""; next }
			     /SSID:/ { sub(/^[ \t]*SSID: /, ""); ssid = $0 }
			     /signal:/ { sig = $2 }
			     /freq:/ { freq = $2 }
			     END { if (ssid != "") printf "%-32s %s dBm  %s MHz\n", ssid, sig, freq }' |
			sort -u
		;;
	connect)
		[ -n "$2" ] || die "usage: wifi connect SSID [PSK]"
		mkdir -p "${CONF%/*}"
		{
			echo "ctrl_interface=$CTRL"
			echo "update_config=1"
			if [ -n "$3" ]; then
				wpa_passphrase "$2" "$3" | grep -v '^[[:space:]]*#psk='
			else
				printf 'network={\n\tssid="%s"\n\tkey_mgmt=NONE\n}\n' "$2"
			fi
		} > "$CONF.new" || die "cannot write $CONF"
		chmod 600 "$CONF.new"
		mv "$CONF.new" "$CONF"
		start
		echo "wifi: joining '$2'; 'wifi status' shows the lease"
		;;
	up)
		start
		;;
	down)
		stop
		ip link set "$IFACE" down 2>/dev/null
		;;
	status)
		if [ -f "$WPA_PID" ]; then
			wpa_cli -p "$CTRL" -i "$IFACE" status 2>/dev/null |
				grep -E '^(ssid|bssid|freq|wpa_state|key_mgmt|ip_address)='
		else
			echo "wpa_supplicant not running"
		fi
		iw dev "$IFACE" link 2>/dev/null | grep -E 'signal|tx bitrate'
		ip addr show "$IFACE" 2>/dev/null | grep 'inet '
		;;
	*)
		sed -n 's/^#   //p' "$0"
		exit 2
		;;
esac
