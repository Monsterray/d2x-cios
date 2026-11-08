/*
 * The d2x debug area on a PC (dbg_host.h). Built with the test's own types
 * (D2XDBG_HAVE_TYPES and its types.h first on the include path).
 */
#include <string.h>

#include "types.h"
#include "hostcheck.h"
#include "dbg_host.h"

#include "dbg_core.c"

u8 dbg_area[D2XDBG_SIZE] __attribute__((aligned(32)));

static u32 __no_clock(void) { return 0; }
static void __no_sleep(u32 ms) { (void)ms; }

u32 (*dbg_host_now)(void) = __no_clock;
void (*dbg_host_sleep_ms)(u32 ms) = __no_sleep;
void (*dbg_host_on_hang)(u32 action);

/* MLOAD's SWI 0xcc, commands 64 and 65 (mload-module/dbg.c) */
s32 Swi_MLoad(u32 cmd, u32 arg1, u32 arg2, u32 arg3)
{
	switch (cmd) {
	case D2XDBG_SWI_EVENT:
		return d2xdbg_event(dbg_area, dbg_host_now(), arg1, arg2, arg3);
	case D2XDBG_SWI_POINT:
		return d2xdbg_point(dbg_area, dbg_host_now(), arg1 >> 8, arg1 & 0xFF, arg2);
	}
	return 0;
}

/* cios-lib/d2xdbg.c */
u32 Dbg_Point(u32 mod, u32 point, u32 a)
{
	u32 r = Swi_MLoad(D2XDBG_SWI_POINT, (mod << 8) | point, a, 0);

	switch (D2XDBG_FAULT_ACTION(r)) {
	case D2XDBG_F_DELAY:
		dbg_host_sleep_ms(D2XDBG_FAULT_ARG(r));
		return 0;

	case D2XDBG_F_BLOCK:
	case D2XDBG_F_SPIN:
		if (dbg_host_on_hang)
			dbg_host_on_hang(D2XDBG_FAULT_ACTION(r));
		hc_hang("an injected hang (action %u) at module %u point '%c'", D2XDBG_FAULT_ACTION(r), mod, point);
	}

	return r;
}

void dbg_host_reset(void)
{
	memset(dbg_area, 0, sizeof(dbg_area));
	d2xdbg_start(dbg_area, "host", 0, dbg_host_now());
	dbg_host_on_hang = NULL;
}

void dbg_host_fault(u32 slot, u32 mod, u32 point, u32 count, u32 skip, u32 action)
{
	u32 in[5] = { slot, (mod << 8) | point, count, skip, action };

	d2xdbg_fault_set(dbg_area, in);
}

u32 dbg_host_count(u32 mod, u32 ev)
{
	d2xdbg_head *h = DBG_HEAD;
	u32 i, n = 0, first = h->trace_head > D2XDBG_RECORDS ? h->trace_head - D2XDBG_RECORDS : 0;

	for (i = first; i < h->trace_head; i++) {
		u32 w = ((u32 *)(dbg_area + D2XDBG_RING_OFF))[(i & (D2XDBG_RECORDS - 1)) * 4 + 1];

		if ((w >> 24) == mod && ((w >> 16) & 0xFF) == ev)
			n++;
	}
	return n;
}
