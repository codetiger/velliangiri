# SPDX-License-Identifier: GPL-2.0-only
# Shared helpers for the S5L8947X (j33iap) on-demand hardware tests

PLAT=/sys/bus/platform/devices
UART1=$PLAT/32600000.serial
UART2=$PLAT/32700000.serial
DISP=$PLAT/3a200000.display-controller
AUD=$PLAT/34190000.audio-controller
WLPWR=$PLAT/wlan-power

# serdev clients are named after their controller; find them by driver
serdev() { ls -d /sys/bus/serial/drivers/"$1"/serial* 2>/dev/null | head -n 1; }
MCU=$(serdev s5l8947x-mcu)
BT=$(serdev hci_uart_bcm)

CLK_SUMMARY=/sys/kernel/debug/clk/clk_summary

PASS=0
FAIL=0
SKIP=0

pass() { PASS=$((PASS + 1)); echo "  PASS  $*"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $*"; }
skip() { SKIP=$((SKIP + 1)); echo "  SKIP  $*"; }
info() { echo "        $*"; }
section() { echo; echo "== $* =="; }

# Read a sysfs attribute; empty if missing
rd() { cat "$1" 2>/dev/null; }

# Write a sysfs attribute; non-zero if the driver rejected it
wr() { echo "$2" 2>/dev/null >"$1"; }

# Driver bound to a device
bound() { [ -n "$1" ] && [ -e "$1/driver" ]; }

# Value of key from a "key=value key=value" line
field() { echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p"; }

# Interrupt count of the /proc/interrupts line named $1
irqs() { awk -v n="$1" '$NF == n { print $2 }' /proc/interrupts; }

# Make clk_summary readable; non-zero if debugfs is not available
clk_ready() {
	[ -r "$CLK_SUMMARY" ] ||
		mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
	[ -r "$CLK_SUMMARY" ]
}

# One line per PMGR gate: "<name> <enable_count> <running Y/N> <consumers|->"
clk_gates() {
	awk '
	function out() { if (g != "") print g, en, hw, (u == "" ? "-" : u) }
	NF > 2 {
		out(); g = ""
		if ($1 ~ /^pmgr-gate/) { g = $1; en = $2; hw = $9; u = "" }
		# The first consumer shares the clock line; later ones follow
		if (g != "" && NF >= 11 && $10 != "deviceless") u = $10
		next
	}
	NF == 2 && g != "" && $1 != "deviceless" { u = u (u == "" ? "" : ",") $1 }
	END { out() }' "$CLK_SUMMARY"
}

# Gates held by device $1, e.g. "pmgr-gate37=Y pmgr-gate38=N". Clocks taken
# without a device name are listed under the DT node name instead.
clk_users() {
	clk_ready || return
	clk_node=$(readlink -f "$PLAT/$1/of_node" 2>/dev/null)
	clk_gates | awk -v d="$1" -v n="${clk_node##*/}" '
	{ c = split($4, u, ","); for (i = 1; i <= c; i++) if (u[i] == d || u[i] == n) printf "%s=%s ", $1, $3 }
	END { print "" }'
}

# Framing errors counted by the serial core for Samsung UART line $1
uart_fe() {
	sed -n "s/^$1:.* fe:\([0-9]*\).*/\1/p" /proc/tty/driver/s3c2410_serial 2>/dev/null |
		grep . || echo 0
}

# Run a command with its output indented; returns the command's status
run() {
	run_out=$("$@" 2>&1)
	run_rc=$?
	[ -n "$run_out" ] && echo "$run_out" | sed 's/^/        /'
	return $run_rc
}

# Ask the operator; 0 yes, 1 no, 2 not interactive
ask() {
	[ "$INTERACTIVE" = 1 ] || return 2
	printf '        %s [y/n] ' "$1"
	read -r ask_ans </dev/tty || return 2
	case "$ask_ans" in
	y* | Y*) return 0 ;;
	esac
	return 1
}

# Record an operator observation as PASS/FAIL (or a hint when not interactive)
confirm() {
	ask "$1"
	case $? in
	0) pass "$2" ;;
	1) fail "$2" ;;
	*) info "check by eye/ear: $1 (run with -i to record it)" ;;
	esac
}
