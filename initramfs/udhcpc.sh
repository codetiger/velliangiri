#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# udhcpc lease script for S5L8947X eth0 with dynamic netconsole configuration

[ -z "$1" ] && exit 1

RESOLV_CONF="/etc/resolv.conf"

case "$1" in
	deconfig)
		ip addr flush dev "$interface" 2>/dev/null || true
		ip link set "$interface" up 2>/dev/null || true
		;;

	bound|renew)
		ip addr flush dev "$interface" 2>/dev/null || true
		if [ -n "$subnet" ]; then
			ip addr add "${ip}/${subnet}" dev "$interface" 2>/dev/null || true
		else
			ip addr add "${ip}/24" dev "$interface" 2>/dev/null || true
		fi

		if [ -n "$router" ]; then
			while ip route del default dev "$interface" 2>/dev/null; do
				:
			done
			for gw in $router; do
				ip route add default via "$gw" dev "$interface" 2>/dev/null || true
			done
		fi

		if [ -n "$dns" ]; then
			: > "$RESOLV_CONF"
			for ns in $dns; do
				echo "nameserver $ns" >> "$RESOLV_CONF"
			done
		fi

		echo "[velliangiri] $interface leased $ip (gw: ${router:-none})" >/dev/kmsg 2>/dev/null || true

		# Configure dynamic UDP netconsole broadcast on port 6666 if configfs is available
		if [ -d /sys/kernel/config/netconsole ] && [ "$interface" = "eth0" ]; then
			mount -t configfs none /sys/kernel/config 2>/dev/null || true
			mkdir -p /sys/kernel/config/netconsole/lan 2>/dev/null || true
			if [ -w /sys/kernel/config/netconsole/lan/enabled ]; then
				echo 0 > /sys/kernel/config/netconsole/lan/enabled 2>/dev/null || true
				echo "$interface" > /sys/kernel/config/netconsole/lan/dev_name 2>/dev/null || true
				echo "$ip" > /sys/kernel/config/netconsole/lan/local_ip 2>/dev/null || true
				echo "255.255.255.255" > /sys/kernel/config/netconsole/lan/remote_ip 2>/dev/null || true
				echo "ff:ff:ff:ff:ff:ff" > /sys/kernel/config/netconsole/lan/remote_mac 2>/dev/null || true
				echo 6666 > /sys/kernel/config/netconsole/lan/remote_port 2>/dev/null || true
				echo 1 > /sys/kernel/config/netconsole/lan/enabled 2>/dev/null || true
			fi
		fi
		;;
esac

exit 0
