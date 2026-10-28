# SPDX-License-Identifier: GPL-2.0-only
# PL310 L2 cache: the controller is enabled and the kernel is using it

t_cache() {
	section "L2 cache (PL310)"
	log=$(dmesg 2>/dev/null | grep -i "L2C-310")
	if [ -z "$log" ]; then
		fail "no L2C-310 line in the kernel log (cache-controller node disabled?)"
		return
	fi
	pass "kernel registered the cache controller"
	info "$(echo "$log" | head -n 1 | sed 's/^\[[^]]*\] *//')"

	ctrl=$(peek r 3e000100 1 | awk '{ print $2 }')
	if [ $((0x${ctrl#0x} & 1)) -eq 1 ]; then
		pass "L2 enabled (CTRL=$ctrl)"
	else
		fail "L2 not enabled (CTRL=$ctrl)"
	fi

	if command -v l2bench >/dev/null; then
		info "latency: $(l2bench -q)"
	fi
}
