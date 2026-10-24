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

static const struct hc_test tests[] = {
	{ "usbs_roundtrip", test_roundtrip, false },
	{ "usbs_write_csw_failure_is_reported", test_write_csw_failure_is_reported, false },
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
