# SPDX-License-Identifier: GPL-2.0-only
# Audio engine: ALSA card, I2S -> HDMI and S/PDIF -> optical, WAV playback
#
# Set WAV=<file> to play a file through wavplay; without it the playback check
# is skipped.

t_audio() {
	section "Audio (I2S -> HDMI, S/PDIF -> optical)"
	if ! bound "$AUD"; then
		fail "audio driver not bound"
		return
	fi
	if grep -q S5L8947X /proc/asound/cards 2>/dev/null; then
		pass "ALSA card registered"
	else
		fail "no S5L8947X ALSA card in /proc/asound/cards"
		return
	fi
	for dev in 0:HDMI 1:optical; do
		node=/dev/snd/pcmC0D${dev%%:*}p
		if [ -c "$node" ]; then
			pass "${dev#*:} PCM node $node"
		else
			fail "${dev#*:} PCM node $node missing"
		fi
	done

	info "clock gates: $(clk_users "${AUD##*/}")"

	if [ "$(rd "$DISP/hpd_connected")" = 1 ]; then
		pass "HDMI sink connected"
	else
		fail "HDMI hot-plug not detected; audio needs a connected TV"
	fi

	if [ -z "$WAV" ]; then
		skip "WAV playback (set WAV=<file>)"
	else
		if run wavplay "$WAV"; then
			pass "WAV playback completed without error"
		else
			fail "WAV playback"
		fi
		confirm "Did the TV play $WAV?" "HDMI audio heard"
	fi

	hdmi=$(rd "$DISP/hdmi_audio")
	if [ "$(field enabled "$hdmi")" = 0 ]; then
		pass "HDMI audio packets disabled after PCM close"
	else
		fail "HDMI audio left enabled after PCM close ($hdmi)"
	fi
}
