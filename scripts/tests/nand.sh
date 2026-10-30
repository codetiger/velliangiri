# SPDX-License-Identifier: GPL-2.0-only
# NAND (FMI0, managed NAND): read-only MTD, repeatable DMA reads, translation layer, volumes

t_nand() {
	section "NAND (FMI0, read-only MTD)"
	dev=/sys/bus/platform/devices/31200000.nand-controller
	if ! bound "$dev"; then
		modprobe s5l8947x-fmi 2>/dev/null
		bound "$dev" || { fail "FMI0 driver not bound (module s5l8947x-fmi not loaded?)"; return; }
	fi
	pass "driver bound"
	info "$(dmesg 2>/dev/null | grep -m1 'managed NAND' | sed 's/^\[[^]]*\] *//')"

	m=$(grep -l '31200000.nand-controller' /sys/class/mtd/mtd*/name 2>/dev/null | head -n 1)
	if [ -z "$m" ]; then
		fail "no MTD device for FMI0"
		return
	fi
	m=${m%/name}
	n=${m##*/}
	size=$(rd "$m/size")
	ws=$(rd "$m/writesize")
	es=$(rd "$m/erasesize")
	oob=$(rd "$m/oobsize")
	info "$n: size $size, page $ws, block $es, oob $oob, flags $(rd "$m/flags")"
	if [ "$ws" = 16384 ] && [ "$es" = 4194304 ] && [ "$oob" = 64 ] && [ "$size" -gt 0 ]; then
		pass "geometry (16 KiB pages, 4 MiB blocks, 64 B metadata)"
	else
		fail "unexpected geometry"
	fi
	if [ $(($(rd "$m/flags") & 0x400)) -eq 0 ]; then
		pass "read-only (no MTD_WRITEABLE)"
	else
		fail "device is writeable"
	fi

	# Same 4 MiB block twice: DMA reads must be repeatable.
	t0=$(date +%s)
	a=$(dd if=/dev/"$n" bs=4194304 skip=100 count=1 2>/dev/null | md5sum | cut -d' ' -f1)
	t1=$(date +%s)
	b=$(dd if=/dev/"$n" bs=4194304 skip=100 count=1 2>/dev/null | md5sum | cut -d' ' -f1)
	if [ -n "$a" ] && [ "$a" = "$b" ] && [ "$a" != d41d8cd98f00b204e9800998ecf8427e ]; then
		pass "block 100 reads back identically ($a)"
	else
		fail "block 100 differs between reads ($a / $b)"
	fi
	info "4 MiB in $((t1 - t0)) s"

	resets=$(dmesg 2>/dev/null | grep -c 'resetting the package')
	if [ "$resets" -eq 0 ]; then
		pass "no package resets in the kernel log"
	else
		fail "$resets package reset(s) in the kernel log"
	fi

	# Translation layer: logical disk nand0 and its volumes nand0pN
	i=0
	while [ ! -b /dev/nand0 ] && [ $i -lt 30 ]; do
		sleep 1
		i=$((i + 1))
	done
	if [ ! -b /dev/nand0 ]; then
		fail "no /dev/nand0 (translation layer not built or not loaded)"
		return
	fi
	pass "logical disk nand0 ($(($(rd /sys/block/nand0/size) / 2048)) MiB)"
	info "$(dmesg 2>/dev/null | grep -m1 'translation map' | sed 's/^\[[^]]*\] *//')"
	for v in /sys/block/nand0p*; do
		[ -e "$v" ] && info "${v##*/}: $(($(rd "$v/size") / 2048)) MiB"
	done
	if [ ! -b /dev/nand0p1 ]; then
		fail "no volume nand0p1 (volume map not found)"
		return
	fi
	pass "volume map read"

	mnt=/tmp/hwtest-nand0p1
	mkdir -p "$mnt"
	if mount -t hfsplus -o ro /dev/nand0p1 "$mnt" 2>/dev/null; then
		n=$(ls "$mnt" | wc -l)
		if [ "$n" -gt 0 ] && [ -d "$mnt/System" ]; then
			pass "nand0p1 mounts read-only ($n entries in /)"
		else
			fail "nand0p1 mounted but its root looks empty"
		fi
		umount "$mnt"
	else
		fail "nand0p1 does not mount as hfsplus"
	fi
	rmdir "$mnt" 2>/dev/null
}
