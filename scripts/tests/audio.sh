# SPDX-License-Identifier: GPL-2.0-only
# Audio engine: ALSA card, I2S0 frame clock, PIO tone, ALSA tone over HDMI

t_audio() {
	section "Audio (I2S0 / SPDIF / HDMI)"
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

	params=/sys/module/s5l8947x_audio/parameters
	info "driver i2s_mod=$(rd $params/i2s_mod) i2s_psr=$(rd $params/i2s_psr)"
	info "clock gates: $(clk_users "${AUD##*/}")"
	info "fifo: $(rd "$AUD/fifo_stats")"

	if [ "$(rd "$DISP/hpd_connected")" = 1 ]; then
		pass "HDMI sink connected"
	else
		fail "HDMI hot-plug not detected; audio needs a connected TV"
	fi

	if run audiotest rate; then
		pass "I2S0 frame clock within 1% of 48 kHz"
	else
		fail "I2S0 frame clock (try 'audiotest rate -c <txcon>')"
	fi

	if run audiotest pio -t 3 -f 440; then
		pass "PIO tone streamed at the I2S0 frame clock"
	else
		fail "PIO tone"
	fi
	confirm "Did the TV play a 3 s 440 Hz tone (PIO, bypassing ALSA)?" \
		"HDMI audio heard via PIO path"

	if run audiotest alsa -t 3 -f 660; then
		pass "ALSA playback paced by the I2S0 frame clock"
	else
		fail "ALSA playback"
	fi
	confirm "Did the TV play a 3 s 660 Hz tone (ALSA PCM)?" \
		"HDMI audio heard via ALSA path"

	hdmi=$(rd "$DISP/hdmi_audio")
	if [ "$(field enabled "$hdmi")" = 0 ]; then
		pass "HDMI audio packets disabled after PCM close"
	else
		fail "HDMI audio left enabled after PCM close ($hdmi)"
	fi
}
