#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Choose the LLM backend tvshell uses for Ask Giri. Edits the LLM_* keys of
# the persistent /mnt/data/linux/etc/shell.conf on the device (other keys such
# as THEME are kept) and restarts tvshell.
#
#   ./tools/tvshell/llm-config.sh [root@HOST] show
#   ./tools/tvshell/llm-config.sh [root@HOST] remote http://192.168.68.101:8080 [MODEL] [API_KEY]
#   ./tools/tvshell/llm-config.sh [root@HOST] device [/mnt/data/linux/giri/models/FILE.gguf]
#   ./tools/tvshell/llm-config.sh [root@HOST] set LLM_TIMEOUT_S=120 LLM_JSON_SCHEMA=0
#
# A KEY= with an empty value removes that key. --no-restart skips the restart.
#
# remote: any OpenAI-compatible server (POST /v1/chat/completions), e.g.
# llama.cpp llama-server (tools/giri/serve-host.sh), Ollama
# (OLLAMA_HOST=0.0.0.0 ollama serve, port 11434) or LM Studio (port 1234).
# Use plain http:// on the LAN; https is not supported.
set -euo pipefail

DEST="root@192.168.68.112"
CONF="/mnt/data/linux/etc/shell.conf"
RESTART=1
ARGS=()

for arg in "$@"; do
	case "$arg" in
	--no-restart) RESTART=0 ;;
	-h|--help) sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*@*) [ "${#ARGS[@]}" -eq 0 ] && DEST="$arg" || ARGS+=("$arg") ;;
	*) ARGS+=("$arg") ;;
	esac
done
[ "${#ARGS[@]}" -gt 0 ] || { echo "usage: $0 [root@HOST] show|remote|device|set ..." >&2; exit 1; }

SSH=(ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
     -o ConnectTimeout=8 -o LogLevel=ERROR "$DEST")

# Apply KEY=VALUE lines read from stdin to shell.conf on the device.
apply_pairs() {
	"${SSH[@]}" "set -e; f=$CONF; mkdir -p \${f%/*}; touch \$f
		while IFS= read -r line; do
			k=\${line%%=*}
			grep -v \"^\$k=\" \$f > \$f.tmp || true
			[ -n \"\${line#*=}\" ] && printf '%s\n' \"\$line\" >> \$f.tmp
			mv \$f.tmp \$f
		done
		sync"
}

show() {
	"${SSH[@]}" "f=$CONF
		echo \"== \$f\"; grep '^LLM_' \$f 2>/dev/null | sed 's/^\(LLM_API_KEY=\).*/\1(set)/' || echo '(no LLM_ keys: on-device default)'
		mode=\$(sed -n 's/^LLM_MODE=//p' \$f 2>/dev/null)
		url=\$(sed -n 's/^LLM_URL=//p' \$f 2>/dev/null)
		if [ \"\$mode\" = remote ] && [ -n \"\$url\" ]; then
			url=\${url%/}; case \$url in */v1) ;; *) url=\$url/v1 ;; esac
			echo \"== GET \$url/models from the device\"
			wget -T 5 -qO- \"\$url/models\" 2>&1 | head -c 400; echo
		fi"
}

restart() {
	[ "$RESTART" = 1 ] || return 0
	echo "==> restarting /bin/tvshell on $DEST"
	# The video playback loop does not honour SIGTERM, so escalate; the
	# boot script's loop respawns /bin/tvshell, otherwise start it here.
	"${SSH[@]}" "old=\$(pidof tvshell); killall tvshell 2>/dev/null || true; sleep 1.5
		for p in \$old; do [ -d /proc/\$p ] && kill -9 \$p; done; sleep 1.5
		pidof tvshell >/dev/null 2>&1 || nohup /bin/tvshell 2>/dev/kmsg >/dev/null </dev/null &
		sleep 1; echo \"tvshell pid: \$(pidof tvshell)\""
}

cmd="${ARGS[0]}"
case "$cmd" in
show)
	show
	;;
remote)
	url="${ARGS[1]:-}"
	case "$url" in
	http://*) ;;
	https://*) echo "error: use http:// (https is not supported on the device)" >&2; exit 1 ;;
	*) echo "error: remote needs a URL, e.g. http://192.168.68.101:8080" >&2; exit 1 ;;
	esac
	{
		echo "LLM_MODE=remote"
		echo "LLM_URL=$url"
		[ "${#ARGS[@]}" -gt 2 ] && echo "LLM_MODEL=${ARGS[2]}"
		[ "${#ARGS[@]}" -gt 3 ] && echo "LLM_API_KEY=${ARGS[3]}"
		true
	} | apply_pairs
	show
	restart
	;;
device)
	{
		echo "LLM_MODE=device"
		[ "${#ARGS[@]}" -gt 1 ] && echo "LLM_DEVICE_MODEL=${ARGS[1]}"
		true
	} | apply_pairs
	show
	restart
	;;
set)
	[ "${#ARGS[@]}" -gt 1 ] || { echo "error: set needs KEY=VALUE pairs" >&2; exit 1; }
	for kv in "${ARGS[@]:1}"; do
		case "$kv" in
		LLM_*=*) echo "$kv" ;;
		*) echo "error: '$kv' is not an LLM_*=value pair" >&2; exit 1 ;;
		esac
	done | apply_pairs
	show
	restart
	;;
*)
	echo "error: unknown command '$cmd' (show|remote|device|set)" >&2
	exit 1
	;;
esac
