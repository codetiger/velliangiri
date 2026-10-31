# SPDX-License-Identifier: GPL-2.0-only
# Driver probe status for the S5L8947X devices

t_probe() {
	section "Driver probe"
	for dev in "$UART1" "$UART2" "$DISP" "$AUD" "$EHCI" "$AES" "$SHA"; do
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
	# The SHA engine serves sha384/sha512 ahead of the generic code
	for a in sha384 sha512; do
		d=$(awk -v a="$a" '$1 == "name" && $3 == a { n = 1; next }
			n && $1 == "driver" { d = $3 } n && $1 == "priority" { if ($3 > best) { best = $3; top = d }; n = 0 }
			END { print top }' /proc/crypto)
		if [ "$d" = "$a-s5l8947x" ]; then
			pass "$a served by $d"
		else
			fail "$a served by '${d:-nothing}', not $a-s5l8947x"
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
		grep -E "AIC-TIMER|apple-wdt|s5l8947x-cpufreq|s5l-fe|dwc2|pmgr-gate|3[26][0-9a-f]{6}\.serial|s5l8947x-(mcu|display|audio|aes|sha)|Bluetooth: hci|hci0" |
		sed 's/^/          /'
}
