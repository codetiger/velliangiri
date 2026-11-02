#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Put the Mesa that tools/mesa-sgx/build.sh built onto the device.
#
#   tools/mesa-sgx/install.sh              # root@192.168.68.112
#   tools/mesa-sgx/install.sh HOST         # another address
#   tools/mesa-sgx/install.sh --system     # ... and make it the system's GL
#
# Installs /usr/local/lib/sgx-mesa (libGL, libEGL, libGLESv1_CM, libGLESv2,
# libgbm, libgallium and the test programs) and, with --system, puts its lib
# ahead of /usr/lib in /etc/ld-musl-armhf.path, so every program loads this
# Mesa.  MESA_BUILD=release installs the release build.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DEST=root@192.168.68.112
SYSTEM=0

for a in "$@"; do
	case $a in
	--system) SYSTEM=1 ;;
	-*) echo "usage: $0 [--system] [root@]HOST" >&2; exit 2 ;;
	*) DEST=$a ;;
	esac
done

if [ "${MESA_BUILD:-}" = release ]; then T=$ROOT/build/mesa-sgx-install-release.tar
else T=$ROOT/build/mesa-sgx-install.tar
fi
[ -f "$T" ] || { echo "error: $T is not built; run tools/mesa-sgx/build.sh" >&2; exit 1; }

PREFIX=/usr/local/lib/sgx-mesa
SSH="ssh -o BatchMode=yes -o ConnectTimeout=8 $DEST"

echo "==> installing $PREFIX on $DEST"
$SSH "rm -rf $PREFIX && mkdir -p $PREFIX && tar -xf - -C / && chmod +x $PREFIX/bin/*" < "$T"

if [ "$SYSTEM" = 1 ]; then
	echo "==> $PREFIX/lib is the system's GL (alpine's stays installed)"
	$SSH "mkdir -p /etc && printf '%s\n' '$PREFIX/lib:/usr/local/lib:/lib:/usr/lib' > /etc/ld-musl-armhf.path"
fi

$SSH "$PREFIX/bin/sgx-gl glclear 2>&1 | head -3 || true"
echo "==> done: $DEST:$PREFIX"
