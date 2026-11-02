#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Build Weston 14 (DRM + GL renderer, kiosk shell) for the device, against
# the Mesa in tools/mesa-sgx (EGL/GLES/GBM through kmsro).
#
# Runs inside an Alpine armv7 image (alpine:3.21, whose libdisplay-info is
# still one Weston 14 accepts):
#
#   docker run --rm --platform linux/arm/v7 -v "$PWD":/workspace \
#       -w /workspace alpine:3.21 sh tools/weston/build.sh
#
# The result is build/weston-armv7.tar: a relocatable-ish /usr/local tree
# (weston, libweston, the DRM backend and GL renderer modules, the kiosk
# shell, weston-simple-egl, the xkb data and the runtime libraries), which
# tools/weston/install.sh puts on Data and stage2 unpacks at boot.
set -eu

ROOT=/workspace
SRC=$ROOT/build/weston-src/weston-14.0.0
BUILD=$ROOT/build/weston-build
STAGE=$ROOT/build/weston-stage
PREFIX=/usr/local
MESA=$PREFIX/lib/sgx-mesa
TAR=$ROOT/build/weston-armv7.tar

say() { printf '==> %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }

[ -d "$SRC" ] || fail "$SRC is missing -- run tools/weston/fetch.sh on the host"
[ -f "$ROOT/build/mesa-sgx-install.tar" ] \
	|| fail "build/mesa-sgx-install.tar is missing -- run tools/mesa-sgx/build.sh"

say "the build dependencies and the SGX Mesa"
apk add --no-cache build-base meson samurai pkgconf \
	libdrm-dev wayland-dev wayland-protocols libinput-dev \
	libxkbcommon-dev libseat-dev pixman-dev eudev-dev \
	libdisplay-info-dev libevdev-dev xkeyboard-config \
	libffi-dev expat-dev zlib-dev cairo-dev seatd >/dev/null
tar -xf "$ROOT/build/mesa-sgx-install.tar" -C /
export PKG_CONFIG_PATH=$MESA/lib/pkgconfig
export LD_LIBRARY_PATH=$MESA/lib

say "configuring"
rm -rf "$STAGE"
# Only what a kiosk compositor on this board needs: the DRM backend with the
# GL renderer, the kiosk shell, and the simple EGL/SHM clients.  No desktop
# shell (cairo/pango), no Xwayland, no pipewire/rdp/vnc/remoting, no lcms.
if [ ! -f "$BUILD/build.ninja" ]; then
meson setup "$BUILD" "$SRC" --prefix="$PREFIX" --libdir=lib \
	--buildtype=release -Db_ndebug=true \
	-Dbackend-drm=true -Dbackend-drm-screencast-vaapi=false \
	-Dbackend-headless=true -Dbackend-pipewire=false \
	-Dbackend-rdp=false -Dbackend-vnc=false -Dbackend-wayland=false \
	-Dbackend-x11=false -Dbackend-default=drm \
	-Drenderer-gl=true -Dxwayland=false -Dsystemd=false \
	-Dremoting=false -Dpipewire=false -Dscreenshare=false \
	-Dshell-desktop=false -Dshell-fullscreen=false -Dshell-ivi=false \
	-Dshell-kiosk=true -Dcolor-management-lcms=false \
	-Dimage-jpeg=false -Dimage-webp=false \
	-Ddemo-clients=false -Dsimple-clients=egl,shm \
	-Dtests=false -Ddoc=false -Dwcap-decode=false >/dev/null
fi

say "building"
ninja -C "$BUILD" >/dev/null
DESTDIR="$STAGE" ninja -C "$BUILD" install >/dev/null

say "stripping"
find "$STAGE$PREFIX" -type f -name '*.so*' -exec strip --strip-debug {} + 2>/dev/null || true
for f in "$STAGE$PREFIX"/bin/*; do strip --strip-debug "$f" 2>/dev/null || true; done

say "the xkb data libxkbcommon looks for"
mkdir -p "$STAGE$PREFIX/share/X11"
cp -a /usr/share/X11/xkb "$STAGE$PREFIX/share/X11/"

say "the runtime libraries (our Mesa is already on the device)"
# Everything the installed ELF files need, except the libraries the SGX Mesa
# provides and libc: those are resolved from /usr/local/lib/sgx-mesa.
libs=""
scan() {
	for f in "$@"; do
		[ -f "$f" ] || continue
		for d in $(LD_LIBRARY_PATH=$STAGE$PREFIX/lib:$STAGE$PREFIX/lib/weston:$STAGE$PREFIX/lib/libweston-14:$MESA/lib \
			ldd "$f" 2>/dev/null | awk '/=>/ {print $3} /^[[:space:]]*\// {print $1}'); do
			case "$d" in
			"$MESA"/*|/lib/ld-musl*|/lib/libc.*) continue ;;
			esac
			case " $libs " in *" $d "*) ;; *) libs="$libs $d" ;; esac
		done
	done
}
scan $(find "$STAGE$PREFIX" -type f)
# the dependencies of what was just collected, until nothing new appears
old=-1
while [ "$(echo "$libs" | wc -w)" != "$old" ]; do
	old=$(echo "$libs" | wc -w)
	scan $libs
done
for d in $libs; do
	cp -L "$d" "$STAGE$PREFIX/lib/$(basename "$d")"
done
say "$(echo "$libs" | wc -w) runtime libraries bundled"

say "the launcher and the seat daemon"
install -m 755 "$ROOT/tools/weston/run.sh" "$STAGE$PREFIX/bin/weston-run"
install -m 755 /usr/bin/seatd "$STAGE$PREFIX/bin/seatd"

say "packing"
tar -C "$STAGE" -cf "$TAR" usr
say "built: $STAGE"
say "packed: $TAR ($(du -h "$TAR" | cut -f1))"
