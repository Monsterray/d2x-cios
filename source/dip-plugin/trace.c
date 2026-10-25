/*
 * DIP trace (see trace.h). Only in builds with -DDIP_DEBUG_TRACE.
 */
#ifdef DIP_DEBUG_TRACE

#include "dip_calls.h"
#include "hollywood.h"
#include "syscalls.h"
#include "trace.h"

u32 DI_TraceMode(void);                 /* plugin.c: the emulation mode bits */

/* dip_calls.s, renamed in trace builds so the wrappers below can log */
extern s32 __DI_HandleIoctl_raw(void *buffer, u32 fd);
extern s32 __DI_HandleCmd_raw(void *inbuf, const void *outbuf, u32 size);

static u32 *trace;                      /* the loader's buffer, or NULL */

void Trace_Set(u32 addr)
{
	trace = (u32 *)addr;
	if (!trace)
		return;
	trace[0] = DIP_TRACE_MAGIC;
	trace[1] = 0;
	trace[2] = trace[3] = 0;
	os_sync_after_write(trace, DIP_TRACE_BYTES);
}

void Trace_Event(u8 where, u8 cmd, u32 arg, u32 arg2)
{
	u32 *rec;

	if (!trace)
		return;
	rec = trace + 4 + (trace[1] % DIP_TRACE_RECORDS) * 4;
	rec[0] = *(vu32 *)HW_TIMER;
	rec[1] = (where << 24) | (cmd << 16) | (DI_TraceMode() & 0xFFFF);
	rec[2] = arg;
	rec[3] = arg2;
	trace[1]++;
	/* write it out now: the next instruction may never come back */
	os_sync_after_write(rec, 16);
	os_sync_after_write(trace, 16);
}

/* The calls into Nintendo's DI module, logged on the way in and out: a hang
   inside the module leaves a 'D' or 'O' with no matching end. */
s32 DI_HandleCmd(void *inbuf, const void *outbuf, u32 size)
{
	u8 cmd = ((u32 *)inbuf)[0] >> 24;
	s32 ret;

	Trace_Event(DIP_T_DI, cmd, ((u32 *)inbuf)[1], ((u32 *)inbuf)[2]);
	ret = __DI_HandleCmd_raw(inbuf, outbuf, size);
	Trace_Event(DIP_T_DI_END, cmd, ret, 0);
	return ret;
}

s32 DI_HandleIoctl(void *buffer, u32 fd)
{
	u8 cmd = ((u32 *)buffer)[0];
	s32 ret;

	Trace_Event(DIP_T_DIIO, cmd, fd, 0);
	ret = __DI_HandleIoctl_raw(buffer, fd);
	Trace_Event(DIP_T_DIIO_END, cmd, ret, 0);
	return ret;
}

#endif
