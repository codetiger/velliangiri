#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Put the Mesa that tools/mesa-sgx/build.sh built onto the device.
#
#   tools/mesa-sgx/install.sh              # root@192.168.68.112
#   tools/mesa-sgx/install.sh HOST         # another address
#   tools/mesa-sgx/install.sh --system     # ... and make it the system's GL
#   tools/mesa-sgx/install.sh --no-cache   # ... and do not put it on Data
#
# Installs /usr/local/lib/sgx-mesa (libGL, libEGL, libGLESv1_CM, libGLESv2,
# libgbm, libgallium and the test programs) and, with --system, puts its lib
# ahead of /usr/lib in /etc/ld-musl-armhf.path, so every program loads this
# Mesa.  MESA_BUILD=release installs the release build.
#
# The root filesystem is a ramfs, so the install is gone after a reboot.  A
# copy of the same tarball is therefore left on Data (unless --no-cache);
# stage2 unpacks it on the next boot, before any GL program runs.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DEST=root@192.168.68.112
SYSTEM=0
CACHE=1

for a in "$@"; do
	case $a in
	--system) SYSTEM=1 ;;
	--no-cache) CACHE=0 ;;
	-*) echo "usage: $0 [--system] [--no-cache] [root@]HOST" >&2; exit 2 ;;
	*) DEST=$a ;;
	esac
done

case "${MESA_BUILD:-}" in
release) T=$ROOT/build/mesa-sgx-install-release.tar ;;
desktop) T=$ROOT/build/mesa-sgx-install-desktop.tar ;;
*)       T=$ROOT/build/mesa-sgx-install.tar ;;
esac
[ -f "$T" ] || { echo "error: $T is not built; run tools/mesa-sgx/build.sh" >&2; exit 1; }

PREFIX=/usr/local/lib/sgx-mesa
SSH="ssh -o BatchMode=yes -o ConnectTimeout=8 $DEST"

echo "==> installing $PREFIX on $DEST"
$SSH "rm -rf $PREFIX && mkdir -p $PREFIX /usr/local/bin && tar -xf - -C / && chmod +x $PREFIX/bin/* && ln -sf $PREFIX/bin/sgx-gl /usr/local/bin/sgx-gl" < "$T"

if [ "$SYSTEM" = 1 ]; then
	echo "==> $PREFIX/lib is the system's GL (alpine's stays installed)"
	$SSH "mkdir -p /etc && printf '%s\n' '$PREFIX/lib:/usr/local/lib:/lib:/usr/lib' > /etc/ld-musl-armhf.path"
fi

# Keep the tarball where stage2 can find it after a reboot.  The temporary
# name is on the same filesystem, so the rename is atomic: a boot never
# unpacks the file while it is being written.
if [ "$CACHE" = 1 ]; then
	if $SSH 'test -d /mnt/data/linux' &&
	   $SSH 'cat > /mnt/data/linux/lib/sgx-mesa-install.tar.tmp' < "$T"; then
		if $SSH 'mv /mnt/data/linux/lib/sgx-mesa-install.tar.tmp /mnt/data/linux/lib/sgx-mesa-install.tar'; then
			echo "==> cached on Data (stage2 unpacks it at the next boot)"
		else
			echo "warning: could not rename the cached tarball" >&2
		fi
	else
		echo "warning: Data is not mounted; the install will not survive a reboot" >&2
	fi
fi

$SSH "$PREFIX/bin/sgx-gl glclear 2>&1 | head -3 || true"
echo "==> done: $DEST:$PREFIX"
