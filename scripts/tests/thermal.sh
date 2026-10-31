# SPDX-License-Identifier: GPL-2.0-only
# PMU temperature sensors (hwmon) and the SoC thermal zone with cpufreq cooling

t_thermal() {
	section "Temperature sensors (PMU ADC)"
	h=
	for d in /sys/class/hwmon/hwmon*; do
		[ "$(rd "$d/name")" = apple_pmu ] && h=$d && break
	done
	if [ -z "$h" ]; then
		fail "no apple_pmu hwmon device"
		return
	fi
	pass "${h##*/} registered"

	n0=$(irqs pmu-adc)
	for i in 1 2 3 4 5; do
		t=$(rd "$h/temp${i}_input")
		l=$(rd "$h/temp${i}_label")
		if [ -n "$t" ] && [ "$t" -ge 10000 ] && [ "$t" -le 90000 ]; then
			pass "temp$i ${l:-?}: $((t / 1000)).$(((t % 1000) / 100)) C"
		else
			fail "temp$i ${l:-?}: '$t' outside 10..90 C"
		fi
	done
	n1=$(irqs pmu-adc)
	if [ -n "$n0" ] && [ "${n1:-0}" -ge $((n0 + 5)) ]; then
		pass "conversions completed by the ADC interrupt (PMU event 13)"
	else
		fail "ADC interrupt count $n0 -> $n1"
	fi

	z=
	for d in /sys/class/thermal/thermal_zone*; do
		[ "$(rd "$d/type")" = soc-thermal ] && z=$d && break
	done
	if [ -z "$z" ]; then
		fail "no soc-thermal zone"
		return
	fi
	zt=$(rd "$z/temp")
	t1=$(rd "$h/temp1_input")
	d=$((zt - t1))
	if [ -n "$zt" ] && [ "${d#-}" -le 2000 ]; then
		pass "soc-thermal follows temp1 ($((zt / 1000)) C, trips $(($(rd "$z/trip_point_0_temp") / 1000)) / $(($(rd "$z/trip_point_1_temp") / 1000)) C)"
	else
		fail "soc-thermal $zt vs temp1 $t1"
	fi
	if [ "$(rd "$z/cdev0/type")" = cpufreq-cpu0 ]; then
		pass "CPU frequency is bound as the cooling device"
	else
		fail "no cpufreq cooling device on soc-thermal"
	fi
}
