/*
 * Host tests of the d2x debug area's logic (mload-module/dbg_core.c): the
 * layout the PowerPC reads, last words, per-module counters, the trace ring,
 * fault points and the watchdog's decision. MLOAD runs the same file inside
 * its SWI handler; here the area is a plain buffer and "now" is passed in.
 * Run with `make -C tests/host`.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define HOSTCHECK_MAIN
#include "hostcheck.h"

/* cios-lib's types.h typedefs bool itself */
#undef bool
#undef true
#undef false

#include "dbg_core.c"	/* the unit under test */

static u8 area[D2XDBG_SIZE] __attribute__((aligned(32)));
#define H	((d2xdbg_head *)area)

static u32 *rec(u32 n)
{
	return (u32 *)(area + D2XDBG_RING_OFF) + (n & (D2XDBG_RECORDS - 1)) * 4;
}

static u32 word(u32 mod, u32 ev, u32 x)
{
	return (mod << 24) | (ev << 16) | x;
}

static void fresh(void)
{
	memset(area, 0xA5, sizeof(area));	/* what MEM2 might hold at power-on */
	d2xdbg_start(area, "-1-0-rc1+dbg@abc1234", D2XDBG_FL_DEBUG, 1000);
}

static void test_layout(void)
{
	/* The PowerPC side (tests/wii/d2xdiag) reads these offsets */
	CHECK(offsetof(d2xdbg_head, build) == 0x10, "build at 0x10");
	CHECK(offsetof(d2xdbg_head, trace_head) == 0x38, "trace_head at 0x38");
	CHECK(offsetof(d2xdbg_head, wd_timeout_ms) == 0x40, "wd_timeout_ms at 0x40");
	CHECK(offsetof(d2xdbg_head, wd_kick) == 0x60, "wd_kick at 0x60");
	CHECK(offsetof(d2xdbg_head, wd_kick) % 32 == 0, "wd_kick starts a cache line");
	CHECK(offsetof(d2xdbg_head, fault) == 0x80, "fault at 0x80");
	CHECK(offsetof(d2xdbg_head, mod) == 0x100, "mod at 0x100");
	CHECK(sizeof(d2xdbg_mod) == 48, "mod is 48 bytes");
	CHECK(sizeof(d2xdbg_head) == 0x280, "head is 0x280 bytes, got 0x%zx", sizeof(d2xdbg_head));
	CHECK(sizeof(d2xdbg_head) <= D2XDBG_RING_OFF, "head fits before the ring");
	CHECK(D2XDBG_RING_OFF + D2XDBG_RECORDS * 16 == D2XDBG_HALF, "ring fills the first half");
	CHECK(D2XDBG_PHYS + D2XDBG_SIZE <= 0x137B5000, "area ends before FAT's code");
	CHECK(D2XDBG_PHYS >= 0x137A9000, "area starts after EHCI's RAM");
}

static void test_start_fresh(void)
{
	fresh();
	CHECK(H->magic == D2XDBG_MAGIC && H->layout == D2XDBG_LAYOUT && H->size == D2XDBG_SIZE, "header");
	CHECK(H->boots == 1, "boots %u", H->boots);
	CHECK(!strcmp(H->build, "-1-0-rc1+dbg@abc1234"), "build '%.31s'", H->build);
	CHECK(H->flags == D2XDBG_FL_DEBUG, "flags");
	CHECK(H->wd_timeout_ms == 0 && H->wd_kick == 0, "watchdog off");
	CHECK(H->mod[1].requests == 0 && H->fault[0].point == 0, "counters and faults zero");
	CHECK(H->trace_head == 1, "one record");
	CHECK(rec(0)[1] == word(D2XDBG_M_MLOAD, D2XDBG_E_BOOT, 0) && rec(0)[2] == 1 && rec(0)[3] == 0,
	      "boot record: boots 1, nothing kept");
}

static void test_build_name_truncated(void)
{
	memset(area, 0, sizeof(area));
	d2xdbg_start(area, "0123456789012345678901234567890123456789", 0, 1);
	CHECK(strlen(H->build) == 31, "31 characters and a NUL, got %zu", strlen(H->build));
}

static void test_start_keeps_last_words(void)
{
	fresh();
	d2xdbg_event(area, 2000, word(D2XDBG_M_EHCI, D2XDBG_E_BEGIN, 0x28), 77, 1);
	d2xdbg_wd_arm(area, 2100, 5000);
	u8 before[D2XDBG_HALF];
	memcpy(before, area, sizeof(before));

	d2xdbg_start(area, "next", 0, 9000);
	CHECK(!memcmp(area + D2XDBG_HALF, before, D2XDBG_HALF), "the last run is kept in the second half");
	CHECK(H->boots == 2, "boots %u", H->boots);
	CHECK(H->mod[D2XDBG_M_EHCI].requests == 0, "counters start again");
	CHECK(H->wd_timeout_ms == 0, "watchdog off after a restart");
	CHECK(H->trace_head == 1 && rec(0)[3] == 1, "boot record says last words kept");
	CHECK(((d2xdbg_head *)(area + D2XDBG_HALF))->wd_timeout_ms == 5000, "last words show the armed watchdog");
}

static void test_wrong_layout_not_kept(void)
{
	fresh();
	H->layout = D2XDBG_LAYOUT + 1;
	memset(area + D2XDBG_HALF, 0, D2XDBG_HALF);
	d2xdbg_start(area, "x", 0, 1);
	CHECK(H->boots == 1, "a different layout starts afresh");
	CHECK(((d2xdbg_head *)(area + D2XDBG_HALF))->magic == 0, "and keeps nothing");
}

static void test_request_counters(void)
{
	d2xdbg_mod *m;

	fresh();
	m = &H->mod[D2XDBG_M_USBS];
	d2xdbg_event(area, 5000, word(D2XDBG_M_USBS, D2XDBG_E_BEGIN, 0x28), 100, 64);
	CHECK(m->requests == 1 && m->cur_start == 5000 && m->cur_cmd == 0x28 && m->cur_arg == 100, "open request");
	d2xdbg_event(area, 8000, word(D2XDBG_M_USBS, D2XDBG_E_END, 0), (u32)-5, 0);
	CHECK(m->errors == 1, "a negative result counts an error");
	CHECK(m->max_ticks == 3000 && m->max_cmd == 0x28 && m->max_arg == 100, "longest request %u", m->max_ticks);
	CHECK(m->cur_start == 0, "closed");

	d2xdbg_event(area, 9000, word(D2XDBG_M_USBS, D2XDBG_E_BEGIN, 0x2A), 200, 1);
	d2xdbg_event(area, 9500, word(D2XDBG_M_USBS, D2XDBG_E_END, 0), 0, 0);
	CHECK(m->requests == 2 && m->errors == 1, "success counts no error");
	CHECK(m->max_ticks == 3000 && m->max_cmd == 0x28, "a shorter request keeps the longest");

	/* An end without a begin: no length */
	d2xdbg_event(area, 20000, word(D2XDBG_M_USBS, D2XDBG_E_END, 0), 0, 0);
	CHECK(m->max_ticks == 3000, "an unmatched end changes nothing");
}

static void test_request_across_timer_wrap(void)
{
	fresh();
	d2xdbg_event(area, 0xFFFFFF00, word(D2XDBG_M_SD, D2XDBG_E_BEGIN, 1), 0, 0);
	d2xdbg_event(area, 0x100, word(D2XDBG_M_SD, D2XDBG_E_END, 1), 0, 0);
	CHECK(H->mod[D2XDBG_M_SD].max_ticks == 0x200, "length across the wrap 0x%x", H->mod[D2XDBG_M_SD].max_ticks);
}

static void test_begin_at_zero(void)
{
	fresh();
	d2xdbg_event(area, 0, word(D2XDBG_M_FAT, D2XDBG_E_BEGIN, 1), 0, 0);
	d2xdbg_event(area, 50, word(D2XDBG_M_FAT, D2XDBG_E_END, 1), 0, 0);
	CHECK(H->mod[D2XDBG_M_FAT].max_ticks >= 49, "a request that began at tick 0 still counts");
}

static void test_other_counters(void)
{
	d2xdbg_mod *m;

	fresh();
	m = &H->mod[D2XDBG_M_EHCI];
	d2xdbg_event(area, 1, word(D2XDBG_M_EHCI, D2XDBG_E_RETRY, 0), 1, 0);
	d2xdbg_event(area, 2, word(D2XDBG_M_EHCI, D2XDBG_E_RESET, 'b'), 0, 0);
	d2xdbg_event(area, 3, word(D2XDBG_M_EHCI, D2XDBG_E_RESET, 'p'), 0, 0);
	d2xdbg_event(area, 4, word(D2XDBG_M_EHCI, D2XDBG_E_TIMEOUT, 0), 0, 0);
	d2xdbg_event(area, 5, word(D2XDBG_M_EHCI, D2XDBG_E_ERROR, 0), 0, 0);
	d2xdbg_event(area, 6, word(D2XDBG_M_SD, D2XDBG_E_SDHS, 0), 0x103, 0);
	d2xdbg_event(area, 7, word(D2XDBG_M_DIP, 'D', 0x7100), 1, 2);	/* trace only */
	CHECK(m->retries == 1 && m->resets == 2 && m->timeouts == 1 && m->errors == 1, "EHCI counters");
	CHECK(H->sd_hs == 0x103, "SD high-speed state mirrored");
	CHECK(H->mod[D2XDBG_M_DIP].requests == 0 && H->mod[D2XDBG_M_DIP].errors == 0, "a trace letter counts nothing");
	CHECK(H->trace_head == 8, "every event traced, %u", H->trace_head);
	CHECK(rec(7)[0] == 7 && rec(7)[1] == word(D2XDBG_M_DIP, 'D', 0x7100) && rec(7)[2] == 1 && rec(7)[3] == 2,
	      "record words");
	CHECK(H->now == 7, "now follows the last event");

	/* A module number past the table is traced, not counted */
	d2xdbg_event(area, 8, word(0xEE, D2XDBG_E_BEGIN, 0), 0, 0);
	CHECK(H->trace_head == 9, "traced");
}

static void test_ring_wraps(void)
{
	u32 i;

	fresh();
	for (i = 1; i < 300; i++)
		d2xdbg_event(area, i, word(D2XDBG_M_FAT, 'x', 0), i, 0);
	CHECK(H->trace_head == 300, "head %u", H->trace_head);
	CHECK(rec(299)[2] == 299 && rec(299)[0] == 299, "newest record in place");
	CHECK(rec(300 - D2XDBG_RECORDS)[2] == 300 - D2XDBG_RECORDS, "oldest kept record");
	/* nothing written past the ring */
	CHECK(area[D2XDBG_HALF] == 0xA5, "the last-words half untouched");
}

static void test_fault_point(void)
{
	u32 in[5] = { 2, (D2XDBG_M_EHCI << 8) | D2XDBG_P_CSW, 2, 1, (D2XDBG_F_LOSE << 24) | 7 };
	u32 head;

	fresh();
	CHECK(d2xdbg_fault_set(area, in) == 0, "set");
	head = H->trace_head;
	CHECK(d2xdbg_point(area, 10, D2XDBG_M_EHCI, D2XDBG_P_DATA, 0) == 0, "another point does not fire");
	CHECK(d2xdbg_point(area, 11, D2XDBG_M_USBS, D2XDBG_P_CSW, 0) == 0, "another module does not fire");
	CHECK(d2xdbg_point(area, 12, D2XDBG_M_EHCI, D2XDBG_P_CSW, 0) == 0, "skip 1: the first pass goes through");
	CHECK(H->trace_head == head, "passes are not traced");
	CHECK(d2xdbg_point(area, 13, D2XDBG_M_EHCI, D2XDBG_P_CSW, 5) == ((D2XDBG_F_LOSE << 24) | 7), "fires");
	CHECK(H->trace_head == head + 1, "a fired fault is traced");
	CHECK(rec(head)[1] == word(D2XDBG_M_EHCI, D2XDBG_E_FAULT, D2XDBG_P_CSW), "fault record");
	CHECK(rec(head)[2] == ((D2XDBG_F_LOSE << 24) | 7) && rec(head)[3] == (2 | (5 << 8)), "action, slot and arg");
	CHECK(d2xdbg_point(area, 14, D2XDBG_M_EHCI, D2XDBG_P_CSW, 0) != 0, "fires a second time");
	CHECK(d2xdbg_point(area, 15, D2XDBG_M_EHCI, D2XDBG_P_CSW, 0) == 0, "count 2: then done");
	CHECK(H->mod[D2XDBG_M_EHCI].faults == 2, "faults counted");
}

static void test_fault_bad_slot(void)
{
	u32 in[5] = { D2XDBG_FAULTS, 1, 1, 0, 1 };

	fresh();
	CHECK(d2xdbg_fault_set(area, in) < 0, "slot past the table refused");
}

static void test_clear(void)
{
	u32 in[5] = { 0, (D2XDBG_M_SD << 8) | D2XDBG_P_REQ, 1, 0, D2XDBG_F_ERROR << 24 };

	fresh();
	d2xdbg_fault_set(area, in);
	d2xdbg_event(area, 1, word(D2XDBG_M_SD, D2XDBG_E_BEGIN, 0), 0, 0);
	d2xdbg_wd_arm(area, 2, 1000);
	d2xdbg_clear(area);
	CHECK(H->trace_head == 0 && rec(0)[1] == 0, "trace cleared");
	CHECK(H->mod[D2XDBG_M_SD].requests == 0 && H->fault[0].point == 0, "counters and faults cleared");
	CHECK(H->magic == D2XDBG_MAGIC && H->boots == 1, "header kept");
	CHECK(H->wd_timeout_ms == 1000, "watchdog left as it is");
}

static void test_ms_to_ticks(void)
{
	CHECK(d2xdbg_ms_to_ticks(1000) == 1898437, "1 s = %u ticks", d2xdbg_ms_to_ticks(1000));
	CHECK(d2xdbg_ms_to_ticks(16) == 30375, "16 ms = 30375 ticks exactly");
	CHECK(d2xdbg_ms_to_ticks(0xFFFFFFFF) == d2xdbg_ms_to_ticks(D2XDBG_WD_MAX_MS), "clamped");
	CHECK(d2xdbg_ms_to_ticks(D2XDBG_WD_MAX_MS) > d2xdbg_ms_to_ticks(D2XDBG_WD_MAX_MS - 1), "no overflow at the limit");
	CHECK(__ticks_to_ms(1898437) >= 998 && __ticks_to_ms(1898437) <= 1000, "back to ms: %u", __ticks_to_ms(1898437));
	CHECK(__ticks_to_ms(0xFFFFFFFF) > 2200000, "no overflow converting back");
}

static void test_watchdog_off(void)
{
	u32 t;

	fresh();
	for (t = 0; t < 100; t++)
		CHECK(d2xdbg_wd_check(area, t * 10000000) == 0, "off never fires");
	CHECK(H->wd_beats == 100, "beats counted");
}

static void test_watchdog_fires(void)
{
	u32 limit = d2xdbg_ms_to_ticks(2000);

	fresh();
	d2xdbg_wd_arm(area, 100000, 2000);
	CHECK(H->wd_timeout_ms == 2000, "armed");
	CHECK(d2xdbg_wd_check(area, 100000 + limit - 1) == 0, "not before the timeout");
	CHECK(H->wd_fired == 0, "not fired");
	CHECK(d2xdbg_wd_check(area, 100000 + limit) == 1, "at the timeout");
	CHECK(H->wd_fired == 100000 + limit, "fired time");
	CHECK(rec(H->trace_head - 1)[1] == word(D2XDBG_M_MLOAD, D2XDBG_E_WDFIRE, 0), "W record");
	CHECK(rec(H->trace_head - 1)[2] >= 1995 && rec(H->trace_head - 1)[2] <= 2000, "ms quiet %u", rec(H->trace_head - 1)[2]);
}

static void test_watchdog_kick(void)
{
	u32 limit = d2xdbg_ms_to_ticks(1000), t = 50;
	int i;

	fresh();
	d2xdbg_wd_arm(area, t, 1000);
	for (i = 0; i < 10; i++) {
		t += limit - 10;
		H->wd_kick++;			/* the PowerPC checks in */
		CHECK(d2xdbg_wd_check(area, t) == 0, "a check-in each period keeps it quiet (%d)", i);
	}
	CHECK(d2xdbg_wd_check(area, t + limit - 1) == 0, "quiet since the last check-in, not long enough");
	CHECK(d2xdbg_wd_check(area, t + limit) == 1, "then fires");
}

static void test_watchdog_arm_ignores_old_kicks(void)
{
	fresh();
	H->wd_kick = 41;	/* check-ins before arming */
	d2xdbg_wd_arm(area, 10, 1000);
	CHECK(H->wd_seen_kick == 41, "arming takes the current count");
	CHECK(d2xdbg_wd_check(area, 10 + d2xdbg_ms_to_ticks(1000)) == 1, "an old count is no check-in");
}

static void test_watchdog_across_timer_wrap(void)
{
	u32 start = 0xFFFFFFFF - 1000;

	fresh();
	d2xdbg_wd_arm(area, start, 1000);
	CHECK(d2xdbg_wd_check(area, 5000) == 0, "after the wrap, not yet");
	CHECK(d2xdbg_wd_check(area, start + d2xdbg_ms_to_ticks(1000)) == 1, "fires on time across the wrap");
}

static void test_watchdog_disarm(void)
{
	fresh();
	d2xdbg_wd_arm(area, 0, 1000);
	d2xdbg_wd_arm(area, 0, 0);
	CHECK(d2xdbg_wd_check(area, 0xF0000000) == 0, "disarmed");
}

static void test_watchdog_arm_clamped(void)
{
	fresh();
	d2xdbg_wd_arm(area, 0, 0xFFFFFFFF);
	CHECK(H->wd_timeout_ms == D2XDBG_WD_MAX_MS, "clamped to %u", D2XDBG_WD_MAX_MS);
}

/* A reset that did not take: recorded, and the watchdog is off so it
   cannot fire again on every check */
static void test_watchdog_noreset(void)
{
	u32 limit = d2xdbg_ms_to_ticks(1000);

	fresh();
	d2xdbg_wd_arm(area, 0, 1000);
	CHECK(d2xdbg_wd_check(area, limit) == 1, "fires");
	d2xdbg_wd_noreset(area, limit + 500);
	CHECK(H->wd_noreset == limit + 500, "noreset time");
	CHECK(H->wd_fired == limit, "fired time kept");
	CHECK(H->wd_timeout_ms == 0, "off again");
	CHECK(rec(H->trace_head - 1)[1] == word(D2XDBG_M_MLOAD, D2XDBG_E_NORESET, 0), "N record");
	CHECK(d2xdbg_wd_check(area, 10 * limit) == 0, "does not fire again");
}

static const struct hc_test tests[] = {
	{ "layout", test_layout, false },
	{ "start_fresh", test_start_fresh, false },
	{ "build_name_truncated", test_build_name_truncated, false },
	{ "start_keeps_last_words", test_start_keeps_last_words, false },
	{ "wrong_layout_not_kept", test_wrong_layout_not_kept, false },
	{ "request_counters", test_request_counters, false },
	{ "request_across_timer_wrap", test_request_across_timer_wrap, false },
	{ "begin_at_zero", test_begin_at_zero, false },
	{ "other_counters", test_other_counters, false },
	{ "ring_wraps", test_ring_wraps, false },
	{ "fault_point", test_fault_point, false },
	{ "fault_bad_slot", test_fault_bad_slot, false },
	{ "clear", test_clear, false },
	{ "ms_to_ticks", test_ms_to_ticks, false },
	{ "watchdog_off", test_watchdog_off, false },
	{ "watchdog_fires", test_watchdog_fires, false },
	{ "watchdog_kick", test_watchdog_kick, false },
	{ "watchdog_arm_ignores_old_kicks", test_watchdog_arm_ignores_old_kicks, false },
	{ "watchdog_across_timer_wrap", test_watchdog_across_timer_wrap, false },
	{ "watchdog_disarm", test_watchdog_disarm, false },
	{ "watchdog_arm_clamped", test_watchdog_arm_clamped, false },
	{ "watchdog_noreset", test_watchdog_noreset, false },
};

int main(int argc, char **argv)
{
	return hc_main(tests, HC_COUNT(tests), argc, argv, NULL);
}
