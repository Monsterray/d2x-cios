/*
 * Host tests of cios-lib/sdio.c's SD high speed (SDIO_HIGH_SPEED): the real
 * sdio.c runs against a fake of IOS's /dev/sdio/slot0 driver that logs the
 * ioctls that matter (reset, host control 0x28 writes, clock, CMD6, block
 * transfers) as one string per sequence. Run with `make -C tests/host`.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOSTCHECK_MAIN
#include "hostcheck.h"

/* cios-lib's types.h typedefs bool itself */
#undef bool
#undef true
#undef false

#include "sdio.c"       /* the unit under test; its statics are reset below */

/* ---- the fake IOS SD driver ---- */
enum card { HS_CARD, NO_HS_CARD, CMD6_FAILS, SWITCH_NOT_CHOSEN };

static enum card card;
static int fresh;               /* status says "not initialized": full init */
static int fail_hs;             /* transfers fail while the clock is high speed */
static int fail_all;            /* transfers always fail */
static u32 hc[0x40], clk_div;
static char log_[160];
static int bad_cmd6;            /* CMD6 sent other than as cmd_type BC, isdma, one aligned 64-byte block */
static int hs_xfers;            /* transfers that ran with the clock at high speed */
static int fail_hcr_read;       /* host control read fails after the CMD6 switch */
static int fail_hcr_write;      /* host control write fails after the CMD6 switch */
static int fail_setclk0;        /* SETCLK(0), the high speed clock, fails */
static int fail_check_rc, fail_switch_rc;   /* CMD6 check / switch fail with this rc */
static int after_switch;        /* the CMD6 switch ran */

static const u8 hs_status[64] = { [13] = 0x03, [16] = 0x01 };

static void lg(const char *fmt, ...)
{
	char t[32];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(t, sizeof(t), fmt, ap);
	va_end(ap);
	if (log_[0])
		strcat(log_, " ");
	strcat(log_, t);
}

/* One SD command, as IOS's driver would run it */
static s32 do_cmd(struct _sdiorequest *r, void *buf, struct _sdioresponse *p)
{
	memset(p, 0, sizeof(*p));

	switch (r->cmd) {
	case SDIO_CMD_SENDIFCOND:
		p->rsp_fields[0] = 0xaa;
		return 0;
	case SDIO_ACMD_SENDOPCOND:
		p->rsp_fields[0] = 0xc0000000;
		return 0;
	case SDHS_CMD6:                 /* also ACMD6, set bus width: no data block */
		if (!buf)
			return 0;
		if (r->cmd_type != SDIOCMD_TYPE_BC || r->blk_cnt != 1 || r->blk_size != 64 ||
		    ((unsigned long)buf & 31))
			bad_cmd6++;
		lg(r->arg == SDHS_CMD6_CHECK ? "C6c" : "C6s");
		if (r->arg == SDHS_CMD6_SWITCH)
			after_switch = 1;
		if (fail_check_rc && r->arg == SDHS_CMD6_CHECK)
			return fail_check_rc;
		if (fail_switch_rc && r->arg == SDHS_CMD6_SWITCH)
			return fail_switch_rc;
		if (card == CMD6_FAILS)
			return -1;
		/* Real IOS (dev Wii, 2026-11): cmd_type ADTC is accepted with rc 0
		 * but the data stage never lands; the buffer stays untouched. */
		if (r->cmd_type == SDIOCMD_TYPE_ADTC)
			return 0;
		if (card == HS_CARD || card == SWITCH_NOT_CHOSEN)
			memcpy(buf, hs_status, 64);
		if (card == SWITCH_NOT_CHOSEN)
			((u8 *)buf)[16] = 0x0f;
		return 0;
	case SDIO_CMD_READMULTIBLOCK:
	case SDIO_CMD_WRITEMULTIBLOCK:
		lg(r->cmd == SDIO_CMD_READMULTIBLOCK ? "RD" : "WR");
		if (fail_all || (fail_hs && clk_div == 0))
			return -1;
		if (clk_div == 0 && (hc[0x28] & 4))
			hs_xfers++;
		return 0;
	}
	return 0;
}

/* sdio.c includes cios-lib's own syscalls.h and timer.h: provide the calls */
void __os_sync_before_read(void *p, s32 n) {}
void __os_sync_after_write(void *p, s32 n) {}
void Timer_Sleep(u32 us) {}

s32 os_open(const char *d, s32 m) { return 3; }
s32 os_close(s32 fd) { return 0; }

s32 os_ioctl(s32 fd, s32 req, void *in, s32 bin, void *out, s32 bout)
{
	u32 *q = in;

	switch (req) {
	case IOCTL_SDIO_READHCREG:
		if (fail_hcr_read && after_switch && q[0] == 0x28)
			return -7;
		*(u32 *)out = hc[q[0]] | (q[0] == 0x2c ? 2 : 0);   /* clock stable */
		return 0;
	case IOCTL_SDIO_WRITEHCREG:
		if (fail_hcr_write && after_switch && q[0] == 0x28)
			return -8;
		hc[q[0]] = q[4];
		if (q[0] == 0x2f)
			hc[0x2f] = 0;                                /* reset done */
		if (q[0] == 0x28)
			lg("H28=%02x", q[4]);
		return 0;
	case IOCTL_SDIO_SETCLK:
		lg("CLK%u", *(u32 *)in);
		if (fail_setclk0 && *(u32 *)in == 0)
			return -9;
		clk_div = *(u32 *)in;
		return 0;
	case IOCTL_SDIO_RESETCARD:
		hc[0x28] = 0;
		clk_div = 0x80;
		lg("RST");
		*(u32 *)out = 0x12340000;
		return 0;
	case IOCTL_SDIO_GETSTATUS:
		*(u32 *)out = 1 | (fresh ? 0 : 0x10000) | 0x100000;
		return 0;
	case IOCTL_SDIO_SENDCMD:        /* a command without data */
		return do_cmd(in, NULL, out);
	}
	return 0;
}

s32 os_ioctlv(s32 fd, s32 req, s32 bin, s32 bout, ioctlv *v)
{
	struct _sdiorequest *r = v[0].data;

	/* data arrives through the DMA vector only when isdma is set */
	return do_cmd(r, r->isdma ? v[1].data : NULL, v[2].data);
}

/* ---- tests ---- */
static void reset_all(enum card c)
{
	card = c;
	bad_cmd6 = fresh = fail_hs = fail_all = hs_xfers = 0;
	fail_hcr_read = fail_hcr_write = fail_setclk0 = fail_check_rc = fail_switch_rc = 0;
	after_switch = 0;
	memset(&__sd0_hs_rec, 0, sizeof(__sd0_hs_rec));
	memset(hc, 0, sizeof(hc));
	clk_div = 0x80;
	log_[0] = 0;
	__sd0_fd = -1;
	__sd0_initialized = __sdio_initialized = 0;
	__sd0_hs_on = __sd0_hs_dead = 0;
}

static u8 sector_buf[512 * 2] __attribute__((aligned(32)));

static void test_parse_status(void)
{
	u8 st[64];

	memset(st, 0, sizeof(st));
	CHECK(!sdhs_group1_supported(st), "all zero: not supported (SD 1.0 style)");
	CHECK(!sdhs_group1_selected(st), "all zero: not selected");
	st[13] = 0x01;
	CHECK(!sdhs_group1_supported(st), "function 0 only: not supported");
	st[13] = 0x02;
	CHECK(sdhs_group1_supported(st), "function 1 bit: supported");
	st[13] = 0xfd;
	CHECK(!sdhs_group1_supported(st), "every bit but function 1: not supported");
	st[12] = 0xff; st[14] = 0xff;
	st[13] = 0;
	CHECK(!sdhs_group1_supported(st), "neighbouring bytes do not count");
	st[16] = 0x01;
	CHECK(sdhs_group1_selected(st), "selected 1");
	st[16] = 0x0f;
	CHECK(!sdhs_group1_selected(st), "selected 0xF = switch failed");
	st[16] = 0x00;
	CHECK(!sdhs_group1_selected(st), "selected 0 = default speed kept");
	st[16] = 0xf1;
	CHECK(sdhs_group1_selected(st), "high nibble is group 2: ignored");
	CHECK(sdhs_hostctl_enable(0x02) == 0x06, "enable keeps 4-bit");
	CHECK(sdhs_hostctl_disable(0x06) == 0x02, "disable keeps 4-bit");
	CHECK(sdhs_hostctl_disable(0x04) == 0x00, "disable from HS only");
	CHECK(SDHS_CMD6_CHECK == 0x00FFFFF1 && SDHS_CMD6_SWITCH == 0x80FFFFF1, "CMD6 args");
}

static void test_hs_card(void)
{
	reset_all(HS_CARD);
	CHECK(sdio_Startup(), "startup");
	CHECK(!strcmp(log_, "RST H28=02 CLK1 C6c C6s H28=06 CLK0"), "sequence: %s", log_);
	CHECK(sdio_HighSpeedState() == 1, "active");
	CHECK(sdio_ReadSectors(0, 2, sector_buf), "read");
	CHECK(sdio_WriteSectors(0, 2, sector_buf), "write");
	CHECK(hs_xfers == 2, "both transfers at high speed: %d", hs_xfers);
	CHECK(bad_cmd6 == 0, "CMD6 is a BC with one 64-byte aligned DMA block");
}

/* A card that answers CMD6 with a status that has no high speed bit: also
 * what a card that ignores CMD6 (SD 1.0) leaves in the zeroed buffer. */
static void test_non_hs_card(void)
{
	reset_all(NO_HS_CARD);
	CHECK(sdio_Startup(), "startup");
	CHECK(!strcmp(log_, "RST H28=02 CLK1 C6c"), "check only: %s", log_);
	CHECK(sdio_HighSpeedState() == 0, "not active, not dead");
	CHECK(sdio_ReadSectors(0, 1, sector_buf), "read at default");
	CHECK(hs_xfers == 0, "default speed");
}

static void test_cmd6_fails(void)
{
	reset_all(CMD6_FAILS);
	CHECK(sdio_Startup(), "a failing CMD6 is not an error");
	CHECK(!strcmp(log_, "RST H28=02 CLK1 C6c"), "%s", log_);
	CHECK(sdio_HighSpeedState() == 0, "default speed");
	CHECK(sdio_ReadSectors(0, 1, sector_buf), "reads work");
}

static void test_switch_not_chosen(void)
{
	reset_all(SWITCH_NOT_CHOSEN);
	CHECK(sdio_Startup(), "startup");
	CHECK(!strcmp(log_, "RST H28=02 CLK1 C6c C6s"), "no host change: %s", log_);
	CHECK(sdio_HighSpeedState() == 0, "default speed");
}

static void test_read_error_falls_back(void)
{
	reset_all(HS_CARD);
	CHECK(sdio_Startup(), "startup");
	log_[0] = 0;
	fail_hs = 1;
	CHECK(sdio_ReadSectors(0, 1, sector_buf), "the retry succeeds");
	CHECK(!strcmp(log_, "RD H28=02 CLK1 RD"), "fallback then retry: %s", log_);
	CHECK(sdio_HighSpeedState() == 2, "off and dead: %u", sdio_HighSpeedState());
	CHECK(hs_xfers == 0, "no transfer at high speed succeeded");
	log_[0] = 0;
	CHECK(sdio_WriteSectors(0, 1, sector_buf), "write at default");
	CHECK(!strcmp(log_, "WR"), "no more switching: %s", log_);
}

static void test_write_error_falls_back(void)
{
	reset_all(HS_CARD);
	CHECK(sdio_Startup(), "startup");
	log_[0] = 0;
	fail_hs = 1;
	CHECK(sdio_WriteSectors(0, 1, sector_buf), "the retry succeeds");
	CHECK(!strcmp(log_, "WR H28=02 CLK1 WR"), "%s", log_);
}

static void test_failure_at_default_speed_is_not_fallback(void)
{
	reset_all(NO_HS_CARD);
	CHECK(sdio_Startup(), "startup");
	fail_all = 1;
	CHECK(!sdio_ReadSectors(0, 1, sector_buf), "a dead card fails");
	CHECK(sdio_HighSpeedState() == 0, "never at high speed, so not marked dead");
}

static void test_reinit_reapplies(void)
{
	reset_all(HS_CARD);
	CHECK(sdio_Startup(), "startup");
	CHECK(sdio_Shutdown(), "shutdown");
	log_[0] = 0;
	CHECK(sdio_Startup(), "startup again");
	CHECK(!strcmp(log_, "RST H28=02 CLK1 C6c C6s H28=06 CLK0"), "re-applied: %s", log_);
	CHECK(sdio_HighSpeedState() == 1, "active");
}

static void test_reinit_after_fallback_stays_default(void)
{
	reset_all(HS_CARD);
	CHECK(sdio_Startup(), "startup");
	fail_hs = 1;
	CHECK(sdio_ReadSectors(0, 1, sector_buf), "read");
	CHECK(sdio_Shutdown(), "shutdown");
	log_[0] = 0;
	CHECK(sdio_Startup(), "startup again");
	CHECK(!strcmp(log_, "RST H28=02 CLK1"), "no CMD6 after a fallback: %s", log_);
	CHECK(sdio_HighSpeedState() == 2, "still dead");
}

static void test_fresh_card_init(void)
{
	reset_all(HS_CARD);
	fresh = 1;
	CHECK(sdio_Startup(), "startup of a card IOS has not initialized");
	CHECK(strstr(log_, "C6c C6s H28=06 CLK0") != NULL, "HS after full init: %s", log_);
	CHECK(sdio_HighSpeedState() == 1, "active");
}

static void test_force_fallback(void)
{
	reset_all(HS_CARD);
	CHECK(sdio_Startup(), "startup");
	log_[0] = 0;
	sdio_HighSpeedForceFallback();
	CHECK(!strcmp(log_, "H28=02 CLK1"), "%s", log_);
	CHECK(sdio_HighSpeedState() == 2, "state");
	sdio_HighSpeedForceFallback();
	CHECK(!strcmp(log_, "H28=02 CLK1"), "second call does nothing: %s", log_);
}

/* The record behind ioctlv 0x22: where the last __sd0_hs_try() stopped */
static struct sdhs_record rec(void)
{
	struct sdhs_record r;

	sdio_HighSpeedRecord(&r);
	return r;
}

static void test_record_layout(void)
{
	/* the host's u32 may be 8 bytes; the layout is only the cIOS's when it is 4 */
	if (sizeof(u32) == 4 && sizeof(s32) == 4) {
		CHECK(sizeof(struct sdhs_record) == 32, "record is 32 bytes: %u", (unsigned)sizeof(struct sdhs_record));
		CHECK(offsetof(struct sdhs_record, step) == 4 && offsetof(struct sdhs_record, rc) == 8 &&
		      offsetof(struct sdhs_record, status) == 12, "field offsets");
	}
	CHECK(offsetof(struct sdhs_record, status) + SDHS_RECORD_STATUS_LEN == sizeof(struct sdhs_record),
	      "status is the last field, no padding");
	reset_all(HS_CARD);
	CHECK(rec().step == SDHS_STEP_NOT_CALLED && rec().calls == 0, "before any call");
}

static void test_record_on(void)
{
	struct sdhs_record r;

	reset_all(HS_CARD);
	CHECK(sdio_Startup(), "startup");
	r = rec();
	CHECK(r.calls == 1 && r.step == SDHS_STEP_ON && r.rc == 0, "on: %d %d %d", (int)r.calls, (int)r.step, (int)r.rc);
	CHECK(r.status[13] == 0x03 && r.status[16] == 0x01, "switch status kept");
	sdio_Shutdown();
	CHECK(sdio_Startup(), "again");
	CHECK(rec().calls == 2, "counts calls across re-inits");
}

static void test_record_check_rc(void)
{
	struct sdhs_record r;

	reset_all(HS_CARD);
	fail_check_rc = -4;
	CHECK(sdio_Startup(), "startup");
	r = rec();
	CHECK(r.step == SDHS_STEP_CHECK_RC && r.rc == -4 && r.calls == 1, "check rc: %d %d", (int)r.step, (int)r.rc);
}

static void test_record_check_unsupported(void)
{
	struct sdhs_record r;

	reset_all(NO_HS_CARD);
	CHECK(sdio_Startup(), "startup");
	r = rec();
	CHECK(r.step == SDHS_STEP_CHECK_UNSUPPORTED && r.rc == 0, "unsupported: %d", (int)r.step);
	CHECK(r.status[13] == 0, "zero status recorded");
}

static void test_record_switch_rc(void)
{
	struct sdhs_record r;

	reset_all(HS_CARD);
	fail_switch_rc = -6;
	CHECK(sdio_Startup(), "startup");
	r = rec();
	CHECK(r.step == SDHS_STEP_SWITCH_RC && r.rc == -6, "switch rc: %d %d", (int)r.step, (int)r.rc);
	CHECK(r.status[13] == 0, "status of the failed switch (zeroed buffer)");
}

static void test_record_switch_not_selected(void)
{
	struct sdhs_record r;

	reset_all(SWITCH_NOT_CHOSEN);
	CHECK(sdio_Startup(), "startup");
	r = rec();
	CHECK(r.step == SDHS_STEP_SWITCH_NOT_SELECTED && r.rc == 0, "not selected: %d", (int)r.step);
	CHECK(r.status[13] == 0x03 && r.status[16] == 0x0f, "the answer is kept: %02x %02x", r.status[13], r.status[16]);
}

static void test_record_hostctl_and_clock(void)
{
	struct sdhs_record r;

	reset_all(HS_CARD);
	fail_hcr_read = 1;
	CHECK(sdio_Startup(), "startup");
	r = rec();
	CHECK(r.step == SDHS_STEP_HOSTCTL_READ && r.rc == -7, "hostctl read: %d %d", (int)r.step, (int)r.rc);
	CHECK(sdio_HighSpeedState() == 0, "not on");

	reset_all(HS_CARD);
	fail_hcr_write = 1;
	CHECK(sdio_Startup(), "startup");
	r = rec();
	CHECK(r.step == SDHS_STEP_HOSTCTL_WRITE && r.rc == -8, "hostctl write: %d %d", (int)r.step, (int)r.rc);
	CHECK(sdio_HighSpeedState() == 0, "not on");

	reset_all(HS_CARD);
	fail_setclk0 = 1;
	CHECK(sdio_Startup(), "startup");
	r = rec();
	CHECK(r.step == SDHS_STEP_SETCLK && r.rc == -9, "setclk: %d %d", (int)r.step, (int)r.rc);
	CHECK(sdio_HighSpeedState() == 2, "fell back, dead");
}

static void test_record_dead(void)
{
	struct sdhs_record r;

	reset_all(HS_CARD);
	CHECK(sdio_Startup(), "startup");
	fail_hs = 1;
	CHECK(sdio_ReadSectors(0, 1, sector_buf), "read");
	CHECK(sdio_Shutdown(), "shutdown");
	CHECK(sdio_Startup(), "startup again");
	r = rec();
	CHECK(r.step == SDHS_STEP_DEAD && r.calls == 2, "dead: %d calls %d", (int)r.step, (int)r.calls);
	CHECK(r.status[16] == 0x01, "status of the earlier switch stays");
}

static const struct hc_test tests[] = {
	{ "record_layout", test_record_layout, false },
	{ "record_on", test_record_on, false },
	{ "record_check_rc", test_record_check_rc, false },
	{ "record_check_unsupported", test_record_check_unsupported, false },
	{ "record_switch_rc", test_record_switch_rc, false },
	{ "record_switch_not_selected", test_record_switch_not_selected, false },
	{ "record_hostctl_and_clock", test_record_hostctl_and_clock, false },
	{ "record_dead", test_record_dead, false },
	{ "parse_status", test_parse_status, false },
	{ "hs_card", test_hs_card, false },
	{ "non_hs_card", test_non_hs_card, false },
	{ "cmd6_fails", test_cmd6_fails, false },
	{ "switch_not_chosen", test_switch_not_chosen, false },
	{ "read_error_falls_back", test_read_error_falls_back, false },
	{ "write_error_falls_back", test_write_error_falls_back, false },
	{ "default_speed_failure_not_fallback", test_failure_at_default_speed_is_not_fallback, false },
	{ "reinit_reapplies", test_reinit_reapplies, false },
	{ "reinit_after_fallback", test_reinit_after_fallback_stays_default, false },
	{ "fresh_card_init", test_fresh_card_init, false },
	{ "force_fallback", test_force_fallback, false },
};

int main(int argc, char **argv)
{
	return hc_main(tests, HC_COUNT(tests), argc, argv, NULL);
}
