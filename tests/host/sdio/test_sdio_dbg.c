/*
 * Host tests of cios-lib/sdio.c's debug hooks (-DD2X_DEBUG, d2xdbg.h): the
 * events a request leaves, and the faults /dev/d2xdbg can inject at an SD
 * request (D2XDBG_P_REQ). The real sdio.c (with SDIO_HIGH_SPEED) runs against
 * a small fake of IOS's /dev/sdio/slot0 driver (a reduced copy of the one in
 * test_sdio.c); the debug area runs on the PC (tests/host/dbg/swi_host.c).
 * Run with `make -C tests/host`.
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
#include "dbg_host.h"

#define SD D2XDBG_M_SD

/* ---- the fake IOS SD driver ---- */
static int sdhc_card = 1;       /* the status says SDHC (block addressed); 0: byte addressed */
static int fail_hs;             /* transfers fail while the clock is high speed */
static u32 hc[0x40], clk_div;
static int hs_xfers;            /* transfers that ran with the clock at high speed */
static unsigned n_rd, n_wr;     /* block commands that reached the driver */
static u32 last_arg;            /* the argument (sector or byte address) of the last one */

static const u8 hs_status[64] = { [13] = 0x03, [16] = 0x01 };

/* One SD command, as IOS's driver would run it */
static s32 do_cmd(struct _sdiorequest *r, void *buf, struct _sdioresponse *p)
{
	memset(p, 0, sizeof(*p));

	switch (r->cmd) {
	case SDHS_CMD6:                 /* also ACMD6, set bus width: no data block */
		if (!buf)
			return 0;
		memcpy(buf, hs_status, 64);
		return 0;
	case SDIO_CMD_READMULTIBLOCK:
	case SDIO_CMD_WRITEMULTIBLOCK:
		if (r->cmd == SDIO_CMD_READMULTIBLOCK)
			n_rd++;
		else
			n_wr++;
		last_arg = r->arg;
		if (fail_hs && clk_div == 0)
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
		*(u32 *)out = hc[q[0]] | (q[0] == 0x2c ? 2 : 0);   /* clock stable */
		return 0;
	case IOCTL_SDIO_WRITEHCREG:
		hc[q[0]] = q[4];
		if (q[0] == 0x2f)
			hc[0x2f] = 0;                                /* reset done */
		return 0;
	case IOCTL_SDIO_SETCLK:
		clk_div = *(u32 *)in;
		return 0;
	case IOCTL_SDIO_RESETCARD:
		hc[0x28] = 0;
		clk_div = 0x80;
		*(u32 *)out = 0x12340000;
		return 0;
	case IOCTL_SDIO_GETSTATUS:      /* inserted, initialized, SDHC or not */
		*(u32 *)out = 1 | 0x10000 | (sdhc_card ? 0x100000 : 0);
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
static u8 sector_buf[512 * 2] __attribute__((aligned(32)));

/* A started card with a fresh debug area. cards: sdhc or byte addressed. */
static void start(int sdhc)
{
	sdhc_card = sdhc;
	fail_hs = hs_xfers = 0;
	n_rd = n_wr = last_arg = 0;
	memset(&__sd0_hs_rec, 0, sizeof(__sd0_hs_rec));
	memset(hc, 0, sizeof(hc));
	clk_div = 0x80;
	__sd0_fd = -1;
	__sd0_initialized = __sdio_initialized = 0;
	__sd0_hs_on = __sd0_hs_dead = 0;
	dbg_host_reset();	/* no fault of the last test; count from here */
	if (!sdio_Startup())
		hc_hang("startup failed");
}

static void test_read_counts_a_request(void)
{
	d2xdbg_mod *m = DBG_MOD(SD);

	start(1);
	CHECK(DBG_HEAD->sd_hs == 1 && sdio_HighSpeedState() == 1, "high speed on after startup: %u", DBG_HEAD->sd_hs);
	CHECK(sdio_ReadSectors(7, 2, sector_buf), "read failed");
	CHECK(m->requests == 1 && m->errors == 0 && m->retries == 0, "counters %u/%u/%u", m->requests, m->errors,
	      m->retries);
	CHECK(m->max_cmd == 0x28 && m->max_arg == 7, "longest: cmd %x arg %u", m->max_cmd, m->max_arg);
	CHECK(dbg_host_count(SD, D2XDBG_E_BEGIN) == 1 && dbg_host_count(SD, D2XDBG_E_END) == 1, "B and E traced");
	CHECK(dbg_host_count(SD, D2XDBG_E_RETRY) == 0 && dbg_host_count(SD, D2XDBG_E_FAULT) == 0, "no retry or fault");
	CHECK(n_rd == 1 && last_arg == 7 && hs_xfers == 1, "one read of sector 7 at high speed: %u %u %d", n_rd,
	      (unsigned)last_arg, hs_xfers);
	CHECK(DBG_HEAD->sd_hs == 1, "still at high speed");
}

static void test_write_counts_a_request(void)
{
	d2xdbg_mod *m = DBG_MOD(SD);

	start(1);
	CHECK(sdio_WriteSectors(9, 1, sector_buf), "write failed");
	CHECK(m->requests == 1 && m->errors == 0 && m->max_cmd == 0x2A && m->max_arg == 9, "counters %u/%u cmd %x arg %u",
	      m->requests, m->errors, m->max_cmd, m->max_arg);
	CHECK(n_wr == 1, "one write command");
}

static void test_error_fault_once_retries(void)
{
	d2xdbg_mod *m = DBG_MOD(SD);

	start(1);
	dbg_host_fault(0, SD, D2XDBG_P_REQ, 1, 0, D2XDBG_F_ERROR << 24);
	CHECK(sdio_ReadSectors(3, 1, sector_buf), "the retry should succeed");
	CHECK(m->requests == 1 && m->retries == 1 && m->errors == 0, "counters %u/%u/%u", m->requests, m->retries,
	      m->errors);
	CHECK(dbg_host_count(SD, D2XDBG_E_RETRY) == 1 && dbg_host_count(SD, D2XDBG_E_END) == 1, "R and E traced");
	CHECK(m->faults == 1 && dbg_host_count(SD, D2XDBG_E_FAULT) == 1, "the fault counted and traced");
	CHECK(n_rd == 1, "the faulted attempt sent no command: %u", n_rd);
	/* a failed attempt always drops to default speed (sdio.c), debug or not */
	CHECK(DBG_HEAD->sd_hs == 2 && sdio_HighSpeedState() == 2, "fallback after the failed attempt: %u", DBG_HEAD->sd_hs);
	CHECK(hs_xfers == 0, "the retry ran at default speed");
}

static void test_sdlow_fault_falls_back_and_reads(void)
{
	d2xdbg_mod *m = DBG_MOD(SD);

	start(1);
	dbg_host_fault(0, SD, D2XDBG_P_REQ, 1, 0, D2XDBG_F_SDLOW << 24);
	memset(sector_buf, 0, sizeof(sector_buf));
	CHECK(sdio_ReadSectors(4, 2, sector_buf), "read failed");
	CHECK(DBG_HEAD->sd_hs == 2 && sdio_HighSpeedState() == 2, "sd_hs shows the fallback: %u", DBG_HEAD->sd_hs);
	CHECK(dbg_host_count(SD, D2XDBG_E_SDHS) == 2, "the startup state and the fallback traced");
	CHECK(n_rd == 1 && last_arg == 4 && hs_xfers == 0, "the transfer went on at default speed: %u %u %d", n_rd,
	      (unsigned)last_arg, hs_xfers);
	CHECK(m->retries == 0 && m->errors == 0 && m->requests == 1, "no retry: it is no failure: %u/%u/%u", m->requests,
	      m->retries, m->errors);
	CHECK(m->faults == 1, "fired once");
}

static void test_sdlow_fault_without_high_speed_does_nothing(void)
{
	start(1);
	sdio_HighSpeedForceFallback();	/* already at default speed, as on a card without it */
	dbg_host_reset();
	dbg_host_fault(0, SD, D2XDBG_P_REQ, 1, 0, D2XDBG_F_SDLOW << 24);
	CHECK(sdio_ReadSectors(4, 1, sector_buf), "read failed");
	CHECK(dbg_host_count(SD, D2XDBG_E_SDHS) == 0, "no second fallback traced");
	CHECK(n_rd == 1 && DBG_MOD(SD)->retries == 0, "read once, no retry");
}

static void test_error_fault_every_time_fails(void)
{
	d2xdbg_mod *m = DBG_MOD(SD);

	start(1);
	dbg_host_fault(0, SD, D2XDBG_P_REQ, 1000000, 0, D2XDBG_F_ERROR << 24);
	CHECK(!sdio_ReadSectors(3, 1, sector_buf), "a read that always fails succeeded");
	CHECK(n_rd == 0, "no command got through");
	CHECK(m->requests == 1 && m->errors == 1 && m->retries == 10, "counters %u/%u/%u (want 1/1/10)", m->requests,
	      m->errors, m->retries);
	CHECK(dbg_host_count(SD, D2XDBG_E_END) == 1 && m->cur_start == 0, "E traced, request closed");
	CHECK(m->faults == 10, "ten attempts, ten faults: %u", m->faults);
	dbg_host_fault(0, 0, 0, 0, 0, 0);
	CHECK(sdio_ReadSectors(3, 1, sector_buf), "the card does not read once the fault is cleared");
}

/* The tenth attempt is the last: nine failures still end in success */
static void test_error_fault_nine_times_succeeds(void)
{
	d2xdbg_mod *m = DBG_MOD(SD);

	start(1);
	dbg_host_fault(0, SD, D2XDBG_P_REQ, 9, 0, D2XDBG_F_ERROR << 24);
	CHECK(sdio_WriteSectors(3, 1, sector_buf), "the tenth attempt should succeed");
	CHECK(m->retries == 9 && m->errors == 0 && n_wr == 1, "counters %u/%u, %u writes", m->retries, m->errors, n_wr);
}

static void test_real_failure_is_counted_like_a_fault(void)
{
	d2xdbg_mod *m = DBG_MOD(SD);

	start(1);
	fail_hs = 1;
	CHECK(sdio_ReadSectors(3, 1, sector_buf), "the retry at default speed should succeed");
	CHECK(m->retries == 1 && m->errors == 0 && n_rd == 2, "counters %u/%u, %u commands", m->retries, m->errors, n_rd);
	CHECK(DBG_HEAD->sd_hs == 2, "fallback traced");
}

/* Upstream bug, fixed (sdio.c, sdio_ReadSectors and sdio_WriteSectors): on a
 * byte addressed (not SDHC) card `sector *= PAGE_SIZE512` was inside the retry
 * loop, so a retry multiplied the address again and read or wrote the wrong
 * place. A retry must send the same address. */
static void test_nonsdhc_read_retry_keeps_address(void)
{
	u32 want;

	start(0);
	CHECK(sdio_ReadSectors(5, 1, sector_buf), "read failed");
	want = last_arg;
	CHECK(want == 5 * 512, "an untouched read sends the byte address: %u", (unsigned)want);

	dbg_host_fault(0, SD, D2XDBG_P_REQ, 1, 0, D2XDBG_F_ERROR << 24);
	CHECK(sdio_ReadSectors(5, 1, sector_buf), "the retry should succeed");
	CHECK(last_arg == want, "the retry sent byte address %u, not %u", (unsigned)last_arg, (unsigned)want);
}

static void test_nonsdhc_write_retry_keeps_address(void)
{
	u32 want;

	start(0);
	CHECK(sdio_WriteSectors(5, 1, sector_buf), "write failed");
	want = last_arg;
	CHECK(want == 5 * 512, "an untouched write sends the byte address: %u", (unsigned)want);

	dbg_host_fault(0, SD, D2XDBG_P_REQ, 1, 0, D2XDBG_F_ERROR << 24);
	CHECK(sdio_WriteSectors(5, 1, sector_buf), "the retry should succeed");
	CHECK(last_arg == want, "the retry sent byte address %u, not %u", (unsigned)last_arg, (unsigned)want);
}

static const struct hc_test tests[] = {
	{ "sd_dbg_read_counts_a_request", test_read_counts_a_request, false },
	{ "sd_dbg_write_counts_a_request", test_write_counts_a_request, false },
	{ "sd_dbg_error_fault_once_retries", test_error_fault_once_retries, false },
	{ "sd_dbg_sdlow_fault_falls_back_and_reads", test_sdlow_fault_falls_back_and_reads, false },
	{ "sd_dbg_sdlow_fault_without_high_speed", test_sdlow_fault_without_high_speed_does_nothing, false },
	{ "sd_dbg_error_fault_every_time_fails", test_error_fault_every_time_fails, false },
	{ "sd_dbg_error_fault_nine_times_succeeds", test_error_fault_nine_times_succeeds, false },
	{ "sd_dbg_real_failure_counted_like_a_fault", test_real_failure_is_counted_like_a_fault, false },
	{ "sd_dbg_nonsdhc_read_retry_keeps_address", test_nonsdhc_read_retry_keeps_address, false },
	{ "sd_dbg_nonsdhc_write_retry_keeps_address", test_nonsdhc_write_retry_keeps_address, false },
};

int main(int argc, char **argv)
{
	return hc_main(tests, HC_COUNT(tests), argc, argv, NULL);
}
