# SPDX-License-Identifier: GPL-2.0-only
# PMU real-time clock: registered, set from the system clock, ticking, alarm event

t_rtc() {
	section "RTC (PMU)"
	r=
	for d in /sys/class/rtc/rtc*; do
		[ "$(rd "$d/name" | cut -d' ' -f1)" = apple-pmu-rtc ] && r=$d && break
	done
	if [ -z "$r" ]; then
		fail "no apple-pmu-rtc device"
		return
	fi
	pass "${r##*/} registered"
	dev=/dev/${r##*/}

	# The clock is invalid until it is set after power-up; set it from
	# the system clock only when that looks like a real date.
	if [ -z "$(rd "$r/since_epoch")" ]; then
		if [ "$(date +%Y)" -ge 2025 ]; then
			hwclock -w -u -f "$dev" 2>/dev/null
			info "clock was not set since power-up; set from the system clock"
		else
			skip "clock not set and the system clock is not set either"
			return
		fi
	fi
	a=$(rd "$r/since_epoch")
	now=$(date +%s)
	if [ -n "$a" ] && [ $((a - now)) -le 2 ] && [ $((now - a)) -le 2 ]; then
		pass "time matches the system clock ($(rd "$r/date") $(rd "$r/time") UTC)"
	else
		fail "RTC $a vs system $now"
	fi
	sleep 3
	b=$(rd "$r/since_epoch")
	if [ -n "$b" ] && [ $((b - a)) -ge 2 ] && [ $((b - a)) -le 4 ]; then
		pass "counts seconds (+$((b - a)) in 3 s)"
	else
		fail "count moved by $((b - a)) in 3 s"
	fi

	n0=$(irqs rtc-alarm)
	if [ -z "$n0" ]; then
		fail "no rtc-alarm interrupt"
		return
	fi
	wr "$r/wakealarm" 0
	wr "$r/wakealarm" +2
	sleep 4
	n1=$(irqs rtc-alarm)
	if [ "${n1:-0}" -gt "$n0" ] && [ -z "$(rd "$r/wakealarm")" ]; then
		pass "alarm fired (PMU event 32)"
	else
		fail "alarm did not fire (interrupts $n0 -> $n1, wakealarm '$(rd "$r/wakealarm")')"
	fi
}
