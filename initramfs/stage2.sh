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
	# Stop the daemons that write to Data before unmounting it
	killall bluetoothd 2>/dev/null || true
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

# Compressed swap in RAM: the root filesystem already lives in RAM, so idle
# pages are packed with lz4 instead of being kept whole. A quarter of RAM
# (about 120 MiB) typically holds two to three times that in idle pages.
if [ -w /sys/block/zram0/disksize ]; then
	mem_kb=$(awk '/^MemTotal:/ { print $2 }' /proc/meminfo)
	echo lz4 >/sys/block/zram0/comp_algorithm 2>/dev/null || true
	if echo "$((mem_kb / 4))K" >/sys/block/zram0/disksize 2>/dev/null &&
		mkswap /dev/zram0 >/dev/null 2>&1 && swapon -p 100 /dev/zram0 2>/dev/null; then
		# zram swap is cheap compared to dropping and re-reading file pages from NAND
		echo 100 >/proc/sys/vm/swappiness 2>/dev/null || true
		log_msg "zram0: $((mem_kb / 4 / 1024)) MiB lz4 swap active"
	else
		log_msg "zram0: swap setup failed"
	fi
fi

# Bring up USB CDC-ECM + CDC-ACM gadget if UDC is active
if [ -d /sys/class/net/usb0 ] || ip link show usb0 >/dev/null 2>&1; then
	ip link set usb0 up 2>/dev/null || true
	ip addr add 10.55.0.2/24 dev usb0 2>/dev/null || true
	ip addr add 169.254.9.2/16 dev usb0 2>/dev/null || true
	log_msg "usb0 configured at 10.55.0.2/24 and 169.254.9.2/16"
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
GPUFW=/lib/firmware/s5l8947x
GPU_FILE=sgx543.fw
GLEVENT=gl-event.pds
VXD_FILE=vxd390.fw
IOP_FILE=iop.fw

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
	# The GPU microkernel lives inside the stock kernel image, not in a file of its own
	KIMG=/mnt/system/System/Library/Caches/com.apple.kernelcaches/kernelcache
	if [ ! -s "$GPUFW/$GPU_FILE" ] && [ -r "$KIMG" ] && command -v sgxcarve >/dev/null; then
		mkdir -p "$GPUFW"
		if out=$(sgxcarve "$KIMG" "$GPUFW/$GPU_FILE.tmp" 2>&1); then
			mv "$GPUFW/$GPU_FILE.tmp" "$GPUFW/$GPU_FILE"
			log_msg "nand0p1: provisioned SGX543 microkernel ($out)"
		else
			rm -f "$GPUFW/$GPU_FILE.tmp"
			log_msg "nand0p1: no SGX543 microkernel ($out)"
		fi
	fi
	# The video decoder and IOP firmwares are in the same stock kernel image
	if [ ! -s "$GPUFW/$VXD_FILE" ] && [ -r "$KIMG" ] && command -v vxdcarve >/dev/null; then
		mkdir -p "$GPUFW"
		if out=$(vxdcarve "$KIMG" "$GPUFW/$VXD_FILE.tmp" 2>&1); then
			mv "$GPUFW/$VXD_FILE.tmp" "$GPUFW/$VXD_FILE"
			log_msg "nand0p1: provisioned VXD decoder firmware ($out)"
		else
			rm -f "$GPUFW/$VXD_FILE.tmp"
			log_msg "nand0p1: no VXD decoder firmware ($out)"
		fi
	fi
	if [ ! -s "$GPUFW/$IOP_FILE" ] && [ -r "$KIMG" ] && command -v iopcarve >/dev/null; then
		mkdir -p "$GPUFW"
		if out=$(iopcarve "$KIMG" "$GPUFW/$IOP_FILE.tmp" 2>&1); then
			mv "$GPUFW/$IOP_FILE.tmp" "$GPUFW/$IOP_FILE"
			log_msg "nand0p1: provisioned IOP firmware ($out)"
		else
			rm -f "$GPUFW/$IOP_FILE.tmp"
			log_msg "nand0p1: no IOP firmware ($out)"
		fi
	fi
	# The stock graphics driver keeps the 3D pass's event PDS program as a
	# template in the system-wide code cache.  Two variants sit next to each
	# other and only one draws, so every candidate is tried through the
	# render node (sgxframe checks the pixels) and the one that works is
	# kept.  A render that does not finish restarts the microkernel; that is
	# expected for the candidates that are not the template.
	SCACHE=/mnt/system/System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv7
	if [ ! -s "$GPUFW/$GLEVENT" ] && [ -r "$SCACHE" ] &&
	   command -v glcarve >/dev/null && command -v sgxframe >/dev/null; then
		CAND=/tmp/gl-candidates
		mkdir -p "$GPUFW"
		rm -rf "$CAND"
		mkdir -p "$CAND"
		# the driver's own image first (fast); the whole cache if it is
		# not found under that name
		if ! out=$(glcarve -i IMGSGX543GLDriver -a "$SCACHE" "$CAND" 2>&1); then
			rm -rf "$CAND"
			mkdir -p "$CAND"
			out=$(glcarve -a "$SCACHE" "$CAND" 2>&1)
		fi
		found=""
		for f in "$CAND"/gl-event-*.pds; do
			[ -s "$f" ] || continue
			if sgxframe -e "$f" >/dev/null 2>&1; then
				cp "$f" "$GPUFW/$GLEVENT"
				found="$f"
				break
			fi
		done
		if [ -n "$found" ]; then
			log_msg "nand0p1: provisioned GPU event program ($(basename "$found"); $out)"
		else
			log_msg "nand0p1: no GPU event program drew a frame ($out)"
		fi
		rm -rf "$CAND"
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
GPUCACHE=/mnt/data/linux/lib/firmware/s5l8947x

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
if [ "$DATA" = 1 ]; then
	for f in $GPU_FILE $GLEVENT $VXD_FILE $IOP_FILE; do
		if [ ! -s "$GPUFW/$f" ] && [ -s "$GPUCACHE/$f" ]; then
			mkdir -p "$GPUFW"
			cp "$GPUCACHE/$f" "$GPUFW/$f.tmp" 2>/dev/null &&
				mv "$GPUFW/$f.tmp" "$GPUFW/$f" &&
				log_msg "nand0p2: firmware from cache: $f"
		fi
	done
fi
missing=0
for f in $FW_FILES; do
	[ -s "$FW/$f" ] || missing=1
done
for f in $GPU_FILE $GLEVENT $VXD_FILE $IOP_FILE; do
	[ -s "$GPUFW/$f" ] || missing=1
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
		for f in $GPU_FILE $GLEVENT $VXD_FILE $IOP_FILE; do
			if [ -s "$GPUFW/$f" ] && [ ! -s "$GPUCACHE/$f" ]; then
				mkdir -p "$GPUCACHE"
				cp "$GPUFW/$f" "$GPUCACHE/$f.tmp" 2>/dev/null &&
					mv "$GPUCACHE/$f.tmp" "$GPUCACHE/$f" &&
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
	# Bluetooth pairings are written by bluetoothd whenever a device pairs, so
	# its state directory lives on Data, which stays mounted for the session
	# (clean_shutdown unmounts it before reboot, halt and power-off)
	BTSTATE=/mnt/data/linux/var/lib/bluetooth
	mkdir -p "$BTSTATE" /var/lib/bluetooth
	chmod 700 "$BTSTATE" 2>/dev/null || true
	if mount --bind "$BTSTATE" /var/lib/bluetooth 2>/dev/null; then
		log_msg "nand0p2: Bluetooth pairings kept in /linux/var/lib/bluetooth"
	fi
	sync
	log_msg "nand0p2: Data stays mounted at /mnt/data"
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
