# SPDX-License-Identifier: GPL-2.0-only
# SGX543 GPU: bound, microkernel firmware provisioned from NAND, microkernel
# starts (handshake), command submission works, render node present

t_gpu() {
	section "GPU (SGX543)"
	g=$PLAT/35100000.gpu
	if ! bound "$g"; then
		fail "SGX543 device not bound"
		return
	fi
	pass "35100000.gpu bound"

	fw=/lib/firmware/s5l8947x/sgx543.fw
	if [ ! -s "$fw" ]; then
		skip "no microkernel firmware ($fw; stage2 carves it from nand0p1)"
		return
	fi
	pass "microkernel firmware present ($(wc -c <"$fw") bytes)"

	d=/sys/kernel/debug/apple-sgx
	[ -r "$d/regs" ] ||
		mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
	if [ ! -r "$d/regs" ]; then
		fail "apple-sgx debugfs not available"
		return
	fi
	pass "debugfs interface present"

	for n in 1 2; do
		if ! wr "$d/boot" 1; then
			fail "boot $n rejected"
			return
		fi
		sleep 1
		if grep -q "microkernel: up" "$d/regs"; then
			pass "boot $n: microkernel up"
		else
			fail "boot $n: $(grep '^microkernel:' "$d/regs")"
			return
		fi
	done

	if wr "$d/cmd" "hwperf 0"; then
		pass "kernel CCB command taken"
	else
		fail "kernel CCB command not taken"
	fi

	if [ -e /dev/dri/renderD128 ]; then
		pass "render node /dev/dri/renderD128 present"
	else
		fail "no render node /dev/dri/renderD128"
	fi

	if command -v sgxinfo >/dev/null 2>&1; then
		if sgxinfo >/dev/null 2>&1; then
			pass "sgxinfo opened the render node"
		else
			fail "sgxinfo failed"
		fi
	fi

	if command -v sgxgem >/dev/null 2>&1; then
		if sgxgem -c >/dev/null 2>&1; then
			pass "contiguous buffer made and exported as a dma-buf"
		else
			fail "sgxgem -c failed"
		fi
		if [ -c /dev/dri/card1 ]; then
			if sgxgem -i /dev/dri/card1 >/dev/null 2>&1; then
				pass "display dma-buf imported and mapped for the GPU"
			else
				fail "sgxgem -i failed"
			fi
		fi
	fi

	if command -v sgx2d >/dev/null 2>&1; then
		if sgx2d >/dev/null 2>&1; then
			pass "2D engine blit landed in the destination"
		else
			fail "sgx2d failed"
		fi
	fi
}
