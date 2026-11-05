#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Cross-compile Doom (doomgeneric + S5L8947X native backend) into build/doom/doom
#
#   bash tools/doom/build.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DG_DIR="${ROOT}/build/doom-src/doomgeneric/doomgeneric"
OUT_DIR="${ROOT}/build/doom"
OBJ_DIR="${OUT_DIR}/obj"

if [ ! -d "${DG_DIR}" ]; then
	echo "error: ${DG_DIR} not found; run ./tools/doom/fetch.sh first" >&2
	exit 1
fi

CC_BIN=""
for cand in \
	"${CROSS_CC:-}" \
	"/opt/homebrew/bin/armv7-linux-musleabihf-gcc" \
	"armv7-linux-musleabihf-gcc" \
	"arm-linux-musleabihf-gcc" \
	"arm-linux-gnueabihf-gcc"; do
	if [ -n "${cand}" ] && command -v "${cand}" >/dev/null 2>&1; then
		CC_BIN="${cand}"
		break
	fi
done

if [ -z "${CC_BIN}" ]; then
	echo "error: no ARMv7 Linux cross-compiler found" >&2
	exit 1
fi

mkdir -p "${OUT_DIR}" "${OBJ_DIR}"

echo "==> applying native high-resolution 3D renderer patch (960x600 / 1066x600)"
python3 -S "${ROOT}/tools/doom/patch-hires.py" "${DG_DIR}"

CFLAGS=(
	-O3
	-mcpu=cortex-a9
	-mfpu=neon
	-std=gnu99
	-Wall
	-Wno-unused-parameter
	-Wno-unused-result
	-Wno-format-truncation
	-Wno-char-subscripts
	-DNORMALUNIX
	-DLINUX
	-D_DEFAULT_SOURCE
	-DCMAP256
	-DDOOMGENERIC_RESX=320
	-DDOOMGENERIC_RESY=200
	-DFEATURE_SOUND
	-I"${ROOT}/tools/doom"
	-I"${DG_DIR}"
)

SRCS=(
	dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop
	d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info
	i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer memio
	m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc
	m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map
	p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight p_spec
	p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane
	r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound
	tables v_video wi_stuff w_checksum w_file w_main w_wad z_zone
	w_file_stdc i_input i_video doomgeneric
)

OBJS=()
echo "==> compiling doomgeneric sources with ${CC_BIN}"
for s in "${SRCS[@]}"; do
	obj="${OBJ_DIR}/${s}.o"
	if [ ! -f "${obj}" ] || [ "${DG_DIR}/${s}.c" -nt "${obj}" ] || [ "${DG_DIR}/r_defs.h" -nt "${obj}" ]; then
		"${CC_BIN}" "${CFLAGS[@]}" -c "${DG_DIR}/${s}.c" -o "${obj}"
	fi
	OBJS+=("${obj}")
done

echo "==> compiling S5L8947X platform backend (doom_s5l8947x.c)"
"${CC_BIN}" "${CFLAGS[@]}" -Wextra -Werror -c "${ROOT}/tools/doom/doom_s5l8947x.c" -o "${OBJ_DIR}/doom_s5l8947x.o"
OBJS+=("${OBJ_DIR}/doom_s5l8947x.o")

echo "==> linking ${OUT_DIR}/doom (static ARMv7 binary)"
"${CC_BIN}" "${CFLAGS[@]}" -static "${OBJS[@]}" -o "${OUT_DIR}/doom" -lm -lpthread
"${CC_BIN%-gcc}-strip" "${OUT_DIR}/doom" 2>/dev/null || true

ls -lh "${OUT_DIR}/doom"
