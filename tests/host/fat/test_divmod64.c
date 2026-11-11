/*
 * Host test of the FAT module's divmod64.c: the 64-bit unsigned division FatFs's exFAT code
 * uses. The function is compiled here under another name and compared with the PC's own
 * / and % on edge values and on random ones. Division by zero is defined by the function
 * (quotient all ones, remainder the dividend, as the ARM divide instruction does) and
 * is tested as that, not left to crash.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HOSTCHECK_MAIN
#include "hostcheck.h"

#define __gnu_uldivmod_helper d2x_uldivmod_helper
#include "divmod64.c"
#undef __gnu_uldivmod_helper

typedef unsigned long long u64;

static void one(u64 n, u64 d)
{
	u64 r = 0x5555555555555555ULL;
	u64 q = d2x_uldivmod_helper(n, d, &r);

	CHECK(q == n / d && r == n % d, "%llu / %llu: got %llu rem %llu, want %llu rem %llu",
	      n, d, q, r, n / d, n % d);
}

static const u64 edge[] = {
	1, 2, 3, 5, 7, 10, 255, 256, 511, 512, 513, 4095, 4096, 65535, 65536,
	0x7FFFFFFFULL, 0x80000000ULL, 0xFFFFFFFFULL, 0x100000000ULL, 0x100000001ULL,
	0x1FFFFFFFFULL, 0x200000000ULL, 0xFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL,
	0x8000000000000000ULL, 0x8000000000000001ULL, 0xFFFFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFFFFFULL,
	5ULL * 1024 * 1024 * 1024 + 1000, 2ULL * 1024 * 1024 * 1024 * 1024 - 1,
};

static void test_edges(void)
{
	size_t i, j;

	for (i = 0; i < HC_COUNT(edge); i++)
		for (j = 0; j < HC_COUNT(edge); j++) {
			one(edge[i], edge[j]);
			CHECK_OK();
		}
	/* a dividend of 0 */
	for (j = 0; j < HC_COUNT(edge); j++) {
		one(0, edge[j]);
		CHECK_OK();
	}
}

static void test_powers_of_two(void)
{
	int a, b;

	for (a = 0; a < 64; a++)
		for (b = 0; b < 64; b++) {
			u64 n = 1ULL << a, d = 1ULL << b;

			one(n, d);
			one(n - 1, d);
			one(n + 1, d);
			one(n | (n - 1), d);
			one(~0ULL, d);
			CHECK_OK();
		}
}

static void test_divisor_larger_than_dividend(void)
{
	u64 r = 0;

	CHECK(d2x_uldivmod_helper(5, 6, &r) == 0 && r == 5, "5 / 6");
	CHECK(d2x_uldivmod_helper(0xFFFFFFFFULL, 0x100000000ULL, &r) == 0 && r == 0xFFFFFFFFULL, "2^32-1 / 2^32");
	CHECK(d2x_uldivmod_helper(0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL, &r) == 0
	      && r == 0x7FFFFFFFFFFFFFFFULL, "2^63-1 / 2^63");
	CHECK(d2x_uldivmod_helper(0, 1, &r) == 0 && r == 0, "0 / 1");
}

static void test_divide_by_zero(void)
{
	u64 r = 0;
	u64 q = d2x_uldivmod_helper(1234567890123ULL, 0, &r);

	CHECK(q == ~0ULL && r == 1234567890123ULL, "n / 0: quotient %llx remainder %llu", q, r);
	q = d2x_uldivmod_helper(0, 0, &r);
	CHECK(q == ~0ULL && r == 0, "0 / 0: quotient %llx remainder %llu", q, r);
	q = d2x_uldivmod_helper(7, 0, &r);
	CHECK(q == ~0ULL && r == 7, "7 / 0: quotient %llx remainder %llu", q, r);
}

static u64 rnd64(unsigned int *seed)
{
	u64 v = 0;
	int i;

	for (i = 0; i < 4; i++) {
		*seed = *seed * 1103515245u + 12345u;
		v = (v << 16) | ((*seed >> 8) & 0xFFFF);
	}
	return v;
}

static void test_random(void)
{
	unsigned int seed = 0xD2C105u;
	int i;

	for (i = 0; i < 200000; i++) {
		u64 n = rnd64(&seed), d = rnd64(&seed);

		/* vary the sizes of both: all of 64, 32, 16 and 8 bits, and sector/cluster-like divisors */
		n >>= (rnd64(&seed) & 3) * 16;
		d >>= (rnd64(&seed) & 3) * 16;
		if (!d)
			d = 1;
		if (i % 7 == 0)
			d = 512u << (rnd64(&seed) % 10);
		one(n, d);
		CHECK_OK();
	}
}

static const struct hc_test tests[] = {
	{ "edges", test_edges, false },
	{ "powers_of_two", test_powers_of_two, false },
	{ "divisor_larger_than_dividend", test_divisor_larger_than_dividend, false },
	{ "divide_by_zero", test_divide_by_zero, false },
	{ "random", test_random, false },
};

int main(int argc, char **argv)
{
	return hc_main(tests, HC_COUNT(tests), argc, argv, NULL);
}
