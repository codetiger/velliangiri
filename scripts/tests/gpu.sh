# SPDX-License-Identifier: GPL-2.0-only
# PowerVR SGX543: power domain cycling and core identification

t_gpu() {
	section "PowerVR SGX543 GPU"
	if ! bound "$GPU"; then
		fail "SGX driver not bound"
		return
	fi
	if [ -r /dev/sgx0 ] && [ -n "$(cat /dev/sgx0 2>/dev/null)" ]; then
		pass "/dev/sgx0 readable"
	else
		fail "/dev/sgx0 missing or unreadable"
	fi

	orig=$(rd "$GPU/power_state")
	if wr "$GPU/power_state" standby && [ "$(rd "$GPU/power_state")" = standby ]; then
		pass "power domain gated"
	else
		fail "power domain did not enter standby"
	fi
	info "gates: $(clk_users "${GPU##*/}")"

	if wr "$GPU/power_state" on && [ "$(rd "$GPU/power_state")" = on ]; then
		pass "power domain ungated"
	else
		fail "power domain did not power on"
	fi
	info "gates: $(clk_users "${GPU##*/}")"

	# An unclocked or held-in-reset core returns stale bus data: every
	# register reads the same value and it changes from one power-up to the next
	core=$(rd "$GPU/core_info")
	id=$(field core_id "$core")
	rev=$(field core_rev "$core")
	wr "$GPU/power_state" standby && wr "$GPU/power_state" on
	id2=$(field core_id "$(rd "$GPU/core_info")")
	case "$id" in
	"" | 0x00000000 | 0xffffffff) fail "core ID not readable ($core)" ;;
	"$rev") fail "core ID equals revision ($id): registers not responding" ;;
	*)
		if [ "$id" = "$id2" ]; then
			pass "core ID $id revision $rev"
		else
			fail "core ID changed across power cycles ($id, $id2): registers not responding"
		fi
		;;
	esac

	[ "$orig" = standby ] && wr "$GPU/power_state" standby
}
