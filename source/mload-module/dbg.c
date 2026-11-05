/*
 * d2x debug area in MLOAD: /dev/d2xdbg, the SWI calls modules send events
 * through, and the ARM watchdog (see d2xdbg.h). Only in -DD2X_DEBUG builds.
 */
#ifdef D2X_DEBUG

#include <string.h>

#include "d2xdbg.h"
#include "d2xdbg_ident.h"
#include "dbg.h"
#include "hollywood.h"
#include "syscalls.h"
#include "tools.h"
#include "types.h"

#define AREA		((u8 *)D2XDBG_PHYS)
#define HEAD		((d2xdbg_head *)D2XDBG_PHYS)

#define WD_PRIO_TOP	0x7F	/* IOS's highest thread priority */

/* The DIP trace (DIP_DEBUG_TRACE) comes with every debug build */
static const u32 buildFlags = D2XDBG_FL_DEBUG | D2XDBG_FL_DIP_TRACE
#ifdef SDIO_HIGH_SPEED
	| D2XDBG_FL_SD_HS
#endif
#ifdef SDIO_HS_DEBUG
	| D2XDBG_FL_SD_HS_DEBUG
#endif
#ifdef DIP_GAP_ERROR
	| D2XDBG_FL_DIP_GAPERR
#endif
#ifdef EHCI_DEBUG_LOG
	| D2XDBG_FL_EHCI_LOG
#endif
	;

static u32 wdStack[0x100] ATTRIBUTE_ALIGN(32);	/* 1 KB */
static u32 wdQueueBuf[4] ATTRIBUTE_ALIGN(32);


static inline u32 __Dbg_Now(void)
{
	return *(vu32 *)HW_TIMER;
}

/* Write the header and the newest record out, so the PowerPC sees them now:
   the next instruction may never come back. */
static void __Dbg_Out(void)
{
	u32 last = (HEAD->trace_head - 1) & (D2XDBG_RECORDS - 1);

	DCFlushRange(AREA, sizeof(d2xdbg_head));
	DCFlushRange(AREA + D2XDBG_RING_OFF + last * 16, 16);
}

static void __Dbg_Reset(void)
{
	/* Priiloader autoboots whatever its boot state says */
	*(vu32 *)D2XDBG_ABRA_PHYS = D2XDBG_ABRA;
	DCFlushRange((void *)(D2XDBG_ABRA_PHYS & ~31), 32);

	/* HW_RESETS bit 0 low: system reset, no IOS needed */
	*(vu32 *)HW_RESETS &= ~1;

	for (;;)
		;
}

static s32 __Dbg_Ctrl(u32 op, u32 *in, u32 *out, u32 outlen)
{
	switch (op) {
	case D2XDBG_IOCTL_INFO:
		out[0] = D2XDBG_PHYS;
		out[1] = D2XDBG_SIZE;
		out[2] = D2XDBG_LAYOUT;
		out[3] = HEAD->magic;
		return 0;

	case D2XDBG_IOCTL_READ:
		if (in[0] >= D2XDBG_SIZE || outlen > D2XDBG_SIZE - in[0])
			return -1;
		memcpy(out, AREA + in[0], outlen);
		return 0;

	case D2XDBG_IOCTL_CLEAR:
		d2xdbg_clear(AREA);
		DCFlushRange(AREA, D2XDBG_HALF);
		return 0;

	case D2XDBG_IOCTL_WD_ARM:
		d2xdbg_wd_arm(AREA, __Dbg_Now(), in[0]);
		break;

	case D2XDBG_IOCTL_WD_KICK:
		HEAD->wd_kick++;
		DCFlushRange(&HEAD->wd_kick, 4);
		return 0;

	case D2XDBG_IOCTL_FAULT:
		if (d2xdbg_fault_set(AREA, in) < 0)
			return -1;
		DCFlushRange(AREA, sizeof(d2xdbg_head));
		return 0;

	default:
		return -1;
	}

	__Dbg_Out();
	return 0;
}

/* SWI 0xcc, commands 64 to 67, in SVC mode with IRQs off: atomic. */
s32 Dbg_Swi(u32 cmd, u32 arg1, u32 arg2, u32 arg3)
{
	u32 perms = Perms_Read();
	s32 ret = 0;

	Perms_Write(0xFFFFFFFF);

	switch (cmd) {
	case D2XDBG_SWI_EVENT:
		ret = d2xdbg_event(AREA, __Dbg_Now(), arg1, arg2, arg3);
		__Dbg_Out();
		break;

	case D2XDBG_SWI_POINT:
		ret = d2xdbg_point(AREA, __Dbg_Now(), arg1 >> 8, arg1 & 0xFF, arg2);
		if (ret)
			__Dbg_Out();
		break;

	case D2XDBG_SWI_WDTICK:
		/* wd_kick is written by the PowerPC: read it from memory */
		DCInvalidateRange(&HEAD->wd_kick, 32);
		if (d2xdbg_wd_check(AREA, __Dbg_Now())) {
			__Dbg_Out();
			__Dbg_Reset();
		}
		DCFlushRange(AREA, 0x60);
		break;

	case D2XDBG_SWI_CTRL:
		switch (arg1 & 0xFFFF) {
		case 0xFF:	/* MLOAD starts: keep the last run's area as last words */
			DCInvalidateRange(AREA, D2XDBG_SIZE);
			d2xdbg_start(AREA, D2X_IDENT, buildFlags, __Dbg_Now());
			DCFlushRange(AREA, D2XDBG_SIZE);
			break;

		case 0xFE:	/* the watchdog thread runs, at this priority */
			HEAD->wd_prio = arg2;
			DCFlushRange(AREA, sizeof(d2xdbg_head));
			break;

		default:
			ret = __Dbg_Ctrl(arg1 & 0xFFFF, (u32 *)arg2, (u32 *)arg3, arg1 >> 16);
		}
		break;
	}

	Perms_Write(perms);
	return ret;
}

static u32 __Dbg_Watchdog(void *arg)
{
	s32 queue;
	u32 msg;

	queue = os_message_queue_create(wdQueueBuf, 4);
	if (queue < 0)
		return 0;
	if (os_create_timer(D2XDBG_WD_PERIOD_US, D2XDBG_WD_PERIOD_US, queue, 0) < 0)
		return 0;

	for (;;) {
		if (os_message_queue_receive(queue, &msg, 0) < 0)
			continue;
		Swi_MLoad(D2XDBG_SWI_WDTICK, 0, 0, 0);
	}

	return 0;
}

void Dbg_Init(void)
{
	s32 prio = WD_PRIO_TOP, tid;

	/* Start the area (keeping the last run's as last words) */
	Swi_MLoad(D2XDBG_SWI_CTRL, 0xFF, 0, 0);

	/* The watchdog thread, as high as IOS lets us */
	tid = os_thread_create(__Dbg_Watchdog, NULL, &wdStack[0x100], sizeof(wdStack), prio, 0);
	if (tid < 0) {
		prio = os_thread_get_priority(os_get_thread_id());
		tid  = os_thread_create(__Dbg_Watchdog, NULL, &wdStack[0x100], sizeof(wdStack), prio, 0);
	}
	if (tid >= 0 && os_thread_continue(tid) >= 0)
		Swi_MLoad(D2XDBG_SWI_CTRL, 0xFE, prio, 0);
}

/* /dev/d2xdbg: IOS_Ioctl on any MLOAD descriptor */
s32 Dbg_Ioctl(u32 cmd, u32 *in, u32 inlen, u32 *out, u32 outlen)
{
	static u32 words[5];

	if (cmd == D2XDBG_IOCTL_HANG) {
		u32 mode = inlen >= 4 ? in[0] : 0;

		Swi_MLoad(D2XDBG_SWI_EVENT, (D2XDBG_M_MLOAD << 24) | (D2XDBG_E_HANG << 16), mode, 0);
		if (mode == 1)
			os_thread_stop(os_get_thread_id());
		if (mode == 3)
			os_thread_set_priority(os_get_thread_id(), WD_PRIO_TOP);
		if (mode >= 1 && mode <= 3)
			for (;;)
				;
		return -1;
	}

	/* In words, copied, so a short buffer reads as zeros */
	memset(words, 0, sizeof(words));
	if (in)
		memcpy(words, in, inlen < sizeof(words) ? inlen : sizeof(words));
	if (out && outlen)
		os_sync_before_read(out, outlen);

	if (cmd == D2XDBG_IOCTL_INFO && outlen < 16)
		return -1;

	cmd = Swi_MLoad(D2XDBG_SWI_CTRL, cmd | (outlen << 16), (u32)words, (u32)out);
	if (out && outlen)
		os_sync_after_write(out, outlen);
	return cmd;
}

#endif
