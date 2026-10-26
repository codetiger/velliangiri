#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Top-level build orchestrator for Project Velliangiri (S5L8947X Linux 6.12)
#
# Usage:
#   ./scripts/velliangiri.sh image          # Build Docker cross-compile container
#   ./scripts/velliangiri.sh rootfs         # Build Alpine 3.21 armhf initramfs
#   ./scripts/velliangiri.sh kernel         # Build Linux 6.12 zImage + DTB
#   ./scripts/velliangiri.sh bundle         # Pack self-relocating boot bundle
#   ./scripts/velliangiri.sh all            # Run rootfs + kernel + bundle
#   ./scripts/velliangiri.sh check          # Run clean-room guardrail verification
#   ./scripts/velliangiri.sh shell          # Open interactive shell in build container

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${IMAGE:-velliangiri-build}"
CMD="${1:-all}"
shift || true

ensure_image() {
	if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
		echo "[velliangiri] Building container image '${IMAGE}' ..."
		docker build -t "${IMAGE}" -f "${ROOT}/scripts/container.conf" "${ROOT}/scripts"
	fi
}

run_in_container() {
	if [ -f /.dockerenv ]; then
		"$@"
	else
		ensure_image
		docker run --rm -i \
			-v "${ROOT}:/workspace" \
			-v velliangiri-kbuild:/kbuild \
			-e KERNEL_TREE=/kbuild/linux \
			-w /workspace \
			"${IMAGE}" "$@"
	fi
}

case "${CMD}" in
	image)
		docker build -t "${IMAGE}" -f "${ROOT}/scripts/container.conf" "${ROOT}/scripts"
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
	all)
		run_in_container bash -c '
			set -euo pipefail
			bash scripts/build-alpine-rootfs.sh
			bash scripts/build-kernel.sh
			bash scripts/build-bundle.sh
		'
		;;
	check)
		bash "${ROOT}/scripts/check-no-proprietary.sh" --all
		;;
	shell)
		ensure_image
		docker run --rm -it -v "${ROOT}:/workspace" -w /workspace "${IMAGE}" /bin/bash
		;;
	*)
		echo "Usage: $0 {all|image|rootfs|kernel|bundle|check|shell}" >&2
		exit 1
		;;
esac
