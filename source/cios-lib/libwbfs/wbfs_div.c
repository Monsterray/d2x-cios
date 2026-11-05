/* Software division for libwbfs.c (see libwbfs.h). Its own object, so a
   module that defines wbfs_udiv() and wbfs_sdiv() never links it. */
#include "libwbfs.h"

u32 wbfs_udiv(u32 n, u32 d)
{
	u32 q = 0, r = 0;
	int i;

	if (!d)
		return 0;
	for (i = 31; i >= 0; i--) {
		u32 top = r >> 31;
		r = (r << 1) | ((n >> i) & 1);
		if (top || r >= d) {
			r -= d;
			q |= 1U << i;
		}
	}
	return q;
}

int wbfs_sdiv(int n, int d)
{
	u32 q = wbfs_udiv(n < 0 ? -(u32)n : (u32)n, d < 0 ? -(u32)d : (u32)d);

	return ((n < 0) != (d < 0)) ? -(int)q : (int)q;
}
