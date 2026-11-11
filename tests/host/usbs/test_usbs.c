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

/* Transfer-count tests: the module's bulk transfers other than CBW and CSW
 * (glue_sim.c), and which buffers it may hand to IOS as they are. */
extern int glue_mem2;
extern unsigned glue_data_xfers;
extern unsigned glue_data_len[64];
extern unsigned glue_short_next;
static unsigned char big[200 * SECTOR + 64] __attribute__((aligned(32)));

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

static void count_reset(void)
{
	glue_data_xfers = 0;
}

/* The data transfers of the last request, as "n: a b c" for a message. */
static const char *xfers(void)
{
	static char s[256];
	int o = snprintf(s, sizeof(s), "%u:", glue_data_xfers);
	for (unsigned i = 0; i < glue_data_xfers && i < 16; i++)
		o += snprintf(s + o, sizeof(s) - o, " %u", glue_data_len[i]);
	return s;
}

/* One read and one write of n sectors at lba, through b; the result must be
 * the drive's data and the data transfers must be exactly want[0..n_want). */
static void split_check(unsigned char *b, unsigned lba, unsigned n, const unsigned *want, unsigned n_want)
{
	size_t bytes = (size_t) n * SECTOR;
	memset(b, 0, bytes);
	count_reset();
	CHECK(usbs_read(lba, n, b) >= 0, "read failed: %s", sim_summary());
	CHECK(memcmp(b, sim.data + (size_t) lba * SECTOR, bytes) == 0, "read returned other data");
	CHECK(glue_data_xfers == n_want, "read of %u sectors: %s", n, xfers());
	for (unsigned i = 0; i < n_want && i < glue_data_xfers; i++)
		CHECK(glue_data_len[i] == want[i], "read of %u sectors, piece %u: %s", n, i, xfers());

	fill(b, bytes, 11);
	count_reset();
	CHECK(usbs_write(lba + 1000, n, b) >= 0, "write failed: %s", sim_summary());
	CHECK(memcmp(sim.data + (size_t) (lba + 1000) * SECTOR, b, bytes) == 0, "drive holds other data");
	CHECK(glue_data_xfers == n_want, "write of %u sectors: %s", n, xfers());
	for (unsigned i = 0; i < n_want && i < glue_data_xfers; i++)
		CHECK(glue_data_len[i] == want[i], "write of %u sectors, piece %u: %s", n, i, xfers());
}

/* An aligned MEM2 buffer goes to IOS as it is, in pieces of
 * USBS_MAX_TRANSFER (16 KB: IOS58's /dev/usb/ven hung on a 32 KB transfer), the same as the bounce path: 64 sectors = 2. */
static void test_direct_64_sectors_two_transfers(void)
{
	static const unsigned want[] = { 32 * SECTOR, 32 * SECTOR };
	mount();
	CHECK_OK();
	glue_mem2 = 1;
	split_check(big, 20, 64, want, 2);
	usbs_close();
}

/* 127 sectors = 4 pieces (32+32+32+31), 200 sectors = 7 (6 x 32 + 8). */
static void test_direct_127_and_200_sectors(void)
{
	static const unsigned w127[] = { 32 * SECTOR, 32 * SECTOR, 32 * SECTOR, 31 * SECTOR };
	static const unsigned w200[] = { 32 * SECTOR, 32 * SECTOR, 32 * SECTOR, 32 * SECTOR, 32 * SECTOR,
					 32 * SECTOR, 8 * SECTOR };
	mount();
	CHECK_OK();
	glue_mem2 = 1;
	split_check(big, 30, 127, w127, 4);
	split_check(big, 40, 200, w200, 7);
	usbs_close();
}

/* An unaligned buffer, or one outside MEM2, goes through the 16 KB bounce
 * buffer in pieces of 16 KB. */
static void test_bounce_pieces_are_16k(void)
{
	static const unsigned w64[] = { 32 * SECTOR, 32 * SECTOR };
	static const unsigned w127[] = { 32 * SECTOR, 32 * SECTOR, 32 * SECTOR, 31 * SECTOR };
	static const unsigned w200[] = { 32 * SECTOR, 32 * SECTOR, 32 * SECTOR, 32 * SECTOR, 32 * SECTOR,
					 32 * SECTOR, 8 * SECTOR };
	mount();
	CHECK_OK();
	glue_mem2 = 1;                  /* unaligned in MEM2 */
	split_check(big + 1, 50, 64, w64, 2);
	split_check(big + 1, 60, 127, w127, 4);
	split_check(big + 1, 70, 200, w200, 7);
	glue_mem2 = 0;                  /* aligned, but not MEM2 */
	split_check(big, 80, 64, w64, 2);
	split_check(big, 90, 200, w200, 7);
	usbs_close();
}

/* The retry on a STALL works with a big direct transfer: the command is sent
 * again and the whole data arrives. */
static void test_direct_read_recovers_from_one_stall(void)
{
	mount();
	CHECK_OK();
	glue_mem2 = 1;
	sim.stall_read_data = 1;
	CHECK(usbs_read(100, 127, big) >= 0, "read failed: %s", sim_summary());
	CHECK(memcmp(big, sim.data + 100 * SECTOR, 127 * SECTOR) == 0, "read returned other data");
	CHECK(sim.bot_resets == 0 && sim.port_resets == 0, "%s", sim_summary());
	fill(big, 127 * SECTOR, 13);
	sim.lose_write_csw = 1;
	CHECK(usbs_write(1100, 127, big) >= 0, "write failed: %s", sim_summary());
	CHECK(memcmp(sim.data + 1100 * SECTOR, big, 127 * SECTOR) == 0, "drive holds other data");
	usbs_close();
}

/* The same STALL recovery for a write straight from a MEM2 buffer, 4 pieces
 * of 16 KB: the data phase STALLs, the command is sent again, all the data
 * arrives, and the retry needs no reset. */
static void test_direct_write_recovers_from_one_stall(void)
{
	mount();
	CHECK_OK();
	glue_mem2 = 1;
	fill(big, 127 * SECTOR, 17);
	sim.stall_write_data = 1;
	CHECK(usbs_write(1300, 127, big) >= 0, "write failed: %s", sim_summary());
	CHECK(memcmp(sim.data + 1300 * SECTOR, big, 127 * SECTOR) == 0, "drive holds other data");
	CHECK(sim.bot_resets == 0 && sim.port_resets == 0, "%s", sim_summary());
	usbs_close();
}

/* IOS reports a write's data transfer short: the module treats it as a data
 * residue error, resets the drive and sends the whole command again. The
 * retry must put all of the data on the drive; both the bounce and the direct
 * path. */
static void test_short_write_is_retried_after_reset(void)
{
	for (int direct = 0; direct < 2; direct++) {
		mount();
		CHECK_OK();
		glue_mem2 = direct;
		fill(big, 70 * SECTOR, 19 + direct);
		glue_short_next = SECTOR;
		CHECK(usbs_write(1500 + direct * 100, 70, big) >= 0, "write failed: %s", sim_summary());
		CHECK(memcmp(sim.data + (1500 + direct * 100) * SECTOR, big, 70 * SECTOR) == 0,
		      "drive holds other data (direct %d)", direct);
		CHECK(sim.bot_resets == 1, "%u BOT resets, expected 1: %s", sim.bot_resets, sim_summary());
		usbs_close();
	}
}

/* A read's data transfer comes back short: the module stops the data phase
 * and reads the CSW. The drive still has data to send, so what arrives is not
 * a CSW (bad signature); the module resets the drive and sends the command
 * again, and the second try returns all the data. */
static void test_short_read_is_retried_after_reset(void)
{
	for (int direct = 0; direct < 2; direct++) {
		mount();
		CHECK_OK();
		glue_mem2 = direct;
		memset(big, 0, 70 * SECTOR);
		glue_short_next = SECTOR;
		CHECK(usbs_read(1600, 70, big) >= 0, "read failed: %s", sim_summary());
		CHECK(glue_short_next == 0, "the short count was not used");
		CHECK(memcmp(big, sim.data + 1600 * SECTOR, 70 * SECTOR) == 0, "read returned other data (direct %d)",
		      direct);
		CHECK(sim.cbws == 2 && sim.bot_resets == 1, "%s", sim_summary());
		usbs_close();
	}
}

/* ---- two drives: logical ports 0 and 1 (docs/usb-ports.md) ----
 * usbglue.c over a list of devices (usbs_unit.c): port 0 is the first
 * mass-storage device that mounts, port 1 the second, found by IOS's device
 * id, so two drives with one VID/PID are two drives. Odd ids are the second
 * simulated drive. The drives differ in data and size. */

void usbg_reset(void);
void usbg_clear_devices(void);
void usbg_add_device(int id, unsigned vid, unsigned pid);
void usbg_remove_device(int id);
int usbg_init(int port);
int usbg_inserted(int port);
int usbg_mounted(int port);
int usbg_read(int port, unsigned sector, unsigned n, void *buf);
int usbg_write(int port, unsigned sector, unsigned n, const void *buf);
unsigned usbg_capacity(int port);
int usbg_shutdown(int port);
int usbg_buffers(void);

/* READ CAPACITY gives the last LBA */
#define CAP(i) (sims[i].sectors - 1)

/* Both drives plugged in, with the signature the module wants on sector 0
 * (0x55AA read as a native u16: the module is big-endian, this host is not) */
static void two_drives(void)
{
	usbg_reset();
	for (int d = 0; d < 2; d++) {
		sims[d].data[510] = 0xAA;
		sims[d].data[511] = 0x55;
	}
	sims[1].disconnected = false;
	sims[1].owner = false;
	usbg_add_device(0x10, 0x1234, 0x5678);
	usbg_add_device(0x11, 0x1234, 0x5678);   /* the same VID/PID */
}

/* A read on a port, the data compared with that port's drive */
static bool read_port(int port, unsigned sector, unsigned n)
{
	if (usbg_read(port, sector, n, buf) <= 0)
		return false;
	return memcmp(buf, sims[port].data + (size_t)sector * SECTOR, (size_t)n * SECTOR) == 0;
}

static void test_ports_two_drives(void)
{
	two_drives();
	CHECK(usbg_init(0), "UMS_INIT on port 0 failed");
	CHECK(usbg_mounted(0), "port 0 not mounted: %s", sim_summary());
	CHECK(!usbg_mounted(1), "port 1 mounted by a client that never asked for it");
	CHECK(usbg_init(1), "UMS_INIT on port 1 failed");
	CHECK(usbg_mounted(0) && usbg_mounted(1), "not both mounted: %s", sim_summary());
	CHECK(usbg_capacity(0) == CAP(0) && usbg_capacity(1) == CAP(1) && CAP(0) != CAP(1), "capacities %u and %u",
	      usbg_capacity(0), usbg_capacity(1));
	CHECK(usbg_buffers() == 1, "%d bounce buffers", usbg_buffers());
	for (int i = 0; i < 24; i++)
		CHECK(read_port(i & 1, 10 + i, 1 + (i % 5)), "read %d on port %d failed: %s", i, i & 1, sim_summary());
	/* a write goes to its own drive */
	fill(big, 8 * SECTOR, 0x42);
	unsigned char before = sims[0].data[300 * SECTOR];
	CHECK(usbg_write(1, 300, 8, big) > 0, "write failed: %s", sim_summary());
	CHECK(memcmp(sims[1].data + 300 * SECTOR, big, 8 * SECTOR) == 0, "the data did not land on drive 1");
	CHECK(sims[0].data[300 * SECTOR] == before && sims[0].writes_ok == 0, "drive 0 saw the write");
	CHECK(read_port(0, 300, 8) && read_port(1, 300, 8), "read back failed");
	usbg_reset();
}

/* A client that never sends SET_PORT: port 0, the first drive, and the
 * second drive is not looked at */
static void test_ports_default_is_unchanged(void)
{
	two_drives();
	CHECK(usbg_init(0) && usbg_mounted(0), "port 0 not mounted: %s", sim_summary());
	CHECK(usbg_inserted(0), "ISINSERTED said no");
	CHECK(read_port(0, 5, 3), "read failed: %s", sim_summary());
	CHECK(!usbg_mounted(1) && sims[1].cbws == 0, "the second drive was opened: %s", sim_summary());
	CHECK(usbg_read(1, 5, 1, buf) <= 0 && usbg_capacity(1) == 0, "a request to port 1 with no drive there worked");
	CHECK(sims[1].cbws == 0, "traffic to the second drive");
	usbg_reset();
}

/* Port 1 with one drive only: no drive there, and port 0 unharmed. The second
 * drive plugged in later is found by the next UMS_INIT. */
static void test_ports_port1_empty_then_plugged(void)
{
	two_drives();
	usbg_remove_device(0x11);
	CHECK(usbg_init(0) && usbg_init(1), "UMS_INIT failed");
	CHECK(usbg_mounted(0) && !usbg_mounted(1), "port 0 %d, port 1 %d", usbg_mounted(0), usbg_mounted(1));
	CHECK(usbg_read(1, 5, 1, buf) <= 0, "read from an empty port 1 worked");
	CHECK(read_port(0, 6, 2), "port 0 failed: %s", sim_summary());
	usbg_add_device(0x11, 0x1234, 0x5678);
	CHECK(usbg_init(1) && usbg_mounted(1), "port 1 not mounted after the plug: %s", sim_summary());
	CHECK(read_port(1, 7, 2) && read_port(0, 8, 2), "reads failed");
	usbg_reset();
}

/* A drive unplugged on one port does not take the other with it; the drive
 * that comes back (a new id) takes the port that is free */
static void test_ports_unplug_one_keeps_the_other(void)
{
	two_drives();
	CHECK(usbg_init(0) && usbg_init(1) && usbg_mounted(0) && usbg_mounted(1), "not both mounted");
	usbg_remove_device(0x11);
	CHECK(!usbg_inserted(1) && !usbg_mounted(1), "port 1 still mounted after the unplug");
	CHECK(usbg_mounted(0) && read_port(0, 11, 2), "port 0 failed: %s", sim_summary());
	CHECK(usbg_read(1, 11, 1, buf) <= 0, "read from the unplugged drive worked");
	CHECK(usbg_buffers() == 1, "%d bounce buffers", usbg_buffers());
	usbg_add_device(0x21, 0x1234, 0x5678);       /* the same drive, a new id */
	CHECK(usbg_init(1) && usbg_mounted(1), "not back on port 1: %s", sim_summary());
	CHECK(read_port(1, 12, 3) && read_port(0, 13, 3), "reads after the replug failed");

	/* now port 0's drive goes: port 1 stays, and does not move to port 0 */
	usbg_remove_device(0x10);
	CHECK(!usbg_inserted(0) && !usbg_mounted(0), "port 0 mounted the other port's drive");
	CHECK(usbg_mounted(1) && read_port(1, 14, 1), "port 1 failed: %s", sim_summary());
	usbg_add_device(0x30, 0x1234, 0x5678);
	CHECK(usbg_init(0) && usbg_mounted(0), "port 0 not mounted again");
	CHECK(read_port(0, 15, 1) && read_port(1, 16, 1), "reads failed");
	CHECK(usbg_buffers() == 1, "%d bounce buffers", usbg_buffers());
	usbg_reset();
}

/* One bounce buffer for both handles, freed with the last */
static void test_ports_one_bounce_buffer(void)
{
	two_drives();
	CHECK(usbg_init(0) && usbg_buffers() == 1, "%d buffers", usbg_buffers());
	CHECK(usbg_init(1) && usbg_buffers() == 1, "%d buffers with two drives", usbg_buffers());
	usbg_shutdown(0);
	CHECK(usbg_buffers() == 1, "the buffer went with the first drive");
	usbg_shutdown(1);
	CHECK(usbg_buffers() == 0, "the buffer stayed after the last drive");
	usbg_reset();
}

/* A drive on port 1 that fails does not stop port 0 (the stall is on its drive) */
static void test_ports_a_failing_drive_leaves_the_other(void)
{
	two_drives();
	CHECK(usbg_init(0) && usbg_init(1), "UMS_INIT failed");
	sims[1].stall_csw = 1000000;
	u64 t = sim.now_us;
	CHECK(usbg_read(1, 20, 1, buf) <= 0, "read from a failing drive worked");
	CHECK(sim.now_us - t <= (u64)(8000 + 2000) * 1000, "gave up after %.1f s", (sim.now_us - t) / 1e6);
	CHECK(read_port(0, 21, 4), "port 0 failed: %s", sim_summary());
	sims[1].stall_csw = 0;
	CHECK(read_port(1, 22, 2), "port 1 did not recover: %s", sim_summary());
	usbg_reset();
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
	{ "usbs_direct_64_sectors_two_transfers", test_direct_64_sectors_two_transfers, false },
	{ "usbs_direct_127_and_200_sectors", test_direct_127_and_200_sectors, false },
	{ "usbs_bounce_pieces_are_16k", test_bounce_pieces_are_16k, false },
	{ "usbs_direct_read_recovers_from_one_stall", test_direct_read_recovers_from_one_stall, false },
	{ "usbs_direct_write_recovers_from_one_stall", test_direct_write_recovers_from_one_stall, false },
	{ "usbs_short_write_is_retried_after_reset", test_short_write_is_retried_after_reset, false },
	{ "usbs_short_read_is_retried_after_reset", test_short_read_is_retried_after_reset, false },
	{ "usbs_ports_two_drives", test_ports_two_drives, false },
	{ "usbs_ports_default_is_unchanged", test_ports_default_is_unchanged, false },
	{ "usbs_ports_port1_empty_then_plugged", test_ports_port1_empty_then_plugged, false },
	{ "usbs_ports_unplug_one_keeps_the_other", test_ports_unplug_one_keeps_the_other, false },
	{ "usbs_ports_one_bounce_buffer", test_ports_one_bounce_buffer, false },
	{ "usbs_ports_a_failing_drive_leaves_the_other", test_ports_a_failing_drive_leaves_the_other, false },
};

static void setup(void)
{
	sim_reset();
	glue_mem2 = 0;
	glue_short_next = 0;
	sim.limit_us = 600 * 1000000ull;
}

int main(int argc, char **argv)
{
	return hc_main(tests, HC_COUNT(tests), argc, argv, setup);
}
