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

extern int unplug_device;

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

/*
 * A drive that takes a WRITE's data, STALLs the CSW and, after the
 * clear-halt, never sends it (the SanDisk "USB 3.2Gen1" on the bench Wii).
 * The write cannot be known to have landed, so it must fail, and fail within
 * USBSTORAGE_REQUEST_MS: a caller (USB Loader GX's libfat, a game) must get
 * an error, not a wait that looks like a hang. (Before, about 4 attempts of
 * up to 3 reset recoveries each, every one waiting out a 1 s timeout: tens
 * of seconds on the bench.)
 */
static void test_write_with_lost_status_fails_in_time(void)
{
	mount();
	CHECK_OK();
	fill(buf, SECTOR, 7);
	u64 start = sim.now_us;
	sim.lose_write_csw = 1000000;
	CHECK(!USBStorage_Write_Sectors(12, 1, buf), "a write whose status never came reported success: %s",
	      sim_summary());
	CHECK(sim.now_us - start <= (ums_request_ms + 2000) * 1000ull,
	      "giving up took %.1f s (limit %d ms): %s", (sim.now_us - start) / 1e6,
	      ums_request_ms, sim_summary());
	sim.lose_write_csw = 0;
	CHECK(USBStorage_Read_Sectors(3, 1, buf), "the drive does not read after the failed write: %s",
	      sim_summary());
}

/* The same drive loses one write's status, then behaves: the reset recovery
 * clears it and the retried write succeeds. */
static void test_write_with_one_lost_status_recovers(void)
{
	mount();
	CHECK_OK();
	fill(buf, SECTOR, 8);
	sim.lose_write_csw = 1;
	CHECK(USBStorage_Write_Sectors(13, 1, buf), "write failed: %s", sim_summary());
	CHECK(memcmp(sim.data + 13 * SECTOR, buf, SECTOR) == 0, "drive holds other data");
	CHECK(sim.port_resets == 0, "a lost status cost a port reset (a remount): %s", sim_summary());
}

/* ---- the port: a drive that is gone, and a drive that is there but dead ----
 * "Stall every time" is sim.stall_csw = 1000000: every CSW STALLs, so every
 * request, reset recovery and remount fails while the port shows the drive. */

/* Make the drive dead: two failed requests, the second 15 s after the first
 * remount failed. Leaves the fault on. */
static void make_dead_mounted(void)
{
	sim.stall_csw = 1000000;
	CHECK(!USBStorage_Read_Sectors(19, 1, buf), "request 1 succeeded");
	CHECK(!ums_is_dead(), "dead after one failed request");
	CHECK(!USBStorage_Read_Sectors(19, 1, buf), "request 2 succeeded");
	CHECK(ums_is_dead(), "not dead after two failed requests, %.1f s: %s", sim.now_us / 1e6, sim_summary());
}

static void make_dead(void)
{
	mount();
	CHECK_OK();
	make_dead_mounted();
}

/* A drive that is unplugged is waited for without limit, however long, and
 * the request then succeeds. */
static void test_unplug_waits_until_replug(void)
{
	mount();
	CHECK_OK();
	sim_unplug_for(60 * 1000000ull);
	CHECK(USBStorage_Read_Sectors(10, 1, buf), "read failed after the replug: %s", sim_summary());
	CHECK(sim.now_us >= 60 * 1000000ull && sim.now_us < 70 * 1000000ull, "took %.1f s", sim.now_us / 1e6);
	CHECK(memcmp(buf, sim.data + 10 * SECTOR, SECTOR) == 0, "other data");
	CHECK(!ums_is_dead(), "dead after a replug");
}

/* A drive that is there but never answers: request 1 fails at its 8 s
 * deadline, request 2 makes it dead (15 s after the first failed remount),
 * and from then on requests fail at once with no USB traffic. */
static void test_dead_drive_fails_fast(void)
{
	u64 t;
	u32 cbws;
	make_dead();
	CHECK(sim.now_us >= 15 * 1000000ull && sim.now_us <= 25 * 1000000ull, "dead after %.1f s", sim.now_us / 1e6);
	t = sim.now_us;
	cbws = sim.cbws;
	sim.port_xfers_gone = 0;
	for (int i = 0; i < 5; i++) {
		CHECK(!USBStorage_Read_Sectors(19 + i, 1, buf), "dead drive read ok");
	}
	CHECK(!USBStorage_Write_Sectors(30, 1, buf), "dead drive write ok");
	CHECK(sim.now_us - t < 100000, "a dead drive took %.3f s", (sim.now_us - t) / 1e6);
	CHECK(sim.cbws == cbws && sim.port_xfers_gone == 0, "USB traffic to a dead drive: %s", sim_summary());
	/* GX asks for the capacity and ignores UMS_INIT's result: it stays valid */
	CHECK(ums_sector_size() == SECTOR, "capacity lost: %u", ums_sector_size());
}

/* A replug (CSC after it died, or CCS 0) ends the dead state. */
static void test_replug_clears_dead(void)
{
	make_dead();
	sim.stall_csw = 0;
	sim.csc = true;                 /* unplugged and back between two requests */
	CHECK(USBStorage_Read_Sectors(21, 1, buf), "read failed after the replug: %s", sim_summary());
	CHECK(!ums_is_dead(), "still dead after a replug");

	/* and again, by the other way: CCS 0 for a while */
	sim.stall_csw = 1000000;
	sim.now_us += 100 * 1000000ull;
	CHECK(!USBStorage_Read_Sectors(19, 1, buf), "request succeeded");
	CHECK(!USBStorage_Read_Sectors(19, 1, buf), "request succeeded");
	CHECK(ums_is_dead(), "not dead again: %s", sim_summary());
	sim.stall_csw = 0;
	sim_unplug_for(3 * 1000000ull);
	CHECK(USBStorage_Read_Sectors(22, 1, buf), "read failed after the second replug: %s", sim_summary());
	CHECK(!ums_is_dead(), "still dead after the second replug");
}

/* UMS_INIT ends the dead state; with the fault still on the drive is dead
 * again and the call is non-zero. */
static void test_ums_init_clears_dead(void)
{
	make_dead();
	CHECK(ums_init() != 0, "UMS_INIT succeeded on a drive that cannot mount");
	CHECK(ums_is_dead(), "not dead after a failed UMS_INIT");
	CHECK(ums_sector_size() == SECTOR, "capacity lost");
	sim.stall_csw = 0;
	CHECK(ums_init() == 0, "UMS_INIT failed on a working drive: %s", sim_summary());
	CHECK(!ums_is_dead(), "still dead after UMS_INIT");
	CHECK(USBStorage_Read_Sectors(23, 1, buf), "read failed: %s", sim_summary());
}

/* The keepalive does not touch a dead drive (no timed retry: it would stall
 * the module's one thread, and the SD card with it, at random moments) and
 * does not start the wait for an unplugged one, for the same reason. */
static void test_keepalive_skips_dead_and_unplugged(void)
{
	u32 cbws;
	mount();
	CHECK_OK();
	CHECK(ums_keepalive_ready(), "a healthy drive is not kept alive");
	unplug_device = 1;
	sim_unplug_for(0);
	CHECK(!ums_keepalive_ready(), "the keepalive would wait for an unplugged drive");
	sim.disconnected = false;
	sim.csc = true;
	CHECK(ums_keepalive_ready(), "a drive that is back is not kept alive");
	unplug_device = 0;

	make_dead_mounted();
	cbws = sim.cbws;
	sim.port_xfers_gone = 0;
	for (int i = 0; i < 12; i++) {
		CHECK(!ums_keepalive_ready(), "the keepalive reads a dead drive");
		sim.now_us += 10 * 1000000ull;
	}
	CHECK(ums_is_dead(), "something ended the dead state without a replug or UMS_INIT");
	CHECK(sim.cbws == cbws && sim.port_xfers_gone == 0,
	      "USB traffic to a dead drive in 2 minutes: %s", sim_summary());
}

/* A dead drive that recovers on its own stays dead (no timed retry) until
 * the loader asks again with UMS_INIT, or the drive is replugged. */
static void test_dead_needs_init_or_replug(void)
{
	u32 cbws;
	make_dead();
	sim.stall_csw = 0;
	cbws = sim.cbws;
	sim.now_us += 60 * 1000000ull;
	CHECK(!ums_keepalive_ready(), "the keepalive reads a dead drive");
	CHECK(ums_is_dead(), "revived with no replug or UMS_INIT");
	CHECK(sim.cbws == cbws, "USB traffic to a dead drive: %s", sim_summary());
	CHECK(ums_init() == 0, "UMS_INIT failed on a working drive: %s", sim_summary());
	CHECK(ums_keepalive_ready(), "not ready after UMS_INIT");
	CHECK(USBStorage_Read_Sectors(24, 1, buf), "read failed: %s", sim_summary());
}

/* --- A drive that never answers: UMS_INIT and the open return an error --- */

#define TEN_S 10000000ull

/* A drive left stuck by a console reset answers no transfer at all and a port
 * reset does not help (devwii, 2026-11-08). UMS_INIT used to retry without
 * end; it must fail within 10 s, and the drive stays unmounted, so requests
 * fail at once with no USB traffic. */
static void test_init_silent_drive_fails_in_time(void)
{
	s32 r;
	u32 xfers;
	sim.silent = true;
	r = ums_init();
	CHECK(r < 0, "UMS_INIT succeeded on a silent drive: %s", sim_summary());
	CHECK(sim.now_us < TEN_S, "UMS_INIT took %s", sim_summary());
	CHECK(sim.now_us > 1000000, "gave up before trying: %s", sim_summary());
	CHECK(!ums_mounted(), "mounted a silent drive");
	xfers = sim.silent_xfers;
	CHECK(!USBStorage_Read_Sectors(1, 1, buf), "read from an unmounted drive");
	CHECK(sim.silent_xfers == xfers && sim.now_us < TEN_S, "traffic to an unmounted drive: %s", sim_summary());
}

/* The same when the module already enumerated the drive once (its id is
 * set): UMS_INIT resets the port and enumerates again. */
static void test_init_silent_drive_with_id_fails_in_time(void)
{
	sim.silent = true;
	ums_set_dev_id(1);
	CHECK(ums_init() < 0, "UMS_INIT succeeded on a silent drive: %s", sim_summary());
	/* The 8 s budget, and the port reset that ends a failed UMS_INIT (which
	   runs on the Wii too: the simulated controller has one port, the Wii's
	   module loops over four and ends the same way). It used to end after the
	   budget alone because the loop over the ports stopped at one. */
	CHECK(sim.now_us < TEN_S + 4000000ull, "UMS_INIT took %s", sim_summary());
	CHECK(!ums_mounted(), "mounted a silent drive");
}

/* A second UMS_INIT after the drive woke up works: no budget is left armed */
static void test_init_after_a_silent_drive_works(void)
{
	sim.silent = true;
	CHECK(ums_init() < 0, "UMS_INIT succeeded on a silent drive");
	sim.silent = false;
	ums_set_dev_id(1);      /* enumerated by the port reset the first call ended with */
	CHECK(ums_init() == 0, "UMS_INIT failed on a working drive: %s", sim_summary());
	CHECK(ums_mounted(), "not mounted");
}

/* The loop in ehci_init_port() after the descriptor: a drive that gives its
 * descriptor but takes no address kept it for ever (cnt never grew). It is
 * bounded by its tries even with no time budget armed (the recovery ladder's
 * port reset, the open of /dev/usb2 before the first budget). */
static void test_enumeration_without_set_address_ends(void)
{
	sim.no_setaddr = true;
	CHECK(ums_enum_port() < 0, "enumerated a drive that takes no address");
	CHECK(sim.now_us < 60 * 1000000ull, "took %s", sim_summary());
}

/* A silent drive with no budget armed: counted retries only, but they end */
static void test_enumeration_of_a_silent_drive_ends(void)
{
	sim.silent = true;
	CHECK(ums_enum_port() < 0, "enumerated a silent drive");
	CHECK(sim.silent_xfers <= 3 + 3 + 3 + 1, "%u transfers", sim.silent_xfers);
}

/* A healthy drive enumerates at once */
static void test_enumeration_of_a_healthy_drive(void)
{
	CHECK(ums_enum_port() >= 0, "healthy drive did not enumerate");
	CHECK(sim.now_us < 2000000, "took %s", sim_summary());
}

/* --- Step 3: a failed transfer's QH is released through the doorbell --- */

/* The controller answers the async advance doorbell: ehci_async_quiesce()
 * rings it, waits for IAA, clears it (W1C) and the doorbell bit, and leaves
 * the schedule running. */
static void test_quiesce_waits_for_iaa(void)
{
	CHECK(ums_quiesce() == 0, "needed the schedule halted");
	CHECK(sim.doorbells == 1, "%u doorbells", sim.doorbells);
	CHECK(sim.ase_clears == 0, "halted the schedule although IAA came");
	CHECK(!sim.iaa_set, "IAA left set (it is write-1-to-clear)");
	CHECK(!(sim.hc_cmd & 0x40), "IAAD left set");
	CHECK(sim.now_us < 20000, "took %.1f ms", sim.now_us / 1e3);
}

/* No doorbell: the schedule is halted (CMD_ASE cleared, STS_ASS waited for),
 * the caller frees, and the schedule is enabled again. It is bounded. */
static void test_quiesce_without_iaa_halts_the_schedule(void)
{
	sim.iaa_never = true;
	CHECK(ums_quiesce() == 1, "did not fall back to halting the schedule");
	CHECK(sim.doorbells == 1 && sim.ase_clears == 1 && sim.ase_sets == 1, "doorbells %u, ASE cleared %u, set %u",
	      sim.doorbells, sim.ase_clears, sim.ase_sets);
	CHECK((sim.hc_cmd & 0x20) && sim.hc_ass, "the schedule is not running again");
	CHECK(sim.async_next == 0x1000, "the list address was not set to the head: %x", sim.async_next);
	CHECK(sim.now_us < 25000, "took %.1f ms", sim.now_us / 1e3);
}

/* Not even STS_ASS clears: still bounded (IAA wait + ASS wait). */
static void test_quiesce_is_bounded_when_the_controller_is_wedged(void)
{
	sim.iaa_never = true;
	sim.ass_stuck = true;
	CHECK(ums_quiesce() == 1, "returned %d", 1);
	CHECK(sim.now_us < 45000, "took %.1f ms", sim.now_us / 1e3);
	CHECK(sim.now_us >= 39000, "gave up after %.1f ms", sim.now_us / 1e3);
}

/* Nothing holds a QH when the schedule is off */
static void test_quiesce_skips_an_idle_schedule(void)
{
	sim.hc_ass = false;
	CHECK(ums_quiesce() == 0 && sim.doorbells == 0 && sim.now_us == 0, "rang the doorbell of an idle controller");
}

/* --- Step 4: the ports are reset at start --- */

static void test_startup_resets_a_connected_port(void)
{
	sim.ppc = false;
	ums_startup();
	CHECK(sim.port_resets == 1, "%u port resets", sim.port_resets);
	CHECK(sim.power_offs == 0 && sim.power_cycles == 0, "a port without power switches was power cycled");
	printf("    added at start, PPC 0: %.0f ms\n", sim.now_us / 1e3);
}

static void test_startup_power_cycles_with_ppc(void)
{
	sim.ppc = true;
	ums_startup();
	CHECK(sim.power_offs == 1 && sim.power_cycles == 1, "power off %u, on %u", sim.power_offs, sim.power_cycles);
	CHECK(sim.port_resets == 1, "%u port resets", sim.port_resets);
	CHECK(!sim.disconnected && !sim.unpowered, "the drive is not back");
	CHECK(!sim.csc, "the power cycle's own connect change was left set");
	CHECK(sim.now_us < 1500000, "added %.0f ms", sim.now_us / 1e3);
	printf("    added at start, PPC 1: %.0f ms\n", sim.now_us / 1e3);
}

static void test_startup_skips_an_empty_port(void)
{
	sim.ppc = true;
	sim.disconnected = true;
	ums_startup();
	CHECK(sim.port_resets == 0 && sim.power_offs == 0 && sim.now_us == 0, "touched a port with no drive");
}

/* Without PPC a power cycle is refused and writes nothing */
static void test_power_cycle_needs_ppc(void)
{
	sim.ppc = false;
	CHECK(ums_power_cycle() < 0, "power cycled without PPC");
	CHECK(sim.portsc_writes == 0, "wrote PORTSC");
}

/* A drive that an earlier reset left stuck mounts after the start-up reset */
static void test_stuck_drive_mounts_after_a_port_reset(void)
{
	sim.stuck = 1;
	sim.ppc = false;
	CHECK(ums_plug() != 0, "a stuck drive mounted without the start-up reset");
	ums_unplug();
	ums_startup();
	CHECK(sim.stuck == 0, "the start-up reset did not restart the drive");
	CHECK(ums_plug() == 0, "mount failed after the start-up reset: %s", sim_summary());
}

static void test_stuck_drive_mounts_after_a_power_cycle(void)
{
	sim.stuck = 2;
	sim.ppc = true;
	ums_startup();
	CHECK(sim.stuck == 0, "the power cycle did not restart the drive");
	CHECK(ums_plug() == 0, "mount failed after the start-up power cycle: %s", sim_summary());
	CHECK(USBStorage_Read_Sectors(5, 1, buf), "read failed: %s", sim_summary());
}

/* Without power switches that drive stays stuck: the limit, stated */
static void test_power_stuck_drive_stays_stuck_without_ppc(void)
{
	sim.stuck = 2;
	sim.ppc = false;
	ums_startup();
	CHECK(sim.stuck == 2, "restarted without a power cycle");
	CHECK(ums_plug() != 0, "mounted");
}

/* --- Step 5: the ladder within one request's deadline --- */

#define IN_TIME(what) CHECK(sim.now_us <= (u64)(ums_request_ms + 2000) * 1000, "%s: %.1f s", what, sim.now_us / 1e6)

/* A fault that only a port reset clears: rung 3 */
static void test_ladder_port_reset_clears_the_fault(void)
{
	mount();
	CHECK_OK();
	sim.stuck = 1;
	CHECK(USBStorage_Read_Sectors(40, 4, buf), "read failed: %s", sim_summary());
	CHECK(memcmp(buf, sim.data + 40 * SECTOR, 4 * SECTOR) == 0, "other data");
	CHECK(sim.port_resets >= 1 && sim.power_cycles == 0, "resets %u, power cycles %u", sim.port_resets, sim.power_cycles);
	IN_TIME("recovered");
}

/* Only a power cycle clears it, and the ports have power switches: rung 4 */
static void test_ladder_power_cycle_clears_the_fault(void)
{
	mount();
	CHECK_OK();
	sim.ppc = true;
	sim.stuck = 2;
	CHECK(USBStorage_Read_Sectors(41, 4, buf), "read failed: %s", sim_summary());
	CHECK(memcmp(buf, sim.data + 41 * SECTOR, 4 * SECTOR) == 0, "other data");
	CHECK(sim.power_cycles == 1, "%u power cycles", sim.power_cycles);
	IN_TIME("recovered");
}

/* The same, but the drive mounts after the port reset and only reads and
 * writes stay stuck: the second failure in the request escalates. */
static void test_ladder_escalates_when_the_remount_works(void)
{
	mount();
	CHECK_OK();
	sim.ppc = true;
	sim.stuck = 2;
	sim.stuck_io_only = true;
	fill(buf, 8 * SECTOR, 9);
	CHECK(USBStorage_Write_Sectors(42, 8, buf), "write failed: %s", sim_summary());
	CHECK(memcmp(sim.data + 42 * SECTOR, buf, 8 * SECTOR) == 0, "the data did not land");
	CHECK(sim.port_resets >= 1 && sim.power_cycles == 1, "resets %u, power cycles %u", sim.port_resets, sim.power_cycles);
	IN_TIME("recovered");
}

/* Without PPC there is no rung 4: the request fails at its deadline, not later */
static void test_ladder_without_ppc_fails_in_time(void)
{
	mount();
	CHECK_OK();
	sim.ppc = false;
	sim.stuck = 2;
	CHECK(!USBStorage_Read_Sectors(43, 4, buf), "read succeeded");
	CHECK(sim.power_cycles == 0 && sim.power_offs == 0, "power cycled without PPC");
	IN_TIME("gave up");
}

/* A fault nothing clears, with PPC: one power cycle, then the deadline */
static void test_ladder_unfixable_fault_fails_in_time(void)
{
	mount();
	CHECK_OK();
	sim.ppc = true;
	sim.stall_csw = 1000000;
	CHECK(!USBStorage_Read_Sectors(44, 4, buf), "read succeeded");
	CHECK(sim.power_cycles <= 1, "%u power cycles in one request", sim.power_cycles);
	IN_TIME("gave up");
}

/* ---- two drives: logical ports 0 and 1 (docs/usb-ports.md) ----
 * Drive 0 is on root port 0, drive 1 in the second socket, which EHCI gave to
 * OHCI at boot (sim.owner). The two differ in data and in size. A test works
 * on a port by ums_select(), as loop.c does for each request, and on a
 * simulated drive by sim_sel(). */

/* READ CAPACITY gives the last LBA, and the driver keeps it as it is */
#define CAP(i) (sims[i].sectors - 1)

/* UMS_INIT on logical port 1 with a drive in the second socket; leaves port 1
 * selected */
static void mount_port1(void)
{
	sim_sel(1);
	sim.disconnected = false;
	ums_select(1);
	CHECK(ums_init() == 0, "UMS_INIT on port 1 failed: %s", sim_summary());
	CHECK(ums_mounted(), "port 1 not mounted");
}

/* Drive 0 mounted as the default port, drive 1 by UMS_INIT on port 1; leaves
 * port 0 selected */
static void mount_both(void)
{
	mount();
	mount_port1();
	ums_select(0);
	sim_sel(0);
}

/* A read on a logical port, the data compared with that port's drive */
static bool read_port(int port, u32 sector, u32 n)
{
	ums_select(port);
	if (!USBStorage_Read_Sectors(sector, n, buf))
		return false;
	return memcmp(buf, sims[port].data + (size_t)sector * SECTOR, (size_t)n * SECTOR) == 0;
}

/* The second socket belongs to OHCI until the first UMS_INIT on port 1 takes it
 * back, once. */
static void test_port1_init_acquires_once(void)
{
	CHECK(sims[1].owner, "the second socket is not with OHCI at the start");
	mount_port1();
	CHECK(sims[1].acquires == 1, "acquired %u times", sims[1].acquires);
	CHECK(!sims[1].owner, "the port is still with OHCI");
	CHECK(ums_sector_size() == SECTOR && ums_sectors() == CAP(1), "capacity %u", ums_sectors());
	CHECK(read_port(1, 7, 2), "read of port 1 failed: %s", sim_summary());
	/* a second UMS_INIT (GX sends one per port, again) does not take it again */
	CHECK(ums_init() == 0, "second UMS_INIT failed");
	CHECK(sims[1].acquires == 1, "acquired %u times", sims[1].acquires);
	CHECK(read_port(1, 9, 1), "read failed after the second UMS_INIT");
	/* and port 0 was not touched */
	ums_select(0);
	CHECK(!ums_mounted(), "port 0 mounted by a UMS_INIT on port 1");
	CHECK(sims[0].port_resets == 0 && sims[0].portsc_writes == 0, "port 0 touched: %s", sim_summary());
}

/* UMS_INIT on port 1 with nothing in the socket fails at once, mounts nothing and
 * leaves port 0 as it was */
static void test_port1_init_without_a_drive_fails(void)
{
	mount();
	CHECK_OK();
	ums_select(1);
	CHECK(ums_init() != 0, "UMS_INIT succeeded with no drive in the second socket");
	CHECK(!ums_mounted(), "mounted nothing");
	CHECK(sim.now_us < 2000000, "took %s", sim_summary());
	CHECK(!USBStorage_Read_Sectors(1, 1, buf), "read from an empty port");
	CHECK(read_port(0, 3, 1), "port 0 read failed: %s", sim_summary());
	CHECK(sims[1].owner, "an empty second socket stayed with EHCI: a keyboard in it never reaches OHCI");
	/* the drive is plugged in later: the next UMS_INIT finds it */
	sim_sel(1);
	sim.disconnected = false;
	ums_select(1);
	CHECK(ums_init() == 0, "UMS_INIT failed once the drive is there: %s", sim_summary());
	CHECK(read_port(1, 3, 1), "read of port 1 failed");
}

/* Each port has its own drive: data, capacity, reads alternating between them,
 * and a write lands on its own drive only */
static void test_two_drives_alternate(void)
{
	u32 c0, c1, r0, r1;
	mount_both();
	CHECK_OK();
	r0 = sims[0].bot_resets;        /* mounting has its own */
	r1 = sims[1].bot_resets;
	ums_select(0);
	c0 = ums_sectors();
	ums_select(1);
	c1 = ums_sectors();
	CHECK(c0 == CAP(0) && c1 == CAP(1) && c0 != c1, "capacities %u and %u", c0, c1);
	CHECK(memcmp(sims[0].data + 10 * SECTOR, sims[1].data + 10 * SECTOR, SECTOR) != 0, "the drives hold the same data");
	for (int i = 0; i < 24; i++)
		CHECK(read_port(i & 1, 10 + i, 1 + (i % 5)), "read %d on port %d failed: %s", i, i & 1, sim_summary());

	fill(buf, 8 * SECTOR, 0x42);
	back[0] = sims[0].data[300 * SECTOR];
	ums_select(1);
	CHECK(USBStorage_Write_Sectors(300, 8, buf), "write failed: %s", sim_summary());
	CHECK(memcmp(sims[1].data + 300 * SECTOR, buf, 8 * SECTOR) == 0, "the data did not land on drive 1");
	CHECK(sims[0].data[300 * SECTOR] == back[0], "drive 0 changed by a write to port 1");
	CHECK(sims[0].writes_ok == 0, "drive 0 saw a write");
	CHECK(read_port(0, 300, 8) && read_port(1, 300, 8), "read back failed");
	CHECK(sims[0].bot_resets == r0 && sims[1].bot_resets == r1, "BOT resets while alternating");
}

/* A drive unplugged on port 1 does not stop port 0: its request fails after
 * UMS_GONE_MS (3 s), then at once, and the drive on port 0 is read the whole time. */
static void test_port1_gone_fails_after_a_bound_then_at_once(void)
{
	u64 t;
	mount_both();
	CHECK_OK();
	sim_sel(1);
	sim_unplug_for(0);
	t = sim.now_us;
	CHECK(!read_port(1, 20, 1), "read from an unplugged drive succeeded");
	CHECK(sim.now_us - t >= 3000000ull && sim.now_us - t <= 4500000ull, "gave up after %.2f s", (sim.now_us - t) / 1e6);
	CHECK(read_port(0, 21, 4), "port 0 read failed: %s", sim_summary());
	t = sim.now_us;
	for (int i = 0; i < 5; i++)
		CHECK(!read_port(1, 22 + i, 1), "a request to the gone drive succeeded");
	CHECK(sim.now_us - t < 100000, "failing at once took %.3f s", (sim.now_us - t) / 1e6);
	CHECK(read_port(0, 30, 2), "port 0 read failed after: %s", sim_summary());
	ums_select(0);
	CHECK(!unplug_device, "the unplug of port 1 marked port 0");
	CHECK(ums_keepalive_ready(), "port 0 is not kept alive");
	ums_select(1);
	CHECK(!ums_keepalive_ready(), "the keepalive would wait for the gone drive on port 1");
}

/* The drive comes back (CSC): port 1 recovers, with its own data, and port 0
 * is as it was */
static void test_port1_replug_recovers(void)
{
	mount_both();
	CHECK_OK();
	sim_sel(1);
	sim_unplug_for(0);
	CHECK(!read_port(1, 20, 1), "read from an unplugged drive succeeded");
	CHECK(!read_port(1, 21, 1), "read succeeded while gone");
	sim_sel(1);
	sim.disconnected = false;
	sim.csc = true;
	CHECK(read_port(1, 22, 3), "read failed after the replug: %s", sim_summary());
	CHECK(read_port(1, 400, 1) && read_port(0, 401, 1), "reads failed after the replug");
	/* gone again, and back before the bound runs out: the request waits and succeeds */
	sim_sel(1);
	sim_unplug_for(1500000ull);
	CHECK(read_port(1, 23, 2), "read failed with a replug inside the bound: %s", sim_summary());
	CHECK(read_port(0, 24, 1), "port 0 failed: %s", sim_summary());
}

/* A drive on port 1 that is there but answers nothing: its request fails at the
 * deadline (as port 0's does), and port 0 keeps working */
static void test_port1_stuck_drive_errors_at_the_deadline(void)
{
	u64 t;
	mount_both();
	CHECK_OK();
	sim_sel(1);
	sim.ppc = false;
	sim.stall_csw = 1000000;
	t = sim.now_us;
	CHECK(!read_port(1, 50, 1), "read from a stuck drive succeeded");
	CHECK(sim.now_us - t <= (u64)(ums_request_ms + 2000) * 1000, "gave up after %.1f s", (sim.now_us - t) / 1e6);
	CHECK(read_port(0, 51, 4), "port 0 read failed: %s", sim_summary());
	CHECK(!read_port(1, 52, 1), "second read from a stuck drive succeeded");
	ums_select(1);
	CHECK(ums_is_dead(), "port 1 not dead after two failed requests");
	ums_select(0);
	CHECK(!ums_is_dead(), "port 0 dead because port 1 is");
	CHECK(read_port(0, 53, 1), "port 0 read failed after port 1 died");
	/* UMS_INIT on port 1 gives the drive its chance again */
	sim_sel(1);
	sim.stall_csw = 0;
	ums_select(1);
	CHECK(ums_init() == 0, "UMS_INIT on port 1 failed: %s", sim_summary());
	CHECK(read_port(1, 54, 1), "read failed after UMS_INIT");
}

/* Without a word about ports the module is as before: port 0, root port 0,
 * and the second socket is not touched */
static void test_default_port_is_unchanged(void)
{
	mount();
	CHECK_OK();
	CHECK(ums_selected() == 0, "port %d selected at start", ums_selected());
	CHECK(USBStorage_Read_Sectors(10, 3, buf), "read failed: %s", sim_summary());
	CHECK(memcmp(buf, sims[0].data + 10 * SECTOR, 3 * SECTOR) == 0, "other data");
	CHECK(ums_sectors() == CAP(0), "capacity");
	CHECK(sims[1].portsc_writes == 0 && sims[1].acquires == 0 && sims[1].cbws == 0 && sims[1].owner,
	      "the second socket was touched");
	CHECK(ums_select(2) < 0 && ums_selected() == 0, "a port 2 was accepted");
	sim_unplug_for(10 * 1000000ull);
	CHECK(USBStorage_Read_Sectors(10, 1, buf), "port 0 gave up waiting for a replug: %s", sim_summary());
	CHECK(sim.now_us >= 10 * 1000000ull, "did not wait for the replug");
}

/* loop.c's fd -> port table */
static void test_fd_port_table(void)
{
	CHECK(ums_fd_port(3) == 0 && ums_fd_port(0) == 0, "default is not port 0");
	CHECK(ums_fd_set_port(3, 1) == 0 && ums_fd_port(3) == 1, "SET_PORT 1 not kept");
	CHECK(ums_fd_port(4) == 0 && ums_fd_port(2) == 0, "another fd moved with it");
	CHECK(ums_fd_set_port(3, 2) < 0 && ums_fd_is_bad(3), "port 2 accepted, or the fd not marked");
	CHECK(ums_fd_set_port(3, 1) == 0 && ums_fd_port(3) == 1, "SET_PORT 1 after a refused one not kept");
	CHECK(ums_fd_set_port(3, 0) == 0 && ums_fd_port(3) == 0, "back to port 0 not kept");
	CHECK(ums_fd_set_port(9, 0) == 0 && ums_fd_port(9) == 0, "port 0 for a new fd");
	/* the table has 12 slots for ports (fd 3 gave its slot back on port 0) */
	for (int fd = 20; fd < 32; fd++)
		CHECK(ums_fd_set_port(fd, 1) == 0, "slot %d refused", fd - 20);
	CHECK(ums_fd_set_port(40, 1) < 0, "a thirteenth fd on port 1 was accepted");
	CHECK(ums_fd_set_port(40, 0) == 0 && ums_fd_port(40) == 0, "port 0 needs no slot");
	for (int fd = 20; fd < 32; fd++)
		CHECK(ums_fd_port(fd) == 1, "fd %d lost its port", fd);
	/* close: the slot is free and the fd is on port 0 again (fd numbers are reused) */
	ums_fd_forget(23);
	CHECK(ums_fd_port(23) == 0, "closed fd keeps its port");
	CHECK(ums_fd_set_port(41, 1) == 0 && ums_fd_port(41) == 1, "the freed slot was not reused");
	CHECK(ums_fd_port(22) == 1 && ums_fd_port(24) == 1, "close moved another fd");
}

/* The second socket goes back to OHCI whenever nothing is mounted on it: a
 * device that is not storage (a LAN adapter) and an empty socket must not stay
 * with EHCI, where IOS never sees what is plugged in. A drive is taken again
 * by the next UMS_INIT. */
static void test_port1_not_a_drive_goes_back_to_ohci(void)
{
	mount();
	CHECK_OK();
	/* something whose enumeration fails, as a device that is not storage does */
	sim_sel(1);
	sim.disconnected = false;
	sim.silent = true;
	ums_select(1);
	CHECK(ums_init() < 0, "UMS_INIT mounted something that is not a drive");
	CHECK(sims[1].owner, "the port stayed with EHCI after a failed UMS_INIT");
	CHECK(ums_dev_id(1) == 0, "the device is still known");
	CHECK(sims[1].acquires == 1, "acquired %u times", sims[1].acquires);
	CHECK(!sims[0].owner && read_port(0, 5, 1), "port 0 touched: %s", sim_summary());
	/* a drive in its place: the next UMS_INIT takes the port again */
	sim_sel(1);
	sim.silent = false;
	ums_select(1);
	CHECK(ums_init() == 0, "UMS_INIT failed with a drive: %s", sim_summary());
	CHECK(!sims[1].owner && sims[1].acquires == 2, "not taken again (owner %d, %u acquires)", sims[1].owner, sims[1].acquires);
	CHECK(read_port(1, 9, 2), "read of port 1 failed");
}

/* UMS_UMOUNT on port 1 gives the socket back; port 0 stays mounted and its
 * socket is never written */
static void test_port1_umount_goes_back_to_ohci(void)
{
	u32 w0;
	mount_both();
	CHECK_OK();
	CHECK(!sims[1].owner, "the second socket is not EHCI's after the mount");
	sim_sel(0);
	w0 = sims[0].portsc_writes;
	ums_select(1);
	ums_umount();
	CHECK(!ums_mounted(), "still mounted after UMS_UMOUNT");
	CHECK(sims[1].owner, "the port stayed with EHCI after UMS_UMOUNT");
	CHECK(ums_dev_id(1) == 0, "the device is still known");
	CHECK(sims[0].portsc_writes == w0 && !sims[0].owner, "port 0's socket was written");
	CHECK(read_port(0, 12, 2), "port 0 read failed after the umount: %s", sim_summary());
	/* mount it again */
	sim_sel(1);
	ums_select(1);
	CHECK(ums_init() == 0, "UMS_INIT after the umount failed: %s", sim_summary());
	CHECK(!sims[1].owner && sims[1].acquires == 2, "not taken again (%u acquires)", sims[1].acquires);
	CHECK(read_port(1, 13, 1), "read of port 1 failed after the remount");
	/* UMS_UMOUNT on port 0 does not touch port 1's socket */
	ums_select(0);
	ums_umount();
	CHECK(!sims[1].owner, "UMS_UMOUNT on port 0 gave port 1's socket away");
	CHECK(sims[0].owner == false, "port 0's socket was released");
}

/* An fd whose SET_PORT could not be honoured must not reach another port's
 * drive: with the slots all taken, the next fd on port 1 gets errors, not port 0 */
static void test_fd_set_port_refused_fails_the_requests(void)
{
	u32 c0;
	mount_both();
	CHECK_OK();
	for (int fd = 20; fd < 32; fd++)
		CHECK(ums_fd_set_port(fd, 1) == 0, "slot %d refused", fd - 20);
	CHECK(ums_fd_set_port(40, 1) < 0, "a thirteenth fd on port 1 was accepted");
	CHECK(ums_fd_is_bad(40), "the refused fd is not marked");
	for (int fd = 20; fd < 32; fd++)
		CHECK(!ums_fd_is_bad(fd), "fd %d marked", fd);
	sim_sel(0);
	c0 = sims[0].cbws;
	CHECK(!ums_fd_read(40, 10, 1, buf), "a read of the refused fd succeeded");
	CHECK(sims[0].cbws == c0, "the refused fd's read reached port 0's drive");
	CHECK(ums_fd_read(20, 10, 1, buf) && memcmp(buf, sims[1].data + 10 * SECTOR, SECTOR) == 0, "fd 20 does not read port 1");
	CHECK(ums_fd_read(3, 10, 1, buf) && memcmp(buf, sims[0].data + 10 * SECTOR, SECTOR) == 0, "an fd that never sent SET_PORT does not read port 0");
	/* it asks for port 0: it is a normal fd on port 0 again */
	CHECK(ums_fd_set_port(40, 0) == 0 && !ums_fd_is_bad(40), "SET_PORT 0 does not clear the mark");
	CHECK(ums_fd_read(40, 11, 1, buf) && memcmp(buf, sims[0].data + 11 * SECTOR, SECTOR) == 0, "fd 40 does not read port 0");
	/* a slot frees up: the next try works */
	CHECK(ums_fd_set_port(40, 1) < 0 && ums_fd_is_bad(40), "the refused fd was accepted again");
	ums_fd_forget(22);
	CHECK(ums_fd_set_port(40, 1) == 0 && !ums_fd_is_bad(40), "the freed slot was not used");
	CHECK(ums_fd_read(40, 12, 1, buf) && memcmp(buf, sims[1].data + 12 * SECTOR, SECTOR) == 0, "fd 40 does not read port 1");
	/* a port that does not exist: refused too, and the close clears the mark */
	CHECK(ums_fd_set_port(41, 2) < 0 && ums_fd_is_bad(41), "port 2 not marked");
	CHECK(!ums_fd_read(41, 10, 1, buf), "a read of an fd on port 2 succeeded");
	ums_fd_forget(41);
	CHECK(!ums_fd_is_bad(41) && ums_fd_read(41, 10, 1, buf), "the closed fd keeps the mark");
	/* an fd on port 1 that asks for a port that is not there is cut off, not left on port 1 */
	CHECK(ums_fd_set_port(20, 3) < 0 && !ums_fd_read(20, 10, 1, buf), "fd 20 still reads after a refused SET_PORT");
	CHECK(ums_fd_set_port(20, 1) == 0 && ums_fd_read(20, 10, 1, buf), "fd 20 did not come back");
	/* the spare slots hold the marks of refused fds: fd 40 has one, so three
	   more refused fds are cut off (fd 50 finds the slot that fd 22 gave back) */
	for (int fd = 50; fd < 54; fd++)
		ums_fd_set_port(fd, 1);
	CHECK(ums_fd_port(50) == 1 && !ums_fd_is_bad(50), "fd 50 did not get the freed slot");
	for (int fd = 51; fd < 54; fd++)
		CHECK(ums_fd_is_bad(fd), "fd %d is not marked", fd);
}

/* The keepalive runs while an fd that sent UMS_INIT is open: the FAT module
 * closing its client (a port change) must not stop it for the loader */
static void test_keepalive_follows_open_fds_that_sent_init(void)
{
	CHECK(ums_fd_init_count() == 0, "count %u at the start", ums_fd_init_count());
	ums_fd_init(3);          /* the loader */
	ums_fd_init(3);          /* once per port: still one fd */
	CHECK(ums_fd_init_count() == 1, "one fd counted %u", ums_fd_init_count());
	ums_fd_init(4);          /* the FAT module, the last to send UMS_INIT */
	CHECK(ums_fd_init_count() == 2, "two fds counted %u", ums_fd_init_count());
	CHECK(ums_fd_close(4) == 1, "the FAT module's close not recognised");
	CHECK(ums_fd_init_count() == 1, "the loader's UMS_INIT was forgotten with the FAT module's close");
	CHECK(ums_fd_close(7) == 0 && ums_fd_init_count() == 1, "an fd that never sent UMS_INIT changed the count");
	CHECK(ums_fd_close(3) == 1 && ums_fd_init_count() == 0, "the last close does not stop it");
	CHECK(ums_fd_close(3) == 0, "a second close counted");
	/* the port and the UMS_INIT mark share a slot and do not disturb each other */
	CHECK(ums_fd_set_port(5, 1) == 0 && ums_fd_port(5) == 1, "SET_PORT 1");
	ums_fd_init(5);
	CHECK(ums_fd_port(5) == 1 && ums_fd_init_count() == 1, "UMS_INIT changed the port, or was not counted");
	CHECK(ums_fd_set_port(5, 0) == 0 && ums_fd_port(5) == 0 && ums_fd_init_count() == 1, "SET_PORT 0 lost the UMS_INIT mark");
	CHECK(ums_fd_close(5) == 1 && ums_fd_init_count() == 0 && ums_fd_port(5) == 0, "close of an fd on port 0 that sent UMS_INIT");
	for (int fd = 10; fd < 23; fd++)
		ums_fd_init(fd);
	CHECK(ums_fd_init_count() == 12, "more fds than slots: %u", ums_fd_init_count());
}

/* Not tested here: a request to the SD card after the drive died. The host
 * harness has no SD card; the keepalive tests above stand for it. */

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
	{ "write_with_lost_status_fails_in_time", test_write_with_lost_status_fails_in_time, false },
	{ "write_with_one_lost_status_recovers", test_write_with_one_lost_status_recovers, false },
	{ "unplug_waits_until_replug", test_unplug_waits_until_replug, false },
	{ "dead_drive_fails_fast", test_dead_drive_fails_fast, false },
	{ "replug_clears_dead", test_replug_clears_dead, false },
	{ "ums_init_clears_dead", test_ums_init_clears_dead, false },
	{ "keepalive_skips_dead_and_unplugged", test_keepalive_skips_dead_and_unplugged, false },
	{ "dead_needs_init_or_replug", test_dead_needs_init_or_replug, false },
	{ "init_silent_drive_fails_in_time", test_init_silent_drive_fails_in_time, false },
	{ "init_silent_drive_with_id_fails_in_time", test_init_silent_drive_with_id_fails_in_time, false },
	{ "init_after_a_silent_drive_works", test_init_after_a_silent_drive_works, false },
	{ "enumeration_without_set_address_ends", test_enumeration_without_set_address_ends, false },
	{ "enumeration_of_a_silent_drive_ends", test_enumeration_of_a_silent_drive_ends, false },
	{ "enumeration_of_a_healthy_drive", test_enumeration_of_a_healthy_drive, false },
	{ "quiesce_waits_for_iaa", test_quiesce_waits_for_iaa, false },
	{ "quiesce_without_iaa_halts_the_schedule", test_quiesce_without_iaa_halts_the_schedule, false },
	{ "quiesce_is_bounded_when_the_controller_is_wedged", test_quiesce_is_bounded_when_the_controller_is_wedged, false },
	{ "quiesce_skips_an_idle_schedule", test_quiesce_skips_an_idle_schedule, false },
	{ "startup_resets_a_connected_port", test_startup_resets_a_connected_port, false },
	{ "startup_power_cycles_with_ppc", test_startup_power_cycles_with_ppc, false },
	{ "startup_skips_an_empty_port", test_startup_skips_an_empty_port, false },
	{ "power_cycle_needs_ppc", test_power_cycle_needs_ppc, false },
	{ "stuck_drive_mounts_after_a_port_reset", test_stuck_drive_mounts_after_a_port_reset, false },
	{ "stuck_drive_mounts_after_a_power_cycle", test_stuck_drive_mounts_after_a_power_cycle, false },
	{ "power_stuck_drive_stays_stuck_without_ppc", test_power_stuck_drive_stays_stuck_without_ppc, false },
	{ "ladder_port_reset_clears_the_fault", test_ladder_port_reset_clears_the_fault, false },
	{ "ladder_power_cycle_clears_the_fault", test_ladder_power_cycle_clears_the_fault, false },
	{ "ladder_escalates_when_the_remount_works", test_ladder_escalates_when_the_remount_works, false },
	{ "ladder_without_ppc_fails_in_time", test_ladder_without_ppc_fails_in_time, false },
	{ "ladder_unfixable_fault_fails_in_time", test_ladder_unfixable_fault_fails_in_time, false },
	{ "port1_init_acquires_once", test_port1_init_acquires_once, false },
	{ "port1_init_without_a_drive_fails", test_port1_init_without_a_drive_fails, false },
	{ "two_drives_alternate", test_two_drives_alternate, false },
	{ "port1_gone_fails_after_a_bound_then_at_once", test_port1_gone_fails_after_a_bound_then_at_once, false },
	{ "port1_replug_recovers", test_port1_replug_recovers, false },
	{ "port1_stuck_drive_errors_at_the_deadline", test_port1_stuck_drive_errors_at_the_deadline, false },
	{ "default_port_is_unchanged", test_default_port_is_unchanged, false },
	{ "fd_port_table", test_fd_port_table, false },
	{ "port1_not_a_drive_goes_back_to_ohci", test_port1_not_a_drive_goes_back_to_ohci, false },
	{ "port1_umount_goes_back_to_ohci", test_port1_umount_goes_back_to_ohci, false },
	{ "fd_set_port_refused_fails_the_requests", test_fd_set_port_refused_fails_the_requests, false },
	{ "keepalive_follows_open_fds_that_sent_init", test_keepalive_follows_open_fds_that_sent_init, false },
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
