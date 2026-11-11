/*
 * 64-bit unsigned division for FatFs's exFAT code (file sizes and offsets are
 * 64 bits, and ff.c divides them by the sector and cluster size).
 *
 * libgcc's __aeabi_uldivmod calls __gnu_uldivmod_helper, which libgcc keeps in
 * bpabi.o next to __gnu_ldivmod_helper. That object needs __divdi3 and
 * __udivdi3, which pull in the C++ exception unwinder: 6.2 KB of code in a
 * module with 20 KB. Defining the helper here keeps bpabi.o out of the link.
 * Nothing in the module divides signed 64-bit numbers; if something starts to,
 * the link fails on __gnu_ldivmod_helper and that one needs adding.
 */

typedef unsigned long long u64_t;

u64_t __gnu_uldivmod_helper(u64_t n, u64_t d, u64_t *remainder);

u64_t __gnu_uldivmod_helper(u64_t n, u64_t d, u64_t *remainder)
{
	u64_t q = 0, r = 0;
	int i;

	/* Both in 32 bits: the usual case, and the only one that is frequent */
	if (!(n >> 32) && !(d >> 32) && d) {
		unsigned int n32 = (unsigned int)n, d32 = (unsigned int)d;

		*remainder = n32 % d32;
		return n32 / d32;
	}

	/* Division by zero gives all ones, as the ARM divide instruction does */
	if (!d) {
		*remainder = n;
		return ~(u64_t)0;
	}

	/* Shift and subtract, a bit of the quotient at a time */
	for (i = 63; i >= 0; i--) {
		r = (r << 1) | ((n >> i) & 1);
		if (r >= d) {
			r -= d;
			q |= (u64_t)1 << i;
		}
	}

	*remainder = r;
	return q;
}
