#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# busybox ntpd event hook: copy the network time to the PMU RTC once it is
# known, so the clock is right after the next boot even without a network

case "$1" in
	step|stratum)
		if [ -e /dev/rtc0 ] && hwclock -w -u -f /dev/rtc0 2>/dev/null; then
			echo "[velliangiri] ntpd: clock set from network ($1), RTC updated" >/dev/kmsg 2>/dev/null || true
		fi
		;;
esac

exit 0
