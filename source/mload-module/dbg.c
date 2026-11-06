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


/* cios-lib's DCFlushRange (clean) and DCInvalidateRange take a count of
   32-byte lines, not bytes: these take bytes. */
static void __Dbg_Clean(const void *p, u32 len)
{
	u32 a = (u32)p & ~31;

	DCFlushRange((void *)a, ((u32)p + len - a + 31) >> 5);
}

static void __Dbg_Discard(const void *p, u32 len)
{
	u32 a = (u32)p & ~31;

	DCInvalidateRange((void *)a, ((u32)p + len - a + 31) >> 5);
}

static inline u32 __Dbg_Now(void)
{
	return *(vu32 *)HW_TIMER;
}

/* Write the header and the newest record out, so the PowerPC sees them now:
   the next instruction may never come back. */
static void __Dbg_Out(void)
{
	u32 last = (HEAD->trace_head - 1) & (D2XDBG_RECORDS - 1);

	__Dbg_Clean(AREA, sizeof(d2xdbg_head));
	__Dbg_Clean(AREA + D2XDBG_RING_OFF + last * 16, 16);
}

/* IOS's page table maps MEM2 only as far as its modules reach (0x1379CFFF
   on base 56, seen in ironic): the area has no ARM mapping, and its first
   access is a translation fault that MEM2_Prot() and Perms_Write() cannot
   help. Map its pages ourselves: small pages, read/write for all, cached,
   in the coarse table IOS already has for that megabyte. */
static u32 __Dbg_MapArea(void)
{
	u32 l1, i, fixed = 0;
	vu32 *l2;

	l1 = *(vu32 *)((MMU_GetTTB() & ~0x3FFF) + ((D2XDBG_PHYS >> 20) << 2));
	if ((l1 & 3) != 1)		/* not a coarse page table */
		return 0;
	l2 = (vu32 *)((l1 & ~0x3FF) + (((D2XDBG_PHYS >> 12) & 0xFF) << 2));
	for (i = 0; i < D2XDBG_SIZE / 0x1000; i++) {
		if (l2[i] != ((D2XDBG_PHYS + i * 0x1000) | 0xFFE)) {
			l2[i] = (D2XDBG_PHYS + i * 0x1000) | 0xFFE;
			fixed++;
		}
	}
	if (fixed) {
		__Dbg_Clean((void *)l2, D2XDBG_SIZE / 0x1000 * 4);
		MMU_InvalidateTLB();
	}
	return fixed;
}

/* Returns only when the reset did not take: then everything is put back and
   the watchdog is off, so IOS goes on (IRQs are off here) and the PowerPC
   can show what happened, instead of a console stuck for good. */
static void __Dbg_Reset(void)
{
	u32 abra = *(vu32 *)D2XDBG_ABRA_PHYS, start;

	/* Priiloader autoboots whatever its boot state says */
	*(vu32 *)D2XDBG_ABRA_PHYS = D2XDBG_ABRA;
	__Dbg_Clean((void *)D2XDBG_ABRA_PHYS, 4);

	/* HW_RESETS bit 0 low: system reset, no IOS needed */
	*(vu32 *)HW_RESETS &= ~1;

	start = __Dbg_Now();
	while (__Dbg_Now() - start < d2xdbg_ms_to_ticks(250))
		;

	*(vu32 *)HW_RESETS |= 1;
	*(vu32 *)D2XDBG_ABRA_PHYS = abra;
	__Dbg_Clean((void *)D2XDBG_ABRA_PHYS, 4);
	d2xdbg_wd_noreset(AREA, __Dbg_Now());
	__Dbg_Out();
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
		__Dbg_Clean(AREA, D2XDBG_HALF);
		return 0;

	case D2XDBG_IOCTL_WD_ARM:
		d2xdbg_wd_arm(AREA, __Dbg_Now(), in[0]);
		break;

	case D2XDBG_IOCTL_WD_KICK:
		HEAD->wd_kick++;
		__Dbg_Clean(&HEAD->wd_kick, 4);
		return 0;

	case D2XDBG_IOCTL_FAULT:
		if (d2xdbg_fault_set(AREA, in) < 0)
			return -1;
		__Dbg_Clean(AREA, sizeof(d2xdbg_head));
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

	/* IOS may rebuild its page table and drop our entries: put them back
	   before touching the area, and count it (a fault here, in SVC mode
	   with IRQs off, would stop the whole console) */
	if (cmd != D2XDBG_SWI_CTRL || (arg1 & 0xFFFF) != 0xFF) {
		if (__Dbg_MapArea()) {
			HEAD->map_fixes++;
			__Dbg_Clean(&HEAD->map_fixes, 4);
		}
	}

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
		__Dbg_Discard(&HEAD->wd_kick, 4);
		if (d2xdbg_wd_check(AREA, __Dbg_Now())) {
			__Dbg_Out();
			__Dbg_Reset();
		}
		__Dbg_Clean(AREA, 0x60);
		break;

	case D2XDBG_SWI_CTRL:
		switch (arg1 & 0xFFFF) {
		case 0xFF:	/* MLOAD starts: keep the last run's area as last words */
			__Dbg_MapArea();
			__Dbg_Discard(AREA, D2XDBG_SIZE);
			d2xdbg_start(AREA, D2X_IDENT, buildFlags, __Dbg_Now());
			__Dbg_Clean(AREA, D2XDBG_SIZE);
			break;

		case 0xFE:	/* the watchdog thread runs, at this priority */
			HEAD->wd_prio = arg2;
			__Dbg_Clean(AREA, sizeof(d2xdbg_head));
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
