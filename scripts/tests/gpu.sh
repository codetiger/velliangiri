# SPDX-License-Identifier: GPL-2.0-only
# SGX543 GPU: registered, microkernel firmware provisioned from NAND, microkernel
# starts (handshake), survives a stop/start cycle

t_gpu() {
	section "GPU (SGX543)"
	g=
	for d in /sys/bus/platform/devices/*.gpu; do
		[ -r "$d/ukernel" ] && g=$d && break
	done
	if [ -z "$g" ]; then
		fail "no SGX543 device with a ukernel attribute"
		return
	fi
	pass "${g##*/} registered ($(rd "$g/core_info" | sed 's/.*cfg0=/cfg0=/'))"

	fw=/lib/firmware/s5l8947x/sgx543.fw
	if [ ! -s "$fw" ]; then
		skip "no microkernel firmware ($fw; stage2 carves it from nand0p1)"
		return
	fi
	pass "microkernel firmware present ($(wc -c <"$fw") bytes)"

	for n in 1 2; do
		wr "$g/ukernel" stop
		if ! wr "$g/ukernel" start; then
			fail "start $n: $(rd "$g/ukernel")"
			return
		fi
		s=$(rd "$g/ukernel")
		case "$s" in
		running*) pass "start $n: microkernel up (${s#running })" ;;
		*) fail "start $n: $s"; return ;;
		esac
	done
	sleep 2
	case "$(rd "$g/ukernel")" in
	running*" ctl0=0x"*[13579bdf]" "*) pass "still running after 2 s" ;;
	*) fail "after 2 s: $(rd "$g/ukernel")" ;;
	esac
}
