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
		pass "framebuffer /dev/fb0 ($(rd /sys/class/graphics/fb0/name))"
	else
		fail "framebuffer /dev/fb0 missing"
	fi

	# DRM connector: EDID modes, and the active mode is the best one listed
	conn=$(ls -d /sys/class/drm/card*-HDMI-A-* 2>/dev/null | head -n 1)
	if [ -n "$conn" ] && [ "$(rd "$conn/status")" = connected ]; then
		modes=$(rd "$conn/modes")
		pass "HDMI connector connected, $(echo "$modes" | wc -l) modes from EDID"
		best=$(echo "$modes" | head -n 1)
		size=$(rd /sys/class/graphics/fb0/virtual_size | tr , x)
		if [ -n "$best" ] && [ "$size" = "$best" ]; then
			pass "running the best supported mode ($best)"
		else
			fail "running $size, best supported mode is '$best'"
		fi
	else
		fail "HDMI connector not connected"
	fi

	fb=$(rd "$DISP/plane_phys")
	if [ -n "$fb" ] && [ $((fb)) -ne 0 ] && [ $((fb & 0x3f)) -eq 0 ]; then
		pass "Plane 0 scans out $fb"
	else
		fail "Plane 0 address '$fb'"
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
