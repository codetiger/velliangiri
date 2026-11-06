#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Phase 0 on-device LLM benchmark (runs ON the S5L8947X, busybox sh).
#
#   GIRI=/mnt/data/linux/giri sh bench.sh [model.gguf ...]
#
# Defaults to every *.gguf in $GIRI/models. Reports prompt-processing (pp)
# and token-generation (tg) tokens/s on one thread at the performance
# governor, plus peak RSS of a short completion run.
set -eu

GIRI="${GIRI:-/mnt/data/linux/giri}"
BIN="${GIRI}/bin"
PP="${PP:-32,128,256}"
TG="${TG:-32}"
REPS="${REPS:-2}"
GOV=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

[ "$#" -gt 0 ] || set -- "${GIRI}"/models/*.gguf

old_gov="$(cat "${GOV}")"
restore() { echo "${old_gov}" > "${GOV}"; }
trap restore EXIT INT TERM
echo performance > "${GOV}"

echo "== $(uname -r), $(nproc) CPU, $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq) kHz"
free -m | sed -n 1,2p

for m in "$@"; do
	echo "== $(basename "${m}") ($(du -m "${m}" | cut -f1) MB)"
	# warm the page cache so pp/tg are not NAND/decrypt bound
	cat "${m}" > /dev/null
	"${BIN}/llama-bench" -m "${m}" -t 1 -p "${PP}" -n "${TG}" -r "${REPS}" -o md 2>/dev/null
	/usr/bin/time -v "${BIN}/llama-completion" -m "${m}" -t 1 -c 1024 -n 16 \
		-p "Summarise: The weather today is sunny." -no-cnv --no-warmup \
		>/dev/null 2>"${GIRI}/rss.log" || true
	grep -E "Maximum resident|Elapsed" "${GIRI}/rss.log" || echo "(no /usr/bin/time -v)"
done
