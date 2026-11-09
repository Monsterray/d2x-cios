/*
 * Host tests of the USBS module's debug hooks (-DD2X_DEBUG, d2xdbg.h): the
 * counters and trace a request leaves, and each fault /dev/d2xdbg can inject
 * at the CBW, data and CSW points, against the simulated drive. The driver
 * must recover from each the way it does from the real fault. The debug area
 * runs on the PC (tests/host/dbg/swi_host.c). Run with `make -C tests/host`.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include "sim.h"
#define HOSTCHECK_MAIN
#include "hostcheck.h"
#include "dbg_host.h"

#define SECTOR SIM_SECTOR_SIZE
#define USBS D2XDBG_M_USBS
#define REQUEST_MS 8000         /* the driver's USBSTORAGE_REQUEST_MS */

int usbs_mount(void);
int usbs_read(unsigned int sector, unsigned int n, void *buf);
int usbs_write(unsigned int sector, unsigned int n, const void *buf);
void usbs_close(void);

/* glue_sim.c: the data transfers of a request, and which buffers are direct */
extern int glue_mem2;
extern unsigned glue_data_xfers;
extern unsigned glue_data_len[64];
extern unsigned glue_short_next;

static unsigned char buf[64 * SECTOR] __attribute__((aligned(32)));

static u32 now_ticks(void) { return (u32)(sim.now_us * 1898 / 1000); }
static void sleep_ms(u32 ms) { sim.now_us += (u64)ms * 1000; }

static jmp_buf hang_jump;
static u32 hang_action;
static void on_hang(u32 action) { hang_action = action; longjmp(hang_jump, 1); }

static void mount(void)
{
	dbg_host_now = now_ticks;
	dbg_host_sleep_ms = sleep_ms;
	dbg_host_reset();	/* no fault of the last test */
	CHECK(usbs_mount() >= 0, "mount failed: %s", sim_summary());
	sim.now_us = 0;
	sim.cbws = sim.csws = sim.bot_resets = sim.clear_halts = 0;
	sim.port_resets = sim.dev_resets = sim.writes_ok = sim.reads_ok = sim.senses = 0;
	glue_data_xfers = 0;
	dbg_host_reset();	/* count from here */
}

static unsigned largest_xfer(void)
{
	unsigned i, m = 0;

	for (i = 0; i < glue_data_xfers && i < 64; i++)
		if (glue_data_len[i] > m)
			m = glue_data_len[i];
	return m;
}

static void test_read_counts_a_request(void)
{
	d2xdbg_mod *m = DBG_MOD(USBS);

	mount();
	CHECK_OK();
	CHECK(usbs_read(10, 4, buf) >= 0, "read failed: %s", sim_summary());
	CHECK(m->requests == 1 && m->errors == 0 && m->retries == 0 && m->resets == 0, "counters %u/%u/%u/%u",
	      m->requests, m->errors, m->retries, m->resets);
	CHECK(m->max_cmd == 0x28 && m->max_arg == 10, "longest: cmd %x arg %u", m->max_cmd, m->max_arg);
	CHECK(m->max_ticks > 0, "the request took simulated time");
	CHECK(m->cur_start == 0, "the request is closed");
	CHECK(dbg_host_count(USBS, D2XDBG_E_BEGIN) == 1 && dbg_host_count(USBS, D2XDBG_E_END) == 1, "B and E traced");
	CHECK(dbg_host_count(USBS, D2XDBG_E_RETRY) == 0 && dbg_host_count(USBS, D2XDBG_E_FAULT) == 0,
	      "no retry or fault traced");
	CHECK(memcmp(buf, sim.data + 10 * SECTOR, 4 * SECTOR) == 0, "read returned other data");
	CHECK(sim.cbws == 1 && sim.bot_resets == 0 && sim.clear_halts == 0, "no change without a fault: %s",
	      sim_summary());
	usbs_close();
}

static void test_write_counts_a_request(void)
{
	d2xdbg_mod *m = DBG_MOD(USBS);

	mount();
	CHECK_OK();
	memset(buf, 0x3C, SECTOR);
	CHECK(usbs_write(21, 1, buf) >= 0, "write failed: %s", sim_summary());
	CHECK(m->requests == 1 && m->errors == 0, "counters %u/%u", m->requests, m->errors);
	CHECK(m->max_cmd == 0x2A && m->max_arg == 21, "longest: cmd %x arg %u", m->max_cmd, m->max_arg);
	CHECK(memcmp(sim.data + 21 * SECTOR, buf, SECTOR) == 0, "drive holds other data");
	usbs_close();
}

static void test_csw_stall_fault_rereads_csw(void)
{
	mount();
	CHECK_OK();
	dbg_host_fault(0, USBS, D2XDBG_P_CSW, 1, 0, D2XDBG_F_STALL << 24);
	CHECK(usbs_read(11, 1, buf) >= 0, "read failed: %s", sim_summary());
	CHECK(memcmp(buf, sim.data + 11 * SECTOR, SECTOR) == 0, "read returned other data");
	/* The module re-reads a STALLed CSW at once; the fault replaces the whole
	 * __USB_BlkMsgTimeout() call, so its clear-halt does not happen either. */
	CHECK(sim.cbws == 1 && sim.csws == 1, "the CBW once, the CSW read once again: %s", sim_summary());
	CHECK(sim.bot_resets == 0 && sim.port_resets == 0, "a STALLed CSW needs no reset: %s", sim_summary());
	CHECK(DBG_MOD(USBS)->faults == 1 && dbg_host_count(USBS, D2XDBG_E_FAULT) == 1, "the fault counted and traced");
	CHECK(DBG_MOD(USBS)->errors == 0, "the request still succeeded");
	usbs_close();
}

static void test_csw_lost_fault_recovers(void)
{
	mount();
	CHECK_OK();
	dbg_host_fault(0, USBS, D2XDBG_P_CSW, 1, 0, D2XDBG_F_LOSE << 24);
	CHECK(usbs_read(12, 1, buf) >= 0, "read failed: %s", sim_summary());
	CHECK(memcmp(buf, sim.data + 12 * SECTOR, SECTOR) == 0, "read returned other data");
	CHECK(sim.bot_resets >= 1, "a lost CSW gets a reset recovery: %s", sim_summary());
	CHECK(dbg_host_count(USBS, D2XDBG_E_RESET) >= 1, "the reset traced");
	CHECK(dbg_host_count(USBS, D2XDBG_E_TIMEOUT) >= 1 && DBG_MOD(USBS)->timeouts >= 1, "the timeout counted");
	CHECK(DBG_MOD(USBS)->retries >= 1 && DBG_MOD(USBS)->errors == 0, "retried, and the request succeeded");
	usbs_close();
}

static void test_data_stall_fault_on_read_recovers(void)
{
	mount();
	CHECK_OK();
	dbg_host_fault(0, USBS, D2XDBG_P_DATA, 1, 0, D2XDBG_F_STALL << 24);
	memset(buf, 0, 8 * SECTOR);
	CHECK(usbs_read(13, 8, buf) >= 0, "read failed: %s", sim_summary());
	CHECK(memcmp(buf, sim.data + 13 * SECTOR, 8 * SECTOR) == 0, "read returned other data");
	CHECK(DBG_MOD(USBS)->faults == 1, "fired once");
	CHECK(DBG_MOD(USBS)->errors == 0, "the request still succeeded");
	usbs_close();
}

static void test_cmd_error_fault_recovers(void)
{
	mount();
	CHECK_OK();
	dbg_host_fault(0, USBS, D2XDBG_P_CMD, 1, 0, D2XDBG_F_ERROR << 24);
	CHECK(usbs_read(14, 1, buf) >= 0, "read failed: %s", sim_summary());
	CHECK(memcmp(buf, sim.data + 14 * SECTOR, SECTOR) == 0, "read returned other data");
	CHECK(DBG_MOD(USBS)->faults == 1 && DBG_MOD(USBS)->errors == 0, "fired once, request succeeded");
	usbs_close();
}

static void test_delay_fault_waits(void)
{
	mount();
	CHECK_OK();
	dbg_host_fault(0, USBS, D2XDBG_P_CMD, 1, 0, (D2XDBG_F_DELAY << 24) | 250);
	CHECK(usbs_read(17, 1, buf) >= 0, "read failed: %s", sim_summary());
	CHECK(sim.now_us >= 250000, "only %llu us passed", (unsigned long long)sim.now_us);
	CHECK(DBG_MOD(USBS)->max_ticks >= 250 * 1898, "the delay shows as the longest request");
	CHECK(sim.bot_resets == 0, "a delay alone is no error");
	usbs_close();
}

static void test_hang_fault_never_returns(void)
{
	mount();
	CHECK_OK();
	dbg_host_fault(0, USBS, D2XDBG_P_DATA, 1, 0, D2XDBG_F_SPIN << 24);
	dbg_host_on_hang = on_hang;
	hang_action = 0;
	if (!setjmp(hang_jump)) {
		usbs_read(18, 1, buf);
		CHECK(0, "the read returned");
	}
	CHECK(hang_action == D2XDBG_F_SPIN, "spun");
	CHECK(DBG_MOD(USBS)->cur_start != 0, "the request is still open: last words show where it stopped");
	usbs_close();
}

/* A status that STALLs every time: the request must end with an error in
 * time (USBS has no unplug wait, unlike EHCI). */
static void test_csw_stall_every_time_fails_in_time(void)
{
	d2xdbg_mod *m = DBG_MOD(USBS);

	mount();
	CHECK_OK();
	dbg_host_fault(0, USBS, D2XDBG_P_CSW, 1000000, 0, D2XDBG_F_STALL << 24);
	CHECK(usbs_read(19, 1, buf) < 0, "a read whose status never came succeeded");
	CHECK(sim.now_us <= (u64)(REQUEST_MS + 2000) * 1000, "gave up after %.1f s", sim.now_us / 1e6);
	CHECK(m->requests == 1 && m->errors == 1, "one failed request, %u/%u", m->requests, m->errors);
	CHECK(m->retries >= 1, "retries %u counted", m->retries);
	CHECK(m->cur_start == 0, "the request is closed");
	dbg_host_fault(0, 0, 0, 0, 0, 0);
	CHECK(usbs_read(20, 1, buf) >= 0, "the drive does not read once the fault is cleared: %s", sim_summary());
	usbs_close();
}

/* A status that never comes, every time (-ETIMEDOUT) */
static void test_csw_lost_every_time_fails_in_time(void)
{
	d2xdbg_mod *m = DBG_MOD(USBS);

	mount();
	CHECK_OK();
	dbg_host_fault(0, USBS, D2XDBG_P_CSW, 1000000, 0, D2XDBG_F_LOSE << 24);
	CHECK(usbs_read(22, 1, buf) < 0, "a read whose status never came succeeded");
	CHECK(sim.now_us <= (u64)(REQUEST_MS + 2000) * 1000, "gave up after %.1f s", sim.now_us / 1e6);
	CHECK(m->requests == 1 && m->errors == 1, "one failed request, %u/%u", m->requests, m->errors);
	CHECK(m->timeouts >= 1, "timeouts %u counted", m->timeouts);
	usbs_close();
}

/* F_BIG: a direct (aligned MEM2) buffer gets the whole data phase in one
 * transfer, past the 16 KB piece size. */
static void test_big_fault_one_transfer_when_direct(void)
{
	mount();
	CHECK_OK();
	glue_mem2 = 1;
	dbg_host_fault(0, USBS, D2XDBG_P_DATA, 1, 0, D2XDBG_F_BIG << 24);
	memset(buf, 0, 64 * SECTOR);
	CHECK(usbs_read(30, 64, buf) >= 0, "read failed: %s", sim_summary());
	CHECK(memcmp(buf, sim.data + 30 * SECTOR, 64 * SECTOR) == 0, "read returned other data");
	CHECK(glue_data_xfers == 1 && largest_xfer() == 32768, "%u transfers, largest %u (want 1 x 32768)",
	      glue_data_xfers, largest_xfer());
	CHECK(DBG_MOD(USBS)->faults == 1, "the fault fired once");
	usbs_close();
}

/* Without the fault, the same read is two pieces of 16 KB */
static void test_no_fault_direct_is_16k_pieces(void)
{
	mount();
	CHECK_OK();
	glue_mem2 = 1;
	CHECK(usbs_read(30, 64, buf) >= 0, "read failed: %s", sim_summary());
	CHECK(glue_data_xfers == 2 && largest_xfer() == 16384, "%u transfers, largest %u (want 2 x 16384)",
	      glue_data_xfers, largest_xfer());
	usbs_close();
}

/* A bounce buffer cannot go as one transfer: F_BIG is ignored */
static void test_big_fault_ignored_with_bounce(void)
{
	mount();
	CHECK_OK();
	glue_mem2 = 0;
	dbg_host_fault(0, USBS, D2XDBG_P_DATA, 1, 0, D2XDBG_F_BIG << 24);
	memset(buf, 0, 64 * SECTOR);
	CHECK(usbs_read(30, 64, buf) >= 0, "read failed: %s", sim_summary());
	CHECK(memcmp(buf, sim.data + 30 * SECTOR, 64 * SECTOR) == 0, "read returned other data");
	CHECK(glue_data_xfers == 2 && largest_xfer() == 16384, "%u transfers, largest %u (want 2 x 16384)",
	      glue_data_xfers, largest_xfer());
	usbs_close();
}

static const struct hc_test tests[] = {
	{ "usbs_dbg_read_counts_a_request", test_read_counts_a_request, false },
	{ "usbs_dbg_write_counts_a_request", test_write_counts_a_request, false },
	{ "usbs_dbg_csw_stall_fault_rereads_csw", test_csw_stall_fault_rereads_csw, false },
	{ "usbs_dbg_csw_lost_fault_recovers", test_csw_lost_fault_recovers, false },
	{ "usbs_dbg_data_stall_fault_on_read_recovers", test_data_stall_fault_on_read_recovers, false },
	{ "usbs_dbg_cmd_error_fault_recovers", test_cmd_error_fault_recovers, false },
	{ "usbs_dbg_delay_fault_waits", test_delay_fault_waits, false },
	{ "usbs_dbg_hang_fault_never_returns", test_hang_fault_never_returns, false },
	{ "usbs_dbg_csw_stall_every_time_fails_in_time", test_csw_stall_every_time_fails_in_time, false },
	{ "usbs_dbg_csw_lost_every_time_fails_in_time", test_csw_lost_every_time_fails_in_time, false },
	{ "usbs_dbg_big_fault_one_transfer_when_direct", test_big_fault_one_transfer_when_direct, false },
	{ "usbs_dbg_no_fault_direct_is_16k_pieces", test_no_fault_direct_is_16k_pieces, false },
	{ "usbs_dbg_big_fault_ignored_with_bounce", test_big_fault_ignored_with_bounce, false },
};

static void setup(void)
{
	sim_reset();
	glue_mem2 = 0;
	glue_short_next = 0;
	dbg_host_on_hang = NULL;
	sim.limit_us = 600 * 1000000ull;
}

int main(int argc, char **argv)
{
	return hc_main(tests, HC_COUNT(tests), argc, argv, setup);
}
