# SPDX-License-Identifier: GPL-2.0-only
# Display pipe: HDMI link, Plane 0 scanout, gamma LUT, M2M scaler

t_display() {
	section "Display pipe / HDMI / scaler"
	if ! bound "$DISP"; then
		fail "display driver not bound"
		return
	fi

	if [ "$(rd "$DISP/hpd_connected")" = 1 ]; then
		pass "HDMI hot-plug detected"
	else
		fail "HDMI hot-plug not detected"
	fi
	if [ "$(rd "$DISP/phy_ready")" = 1 ]; then
		pass "HDMI PHY ready"
	else
		fail "HDMI PHY not ready"
	fi
	if [ -c /dev/fb0 ]; then
		pass "framebuffer /dev/fb0"
	else
		fail "framebuffer /dev/fb0 missing"
	fi

	fb=$(rd "$DISP/plane_phys")
	if [ -n "$fb" ] && [ $((fb)) -ge $((0x9ec00000)) ] && [ $((fb)) -lt $((0x9f000000)) ]; then
		pass "Plane 0 scans out reserved framebuffer at $fb"
		if wr "$DISP/plane_phys" "$fb"; then
			pass "Plane 0 address commit accepted"
		else
			fail "Plane 0 address commit rejected"
		fi
	else
		fail "Plane 0 address '$fb' outside reserved framebuffer"
	fi

	orig=$(rd "$DISP/gamma_brightness")
	if wr "$DISP/gamma_brightness" 40 && [ "$(rd "$DISP/gamma_brightness")" = 40 ]; then
		pass "gamma LUT reprogrammed to 40%"
		confirm "Did the screen dim noticeably?" "gamma LUT dims the picture"
		sleep 1
	else
		fail "gamma LUT write"
	fi
	wr "$DISP/gamma_brightness" "${orig:-100}"

	scaler=$(rd "$DISP/scaler_status")
	if [ "$(field enabled "$scaler")" = 1 ]; then
		pass "M2M scaler clock gate active"
	else
		fail "M2M scaler gated ($scaler)"
	fi
	info "hdmi audio: $(rd "$DISP/hdmi_audio")"
}
