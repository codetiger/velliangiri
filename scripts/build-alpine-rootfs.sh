#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Build the Alpine armv7 initramfs cpio archive for S5L8947X (j33iap)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ALPINE_VER="${ALPINE_VER:-3.21.3}"
ALPINE_BRANCH="${ALPINE_VER%.*}"
ALPINE_ARCH="${ALPINE_ARCH:-armv7}"
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

# Install APK packages (dropbear, nfs-utils, busybox-static, Wi-Fi and Bluetooth tools) via pure-Python resolver
echo "[rootfs] Resolving and unpacking Alpine ${ALPINE_ARCH} packages ..."
python3 "${ROOT}/scripts/apk-unpack.py" \
	--repo "${REPO_MAIN}" \
	--dest "${APK_WORK}" \
	--rootfs "${STAGE_DIR}" \
	dropbear dropbear-scp nfs-utils busybox-static iw wpa_supplicant wireless-regdb \
	bluez dbus mtd-utils-ubi

(
	cd "${APK_WORK}"
	find . \( -type f -o -type l \) -print | while read -r rel; do
		mkdir -p "${STAGE_DIR}/$(dirname "${rel}")"
		cp -a "${rel}" "${STAGE_DIR}/${rel}"
	done
)

# Cross-compile the early entropy seeder, diagnostic tools, the WAV player, the BT patch/tools and the firmware carvers
# ("<installed name>:<source in scripts/tools>")
echo "[rootfs] Cross-compiling userspace tools (-O2 -mcpu=cortex-a9 -mfpu=neon) ..."
for tool in jitterseed:jitterseed.c peek:mmio-peek.c i2cpoke:i2c-poke.c wavplay:wavplay.c l2bench:l2bench.c fbtest:fbtest.c btscan:btscan.c hcdcarve:hcdcarve.c sgxcarve:sgxcarve.c glcarve:glcarve.c vxdcarve:vxdcarve.c iopcarve:iopcarve.c m2mtest:m2mtest.c m2mshow:m2mshow.c jpegtest:jpegtest.c sgxinfo:sgxinfo.c sgxgem:sgxgem.c sgx2d:sgx2d.c sgxpresent:sgxpresent.c nand-restore:nand-restore.c; do
	"${CROSS_CC}" -O2 -mcpu=cortex-a9 -mfpu=neon -Wall -Wextra -static \
		-I "${ROOT}/tools" \
		-o "${STAGE_DIR}/bin/${tool%%:*}" "${ROOT}/scripts/tools/${tool#*:}" -lm
	chmod 755 "${STAGE_DIR}/bin/${tool%%:*}"
done

# The first render the port runs: a clear through the TA/3D queue, built
# from the template-frame library in the Mesa driver (no Mesa headers).
SGXLIB="${ROOT}/tools/mesa-sgx/files/src/gallium/drivers/sgx"
if [ -f "${ROOT}/tools/sgx/sgxframe.c" ]; then
	"${CROSS_CC}" -O2 -mcpu=cortex-a9 -mfpu=neon -Wall -Wextra -static \
		-I "${ROOT}/tools" -I "${SGXLIB}" -o "${STAGE_DIR}/bin/sgxframe" \
		"${ROOT}/tools/sgx/sgxframe.c" \
		"${SGXLIB}/sgx_template.c" \
		"${SGXLIB}/sgx_rt.c" -lm
	chmod 755 "${STAGE_DIR}/bin/sgxframe"
fi

# Build and install the 1280x720@30 TV Shell (/bin/tvshell), its fonts, and on-device JSON catalogs
echo "[rootfs] Building 1280x720@30 TV Shell (/bin/tvshell) ..."
CROSS_CC="${CROSS_CC}" "${ROOT}/tools/tvshell/build.sh"
cp "${ROOT}/build/tvshell/tvshell" "${STAGE_DIR}/bin/tvshell"
chmod 755 "${STAGE_DIR}/bin/tvshell"

if [ ! -f "${DL_DIR}/fonts/PlusJakartaSans-Medium.ttf" ]; then
	"${ROOT}/tools/tvshell/fetch-fonts.sh"
fi
if [ ! -f "${DL_DIR}/placeholders/dawn_ridge.jpg" ] || [ ! -f "${DL_DIR}/gallery/IMG_6452.jpeg" ]; then
	"${ROOT}/tools/tvshell/prepare-media.sh"
fi
mkdir -p "${STAGE_DIR}/usr/share/fonts/tvshell"
for fn in \
	PlusJakartaSans-Medium.ttf \
	PlusJakartaSans-SemiBold.ttf \
	PlusJakartaSans-Bold.ttf \
	JetBrainsMono-Medium.ttf \
	JetBrainsMono-SemiBold.ttf; do
	cp "${DL_DIR}/fonts/${fn}" "${STAGE_DIR}/usr/share/fonts/tvshell/${fn}"
done

mkdir -p "${STAGE_DIR}/usr/share/tvshell" "${STAGE_DIR}/etc/tvshell"
for jn in screen.json supported_models.json apps.json tokens.json; do
	cp "${ROOT}/tools/tvshell/design/${jn}" "${STAGE_DIR}/usr/share/tvshell/${jn}"
done

# Build and install the native S5L8947X Doom port (/bin/doom) if fetched
if [ -d "${ROOT}/build/doom-src/doomgeneric/doomgeneric" ]; then
	echo "[rootfs] Building S5L8947X Doom (/bin/doom) ..."
	CROSS_CC="${CROSS_CC}" "${ROOT}/tools/doom/build.sh"
fi
if [ -x "${ROOT}/build/doom/doom" ]; then
	cp "${ROOT}/build/doom/doom" "${STAGE_DIR}/bin/doom"
	chmod 755 "${STAGE_DIR}/bin/doom"
fi
if [ "${INCLUDE_DOOM_WAD:-0}" = "1" ] && [ -s "${DL_DIR}/doom/freedoom1.wad" ]; then
	mkdir -p "${STAGE_DIR}/usr/share/games/doom"
	cp "${DL_DIR}/doom/freedoom1.wad" "${STAGE_DIR}/usr/share/games/doom/freedoom1.wad"
fi

# Install on-demand hardware test suite (/bin/hwtest); nothing runs at boot
TEST_DIR="${STAGE_DIR}/usr/share/velliangiri/tests"
mkdir -p "${TEST_DIR}"
cp "${ROOT}"/scripts/tests/*.sh "${TEST_DIR}/"
chmod 755 "${TEST_DIR}/hwtest.sh"
ln -sf /usr/share/velliangiri/tests/hwtest.sh "${STAGE_DIR}/bin/hwtest"

# Wi-Fi, Bluetooth and GPU firmware are never part of the image: stage2.sh
# provisions them at boot from the on-device System volume (/dev/nand0p1).
mkdir -p "${STAGE_DIR}/lib/firmware/brcm" "${STAGE_DIR}/lib/firmware/s5l8947x"

# Install initramfs boot and DHCP scripts
echo "[rootfs] Installing initramfs boot scripts ..."
mkdir -p "${STAGE_DIR}/sbin" "${STAGE_DIR}/etc/udhcpc" "${STAGE_DIR}/etc/dropbear" "${STAGE_DIR}/root/.ssh"
cp "${ROOT}/initramfs/init.sh" "${STAGE_DIR}/init"
cp "${ROOT}/initramfs/stage2.sh" "${STAGE_DIR}/sbin/velliangiri-stage2"
cp "${ROOT}/scripts/tools/nandboot-install.sh" "${STAGE_DIR}/sbin/nandboot-install"
cp "${ROOT}/scripts/tools/nand-migrate.sh" "${STAGE_DIR}/sbin/nand-migrate"
cp "${ROOT}/initramfs/udhcpc.sh" "${STAGE_DIR}/etc/udhcpc/default.script"
cp "${ROOT}/initramfs/wifi.sh" "${STAGE_DIR}/bin/wifi"
cp "${ROOT}/initramfs/bt.sh" "${STAGE_DIR}/bin/bt"
cp "${ROOT}/initramfs/ntpd-event.sh" "${STAGE_DIR}/etc/ntpd-event.sh"
chmod 755 "${STAGE_DIR}/init" "${STAGE_DIR}/sbin/velliangiri-stage2" \
	"${STAGE_DIR}/sbin/nandboot-install" "${STAGE_DIR}/sbin/nand-migrate" \
	"${STAGE_DIR}/etc/udhcpc/default.script" "${STAGE_DIR}/bin/wifi" \
	"${STAGE_DIR}/bin/bt" "${STAGE_DIR}/etc/ntpd-event.sh"

# Optional Wi-Fi network joined at boot: a wpa_supplicant.conf kept out of git
# (it holds the passphrase); without it, join at runtime with 'wifi connect'
WIFI_CONF="${WIFI_CONF:-${ROOT}/research/wpa_supplicant.conf}"
if [ -f "${WIFI_CONF}" ]; then
	echo "[rootfs] Installing Wi-Fi network configuration from ${WIFI_CONF} ..."
	mkdir -p "${STAGE_DIR}/etc/wpa_supplicant"
	cp "${WIFI_CONF}" "${STAGE_DIR}/etc/wpa_supplicant/wpa_supplicant.conf"
	chmod 600 "${STAGE_DIR}/etc/wpa_supplicant/wpa_supplicant.conf"
fi

# D-Bus system bus account for bluetoothd (package install scripts are not run)
grep -q '^messagebus:' "${STAGE_DIR}/etc/group" || echo 'messagebus:x:101:messagebus' >>"${STAGE_DIR}/etc/group"
grep -q '^messagebus:' "${STAGE_DIR}/etc/passwd" ||
	echo 'messagebus:x:100:101:messagebus:/var/run/dbus:/sbin/nologin' >>"${STAGE_DIR}/etc/passwd"
grep -q '^messagebus:' "${STAGE_DIR}/etc/shadow" 2>/dev/null || echo 'messagebus:!::0:::::' >>"${STAGE_DIR}/etc/shadow"

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
if command -v mksquashfs >/dev/null 2>&1; then
	if [ -s "${DL_DIR}/doom/freedoom1.wad" ]; then
		mkdir -p "${STAGE_DIR}/usr/share/games/doom"
		cp "${DL_DIR}/doom/freedoom1.wad" "${STAGE_DIR}/usr/share/games/doom/freedoom1.wad"
	fi
	if [ -d "${DL_DIR}/placeholders" ]; then
		mkdir -p "${STAGE_DIR}/usr/share/tvshell/placeholders"
		cp "${DL_DIR}/placeholders/"*.jpg "${STAGE_DIR}/usr/share/tvshell/placeholders/" 2>/dev/null || true
	fi
	if [ -d "${DL_DIR}/gallery" ]; then
		mkdir -p "${STAGE_DIR}/usr/share/tvshell/media/.frames"
		cp "${DL_DIR}/gallery/"*.jpeg "${DL_DIR}/gallery/"*.mov "${STAGE_DIR}/usr/share/tvshell/media/" 2>/dev/null || true
		cp "${DL_DIR}/gallery/.frames/"*.jpg "${STAGE_DIR}/usr/share/tvshell/media/.frames/" 2>/dev/null || true
	fi
	echo "[rootfs] Packing uncompressed SquashFS image -> ${ROOT}/out/rootfs.sqsh ..."
	mksquashfs "${STAGE_DIR}" "${ROOT}/out/rootfs.sqsh" \
		-noappend -all-root -noI -noD -noF -noX -b 131072 >/dev/null
	if [ -s "${ROOT}/build/mesa-sgx-install.tar" ] || [ -s "${ROOT}/build/weston-armv7.tar" ] || [ -s "${DL_DIR}/doom/freedoom1.wad" ] || [ -d "${DL_DIR}/placeholders" ]; then
		echo "[rootfs] Packing usr-local SquashFS image -> ${ROOT}/build/usr-local.sqsh ..."
		ULOCAL_STAGE="/tmp/velliangiri-usr-local-stage"
		rm -rf "${ULOCAL_STAGE}"
		mkdir -p "${ULOCAL_STAGE}/bin" "${ULOCAL_STAGE}/lib" "${ULOCAL_STAGE}/share"
		if [ -s "${ROOT}/build/mesa-sgx-install.tar" ]; then
			tar --no-same-owner -xf "${ROOT}/build/mesa-sgx-install.tar" -C "${ULOCAL_STAGE}" --strip-components=2
			chmod +x "${ULOCAL_STAGE}/lib/sgx-mesa/bin/"* 2>/dev/null || true
			ln -sf /usr/local/lib/sgx-mesa/bin/sgx-gl "${ULOCAL_STAGE}/bin/sgx-gl"
		fi
		if [ -s "${ROOT}/build/weston-armv7.tar" ]; then
			tar --no-same-owner -xf "${ROOT}/build/weston-armv7.tar" -C "${ULOCAL_STAGE}" --strip-components=2
		fi
		if [ -s "${DL_DIR}/doom/freedoom1.wad" ]; then
			mkdir -p "${ULOCAL_STAGE}/share/games/doom"
			cp "${DL_DIR}/doom/freedoom1.wad" "${ULOCAL_STAGE}/share/games/doom/freedoom1.wad"
		fi
		if [ -d "${DL_DIR}/placeholders" ]; then
			mkdir -p "${ULOCAL_STAGE}/share/tvshell/placeholders"
			cp "${DL_DIR}/placeholders/"*.jpg "${ULOCAL_STAGE}/share/tvshell/placeholders/" 2>/dev/null || true
		fi
		if [ -d "${DL_DIR}/gallery" ]; then
			mkdir -p "${ULOCAL_STAGE}/share/tvshell/media/.frames"
			cp "${DL_DIR}/gallery/"*.jpeg "${DL_DIR}/gallery/"*.mov "${ULOCAL_STAGE}/share/tvshell/media/" 2>/dev/null || true
			cp "${DL_DIR}/gallery/.frames/"*.jpg "${ULOCAL_STAGE}/share/tvshell/media/.frames/" 2>/dev/null || true
		fi
		mksquashfs "${ULOCAL_STAGE}" "${ROOT}/build/usr-local.sqsh" \
			-noappend -all-root -noI -noD -noF -noX -b 131072 >/dev/null
		rm -rf "${ULOCAL_STAGE}"
	fi
fi
rm -rf "${STAGE_DIR}" "${APK_WORK}"

echo "[rootfs] Ready: ${OUT_CPIO} ($(du -h "${OUT_CPIO}" | awk '{print $1}'))"
