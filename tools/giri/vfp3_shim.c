// SPDX-License-Identifier: GPL-2.0-only
/*
 * The armv7-linux-musleabihf toolchain builds libgcc.a with -mfpu=neon-vfpv4,
 * so its float/double -> uint64 helpers use VFPv4 FMA, which the Cortex-A9
 * (VFPv3) lacks: any static binary calling them dies with SIGILL. These
 * replacements are linked ahead of libgcc.a and use only VFPv3 operations.
 * Out-of-range inputs saturate (negative/NaN -> 0, too large -> UINT64_MAX).
 */
#include <stdint.h>

#define TWO32 4294967296.0

static uint64_t d2u64(double x)
{
	uint32_t hi, lo;

	if (!(x > 0.0))		/* also catches NaN */
		return 0;
	if (x >= TWO32 * TWO32)
		return UINT64_MAX;
	hi = (uint32_t)(x / TWO32);	/* exact: division by a power of two */
	x -= (double)hi * TWO32;	/* exact: hi * 2^32 <= x */
	lo = (uint32_t)x;
	return ((uint64_t)hi << 32) | lo;
}

uint64_t __aeabi_d2ulz(double x);
uint64_t __fixunsdfdi(double x);
uint64_t __aeabi_f2ulz(float x);
uint64_t __fixunssfdi(float x);

uint64_t __aeabi_d2ulz(double x) { return d2u64(x); }
uint64_t __fixunsdfdi(double x) { return d2u64(x); }
/* every float is exactly representable as a double */
uint64_t __aeabi_f2ulz(float x) { return d2u64((double)x); }
uint64_t __fixunssfdi(float x) { return d2u64((double)x); }
