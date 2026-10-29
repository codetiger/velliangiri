# SPDX-License-Identifier: GPL-2.0-only
# Audio engine: ALSA card, S/PDIF -> HDMI packet layer, WAV playback
#
# Set WAV=<file> to play a file through wavplay; without it the playback check
# is skipped.

t_audio() {
	section "Audio (S/PDIF -> HDMI / optical)"
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
	if [ -c /dev/snd/pcmC0D0p ]; then
		pass "PCM node /dev/snd/pcmC0D0p"
	else
		fail "PCM node /dev/snd/pcmC0D0p missing"
	fi

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
