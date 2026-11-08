/*
 * Host tests of the EHCI module's debug hooks (-DD2X_DEBUG, d2xdbg.h): the
 * counters and trace a request leaves, and each fault /dev/d2xdbg can inject
 * at the CBW, data and CSW points, against the simulated drive. The driver
 * must recover from each the way it does from the real fault. The debug
 * area runs on the PC (tests/host/dbg/swi_host.c). Run with `make -C tests/host`.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include "sim.h"
#define HOSTCHECK_MAIN
#include "hostcheck.h"
#include "dbg_host.h"

#define SECTOR SIM_SECTOR_SIZE
#define EHCI D2XDBG_M_EHCI

static u8 buf[64 * SECTOR];

static u32 now_ticks(void) { return (u32)(sim.now_us * 1898 / 1000); }
static void sleep_ms(u32 ms) { sim.now_us += (u64)ms * 1000; }

static jmp_buf hang_jump;
static u32 hang_action;
static void on_hang(u32 action) { hang_action = action; longjmp(hang_jump, 1); }

static void mount(void)
{
	dbg_host_now = now_ticks;
	dbg_host_sleep_ms = sleep_ms;
	CHECK(ums_plug() == 0, "mount failed: %s", sim_summary());
	sim.now_us = 0;
	sim.cbws = sim.csws = sim.bot_resets = sim.clear_halts = 0;
	sim.port_resets = sim.dev_resets = sim.writes_ok = sim.reads_ok = 0;
	dbg_host_reset();	/* count from here */
}

static void test_read_counts_a_request(void)
{
	d2xdbg_mod *m = DBG_MOD(EHCI);

	mount();
	CHECK_OK();
	CHECK(USBStorage_Read_Sectors(10, 4, buf), "read failed: %s", sim_summary());
	CHECK(m->requests == 1 && m->errors == 0 && m->retries == 0 && m->resets == 0, "counters %u/%u/%u/%u",
	      m->requests, m->errors, m->retries, m->resets);
	CHECK(m->max_cmd == 0x28 && m->max_arg == 10, "longest: cmd %x arg %u", m->max_cmd, m->max_arg);
	CHECK(m->max_ticks > 0, "the request took simulated time");
	CHECK(dbg_host_count(EHCI, D2XDBG_E_BEGIN) == 1 && dbg_host_count(EHCI, D2XDBG_E_END) == 1, "B and E traced");
	CHECK(dbg_host_count(EHCI, 'u') >= 3, "the CBW, data and CSW steps traced");
	CHECK(sim.cbws == 1 && sim.bot_resets == 0, "no change without a fault: %s", sim_summary());
}

static void test_csw_stall_fault_clears_halt(void)
{
	mount();
	CHECK_OK();
	dbg_host_fault(0, EHCI, D2XDBG_P_CSW, 1, 0, D2XDBG_F_STALL << 24);
	CHECK(USBStorage_Read_Sectors(11, 1, buf), "read failed: %s", sim_summary());
	CHECK(memcmp(buf, sim.data + 11 * SECTOR, SECTOR) == 0, "read returned other data");
	CHECK(sim.clear_halts == 1, "%u clear-halts, expected 1: %s", sim.clear_halts, sim_summary());
	CHECK(sim.bot_resets == 0 && sim.port_resets == 0, "a STALLed CSW needs no reset: %s", sim_summary());
	CHECK(DBG_MOD(EHCI)->faults == 1 && dbg_host_count(EHCI, D2XDBG_E_FAULT) == 1, "the fault counted and traced");
}

static void test_csw_lost_fault_recovers(void)
{
	mount();
	CHECK_OK();
	dbg_host_fault(0, EHCI, D2XDBG_P_CSW, 1, 0, D2XDBG_F_LOSE << 24);
	CHECK(USBStorage_Read_Sectors(12, 1, buf), "read failed: %s", sim_summary());
	CHECK(memcmp(buf, sim.data + 12 * SECTOR, SECTOR) == 0, "read returned other data");
	CHECK(sim.bot_resets >= 1, "a lost CSW gets a reset recovery: %s", sim_summary());
	CHECK(DBG_MOD(EHCI)->resets >= 1, "the reset counted");
	CHECK(DBG_MOD(EHCI)->errors == 0, "the request still succeeded");
}

static void test_data_stall_fault_on_read_recovers(void)
{
	mount();
	CHECK_OK();
	dbg_host_fault(0, EHCI, D2XDBG_P_DATA, 1, 0, D2XDBG_F_STALL << 24);
	CHECK(USBStorage_Read_Sectors(13, 8, buf), "read failed: %s", sim_summary());
	CHECK(memcmp(buf, sim.data + 13 * SECTOR, 8 * SECTOR) == 0, "read returned other data");
	CHECK(DBG_MOD(EHCI)->faults == 1, "fired once");
}

static void test_cmd_error_fault_remounts(void)
{
	mount();
	CHECK_OK();
	dbg_host_fault(0, EHCI, D2XDBG_P_CMD, 1, 0, D2XDBG_F_ERROR << 24);
	CHECK(USBStorage_Read_Sectors(14, 1, buf), "read failed: %s", sim_summary());
	CHECK(memcmp(buf, sim.data + 14 * SECTOR, SECTOR) == 0, "read returned other data");
	CHECK(sim.port_resets >= 1, "an error is the device gone: a port reset and remount: %s", sim_summary());
	CHECK(dbg_host_count(EHCI, D2XDBG_E_RESET) >= 1, "the remount traced");
}

static void test_skip_lets_the_first_through(void)
{
	mount();
	CHECK_OK();
	dbg_host_fault(0, EHCI, D2XDBG_P_CSW, 1, 1, D2XDBG_F_LOSE << 24);
	CHECK(USBStorage_Read_Sectors(15, 1, buf), "first read failed");
	CHECK(sim.bot_resets == 0 && DBG_MOD(EHCI)->faults == 0, "skip 1: the first CSW is real");
	CHECK(USBStorage_Read_Sectors(16, 1, buf), "second read failed: %s", sim_summary());
	CHECK(sim.bot_resets >= 1 && DBG_MOD(EHCI)->faults == 1, "the second gets the fault: %s", sim_summary());
}

static void test_delay_fault_waits(void)
{
	mount();
	CHECK_OK();
	dbg_host_fault(0, EHCI, D2XDBG_P_CMD, 1, 0, (D2XDBG_F_DELAY << 24) | 250);
	CHECK(USBStorage_Read_Sectors(17, 1, buf), "read failed: %s", sim_summary());
	CHECK(sim.now_us >= 250000, "only %llu us passed", (unsigned long long)sim.now_us);
	CHECK(DBG_MOD(EHCI)->max_ticks >= 250 * 1898, "the delay shows as the longest request");
	CHECK(sim.bot_resets == 0, "a delay alone is no error");
}

static void test_hang_fault_never_returns(void)
{
	mount();
	CHECK_OK();
	dbg_host_fault(0, EHCI, D2XDBG_P_DATA, 1, 0, D2XDBG_F_SPIN << 24);
	dbg_host_on_hang = on_hang;
	hang_action = 0;
	if (!setjmp(hang_jump)) {
		USBStorage_Read_Sectors(18, 1, buf);
		CHECK(0, "the read returned");
	}
	CHECK(hang_action == D2XDBG_F_SPIN, "spun");
	CHECK(DBG_MOD(EHCI)->cur_start != 0, "the request is still open: last words show where it stopped");
}

/* A CSW that STALLs every time ends the request with an error once its time
 * is up. When __cycle() runs out of reset recoveries it forces an unplug
 * (-ENODEV); the remount's own commands fail the same way, but the port still
 * shows the drive, so the wait keeps the request deadline (rule 1 of
 * __USBStorage_Sectors()) and the drive is then marked dead. (A CSW lost every
 * time, -ETIMEDOUT, ends the same way.) docs/usb-storage.md. */
static void test_csw_stall_every_time_fails_in_time(void)
{
	d2xdbg_mod *m = DBG_MOD(EHCI);

	mount();
	CHECK_OK();
	dbg_host_fault(0, EHCI, D2XDBG_P_CSW, 1000000, 0, D2XDBG_F_STALL << 24);
	CHECK(!USBStorage_Read_Sectors(19, 1, buf), "a read whose status never came succeeded");
	CHECK(sim.now_us <= (u64)(ums_request_ms + 2000) * 1000, "gave up after %.1f s", sim.now_us / 1e6);
	CHECK(m->requests == 1 && m->errors == 1, "one failed request, %u/%u", m->requests, m->errors);
	CHECK(m->resets >= 1 && m->retries >= 1, "resets %u and retries %u counted", m->resets, m->retries);
	CHECK(m->max_ticks + 1898 * 2 >= (u32)(sim.now_us * 1898 / 1000), "its length recorded");
	dbg_host_fault(0, 0, 0, 0, 0, 0);
	CHECK(USBStorage_Read_Sectors(20, 1, buf), "the drive does not read once the fault is cleared: %s",
	      sim_summary());
}

static const struct hc_test tests[] = {
	{ "read_counts_a_request", test_read_counts_a_request, false },
	{ "csw_stall_fault_clears_halt", test_csw_stall_fault_clears_halt, false },
	{ "csw_lost_fault_recovers", test_csw_lost_fault_recovers, false },
	{ "data_stall_fault_on_read_recovers", test_data_stall_fault_on_read_recovers, false },
	{ "cmd_error_fault_remounts", test_cmd_error_fault_remounts, false },
	{ "skip_lets_the_first_through", test_skip_lets_the_first_through, false },
	{ "delay_fault_waits", test_delay_fault_waits, false },
	{ "hang_fault_never_returns", test_hang_fault_never_returns, false },
	{ "csw_stall_every_time_fails_in_time", test_csw_stall_every_time_fails_in_time, false },
};

static void setup(void)
{
	sim_reset();
	ums_unplug();
	sim.limit_us = 600 * 1000000ull;
}

int main(int argc, char **argv)
{
	return hc_main(tests, HC_COUNT(tests), argc, argv, setup);
}
