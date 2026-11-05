/*
 * d2x debug area: the logic, with no IOS call in it (see d2xdbg.h).
 *
 * MLOAD runs it inside its SWI handler, with every permission and IRQs off,
 * so each call is atomic. tests/host/dbg runs the same file on a PC.
 * The caller passes the Hollywood timer in "now" and does the cache work.
 */
#include <string.h>

#include "types.h"
#include "d2xdbg.h"

#define HEAD(area)	((d2xdbg_head *)(area))

/* 243 MHz / 128 = 1898437.5 ticks per second, 1898.4375 per ms */
u32 d2xdbg_ms_to_ticks(u32 ms)
{
	if (ms > D2XDBG_WD_MAX_MS)
		ms = D2XDBG_WD_MAX_MS;
	return ms * 1898 + ((ms * 7) >> 4);
}

static u32 __ticks_to_ms(u32 ticks)
{
	/* ticks / 1898.4375 to 0.1%, with no divide (MLOAD links no libgcc
	   division) and no overflow for any u32: 138 / 2^18 = 1 / 1899.6 */
	return ((ticks >> 8) * 138) >> 10;
}

static void __record(u8 *area, u32 now, u32 word, u32 a, u32 b)
{
	d2xdbg_head *h = HEAD(area);
	u32 *rec = (u32 *)(area + D2XDBG_RING_OFF) + (h->trace_head & (D2XDBG_RECORDS - 1)) * 4;

	rec[0] = now;
	rec[1] = word;
	rec[2] = a;
	rec[3] = b;
	h->trace_head++;
	h->now = now;
}

void d2xdbg_clear(u8 *area)
{
	d2xdbg_head *h = HEAD(area);

	memset(h->fault, 0, sizeof(h->fault));
	memset(h->mod, 0, sizeof(h->mod));
	memset(area + D2XDBG_RING_OFF, 0, D2XDBG_HALF - D2XDBG_RING_OFF);
	h->trace_head = 0;
}

void d2xdbg_start(u8 *area, const char *build, u32 flags, u32 now)
{
	d2xdbg_head *h = HEAD(area);
	u32 boots = 1, kept = 0;

	/* A valid area from the run before: keep it as last words */
	if (h->magic == D2XDBG_MAGIC && h->layout == D2XDBG_LAYOUT && h->size == D2XDBG_SIZE) {
		memcpy(area + D2XDBG_HALF, area, D2XDBG_HALF);
		boots = h->boots + 1;
		kept = 1;
	}

	memset(area, 0, D2XDBG_HALF);
	h->magic  = D2XDBG_MAGIC;
	h->layout = D2XDBG_LAYOUT;
	h->size   = D2XDBG_SIZE;
	h->boots  = boots;
	strncpy(h->build, build, sizeof(h->build) - 1);
	h->flags  = flags;

	__record(area, now, (D2XDBG_M_MLOAD << 24) | (D2XDBG_E_BOOT << 16), boots, kept);
}

s32 d2xdbg_event(u8 *area, u32 now, u32 word, u32 a, u32 b)
{
	d2xdbg_head *h = HEAD(area);
	u32 m = word >> 24, ev = (word >> 16) & 0xFF;
	d2xdbg_mod *mod;

	__record(area, now, word, a, b);

	if (m >= D2XDBG_MODS)
		return 0;
	mod = &h->mod[m];

	switch (ev) {
	case D2XDBG_E_BEGIN:
		mod->requests++;
		mod->cur_start = now ? now : 1;
		mod->cur_cmd   = word & 0xFFFF;
		mod->cur_arg   = a;
		break;

	case D2XDBG_E_END:
		if (a & 0x80000000)	/* negative; s32 is long on a host build */
			mod->errors++;
		if (mod->cur_start) {
			u32 len = now - mod->cur_start;	/* modulo 2^32: the timer wraps */

			if (len > mod->max_ticks) {
				mod->max_ticks = len;
				mod->max_cmd   = mod->cur_cmd;
				mod->max_arg   = mod->cur_arg;
			}
			mod->cur_start = 0;
		}
		break;

	case D2XDBG_E_RETRY:	mod->retries++;	 break;
	case D2XDBG_E_RESET:	mod->resets++;	 break;
	case D2XDBG_E_TIMEOUT:	mod->timeouts++; break;
	case D2XDBG_E_ERROR:	mod->errors++;	 break;
	case D2XDBG_E_SDHS:	h->sd_hs = a;	 break;
	}

	return 0;
}

u32 d2xdbg_point(u8 *area, u32 now, u32 mod, u32 point, u32 a)
{
	d2xdbg_head *h = HEAD(area);
	u32 key = (mod << 8) | point;
	u32 i;

	for (i = 0; i < D2XDBG_FAULTS; i++) {
		d2xdbg_fault *f = &h->fault[i];

		if (f->point != key || !f->count)
			continue;
		if (f->skip) {
			f->skip--;
			continue;
		}
		f->count--;
		if (mod < D2XDBG_MODS)
			h->mod[mod].faults++;
		__record(area, now, (mod << 24) | (D2XDBG_E_FAULT << 16) | point, f->action, i | (a << 8));
		return f->action;
	}

	return 0;
}

s32 d2xdbg_fault_set(u8 *area, const u32 *in)
{
	d2xdbg_head *h = HEAD(area);
	d2xdbg_fault *f;

	if (in[0] >= D2XDBG_FAULTS)
		return -1;
	f = &h->fault[in[0]];
	f->point  = in[1];
	f->count  = in[2];
	f->skip   = in[3];
	f->action = in[4];
	return 0;
}

void d2xdbg_wd_arm(u8 *area, u32 now, u32 ms)
{
	d2xdbg_head *h = HEAD(area);

	if (ms > D2XDBG_WD_MAX_MS)
		ms = D2XDBG_WD_MAX_MS;
	h->wd_timeout_ms = ms;
	h->wd_seen_kick  = h->wd_kick;
	h->wd_seen_ticks = now;
	__record(area, now, (D2XDBG_M_MLOAD << 24) | (D2XDBG_E_WDARM << 16), ms, 0);
}

/* One watchdog check: 1 when the console is to be reset now. */
s32 d2xdbg_wd_check(u8 *area, u32 now)
{
	d2xdbg_head *h = HEAD(area);
	u32 quiet;

	h->wd_beats++;
	h->now = now;

	if (!h->wd_timeout_ms)
		return 0;

	if (h->wd_kick != h->wd_seen_kick) {
		h->wd_seen_kick  = h->wd_kick;
		h->wd_seen_ticks = now;
		return 0;
	}

	quiet = now - h->wd_seen_ticks;
	if (quiet < d2xdbg_ms_to_ticks(h->wd_timeout_ms))
		return 0;

	h->wd_fired = now ? now : 1;
	__record(area, now, (D2XDBG_M_MLOAD << 24) | (D2XDBG_E_WDFIRE << 16), __ticks_to_ms(quiet), h->wd_timeout_ms);
	return 1;
}
