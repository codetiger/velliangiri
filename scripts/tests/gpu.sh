# SPDX-License-Identifier: GPL-2.0-only
# PowerVR SGX543: power domain cycling, core identification, robustness
#
# Environment:
#   GPU_CFG0=<hex>       expected master cfg0 (default 0x01194101)
#   GPU_STRESS=<n>       power cycles in the concurrency test (default 200)

GPU_DRV=/sys/bus/platform/drivers/s5l8947x-sgx

# Kernel log lines that indicate a driver or core fault
gpu_splats() { dmesg 2>/dev/null | grep -c -E 'WARNING:|BUG:|Oops|Unable to handle|s5l8947x-sgx.*(fail|error)'; }

# Count of this device's gates reporting $1 (Y running, N gated)
gpu_gates() { clk_users "${GPU##*/}" | tr ' ' '\n' | grep -c "=$1\$"; }

t_gpu() {
	section "PowerVR SGX543 GPU"
	if [ ! -e "$GPU" ]; then
		skip "SGX node disabled in the device tree"
		return
	fi
	if ! bound "$GPU"; then
		fail "SGX driver not bound"
		return
	fi
	if [ -r /dev/sgx0 ] && [ -n "$(cat /dev/sgx0 2>/dev/null)" ]; then
		pass "/dev/sgx0 readable"
	else
		fail "/dev/sgx0 missing or unreadable"
	fi

	splats0=$(gpu_splats)
	orig=$(rd "$GPU/power_state")

	# --- power domain ---
	if wr "$GPU/power_state" standby && [ "$(rd "$GPU/power_state")" = standby ]; then
		pass "power domain gated"
	else
		fail "power domain did not enter standby"
	fi
	info "gates: $(clk_users "${GPU##*/}")"
	if clk_ready; then
		[ "$(gpu_gates Y)" -eq 0 ] ||
			info "gates still running in standby (shared or left on by the boot loader)"
	fi

	if wr "$GPU/power_state" on && [ "$(rd "$GPU/power_state")" = on ]; then
		pass "power domain ungated"
	else
		fail "power domain did not power on"
	fi
	info "gates: $(clk_users "${GPU##*/}")"
	if clk_ready; then
		n=$(gpu_gates Y)
		if [ "$n" -eq 3 ]; then
			pass "all 3 clock gates running while on"
		else
			fail "$n of 3 clock gates running while on"
		fi
	else
		skip "clock gate state (debugfs not available)"
	fi

	# --- master block identification ---
	# An unclocked core returns stale bus data: every register reads the same
	# value and it changes from one power-up to the next
	core=$(rd "$GPU/core_info")
	id=$(field cfg0 "$core")
	rev=$(field cfg1 "$core")
	wr "$GPU/power_state" standby && wr "$GPU/power_state" on
	id2=$(field cfg0 "$(rd "$GPU/core_info")")
	case "$id" in
	"" | 0x00000000 | 0xffffffff) fail "cfg0 not readable ($core)" ;;
	"$rev") fail "cfg0 equals cfg1 ($id): registers not responding" ;;
	*)
		if [ "$id" = "$id2" ]; then
			pass "cfg0 $id cfg1 $rev"
		else
			fail "cfg0 changed across power cycles ($id, $id2): registers not responding"
		fi
		;;
	esac
	want=${GPU_CFG0:-0x01194101}
	if [ "$id" = "$want" ]; then
		pass "cfg0 matches expected $want"
	else
		fail "cfg0 $id, expected $want"
	fi
	cg=$(field clkgate "$(rd "$GPU/core_info")")
	case "$cg" in
	"" | 0x00000000 | 0xffffffff) fail "clock gate register not live ($cg)" ;;
	*) pass "clock gate register live ($cg)" ;;
	esac
	case "$(rd /dev/sgx0)" in
	*"cfg0=$id"*) pass "/dev/sgx0 agrees with sysfs" ;;
	*) fail "/dev/sgx0 cfg0 differs from sysfs ($id)" ;;
	esac

	# --- input handling ---
	wr "$GPU/power_state" on
	if wr "$GPU/power_state" bogus; then
		fail "invalid power_state value accepted"
	elif [ "$(rd "$GPU/power_state")" = on ]; then
		pass "invalid power_state rejected, state unchanged"
	else
		fail "invalid power_state write changed the state"
	fi
	ok=1
	for v in "on on" "on 1" "standby standby" "standby off" "on 1" "standby 0"; do
		set -- $v
		wr "$GPU/power_state" "$2" && [ "$(rd "$GPU/power_state")" = "$1" ] || {
			ok=0
			info "write '$2' did not give '$1'"
		}
	done
	if [ "$ok" = 1 ]; then
		pass "power_state aliases and repeated writes are idempotent"
	else
		fail "power_state alias/idempotency check"
	fi
	wr "$GPU/power_state" on

	# --- concurrency: power cycling while readers hammer the interfaces ---
	n=${GPU_STRESS:-200}
	(
		while :; do
			cat "$GPU/core_info" /dev/sgx0 "$GPU/power_state" >/dev/null 2>&1
		done
	) &
	reader=$!
	i=0
	bad=0
	while [ "$i" -lt "$n" ]; do
		wr "$GPU/power_state" standby || bad=$((bad + 1))
		wr "$GPU/power_state" on || bad=$((bad + 1))
		i=$((i + 1))
	done
	kill "$reader" 2>/dev/null
	wait "$reader" 2>/dev/null
	id3=$(field cfg0 "$(rd "$GPU/core_info")")
	if [ "$bad" -eq 0 ] && [ "$id3" = "$id" ] && [ "$(rd "$GPU/power_state")" = on ]; then
		pass "$n power cycles with concurrent readers"
	else
		fail "$n power cycles: $bad write errors, cfg0 now $id3"
	fi

	# --- unbind / rebind ---
	if [ -w "$GPU_DRV/unbind" ]; then
		dev=${GPU##*/}
		if echo "$dev" >"$GPU_DRV/unbind" 2>/dev/null; then
			sleep 1
			[ ! -e /dev/sgx0 ] && pass "unbind removed /dev/sgx0" ||
				fail "/dev/sgx0 still present after unbind"
			if clk_ready; then
				left=$(clk_users "$dev" | tr -d ' ')
				[ -z "$left" ] && pass "unbind released all clocks" ||
					fail "clocks still held after unbind: $left"
			fi
			if echo "$dev" >"$GPU_DRV/bind" 2>/dev/null; then
				sleep 1
				if [ "$(field cfg0 "$(rd "$GPU/core_info")")" = "$id" ] && [ -e /dev/sgx0 ]; then
					pass "rebind restored the device with the same cfg0"
				else
					fail "device not restored after rebind"
				fi
			else
				fail "rebind failed"
			fi
		else
			fail "unbind failed"
		fi
	else
		skip "unbind/rebind (driver sysfs not writable)"
	fi

	# --- kernel log ---
	splats1=$(gpu_splats)
	if [ "$splats1" -gt "$splats0" ]; then
		fail "kernel log gained $((splats1 - splats0)) warning/error line(s) during the test"
		dmesg | grep -E 'WARNING:|BUG:|Oops|Unable to handle|s5l8947x-sgx' | tail -n 5 | sed 's/^/        /'
	else
		pass "no kernel warnings during the GPU tests"
	fi

	[ "$orig" = standby ] && wr "$GPU/power_state" standby
}
