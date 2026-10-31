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

	if [ "$(rd "$DISP/hpd_connected")" = 1 ]; then
		t_audio_rates
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

# HDMI measured CTS (hardware-owned, three byte-wide registers)
hdmi_mcts() {
	b0=$(peek 0x39710410 1 | awk '{ print $2 }')
	b1=$(peek 0x39710414 1 | awk '{ print $2 }')
	b2=$(peek 0x39710418 1 | awk '{ print $2 }')
	echo $(((0x$b2 & 255) << 16 | (0x$b1 & 255) << 8 | (0x$b0 & 255)))
}

# Each HDMI rate must come out exactly: play 3 s of silence at the rate and
# compare the CTS the HDMI block measures with the one the driver announces
# (fTMDS * N / 128 fs). They differ by the TMDS clock error, well under 0.1 %.
t_audio_rates() {
	wav=/tmp/hwtest-silence.wav
	{
		printf '\122\111\106\106\244\273\000\000\127\101\126\105\146\155\164\040'
		printf '\020\000\000\000\001\000\001\000\100\037\000\000\200\076\000\000'
		printf '\002\000\020\000\144\141\164\141\200\273\000\000'
		head -c 48000 /dev/zero
	} >"$wav"
	for r in 32000 44100 48000; do
		wavplay -r "$r" "$wav" >/dev/null 2>&1 &
		sleep 2
		hdmi=$(rd "$DISP/hdmi_audio")
		cts=$(field cts "$hdmi")
		mcts=$(hdmi_mcts)
		wait
		if [ "$(field rate "$hdmi")" = "$r" ] && [ -n "$cts" ] &&
		   [ $((mcts > cts ? mcts - cts : cts - mcts)) -le $((cts / 1000)) ]; then
			pass "HDMI $r Hz exact (N $(field n "$hdmi"), CTS $cts, measured $mcts)"
		else
			fail "HDMI $r Hz: announced CTS $cts, measured $mcts ($hdmi)"
		fi
	done
	rm -f "$wav"
}
