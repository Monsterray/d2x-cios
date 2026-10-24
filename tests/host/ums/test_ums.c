/*
 * Host tests of the EHCI module's USB mass-storage driver
 * (source/ehci-module/tinyehci/usbstorage.c) against the simulated drive in
 * sim.c. Run with `make -C tests/host`.
 *
 * A test marked known_bug describes the behaviour the driver should have and
 * is expected to fail until the bug is fixed. It is reported as "known" and
 * does not fail the run; once it passes, the run fails with "FIXED?" so the
 * mark gets removed (hostcheck.h, from wii-homebrew-kit).
 */
#include <stdio.h>
#include <stdlib.h>
#include "sim.h"
#define HOSTCHECK_MAIN
#include "hostcheck.h"

#define SECTOR SIM_SECTOR_SIZE

static u8 buf[128 * SECTOR], back[128 * SECTOR];

static void fill(u8 *p, size_t n, u8 seed)
{
	for (size_t i = 0; i < n; i++)
		p[i] = (u8)(seed + i * 13);
}

static void mount(void)
{
	CHECK(ums_plug() == 0, "mount failed: %s", sim_summary());
	/* count from here: mounting has its own resets */
	sim.now_us = 0;
	sim.cbws = sim.csws = sim.bot_resets = sim.clear_halts = 0;
	sim.port_resets = sim.dev_resets = sim.writes_ok = sim.reads_ok = 0;
}

static void test_mount(void)
{
	mount();
	CHECK_OK();
	CHECK(ums_sector_size() == SECTOR, "sector size %u", ums_sector_size());
}

static void test_read(void)
{
	mount();
	CHECK_OK();
	CHECK(USBStorage_Read_Sectors(10, 1, buf), "read failed: %s", sim_summary());
	CHECK(memcmp(buf, sim.data + 10 * SECTOR, SECTOR) == 0, "read returned other data");
	CHECK(sim.cbws == 1 && sim.bot_resets == 0, "%s", sim_summary());
}

static void test_write_roundtrip(void)
{
	mount();
	CHECK_OK();
	fill(buf, 100 * SECTOR, 1);
	/* 100 sectors is 51200 bytes: the driver splits at 32 KB */
	CHECK(USBStorage_Write_Sectors(200, 100, buf), "write failed: %s", sim_summary());
	CHECK(memcmp(sim.data + 200 * SECTOR, buf, 100 * SECTOR) == 0, "drive holds other data");
	CHECK(sim.cbws == 2, "%u CBWs for 51200 bytes, expected 2", sim.cbws);
	CHECK(USBStorage_Read_Sectors(200, 100, back), "read back failed: %s", sim_summary());
	CHECK(memcmp(back, buf, 100 * SECTOR) == 0, "read back other data");
	CHECK(sim.bot_resets == 0 && sim.port_resets == 0, "%s", sim_summary());
}

static void test_one_sector_write_is_fast(void)
{
	mount();
	CHECK_OK();
	fill(buf, SECTOR, 2);
	CHECK(USBStorage_Write_Sectors(1, 1, buf), "write failed: %s", sim_summary());
	CHECK(sim.now_us < 10000, "one healthy sector write took %s", sim_summary());
}

/* A transfer that times out once (a slow drive) is retried and succeeds. */
static void test_write_after_one_timeout(void)
{
	mount();
	CHECK_OK();
	fill(buf, SECTOR, 3);
	sim.timeout_xfers = 1;
	CHECK(USBStorage_Write_Sectors(5, 1, buf), "write failed: %s", sim_summary());
	CHECK(memcmp(sim.data + 5 * SECTOR, buf, SECTOR) == 0, "drive holds other data");
	CHECK(sim.now_us < 5000000, "recovery took %s", sim_summary());
}

/*
 * The drive answers a WRITE with CSW status 1 (command failed). The write
 * must be reported as failed, or retried until it succeeds; it must not be
 * reported as done while the data is not on the drive.
 *
 * Today __USBStorage_Write() checks `retval > 0 && status != 0`, but
 * __cycle() returns 0 on success, so the status is never looked at.
 */
static void test_write_csw_failure_is_not_success(void)
{
	mount();
	CHECK_OK();
	fill(buf, SECTOR, 4);
	sim.fail_write_csw = 1;
	s32 ok = USBStorage_Write_Sectors(7, 1, buf);
	CHECK(!ok || sim.writes_ok == 1,
	      "write reported success, but the drive failed it and it was not retried: %s",
	      sim_summary());
}

/*
 * The drive STALLs bulk-out at a WRITE's data phase once (a transient
 * error). Bulk-Only Transport 6.7.3: the host clears the halt and reads the
 * CSW; then it can retry. The write should succeed, in well under a second.
 *
 * Today __cycle() resends the data in its inner loop: each try moves 0
 * bytes, and the "continue; //reset+retry" there restarts the inner loop,
 * not the command, so it never ends.
 */
static void test_write_recovers_from_one_stall(void)
{
	mount();
	CHECK_OK();
	fill(buf, SECTOR, 5);
	sim.stall_write_data = 1;
	sim.limit_us = 60 * 1000000ull;
	s32 ok = USBStorage_Write_Sectors(9, 1, buf);
	CHECK(ok, "write failed: %s", sim_summary());
	CHECK(memcmp(sim.data + 9 * SECTOR, buf, SECTOR) == 0, "drive holds other data");
	CHECK(sim.now_us < 1000000, "recovery took %s", sim_summary());
}

/*
 * The drive refuses every write (write-protected). USBStorage_Write_Sectors()
 * must give up and return false in bounded time, so the caller (FAT, the
 * loader) can report an error.
 *
 * Today its retry loop has `if(retry>12) retry=12; // infinite loop`.
 */
static void test_write_to_protected_drive_gives_up(void)
{
	mount();
	CHECK_OK();
	fill(buf, SECTOR, 6);
	sim.stall_write_data = 1000000;
	sim.limit_us = 600 * 1000000ull;
	s32 ok = USBStorage_Write_Sectors(11, 1, buf);
	CHECK(!ok, "write to a write-protected drive reported success: %s", sim_summary());
	CHECK(sim.now_us < 30 * 1000000ull, "giving up took %s", sim_summary());
}

/*
 * A Bulk-Only reset recovery (BOT 5.3.4: the class reset, then
 * CLEAR_FEATURE(ENDPOINT_HALT) on both bulk endpoints) puts the drive's data
 * toggles back to DATA0 (USB 2.0 9.4.5). The host must do the same, or the
 * next transfer on an endpoint whose toggle was DATA1 is discarded as a
 * retransmission, and the command times out into a port reset.
 *
 * Each READ flips the bulk-out toggle once (the CBW), so of two reads one
 * leaves it at DATA1 when the reset comes.
 *
 * Today USB_ClearHalt() (tinyehci/usb.c) sends the request but leaves the
 * host's toggle in ehci_device.toggles as it was.
 */
static void test_reset_recovery_keeps_toggles_in_step(void)
{
	mount();
	CHECK_OK();
	for (int i = 0; i < 2; i++) {
		CHECK(USBStorage_Read_Sectors(20 + i, 1, buf), "read %d failed: %s", i, sim_summary());
		CHECK(ums_reset() >= 0, "reset recovery failed: %s", sim_summary());
		CHECK(USBStorage_Read_Sectors(30 + i, 1, buf), "read after reset %d failed: %s", i,
		      sim_summary());
		CHECK(memcmp(buf, sim.data + (30 + i) * SECTOR, SECTOR) == 0, "read returned other data");
	}
	CHECK(sim.toggle_losses == 0 && sim.port_resets == 0, "%s", sim_summary());
}

static const struct hc_test tests[] = {
	{ "mount", test_mount, false },
	{ "read", test_read, false },
	{ "write_roundtrip", test_write_roundtrip, false },
	{ "one_sector_write_is_fast", test_one_sector_write_is_fast, false },
	{ "write_after_one_timeout", test_write_after_one_timeout, false },
	{ "write_csw_failure_is_not_success", test_write_csw_failure_is_not_success, true },
	{ "write_recovers_from_one_stall", test_write_recovers_from_one_stall, true },
	{ "write_to_protected_drive_gives_up", test_write_to_protected_drive_gives_up, true },
	{ "reset_recovery_keeps_toggles_in_step", test_reset_recovery_keeps_toggles_in_step, true },
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
