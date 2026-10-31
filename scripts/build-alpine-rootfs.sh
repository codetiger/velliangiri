#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Build the Alpine armhf initramfs cpio archive for S5L8947X (j33iap)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ALPINE_VER="${ALPINE_VER:-3.21.3}"
ALPINE_BRANCH="${ALPINE_VER%.*}"
ALPINE_ARCH="${ALPINE_ARCH:-armhf}"
ALPINE_MIRROR="${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org}"
TARBALL="alpine-minirootfs-${ALPINE_VER}-${ALPINE_ARCH}.tar.gz"
URL="${ALPINE_MIRROR}/alpine/v${ALPINE_BRANCH}/releases/${ALPINE_ARCH}/${TARBALL}"
REPO_MAIN="${ALPINE_MIRROR}/alpine/v${ALPINE_BRANCH}/main/${ALPINE_ARCH}"

DL_DIR="${ROOT}/build/dl"
STAGE_DIR="/tmp/velliangiri-rootfs-stage"
APK_WORK="/tmp/velliangiri-apk-unpack"
OUT_CPIO="${ROOT}/build/initramfs-s5l8947x.cpio"
CROSS_CC="${CROSS_COMPILE:-arm-linux-gnueabihf-}gcc"

mkdir -p "${DL_DIR}" "${ROOT}/build" "${ROOT}/out"

if [ ! -f "${DL_DIR}/${TARBALL}" ]; then
	echo "[rootfs] Downloading ${URL} ..."
	curl -fL --retry 3 -o "${DL_DIR}/${TARBALL}.part" "${URL}"
	mv "${DL_DIR}/${TARBALL}.part" "${DL_DIR}/${TARBALL}"
fi

echo "[rootfs] Extracting ${TARBALL} -> ${STAGE_DIR} ..."
rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}"
tar --no-same-owner --no-same-permissions -xzf "${DL_DIR}/${TARBALL}" -C "${STAGE_DIR}"

# Install APK packages (dropbear, nfs-utils, busybox-static, Wi-Fi tools) via pure-Python resolver
echo "[rootfs] Resolving and unpacking Alpine armhf packages ..."
python3 "${ROOT}/scripts/apk-unpack.py" \
	--repo "${REPO_MAIN}" \
	--dest "${APK_WORK}" \
	--rootfs "${STAGE_DIR}" \
	dropbear dropbear-scp nfs-utils busybox-static iw wpa_supplicant wireless-regdb

(
	cd "${APK_WORK}"
	find . \( -type f -o -type l \) -print | while read -r rel; do
		mkdir -p "${STAGE_DIR}/$(dirname "${rel}")"
		cp -a "${rel}" "${STAGE_DIR}/${rel}"
	done
)

# Cross-compile the early entropy seeder, diagnostic tools, the WAV player and the BT patch carver
# ("<installed name>:<source in scripts/tools>")
echo "[rootfs] Cross-compiling userspace tools ..."
for tool in jitterseed:jitterseed.c peek:mmio-peek.c i2cpoke:i2c-poke.c wavplay:wavplay.c l2bench:l2bench.c btscan:btscan.c hcdcarve:hcdcarve.c; do
	"${CROSS_CC}" -O2 -Wall -Wextra -static \
		-o "${STAGE_DIR}/bin/${tool%%:*}" "${ROOT}/scripts/tools/${tool#*:}" -lm
	chmod 755 "${STAGE_DIR}/bin/${tool%%:*}"
done

# Install on-demand hardware test suite (/bin/hwtest); nothing runs at boot
TEST_DIR="${STAGE_DIR}/usr/share/velliangiri/tests"
mkdir -p "${TEST_DIR}"
cp "${ROOT}"/scripts/tests/*.sh "${TEST_DIR}/"
chmod 755 "${TEST_DIR}/hwtest.sh"
ln -sf /usr/share/velliangiri/tests/hwtest.sh "${STAGE_DIR}/bin/hwtest"

# Wi-Fi and Bluetooth firmware are never part of the image: stage2.sh
# provisions them at boot from the on-device System volume (/dev/nand0p1).
mkdir -p "${STAGE_DIR}/lib/firmware/brcm"

# Install initramfs boot and DHCP scripts
echo "[rootfs] Installing initramfs boot scripts ..."
mkdir -p "${STAGE_DIR}/sbin" "${STAGE_DIR}/etc/udhcpc" "${STAGE_DIR}/etc/dropbear" "${STAGE_DIR}/root/.ssh"
cp "${ROOT}/initramfs/init.sh" "${STAGE_DIR}/init"
cp "${ROOT}/initramfs/stage2.sh" "${STAGE_DIR}/sbin/velliangiri-stage2"
cp "${ROOT}/initramfs/udhcpc.sh" "${STAGE_DIR}/etc/udhcpc/default.script"
cp "${ROOT}/initramfs/wifi.sh" "${STAGE_DIR}/bin/wifi"
chmod 755 "${STAGE_DIR}/init" "${STAGE_DIR}/sbin/velliangiri-stage2" "${STAGE_DIR}/etc/udhcpc/default.script" "${STAGE_DIR}/bin/wifi"

# Optional Wi-Fi network joined at boot: a wpa_supplicant.conf kept out of git
# (it holds the passphrase); without it, join at runtime with 'wifi connect'
WIFI_CONF="${WIFI_CONF:-${ROOT}/research/wpa_supplicant.conf}"
if [ -f "${WIFI_CONF}" ]; then
	echo "[rootfs] Installing Wi-Fi network configuration from ${WIFI_CONF} ..."
	mkdir -p "${STAGE_DIR}/etc/wpa_supplicant"
	cp "${WIFI_CONF}" "${STAGE_DIR}/etc/wpa_supplicant/wpa_supplicant.conf"
	chmod 600 "${STAGE_DIR}/etc/wpa_supplicant/wpa_supplicant.conf"
fi

# Record build timestamp for early clock initialization
date -u +%s > "${STAGE_DIR}/etc/build-date"
echo "velliangiri" > "${STAGE_DIR}/etc/hostname"

# Permit passwordless root login on local consoles / SSH bring-up
if [ -f "${STAGE_DIR}/etc/shadow" ]; then
	sed 's|^root:[^:]*:|root::|' "${STAGE_DIR}/etc/shadow" > "${STAGE_DIR}/etc/shadow.new"
	cat "${STAGE_DIR}/etc/shadow.new" > "${STAGE_DIR}/etc/shadow"
	rm -f "${STAGE_DIR}/etc/shadow.new"
fi

# Generate Dropbear ed25519 host key
python3 "${ROOT}/scripts/dropbear-hostkey.py" \
	"${STAGE_DIR}/etc/dropbear/dropbear_ed25519_host_key"

# Seed static /dev console and null nodes so console_on_rootfs() succeeds before /init
mkdir -p "${STAGE_DIR}/dev"
mknod -m 600 "${STAGE_DIR}/dev/console" c 5 1 2>/dev/null || true
mknod -m 666 "${STAGE_DIR}/dev/tty"     c 5 0 2>/dev/null || true
mknod -m 620 "${STAGE_DIR}/dev/tty0"    c 4 0 2>/dev/null || true
mknod -m 620 "${STAGE_DIR}/dev/tty1"    c 4 1 2>/dev/null || true
mknod -m 666 "${STAGE_DIR}/dev/null"    c 1 3 2>/dev/null || true
mknod -m 666 "${STAGE_DIR}/dev/zero"    c 1 5 2>/dev/null || true

# Install optional SSH public key if provided via SSH_PUBKEY env var
if [ -n "${SSH_PUBKEY:-}" ] && [ -f "${SSH_PUBKEY}" ]; then
	cp "${SSH_PUBKEY}" "${STAGE_DIR}/root/.ssh/authorized_keys"
	chmod 700 "${STAGE_DIR}/root/.ssh"
	chmod 600 "${STAGE_DIR}/root/.ssh/authorized_keys"
fi

# Prune unneeded files according to config/initramfs-prune.txt
PRUNE_LIST="${ROOT}/config/initramfs-prune.txt"
if [ -f "${PRUNE_LIST}" ]; then
	echo "[rootfs] Applying prune list ${PRUNE_LIST} ..."
	(
		cd "${STAGE_DIR}"
		grep -vE '^\s*(#|$)' "${PRUNE_LIST}" | while read -r pat; do
			# shellcheck disable=SC2086
			rm -rf $pat 2>/dev/null || true
		done
		if [ -f bin/busybox.static ] && [ -f bin/busybox ]; then
			cp bin/busybox.static bin/busybox
			ln -sf busybox bin/busybox.static
		fi
	)
fi

echo "[rootfs] Packing uncompressed cpio archive -> ${OUT_CPIO} ..."
(
	cd "${STAGE_DIR}"
	find . -print0 | cpio --null -o -H newc 2>/dev/null
) > "${OUT_CPIO}"

python3 "${ROOT}/scripts/cpio-owners.py" "${OUT_CPIO}"
cp "${OUT_CPIO}" "${ROOT}/out/initramfs-s5l8947x.cpio"
rm -rf "${STAGE_DIR}" "${APK_WORK}"

echo "[rootfs] Ready: ${OUT_CPIO} ($(du -h "${OUT_CPIO}" | awk '{print $1}'))"
