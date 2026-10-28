# SPDX-License-Identifier: GPL-2.0-only
# System / IR remote MCU on UART1: input device, interrupt-driven receive,
# firmware query, remote key frames
#
# Nothing here sends the standby state: the MCU puts the whole box into
# standby when it receives it.

t_mcu() {
	section "System / IR remote MCU (UART1)"
	if bound "$UART1"; then
		pass "UART1 bound to the Samsung UART driver"
	else
		fail "UART1 not bound (clock gate or serial driver)"
		return
	fi
	if ! bound "$MCU"; then
		fail "MCU driver not bound"
		return
	fi
	if grep -q "S5L8947X IR Remote" /proc/bus/input/devices 2>/dev/null; then
		ev=$(grep -A5 "S5L8947X IR Remote" /proc/bus/input/devices |
			sed -n 's/.*\(event[0-9]*\).*/\1/p')
		pass "input device registered (/dev/input/${ev:-?})"
	else
		fail "IR input device not registered"
	fi
	info "fw_version=$(rd "$MCU/fw_version") rx_count=$(rd "$MCU/rx_count") crc_errors=$(rd "$MCU/crc_errors")"
	info "last packet: $(rd "$MCU/last_packet")"
	info "UART1 interrupts: $(irqs 32600000.serial)"

	# The version query is read-only; HWTEST_MCU_PING=0 skips it
	if [ "${HWTEST_MCU_PING:-1}" = 1 ]; then
		if wr "$MCU/send_cmd" ping; then
			pass "MCU answered the firmware version query ($(rd "$MCU/fw_version"))"
		else
			fail "MCU did not answer the firmware version query"
		fi
	else
		skip "MCU firmware query (HWTEST_MCU_PING=0)"
	fi

	if [ "$INTERACTIVE" = 1 ]; then
		rx0=$(rd "$MCU/rx_count")
		crc0=$(rd "$MCU/crc_errors")
		irq0=$(irqs 32600000.serial)
		fe0=$(uart_fe 1)
		# evdev rejects hexdump's read sizes, so capture with cat first.
		# struct input_event on 32-bit ARM: sec, usec, code << 16 | type, value
		keys=/tmp/hwtest-mcu-keys
		info "press a few remote buttons in the next 10 seconds ..."
		timeout 10 cat "/dev/input/${ev:-event0}" >"$keys" 2>/dev/null
		presses=$(hexdump -v -e '4/4 "%08x " "\n"' "$keys" |
			awk '$3 ~ /0001$/ && $4 == "00000001"' | wc -l)
		rm -f "$keys"
		fe1=$(uart_fe 1)
		rx1=$(rd "$MCU/rx_count")
		crc1=$(rd "$MCU/crc_errors")
		irq1=$(irqs 32600000.serial)
		if [ "${rx1:-0}" -gt "${rx0:-0}" ]; then
			pass "IR frames received ($((rx1 - rx0)))"
		else
			fail "no IR frames received from the MCU"
		fi
		if [ "${irq1:-0}" -gt "${irq0:-0}" ]; then
			pass "UART1 receive is interrupt driven ($((irq1 - irq0)) IRQs)"
		else
			fail "no UART1 interrupts while frames were expected"
		fi
		if [ "${crc1:-0}" -gt "${crc0:-0}" ]; then
			fail "$((crc1 - crc0)) frames with bad CRC (baud rate or framing)"
		fi
		if [ "${fe1:-0}" -gt "${fe0:-0}" ]; then
			fail "$((fe1 - fe0)) UART1 framing errors (baud rate mismatch)"
		fi
		if [ "$presses" -gt 0 ]; then
			pass "remote key presses reported to input ($presses)"
		else
			fail "no key presses reached /dev/input/${ev:-event0}"
		fi
		info "last packet: $(rd "$MCU/last_packet")"
	else
		skip "IR remote key check (needs -i)"
	fi
}
