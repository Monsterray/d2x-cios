/*
 * The d2x debug area on a PC, for host tests of modules built with
 * -DD2X_DEBUG: Swi_MLoad() and Dbg_Point() run mload-module/dbg_core.c on a
 * buffer, as MLOAD's SWI handler and cios-lib's d2xdbg.c do on the Wii.
 *
 * The test sets dbg_host_now (the simulated Hollywood timer) and
 * dbg_host_sleep_ms (what a D2XDBG_F_DELAY fault sleeps on); a
 * D2XDBG_F_BLOCK or D2XDBG_F_SPIN fault calls dbg_host_on_hang, which must
 * not return (a test longjmps out), or ends the test as hung.
 */
#ifndef DBG_HOST_H
#define DBG_HOST_H

#include "d2xdbg.h"

extern u8 dbg_area[D2XDBG_SIZE];
extern u32 (*dbg_host_now)(void);
extern void (*dbg_host_sleep_ms)(u32 ms);
extern void (*dbg_host_on_hang)(u32 action);

#define DBG_HEAD	((d2xdbg_head *)dbg_area)
#define DBG_MOD(m)	(&DBG_HEAD->mod[m])

void dbg_host_reset(void);	/* a fresh area, no faults */
void dbg_host_fault(u32 slot, u32 mod, u32 point, u32 count, u32 skip, u32 action);
u32  dbg_host_count(u32 mod, u32 ev);	/* records of this module and event in the ring */

#endif
