# SPDX-License-Identifier: GPL-2.0-only
# PMGR clock gates: every gate a driver holds must report that it is running

t_clocks() {
	section "PMGR clock gates"
	if ! clk_ready; then
		skip "clk_summary not available (debugfs)"
		return
	fi

	stuck=$(dmesg 2>/dev/null | sed -n 's/.*\(pmgr-gate[0-9]*\): stuck at \(0x[0-9a-f]*\).*/\1=\2/p' | sort -u)
	for g in $stuck; do
		fail "${g%%=*} did not reach the requested state (reg ${g#*=}); its driver failed to probe"
	done

	held=0
	boot_on=""
	while read -r g en hw users; do
		[ -n "$g" ] || continue
		if [ "$en" -gt 0 ]; then
			held=$((held + 1))
			if [ "$hw" = Y ]; then
				pass "$g running for $users"
			else
				fail "$g enabled for $users but reports off"
			fi
		elif [ "$hw" = Y ]; then
			boot_on="$boot_on ${g#pmgr-gate}"
		fi
	done <<EOF_GATES
$(clk_gates)
EOF_GATES

	[ "$held" -gt 0 ] || fail "no PMGR gate is held by any driver"
	info "running without a driver (left on by the boot loader):$boot_on"
}
