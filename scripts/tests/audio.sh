# SPDX-License-Identifier: GPL-2.0-only
# Audio engine: ALSA card, S/PDIF -> HDMI packet layer, frame clock, PIO and
# ALSA tones, FIFO health

t_audio() {
	section "Audio (S/PDIF -> HDMI / I2S0)"
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

	info "engine: $(rd "$AUD/fifo_stats")"
	info "clock gates: $(clk_users "${AUD##*/}")"
	info "PCM rates: $(cat /proc/asound/card0/pcm0p/sub0/hw_params 2>/dev/null | sed -n 's/^rate: //p')"

	if [ "$(rd "$DISP/hpd_connected")" = 1 ]; then
		pass "HDMI sink connected"
	else
		fail "HDMI hot-plug not detected; audio needs a connected TV"
	fi

	# The HDMI packet layer is independent of the PCM engine: this drives the
	# S/PDIF transmitter and programs ACR/AUI directly. 'audiotest hdmi -l'
	# lists the matrix; the first run is the 4-words-per-frame layout.
	if run audiotest hdmi -i 1 -t 3; then
		pass "S/PDIF -> HDMI packet layer (4 words/frame, AUI 0x04)"
	else
		fail "S/PDIF -> HDMI packet layer"
	fi
	confirm "Did the TV play a 3 s tone (S/PDIF -> HDMI, 4 words/frame, AUI 0x04)?" \
		"HDMI audio heard via the S/PDIF path"

	if run audiotest rate; then
		pass "I2S0 frame clock within 1% of 46.875 kHz"
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

	stats_before=$(rd "$AUD/fifo_stats")
	if run audiotest alsa -t 4 -f 660; then
		pass "ALSA playback paced by the audio engine frame clock"
	else
		fail "ALSA playback"
	fi
	confirm "Did the TV play a 4 s 660 Hz tone (ALSA PCM)?" \
		"HDMI audio heard via ALSA path"

	stats=$(rd "$AUD/fifo_stats")
	info "fifo after playback: $stats"
	# PIO into a 16-entry FIFO (64 us of margin) cannot be gap free; DMA is
	# the fix. Assert no XRUN and a small number of gaps for this 4 s run.
	g0=$(field gaps "$stats_before")
	g1=$(field gaps "$stats")
	gaps=$(( ${g1:-0} - ${g0:-0} ))
	if [ "${g1:-1}" -eq 0 ] 2>/dev/null || [ "$(field xruns "$stats")" != 0 ]; then
		fail "ALSA playback xrun or missing stats ($stats)"
	elif [ "$gaps" -le 8 ]; then
		pass "ALSA playback: $gaps FIFO gaps, no xruns in 4 s (PIO limit)"
	else
		fail "ALSA playback dropped $gaps FIFO samples in 4 s ($stats)"
	fi

	hdmi=$(rd "$DISP/hdmi_audio")
	if [ "$(field enabled "$hdmi")" = 0 ]; then
		pass "HDMI audio packets disabled after PCM close"
	else
		fail "HDMI audio left enabled after PCM close ($hdmi)"
	fi
}
