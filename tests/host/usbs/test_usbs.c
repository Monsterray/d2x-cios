/*
 * Host tests of the USBS module's USB mass-storage driver
 * (source/usb-module/usbstorage.c) against the simulated drive in
 * ../ums/sim.c. Run with `make -C tests/host`.
 */
#include <stdio.h>
#include <stdlib.h>
#include "sim.h"
#define HOSTCHECK_MAIN
#include "hostcheck.h"

#define SECTOR SIM_SECTOR_SIZE

int usbs_mount(void);
int usbs_read(unsigned int sector, unsigned int n, void *buf);
int usbs_write(unsigned int sector, unsigned int n, const void *buf);
void usbs_close(void);

static unsigned char buf[16 * SECTOR];

static void mount(void)
{
	CHECK(usbs_mount() >= 0, "mount failed: %s", sim_summary());
	sim.now_us = 0;
	sim.cbws = sim.csws = sim.bot_resets = sim.clear_halts = 0;
	sim.writes_ok = sim.reads_ok = sim.senses = 0;
}

static void test_roundtrip(void)
{
	mount();
	CHECK_OK();
	for (size_t i = 0; i < sizeof(buf); i++)
		buf[i] = (unsigned char) (i * 5 + 1);
	CHECK(usbs_write(100, 16, buf) >= 0, "write failed: %s", sim_summary());
	CHECK(memcmp(sim.data + 100 * SECTOR, buf, sizeof(buf)) == 0, "drive holds other data");
	memset(buf, 0, sizeof(buf));
	CHECK(usbs_read(100, 16, buf) >= 0, "read failed: %s", sim_summary());
	CHECK(memcmp(sim.data + 100 * SECTOR, buf, sizeof(buf)) == 0, "read returned other data");
	usbs_close();
}

/*
 * The drive answers a WRITE with CSW status 1 (command failed). The driver
 * must not report success while the data is not on the drive.
 * (USBStorage_Write() tests `retval > 0 && status != 0`, but __cycle()
 * returns 0 on success, so the status is never looked at.)
 */
static void test_write_csw_failure_is_reported(void)
{
	mount();
	CHECK_OK();
	memset(buf, 0x5A, SECTOR);
	sim.fail_write_csw = 1;
	int ret = usbs_write(7, 1, buf);
	CHECK(ret < 0 || sim.writes_ok == 1, "write returned %d, but the drive failed it: %s", ret,
	      sim_summary());
	usbs_close();
}

static void fill(unsigned char *p, size_t n, int seed)
{
	for (size_t i = 0; i < n; i++)
		p[i] = (unsigned char) (i * 5 + seed);
}

#define REQUEST_MS 8000         /* the driver's USBSTORAGE_REQUEST_MS */

extern int glue_cancel_clears_halt;

/* The drive answers a WRITE with CSW status 1 once, as a transient error: the
 * driver sends REQUEST SENSE and the write is tried again. */
static void test_write_csw_failure_is_retried(void)
{
	mount();
	CHECK_OK();
	fill(buf, SECTOR, 4);
	sim.fail_write_csw = 1;
	CHECK(usbs_write(7, 1, buf) >= 0, "write failed: %s", sim_summary());
	CHECK(sim.writes_ok == 1 && memcmp(sim.data + 7 * SECTOR, buf, SECTOR) == 0,
	      "the write was not retried: %s", sim_summary());
	CHECK(sim.senses == 1, "%u REQUEST SENSE, expected 1: %s", sim.senses, sim_summary());
	CHECK(sim.bot_resets == 0 && sim.port_resets == 0, "%s", sim_summary());
	usbs_close();
}

/* The drive STALLs bulk-out at a WRITE's data phase once, twice over with a
 * READ between. Bulk-Only 6.7.3: clear the halt, read the CSW (status 1),
 * try again: no reset recovery needed. */
static void test_write_recovers_from_one_stall(void)
{
	mount();
	CHECK_OK();
	for (int i = 0; i < 2; i++) {
		if (i)
			CHECK(usbs_read(40, 1, buf) >= 0, "read failed: %s", sim_summary());
		fill(buf, SECTOR, 5 + i);
		sim.stall_write_data = 1;
		CHECK(usbs_write(9 + i, 1, buf) >= 0, "write %d failed: %s", i, sim_summary());
		CHECK(memcmp(sim.data + (9 + i) * SECTOR, buf, SECTOR) == 0, "drive holds other data");
	}
	CHECK(sim.bot_resets == 0 && sim.port_resets == 0 && sim.toggle_losses == 0, "%s",
	      sim_summary());
	CHECK(sim.now_us < 100000, "recovery took %s", sim_summary());
	usbs_close();
}

/* A READ whose data phase STALLs (status 1 in the CSW): the next try works. */
static void test_read_recovers_from_one_stall(void)
{
	mount();
	CHECK_OK();
	sim.stall_read_data = 1;
	CHECK(usbs_read(50, 2, buf) >= 0, "read failed: %s", sim_summary());
	CHECK(memcmp(buf, sim.data + 50 * SECTOR, 2 * SECTOR) == 0, "read returned other data");
	CHECK(sim.bot_resets == 0 && sim.port_resets == 0 && sim.toggle_losses == 0, "%s",
	      sim_summary());
	usbs_close();
}

/* The CSW read STALLs once (BOT 5.3.3): clear the halt, read it again. The
 * command itself worked, so no reset recovery and no repeat of the command. */
static void test_csw_stall_is_cleared_and_reread(void)
{
	mount();
	CHECK_OK();
	sim.stall_csw = 1;
	CHECK(usbs_read(60, 1, buf) >= 0, "read failed: %s", sim_summary());
	CHECK(memcmp(buf, sim.data + 60 * SECTOR, SECTOR) == 0, "read returned other data");
	CHECK(sim.bot_resets == 0 && sim.cbws == 1, "%s", sim_summary());
	fill(buf, SECTOR, 9);
	sim.stall_csw = 1;
	CHECK(usbs_write(61, 1, buf) >= 0, "write failed: %s", sim_summary());
	CHECK(sim.bot_resets == 0 && sim.cbws == 2 && sim.writes_ok == 1, "%s", sim_summary());
	usbs_close();
}

/* A write-protected drive refuses every write: give up quickly with an error
 * and no reset recovery (the drive answers; it just says no). */
static void test_write_to_protected_drive_gives_up(void)
{
	mount();
	CHECK_OK();
	fill(buf, SECTOR, 6);
	sim.stall_write_data = 1000000;
	CHECK(usbs_write(11, 1, buf) < 0, "write to a write-protected drive reported success: %s",
	      sim_summary());
	CHECK(sim.bot_resets == 0 && sim.port_resets == 0, "needless recovery: %s", sim_summary());
	CHECK(sim.now_us < 100000, "giving up took %s", sim_summary());
	usbs_close();
}

/* A drive that takes a WRITE's data, STALLs the CSW and never sends it (the
 * SanDisk on the bench): fail, and within the request's time. */
static void test_write_with_lost_status_fails_in_time(void)
{
	mount();
	CHECK_OK();
	fill(buf, SECTOR, 7);
	sim.lose_write_csw = 1000000;
	int ret = usbs_write(12, 1, buf);
	printf("        lost status: error after %.1f s, %u BOT resets\n", sim.now_us / 1e6, sim.bot_resets);
	CHECK(ret < 0, "a write whose status never came reported success: %s", sim_summary());
	CHECK(sim.now_us <= (REQUEST_MS + 2000) * 1000ull, "giving up took %.1f s (limit %d ms): %s",
	      sim.now_us / 1e6, REQUEST_MS, sim_summary());
	sim.lose_write_csw = 0;
	CHECK(usbs_read(3, 1, buf) >= 0, "the drive does not read after the failed write: %s",
	      sim_summary());
	usbs_close();
}

static void test_write_with_one_lost_status_recovers(void)
{
	mount();
	CHECK_OK();
	fill(buf, SECTOR, 8);
	sim.lose_write_csw = 1;
	CHECK(usbs_write(13, 1, buf) >= 0, "write failed: %s", sim_summary());
	CHECK(memcmp(sim.data + 13 * SECTOR, buf, SECTOR) == 0, "drive holds other data");
	usbs_close();
}

/* No transfer is answered at all: the request still ends in time. */
static void test_dead_drive_fails_in_time(void)
{
	mount();
	CHECK_OK();
	sim.timeout_xfers = 1000000;
	int ret = usbs_read(3, 1, buf);
	printf("        dead drive: error after %.1f s\n", sim.now_us / 1e6);
	CHECK(ret < 0, "read from a dead drive reported success: %s", sim_summary());
	CHECK(sim.now_us <= (REQUEST_MS + 2000) * 1000ull, "giving up took %.1f s (limit %d ms): %s",
	      sim.now_us / 1e6, REQUEST_MS, sim_summary());
	usbs_close();
}

/* If IOS's cancel-endpoint request does not clear a halt (REVIEW.md: unknown),
 * a STALLed drive stays stuck, but the request still ends in time. */
static void test_stuck_halt_fails_in_time(void)
{
	mount();
	CHECK_OK();
	glue_cancel_clears_halt = 0;
	fill(buf, SECTOR, 3);
	sim.stall_write_data = 1;
	int ret = usbs_write(14, 1, buf);
	glue_cancel_clears_halt = 1;
	CHECK(ret < 0, "write through a stuck halt reported success: %s", sim_summary());
	CHECK(sim.now_us <= (REQUEST_MS + 2000) * 1000ull, "giving up took %.1f s: %s",
	      sim.now_us / 1e6, sim_summary());
	usbs_close();
}

/* BOT 5.1: bCBWCBLength is the command's length: READ(10) carries 10. */
static void test_cbw_carries_command_length(void)
{
	mount();
	CHECK_OK();
	CHECK(usbs_read(3, 1, buf) >= 0, "read failed: %s", sim_summary());
	CHECK(sim.last_cb_op == 0x28 && sim.last_cb_len == 10, "READ(10) CBW length %u, expected 10",
	      sim.last_cb_len);
	usbs_close();
}

static const struct hc_test tests[] = {
	{ "usbs_roundtrip", test_roundtrip, false },
	{ "usbs_write_csw_failure_is_reported", test_write_csw_failure_is_reported, false },
	{ "usbs_write_csw_failure_is_retried", test_write_csw_failure_is_retried, false },
	{ "usbs_write_recovers_from_one_stall", test_write_recovers_from_one_stall, false },
	{ "usbs_read_recovers_from_one_stall", test_read_recovers_from_one_stall, false },
	{ "usbs_csw_stall_is_cleared_and_reread", test_csw_stall_is_cleared_and_reread, false },
	{ "usbs_write_to_protected_drive_gives_up", test_write_to_protected_drive_gives_up, false },
	{ "usbs_write_with_lost_status_fails_in_time", test_write_with_lost_status_fails_in_time, false },
	{ "usbs_write_with_one_lost_status_recovers", test_write_with_one_lost_status_recovers, false },
	{ "usbs_dead_drive_fails_in_time", test_dead_drive_fails_in_time, false },
	{ "usbs_stuck_halt_fails_in_time", test_stuck_halt_fails_in_time, false },
	{ "usbs_cbw_carries_command_length", test_cbw_carries_command_length, false },
};

static void setup(void)
{
	sim_reset();
	sim.limit_us = 600 * 1000000ull;
}

int main(int argc, char **argv)
{
	return hc_main(tests, HC_COUNT(tests), argc, argv, setup);
}
