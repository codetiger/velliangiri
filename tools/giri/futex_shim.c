// SPDX-License-Identifier: GPL-2.0-only
/*
 * The armv7-linux-musleabihf libstdc++.a is built for baseline ARMv7, where
 * ATOMIC_INT_LOCK_FREE == 1, so it omits std::__atomic_futex_unsigned_base.
 * Code compiled with -mcpu=cortex-a9 sees lock-free int atomics and takes the
 * futex-based std::future path, so it needs these three members at link time.
 *
 * They are written in C under their Itanium-mangled names. Under the ARM
 * AAPCS, the C++ signatures pass exactly like the C ones below: the implicit
 * `this` pointer comes first for the two non-static members, and each
 * std::chrono::duration<long long, ...> is a trivially copyable 8-byte
 * aggregate, so it passes like `struct dur`. A bool return goes in r0.
 */
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>

struct dur {
	long long count;
};

struct kts64 {
	int64_t tv_sec;
	int64_t tv_nsec;
};

static long futex_call(unsigned *addr, int op, unsigned val,
		       const struct kts64 *ts)
{
#ifdef SYS_futex_time64
	return syscall(SYS_futex_time64, addr, op, val, ts, NULL,
		       FUTEX_BITSET_MATCH_ANY);
#else
	struct { long tv_sec; long tv_nsec; } t32, *pt = NULL;

	if (ts) {
		t32.tv_sec = (long)ts->tv_sec;
		t32.tv_nsec = (long)ts->tv_nsec;
		pt = &t32;
	}
	return syscall(SYS_futex, addr, op, val, pt, NULL,
		       FUTEX_BITSET_MATCH_ANY);
#endif
}

/* Returns false only on timeout. s/ns form an absolute time on the clock. */
static bool wait_abs(unsigned *addr, unsigned val, bool has_timeout,
		     struct dur s, struct dur ns, bool realtime)
{
	struct kts64 ts;
	int op = FUTEX_WAIT_BITSET_PRIVATE;

	if (!has_timeout) {
		futex_call(addr, FUTEX_WAIT_PRIVATE, val, NULL);
		return true;
	}
	if (s.count < 0)
		return false;
	ts.tv_sec = s.count;
	ts.tv_nsec = ns.count;
	if (realtime)
		op |= FUTEX_CLOCK_REALTIME;
	if (futex_call(addr, op, val, &ts) == -1 && errno == ETIMEDOUT)
		return false;
	return true;
}

/* bool __atomic_futex_unsigned_base::_M_futex_wait_until(
 *	unsigned*, unsigned, bool, chrono::seconds, chrono::nanoseconds) */
bool futex_wait_until_rt(void *self, unsigned *addr, unsigned val,
			 bool has_timeout, struct dur s, struct dur ns)
	__asm__("_ZNSt28__atomic_futex_unsigned_base19_M_futex_wait_untilEPjjbNSt6chrono8durationIxSt5ratioILx1ELx1EEEENS2_IxS3_ILx1ELx1000000000EEEE");
bool futex_wait_until_rt(void *self, unsigned *addr, unsigned val,
			 bool has_timeout, struct dur s, struct dur ns)
{
	(void)self;
	return wait_abs(addr, val, has_timeout, s, ns, true);
}

/* bool __atomic_futex_unsigned_base::_M_futex_wait_until_steady(...) */
bool futex_wait_until_steady(void *self, unsigned *addr, unsigned val,
			     bool has_timeout, struct dur s, struct dur ns)
	__asm__("_ZNSt28__atomic_futex_unsigned_base26_M_futex_wait_until_steadyEPjjbNSt6chrono8durationIxSt5ratioILx1ELx1EEEENS2_IxS3_ILx1ELx1000000000EEEE");
bool futex_wait_until_steady(void *self, unsigned *addr, unsigned val,
			     bool has_timeout, struct dur s, struct dur ns)
{
	(void)self;
	return wait_abs(addr, val, has_timeout, s, ns, false);
}

/* static void __atomic_futex_unsigned_base::_M_futex_notify_all(unsigned*) */
void futex_notify_all(unsigned *addr)
	__asm__("_ZNSt28__atomic_futex_unsigned_base19_M_futex_notify_allEPj");
void futex_notify_all(unsigned *addr)
{
	futex_call(addr, FUTEX_WAKE_PRIVATE, INT_MAX, NULL);
}
