#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Stage 2 service bring-up for S5L8947X (j33iap)

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export HOME=/root USER=root LOGNAME=root SHELL=/bin/sh

log_msg() {
	echo "[velliangiri] $*" >/dev/kmsg 2>/dev/null || echo "[velliangiri] $*"
}

clean_shutdown() {
	log_msg "syncing and unmounting NAND filesystems before $1..."
	sync
	if [ -r /proc/mounts ]; then
		awk '$1 ~ /^\/dev\/nand/ || $3 == "hfsplus" { print $2 }' /proc/mounts | sort -r | while read -r mp; do
			if ! umount "$mp" 2>/dev/null; then
				mount -o remount,ro "$mp" 2>/dev/null || true
				umount -l "$mp" 2>/dev/null || true
			fi
		done
	fi
	sync
	"$1" -f
}

trap 'clean_shutdown reboot' TERM INT
trap 'clean_shutdown halt' USR1
trap 'clean_shutdown poweroff' USR2

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

FW=/lib/firmware/brcm
FW_FILES="brcmfmac4334.bin brcmfmac4334.txt BCM4334B0.hcd"
BT_REBIND=0
# The System volume carries firmware for several boards; this one's files carry its board name
BOARD=monopoly

# Copy the BCM4334 Wi-Fi firmware, NVRAM and Bluetooth patch out of the on-device
# System volume (/dev/nand0p1): its files are compressed, and the Bluetooth patch is
# embedded in a system tool
provision_from_system() {
	mkdir -p /mnt/system "$FW"
	mount -t hfsplus -o ro /dev/nand0p1 /mnt/system 2>/dev/null || return
	WDIR=/mnt/system/usr/share/firmware/wifi/4334b1
	if [ ! -s "$FW/brcmfmac4334.bin" ] && [ -r "$WDIR/$BOARD.trx" ]; then
		cp "$WDIR/$BOARD.trx" "$FW/brcmfmac4334.bin" 2>/dev/null
		cp "$WDIR/$BOARD-u-st.txt" "$FW/brcmfmac4334.txt" 2>/dev/null
		# No CLM blob: this firmware rejects a CLM download (-52) and brcmfmac then fails to attach
		log_msg "nand0p1: provisioned BCM4334 Wi-Fi firmware and NVRAM"
	fi
	# Each Bluetooth patch names its board in the local name it sets, so pick the
	# one patch naming this board
	BTOOL=/mnt/system/usr/sbin/BlueTool
	if [ ! -s "$FW/BCM4334B0.hcd" ] && [ -r "$BTOOL" ] && command -v hcdcarve >/dev/null; then
		if out=$(hcdcarve "$BTOOL" "MHz $BOARD " "$FW/BCM4334B0.hcd.tmp" 2>&1); then
			mv "$FW/BCM4334B0.hcd.tmp" "$FW/BCM4334B0.hcd"
			BT_REBIND=1
			log_msg "nand0p1: provisioned BCM4334B0.hcd Bluetooth patch ($out)"
		else
			rm -f "$FW/BCM4334B0.hcd.tmp"
			log_msg "nand0p1: no Bluetooth patch for this board ($out)"
		fi
	fi
	umount /mnt/system 2>/dev/null || true
}

# Wait briefly for the NAND translation layer (/dev/nand0p1) to finish probing
n=0
while [ ! -b /dev/nand0p1 ] && [ "$n" -lt 20 ]; do
	n=$((n + 1))
	sleep 1
done

# Data (/dev/nand0p2) holds the system keybag, a cache of the firmware above, the
# persistent SSH host key and Wi-Fi credentials
DATA=0
if [ -b /dev/nand0p2 ]; then
	mkdir -p /mnt/data
	if mount -t hfsplus -o rw /dev/nand0p2 /mnt/data 2>/dev/null; then
		DATA=1
		# Hand the system keybag to the FTL so files of protection classes 1-3 decrypt
		if [ -s /mnt/data/keybags/systembag.kb ] && [ -w /sys/block/nand0/keybag ]; then
			if cat /mnt/data/keybags/systembag.kb >/sys/block/nand0/keybag 2>/dev/null; then
				log_msg "nand0p2: system keybag loaded ($(grep -c ' unlocked' /sys/block/nand0/keybag) of 4 classes unlocked)"
			else
				log_msg "nand0p2: system keybag rejected"
			fi
		fi
	fi
fi
PDIR=/mnt/data/linux/etc
FWCACHE=/mnt/data/linux/lib/firmware/brcm

# Firmware: use the copy cached on Data, so later boots skip mounting System,
# inflating its compressed files and carving the Bluetooth patch
if [ "$DATA" = 1 ] && [ -d "$FWCACHE" ]; then
	mkdir -p "$FW"
	got=""
	for f in $FW_FILES; do
		if [ ! -s "$FW/$f" ] && [ -s "$FWCACHE/$f" ] && cp "$FWCACHE/$f" "$FW/$f.tmp" 2>/dev/null; then
			mv "$FW/$f.tmp" "$FW/$f"
			got="$got $f"
		fi
	done
	case "$got" in *BCM4334B0.hcd*) BT_REBIND=1 ;; esac
	[ -n "$got" ] && log_msg "nand0p2: firmware from cache:$got"
fi
missing=0
for f in $FW_FILES; do
	[ -s "$FW/$f" ] || missing=1
done
if [ "$missing" = 1 ] && [ -b /dev/nand0p1 ]; then
	provision_from_system
	if [ "$DATA" = 1 ]; then
		mkdir -p "$FWCACHE"
		for f in $FW_FILES; do
			if [ -s "$FW/$f" ] && [ ! -s "$FWCACHE/$f" ] && cp "$FW/$f" "$FWCACHE/$f.tmp" 2>/dev/null; then
				mv "$FWCACHE/$f.tmp" "$FWCACHE/$f"
				log_msg "nand0p2: cached $f"
			fi
		done
	fi
fi

if [ "$DATA" = 1 ]; then
	if [ -s "$PDIR/dropbear/dropbear_ed25519_host_key" ]; then
		mkdir -p /etc/dropbear
		cp "$PDIR/dropbear/dropbear_ed25519_host_key" /etc/dropbear/dropbear_ed25519_host_key 2>/dev/null
		chmod 600 /etc/dropbear/dropbear_ed25519_host_key
	elif [ -s /etc/dropbear/dropbear_ed25519_host_key ]; then
		mkdir -p "$PDIR/dropbear"
		cp /etc/dropbear/dropbear_ed25519_host_key "$PDIR/dropbear/dropbear_ed25519_host_key" 2>/dev/null
	fi
	if [ -s "$PDIR/wpa_supplicant/wpa_supplicant.conf" ]; then
		mkdir -p /etc/wpa_supplicant
		cp "$PDIR/wpa_supplicant/wpa_supplicant.conf" /etc/wpa_supplicant/wpa_supplicant.conf 2>/dev/null
		chmod 600 /etc/wpa_supplicant/wpa_supplicant.conf
		log_msg "nand0p2: loaded persistent wpa_supplicant.conf"
	elif [ -s /etc/wpa_supplicant/wpa_supplicant.conf ]; then
		mkdir -p "$PDIR/wpa_supplicant"
		cp /etc/wpa_supplicant/wpa_supplicant.conf "$PDIR/wpa_supplicant/wpa_supplicant.conf" 2>/dev/null
		chmod 600 "$PDIR/wpa_supplicant/wpa_supplicant.conf" 2>/dev/null
		log_msg "nand0p2: seeded persistent wpa_supplicant.conf"
	fi
	sync
	umount /mnt/data 2>/dev/null || true
fi

# Start Dropbear SSH server on port 22 in background
mkdir -p /etc/dropbear
if [ -x /usr/sbin/dropbear ]; then
	/usr/sbin/dropbear -B -R -E -p 22 2>/dev/kmsg &
	log_msg "dropbear SSH daemon listening on port 22 (root login permitted)"
fi

# Bring up Fast Ethernet (eth0) via DHCP in background
if [ -d /sys/class/net/eth0 ] || ip link show eth0 >/dev/null 2>&1; then
	EMAC=$(cat /sys/block/nand0/emac 2>/dev/null)
	if [ -n "$EMAC" ]; then
		ip link set eth0 down 2>/dev/null || true
		ip link set eth0 address "$EMAC" 2>/dev/null || true
	fi
	ip link set eth0 up 2>/dev/null || true
	udhcpc -i eth0 -b -s /etc/udhcpc/default.script -p /run/udhcpc.eth0.pid >/dev/null 2>&1 &
	log_msg "eth0 link up; udhcpc started in background"
fi

# Rebind wireless drivers if firmware or factory MAC was provisioned from NAND after initial probe
if [ -s /lib/firmware/brcm/brcmfmac4334.bin ] && [ ! -d /sys/class/net/wlan0 ]; then
	for udev in /sys/bus/usb/devices/*; do
		[ "$(cat "$udev/idVendor" 2>/dev/null)" = "0a5c" ] || continue
		uif="$udev:1.0"
		if [ -z "$(cat "$udev/bConfigurationValue" 2>/dev/null)" ]; then
			# A failed firmware load leaves the device unconfigured; configuring it re-probes brcmfmac
			echo 1 >"$udev/bConfigurationValue" 2>/dev/null || true
		elif [ -e "$uif" ] && [ ! -e "$uif/driver" ]; then
			echo "${uif##*/}" >/sys/bus/usb/drivers/brcmfmac/bind 2>/dev/null || true
		fi
	done
fi
if [ "$BT_REBIND" = 1 ] || [ -n "$(cat /sys/block/nand0/bmac 2>/dev/null)" ]; then
	for sdev in /sys/bus/serial/drivers/hci_uart_bcm/serial*-*; do
		[ -e "$sdev" ] || continue
		echo "${sdev##*/}" >/sys/bus/serial/drivers/hci_uart_bcm/unbind 2>/dev/null || true
		echo "${sdev##*/}" >/sys/bus/serial/drivers/hci_uart_bcm/bind 2>/dev/null || true
	done
fi

# D-Bus system bus and bluetoothd (bluetoothctl for pairing and connections)
if [ -x /usr/lib/bluetooth/bluetoothd ] && [ -x /usr/bin/dbus-daemon ]; then
	mkdir -p /run/dbus /var/lib/bluetooth
	dbus-uuidgen --ensure 2>/dev/null
	if dbus-daemon --system 2>/dev/null; then
		/usr/lib/bluetooth/bluetoothd >/dev/null 2>&1 &
		log_msg "dbus and bluetoothd started"
	fi
fi

# Join Wi-Fi in background when a network is configured
if [ -f /etc/wpa_supplicant/wpa_supplicant.conf ] && [ -x /bin/wifi ]; then
	/bin/wifi up >/dev/kmsg 2>&1 &
	log_msg "wlan0: wpa_supplicant and udhcpc starting in background"
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
