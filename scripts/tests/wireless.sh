# SPDX-License-Identifier: GPL-2.0-only
# BCM4334 combo: Wi-Fi over EHCI HSIC (brcmfmac), Bluetooth HCI over UART2

t_wireless() {
	section "BCM4334 Wi-Fi (EHCI HSIC)"
	if ! bound "$EHCI"; then
		fail "EHCI host not bound (ehci-platform)"
	else
		pass "EHCI host bound"
		# The chip enumerates as the Broadcom USB boot loader, then as the
		# firmware once brcmfmac has downloaded it
		dev=$(grep -l '^0a5c$' /sys/bus/usb/devices/*/idVendor 2>/dev/null | head -n 1)
		if [ -n "$dev" ]; then
			dev=${dev%/idVendor}
			pass "Broadcom device on HSIC: 0a5c:$(rd "$dev/idProduct") at ${dev##*/}"
		else
			fail "no Broadcom device enumerated on the HSIC port"
			info "$(dmesg | grep -iE 'ehci|usb [0-9]-' | tail -n 3 | tr '\n' ' ')"
		fi

		if [ ! -f /lib/firmware/brcm/brcmfmac4334.bin ]; then
			skip "brcmfmac4334.bin not provisioned from nand0p1; firmware download not attempted"
		else
			wl=$(ls -d /sys/class/net/*/wireless 2>/dev/null | head -n 1)
			if [ -n "$wl" ]; then
				wl=${wl%/wireless}
				wl=${wl##*/}
				pass "$wl registered"
				info "$(dmesg | grep -E 'brcmfmac: .*(Firmware|version)' | tail -n 1 | sed 's/^\[[^]]*\] //')"
				# A scan issued while the interface is still coming up can spin
				ip link set "$wl" up 2>/dev/null && sleep 2
				n=$(timeout 20 iw dev "$wl" scan 2>/dev/null | grep -c '^BSS')
				if [ "$n" -gt 0 ]; then
					pass "scan found $n networks"
				else
					fail "scan found no networks"
				fi
				if command -v wpa_supplicant >/dev/null && command -v wifi >/dev/null; then
					pass "wpa_supplicant and the wifi helper installed"
				else
					fail "wpa_supplicant or /bin/wifi missing from the rootfs"
				fi
			else
				fail "no wireless interface registered"
				info "$(dmesg | grep brcmf | tail -n 3 | tr '\n' ' ')"
			fi
		fi
	fi

	section "BCM4334 Bluetooth (UART2)"
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

	# Exercise the radio: a passive LE scan should hear nearby advertisers
	if ! command -v btscan >/dev/null; then
		skip "btscan not installed; radio not exercised"
		return
	fi
	out=$(timeout 20 btscan 5 "${hci#hci}" 2>&1)
	n=$(echo "$out" | sed -n 's/^\([0-9]*\) advertisers.*/\1/p')
	if [ -n "$n" ] && [ "$n" -gt 0 ]; then
		pass "LE scan heard $n advertisers"
		info "$(echo "$out" | tail -n 1)"
	else
		fail "LE scan heard no advertisers"
		info "$(echo "$out" | tail -n 2 | tr '\n' ' ')"
	fi
}
