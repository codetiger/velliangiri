#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# On-demand hardware tests for S5L8947X (j33iap); installed as /bin/hwtest
#
# Usage: hwtest [-i] [all|probe|clocks|display|audio|mcu|wireless]...
#   -i   interactive: ask the operator to confirm what was seen/heard
#
# Environment:
#   HWTEST_DIR=<dir>     load test files from <dir> (e.g. scp'd from the repo)
#   HWTEST_MCU_PING=0    skip the (read-only) IR MCU firmware version query

ALL="probe clocks display audio mcu wireless"

usage() {
	echo "Usage: hwtest [-i] [all|$(echo "$ALL" | tr ' ' '|')]..." >&2
	exit 2
}

INTERACTIVE=0
while getopts ih opt; do
	case $opt in
	i) INTERACTIVE=1 ;;
	*) usage ;;
	esac
done
shift $((OPTIND - 1))
[ $# -eq 0 ] && set -- all

SELF=$(readlink -f "$0")
TESTDIR=${HWTEST_DIR:-${SELF%/*}}
. "$TESTDIR/lib.sh" || exit 2
for t in $ALL; do
	. "$TESTDIR/$t.sh" || exit 2
done

for arg in "$@"; do
	case " all $ALL " in
	*" $arg "*) ;;
	*) usage ;;
	esac
done

for arg in "$@"; do
	if [ "$arg" = all ]; then
		for t in $ALL; do
			"t_$t"
		done
	else
		"t_$arg"
	fi
done

echo
echo "== Summary: $PASS passed, $FAIL failed, $SKIP skipped =="
[ "$FAIL" -eq 0 ]
