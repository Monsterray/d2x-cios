/*
 * DIP trace: breadcrumbs a loader can show on the TV while IOS is stuck.
 *
 * Built only with -DDIP_DEBUG_TRACE (EXTRA_CFLAGS); release builds compile
 * every DIP_TRACE() to nothing and do not answer IOCTL_DI_TRACE_SET.
 *
 * Why: when IOS hangs inside the DI module (for example waiting on a dead
 * disc drive) nothing in IOS can report it, and a power cycle wipes RAM. The
 * PowerPC side keeps running, though, so a loader that gives the plugin a
 * buffer in MEM1 or MEM2 can draw the last records each frame and show
 * where the plugin stopped.
 *
 * Use from a loader (USB Loader GX's bench build does this):
 *
 *   u32 *trace = memalign(32, DIP_TRACE_BYTES);   // MEM1 or MEM2
 *   memset(trace, 0, DIP_TRACE_BYTES);
 *   DCFlushRange(trace, DIP_TRACE_BYTES);
 *   inbuf[0] = IOCTL_DI_TRACE_SET << 24;
 *   inbuf[1] = MEM_VIRTUAL_TO_PHYSICAL(trace);     // 0 stops tracing
 *   IOS_Ioctl(di_fd, IOCTL_DI_TRACE_SET, inbuf, 0x20, outbuf, 0x20);
 *   // each frame: DCInvalidateRange(trace, DIP_TRACE_BYTES), then show
 *   // the records before trace[1] (the next record's index, it wraps)
 *
 * Buffer layout (big-endian u32 words, as both CPUs see them):
 *   [0] magic 'DIPT'  [1] records written so far  [2..3] reserved
 *   then DIP_TRACE_RECORDS records of 4 words:
 *   [0] Hollywood timer ticks (about 1898 per ms)
 *   [1] where << 24 | cmd << 16 | mode     (where: a DIP_T_* letter)
 *   [2] arg, [3] arg2                      (see each DIP_T_* below)
 */
#ifndef _DIP_TRACE_H_
#define _DIP_TRACE_H_

#include "types.h"

#define IOCTL_DI_TRACE_SET	0xFC	/* debug builds only */

#define DIP_TRACE_MAGIC		0x44495054	/* 'DIPT' */
#define DIP_TRACE_RECORDS	31
#define DIP_TRACE_BYTES		(16 + DIP_TRACE_RECORDS * 16)

/* where: what the plugin was doing */
#define DIP_T_CMD	'C'	/* DI_EmulateCmd entered: cmd, arg = inbuf[1], arg2 = inbuf[2] */
#define DIP_T_CMD_END	'c'	/* DI_EmulateCmd returns: arg = ret */
#define DIP_T_IOCTL	'I'	/* DI_EmulateIoctl entered: cmd */
#define DIP_T_IOCTL_END	'i'	/* DI_EmulateIoctl returns: arg = ret */
#define DIP_T_DI	'D'	/* calling Nintendo's DI command handler: cmd */
#define DIP_T_DI_END	'd'	/* it returned: arg = ret */
#define DIP_T_DIIO	'O'	/* calling Nintendo's DI ioctl handler: cmd */
#define DIP_T_DIIO_END	'o'	/* it returned: arg = ret */
#define DIP_T_READ	'R'	/* emulated read: arg = length, arg2 = offset (words) */
#define DIP_T_READ_END	'r'	/* emulated read returns: arg = ret */

#ifdef DIP_DEBUG_TRACE
void Trace_Set(u32 addr);
void Trace_Event(u8 where, u8 cmd, u32 arg, u32 arg2);
#define DIP_TRACE(where, cmd, arg, arg2)	Trace_Event(where, cmd, arg, arg2)
#else
#define DIP_TRACE(where, cmd, arg, arg2)	do {} while (0)
#endif

#endif
