#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Top-level build orchestrator for Project Velliangiri (S5L8947X Linux 7.x)
#
# Usage:
#   ./scripts/velliangiri.sh image          # Build Docker cross-compile container
#   ./scripts/velliangiri.sh rootfs         # Build Alpine 3.21 armhf initramfs
#   ./scripts/velliangiri.sh kernel         # Build Linux 7.x zImage + DTB
#   ./scripts/velliangiri.sh bundle         # Pack self-relocating boot bundle
#   ./scripts/velliangiri.sh all            # Run rootfs + kernel + bundle
#   ./scripts/velliangiri.sh tvshell        # Build 1280x720@30 TV shell (/bin/tvshell)
#   ./scripts/velliangiri.sh doom           # Fetch & build S5L8947X Doom port (/bin/doom)
#   ./scripts/velliangiri.sh clean          # Clean build outputs (preserves downloaded tarballs)
#   ./scripts/velliangiri.sh mesa           # Build Mesa with the SGX driver (tools/mesa-sgx)
#   ./scripts/velliangiri.sh check          # Run clean-room guardrail verification
#   ./scripts/velliangiri.sh shell          # Open interactive shell in build container

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${IMAGE:-velliangiri-build}"
CMD="${1:-all}"
shift || true

build_image() {
	echo "[velliangiri] Building container image '${IMAGE}' ..."
	docker build -t "${IMAGE}" -f "${ROOT}/scripts/container.conf" "${ROOT}/scripts"
}

ensure_image() {
	docker image inspect "${IMAGE}" >/dev/null 2>&1 || build_image
}

# The kernel tree lives in a named volume: a case-sensitive Linux filesystem
# that survives between runs and keeps incremental builds fast.
docker_run() { # <-i|-it> <command...>
	local tty="$1"
	shift
	ensure_image
	docker run --rm "${tty}" \
		-v "${ROOT}:/workspace" \
		-v velliangiri-kbuild:/kbuild \
		-e KERNEL_TREE=/kbuild/linux \
		-e SSH_PUBKEY -e WIFI_CONF \
		-w /workspace \
		"${IMAGE}" "$@"
}

run_in_container() {
	if [ -f /.dockerenv ]; then
		"$@"
	else
		docker_run -i "$@"
	fi
}

case "${CMD}" in
	image)
		build_image
		;;
	rootfs)
		run_in_container bash scripts/build-alpine-rootfs.sh "$@"
		;;
	kernel)
		run_in_container bash scripts/build-kernel.sh "$@"
		;;
	bundle)
		run_in_container bash scripts/build-bundle.sh "$@"
		;;
	nandboot)
		run_in_container bash scripts/nandboot/build-nandboot.sh "$@"
		;;
	bootslot)
		run_in_container bash scripts/build-bootslot.sh "$@"
		;;
	all)
		run_in_container bash -c '
			set -euo pipefail
			bash scripts/build-alpine-rootfs.sh
			bash scripts/build-kernel.sh
			bash scripts/build-bundle.sh
			bash scripts/nandboot/build-nandboot.sh
			bash scripts/build-bootslot.sh
		'
		;;
	clean)
		echo "[velliangiri] Cleaning build outputs and staging directories ..."
		rm -rf "${ROOT}/out" \
			"${ROOT}/build/boot" \
			"${ROOT}/build/rootfs-stage" \
			"${ROOT}/build/initramfs-s5l8947x.cpio"
		;;
	check)
		bash "${ROOT}/scripts/check-no-proprietary.sh" --all
		;;
	mesa)
		# Mesa with the SGX543's driver: fetched on the host, built in the
		# Alpine armv7 image, packed into build/mesa-sgx-install.tar.
		# MESA_BUILD=desktop builds the X11/Wayland/GLX variant instead
		# (build/mesa-sgx-install-desktop.tar).
		if [ ! -d "${ROOT}/build/mesa/src" ]; then
			bash "${ROOT}/tools/mesa-sgx/fetch.sh"
		fi
		docker image inspect velliangiri-mesa >/dev/null 2>&1 ||
			docker build --platform linux/arm/v7 -t velliangiri-mesa \
				-f "${ROOT}/tools/mesa-sgx/container.conf" "${ROOT}/tools/mesa-sgx"
		docker volume inspect velliangiri-mesa >/dev/null 2>&1 ||
			docker volume create velliangiri-mesa >/dev/null
		docker run --rm --platform linux/arm/v7 \
			-v "${ROOT}/build/mesa/src":/from:ro -v velliangiri-mesa:/to \
			alpine:3.21 sh -c '[ -d /to/src/.git ] || { rm -rf /to/src && cp -a /from /to/src; }'
		docker run --rm --platform linux/arm/v7 -v "${ROOT}":/workspace \
			-e MESA_BUILD \
			-v velliangiri-mesa:/workspace/build/mesa -w /workspace \
			velliangiri-mesa sh tools/mesa-sgx/build.sh
		;;
	weston)
		# Weston 14 (DRM + GL, kiosk shell) against the SGX Mesa; the
		# result is build/weston-armv7.tar
		bash "${ROOT}/tools/weston/fetch.sh"
		docker run --rm --platform linux/arm/v7 \
			-v "${ROOT}":/workspace -w /workspace \
			alpine:3.21 sh tools/weston/build.sh
		;;
	tvshell)
		bash "${ROOT}/tools/tvshell/fetch-fonts.sh"
		if command -v armv7-linux-musleabihf-gcc >/dev/null 2>&1 ||
		   [ -x /opt/homebrew/bin/armv7-linux-musleabihf-gcc ] ||
		   command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1; then
			bash "${ROOT}/tools/tvshell/build.sh" "$@"
		else
			run_in_container bash tools/tvshell/build.sh "$@"
		fi
		;;
	doom)
		bash "${ROOT}/tools/doom/fetch.sh"
		if command -v armv7-linux-musleabihf-gcc >/dev/null 2>&1 ||
		   [ -x /opt/homebrew/bin/armv7-linux-musleabihf-gcc ] ||
		   command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1; then
			bash "${ROOT}/tools/doom/build.sh" "$@"
		else
			run_in_container bash tools/doom/build.sh "$@"
		fi
		;;
	shell)
		docker_run -it /bin/bash
		;;
	*)
		echo "Usage: $0 {all|image|rootfs|kernel|bundle|tvshell|doom|mesa|weston|clean|check|shell}" >&2
		exit 1
		;;
esac
