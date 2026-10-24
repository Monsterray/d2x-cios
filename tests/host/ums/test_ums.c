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
 * The drive answers a WRITE with CSW status 1 (command failed) once. The
 * driver must not report the write as done while the data is not on the
 * drive: it reads the sense data (which clears the condition) and retries.
 * (Before, `retval > 0 && status != 0` never saw the
 * status, because __cycle() returns 0 on success.)
 */
static void test_write_csw_failure_is_retried(void)
{
	mount();
	CHECK_OK();
	fill(buf, SECTOR, 4);
	sim.fail_write_csw = 1;
	CHECK(USBStorage_Write_Sectors(7, 1, buf), "write failed: %s", sim_summary());
	CHECK(sim.writes_ok == 1 && memcmp(sim.data + 7 * SECTOR, buf, SECTOR) == 0,
	      "the write was not retried: %s", sim_summary());
	CHECK(sim.senses == 1, "%u REQUEST SENSE, expected 1: %s", sim.senses, sim_summary());
	CHECK(sim.port_resets == 0 && sim.bot_resets == 0, "%s", sim_summary());
}

/*
 * The drive STALLs bulk-out at a WRITE's data phase once (a transient
 * error). Bulk-Only Transport 6.7.3: the host clears the halt and reads the
 * CSW (status 1), then retries. Done twice, a READ between them, so the
 * clear-halt meets the host's bulk-out toggle at DATA0 once and at DATA1
 * once: neither may lose a transfer or need a port reset.
 * (Before, __cycle()'s inner loop resent to the halted endpoint forever.)
 */
static void test_write_recovers_from_one_stall(void)
{
	mount();
	CHECK_OK();
	for (int i = 0; i < 2; i++) {
		if (i)
			CHECK(USBStorage_Read_Sectors(40, 1, back), "read failed: %s", sim_summary());
		fill(buf, SECTOR, 5 + i);
		sim.stall_write_data = 1;
		CHECK(USBStorage_Write_Sectors(9 + i, 1, buf), "write %d failed: %s", i, sim_summary());
		CHECK(memcmp(sim.data + (9 + i) * SECTOR, buf, SECTOR) == 0, "drive holds other data");
	}
	CHECK(sim.clear_halts == 2, "%u clear-halts, expected 2: %s", sim.clear_halts, sim_summary());
	CHECK(sim.toggle_losses == 0 && sim.port_resets == 0 && sim.bot_resets == 0, "%s",
	      sim_summary());
	CHECK(sim.now_us < 100000, "recovery took %s", sim_summary());
}

/*
 * The drive refuses every write (write-protected). USBStorage_Write_Sectors()
 * must give up and return false quickly, so the caller (FAT, the loader) can
 * report an error: USBSTORAGE_SECTOR_TRIES (4) attempts, each a STALL, a
 * clear-halt, the CSW and a REQUEST SENSE.
 * (Before, its retry loop had `if(retry>12) retry=12; // infinite loop`.)
 */
static void test_write_to_protected_drive_gives_up(void)
{
	mount();
	CHECK_OK();
	fill(buf, SECTOR, 6);
	sim.stall_write_data = 1000000;
	CHECK(!USBStorage_Write_Sectors(11, 1, buf), "write to a write-protected drive reported success: %s",
	      sim_summary());
	CHECK(sim.senses == 4 && sim.clear_halts == 4, "expected 4 attempts: %s", sim_summary());
	CHECK(sim.port_resets == 0 && sim.toggle_losses == 0, "%s", sim_summary());
	CHECK(sim.now_us < 100000, "giving up took %s", sim_summary());
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
 * (Before, USB_ClearHalt() in tinyehci/usb.c left the host's toggle as it
 * was: 4 reads cost 3 lost transfers and 4 port resets.)
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

/*
 * BOT 5.1: bCBWCBLength is the valid length of the command block. The
 * simulated drive is SCSI (subclass 6), so READ(10) carries 10.
 * (Before, USBStorage_Open() set ata_protocol with
 * `subclass != SCSI || subclass != RBC`, always true, and every CBW said 12.)
 */
static void test_cbw_carries_command_length(void)
{
	mount();
	CHECK_OK();
	CHECK(USBStorage_Read_Sectors(3, 1, buf), "read failed: %s", sim_summary());
	CHECK(sim.last_cb_op == 0x28 && sim.last_cb_len == 10,
	      "READ(10) CBW length %u, expected 10", sim.last_cb_len);
}

static const struct hc_test tests[] = {
	{ "mount", test_mount, false },
	{ "read", test_read, false },
	{ "write_roundtrip", test_write_roundtrip, false },
	{ "one_sector_write_is_fast", test_one_sector_write_is_fast, false },
	{ "write_after_one_timeout", test_write_after_one_timeout, false },
	{ "write_csw_failure_is_retried", test_write_csw_failure_is_retried, false },
	{ "write_recovers_from_one_stall", test_write_recovers_from_one_stall, false },
	{ "write_to_protected_drive_gives_up", test_write_to_protected_drive_gives_up, false },
	{ "reset_recovery_keeps_toggles_in_step", test_reset_recovery_keeps_toggles_in_step, false },
	{ "cbw_carries_command_length", test_cbw_carries_command_length, false },
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
