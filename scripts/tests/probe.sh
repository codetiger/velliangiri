# SPDX-License-Identifier: GPL-2.0-only
# Driver probe status for the S5L8947X devices

t_probe() {
	section "Driver probe"
	for dev in "$UART1" "$UART2" "$DISP" "$AUD" "$GPU" "$WLPWR"; do
		name=${dev##*/}
		if bound "$dev"; then
			drv=$(readlink "$dev/driver")
			pass "$name bound to ${drv##*/}"
		elif [ -e "$dev" ]; then
			fail "$name present but no driver bound"
		else
			fail "$name missing (device tree node disabled?)"
		fi
	done
	if bound "$MCU"; then
		pass "${MCU##*/} (UART1) bound to s5l8947x-mcu"
	else
		fail "no serdev device bound to s5l8947x-mcu"
	fi
	if bound "$BT"; then
		pass "${BT##*/} (UART2) bound to hci_uart_bcm"
	else
		fail "no serdev device bound to hci_uart_bcm"
	fi

	info "kernel log:"
	dmesg 2>/dev/null |
		grep -E "AIC-TIMER|apple-wdt|s5l8947x-cpufreq|s5l-fe|dwc2|pmgr-gate|3[26][0-9a-f]{6}\.serial|s5l8947x-(mcu|display|audio|sgx)|Bluetooth: hci|hci0" |
		sed 's/^/          /'
}
