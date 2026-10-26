#!/usr/bin/awk -f
# SPDX-License-Identifier: GPL-2.0-only
# Verify that every CONFIG_FOO=y/m in a fragment survived olddefconfig in .config.

FNR == NR {
	if ($0 ~ /^CONFIG_[A-Za-z0-9_]+=[ym]/) {
		split($0, parts, "=")
		expected[parts[1]] = parts[2]
	}
	next
}

$0 ~ /^CONFIG_[A-Za-z0-9_]+=/ {
	split($0, parts, "=")
	actual[parts[1]] = parts[2]
}

END {
	missing = 0
	for (sym in expected) {
		if (actual[sym] != expected[sym]) {
			printf("ERROR: %s expected %s, got %s\n",
			       sym, expected[sym],
			       (sym in actual) ? actual[sym] : "unset") > "/dev/stderr"
			missing++
		}
	}
	if (missing > 0)
		exit 1
}
