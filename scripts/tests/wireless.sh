# SPDX-License-Identifier: GPL-2.0-only
# BCM4334 combo: Wi-Fi REG_ON power switch, Bluetooth HCI over UART2

t_wireless() {
	section "BCM4334 Wi-Fi power / Bluetooth (UART2)"
	if bound "$WLPWR"; then
		orig=$(rd "$WLPWR/state")
		wr "$WLPWR/state" enabled
		if [ "$(rd "$WLPWR/state")" = enabled ]; then
			pass "Wi-Fi REG_ON switched on"
		else
			fail "Wi-Fi REG_ON did not switch on"
		fi
		[ "$orig" = disabled ] && wr "$WLPWR/state" disabled
	else
		fail "wlan-power regulator output not bound"
	fi

	if ! bound "$UART2"; then
		fail "UART2 not bound (clock gate or serial driver)"
		return
	fi
	if ! bound "$BT"; then
		fail "Bluetooth driver (hci_uart_bcm) not bound"
		return
	fi
	hci=$(ls /sys/class/bluetooth 2>/dev/null | head -n 1)
	if [ -n "$hci" ]; then
		pass "$hci registered"
	else
		fail "no HCI device registered"
	fi

	# The Bluetooth core powers the controller up once at registration
	id=$(dmesg 2>/dev/null | sed -n 's/.*BCM: chip id \([0-9]*\).*/\1/p' | tail -n 1)
	if [ -n "$id" ]; then
		pass "controller answered over UART2 (chip id $id)"
		info "$(dmesg | grep -E "hci0: BCM[0-9]|BCM: features" | sed 's/^\[[^]]*\] //' | tr '\n' ' ')"
	else
		fail "controller did not answer on UART2 (no 'BCM: chip id' in the kernel log)"
		info "$(dmesg | grep -E "hci0:|Bluetooth: hci" | tail -n 3 | tr '\n' ' ')"
	fi

	skip "Wi-Fi data path (EHCI HSIC + brcmfmac not enabled yet)"
}
