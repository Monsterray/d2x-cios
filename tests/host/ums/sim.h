/*
 * A simulated USB mass-storage drive for host tests of the EHCI module's
 * usbstorage.c. It stands in for the EHCI layer (USB_WriteBlkMsg and friends)
 * and answers the Bulk-Only Transport protocol: CBW, data, CSW.
 *
 * Errors are reported the way tinyehci reports them on a Wii:
 *  - a STALLed transfer comes back as a short count (qh_end_transfer()
 *    returns the bytes moved before the halt), not as an error code;
 *  - a transfer that gets no answer in usb_timeout returns -ENODEV and sets
 *    unplug_device (handshake_mode 0) or returns -ETIMEDOUT (handshake_mode 1).
 *
 * Time is simulated: sim.now_us advances by a fixed cost per transfer, by
 * ehci_msleep()/ehci_usleep(), and by usb_timeout for each timeout. A test
 * that would hang the module stops at sim.limit_us: sim.hung is set and
 * hc_hang() (hostcheck.h) ends the test as failed.
 */
#ifndef SIM_H
#define SIM_H

#include "types.h"

#define SIM_SECTOR_SIZE 512
#define SIM_SECTORS     4096

struct sim {
	/* the drive */
	u8 data[SIM_SECTORS * SIM_SECTOR_SIZE];
	u8 sense_key, asc;

	/* BOT state */
	enum { SIM_CBW, SIM_DATA_OUT, SIM_DATA_IN, SIM_CSW } phase;
	u32 tag, residue;
	u8 status;
	u8 cmd[16];
	u32 lba, xfer_len, xfer_done;
	u8 in_buf[64 * 1024];
	bool halt_in, halt_out;

	/* faults: each counts down once per event */
	int stall_write_data;   /* WRITE(10): STALL bulk-out at the data phase */
	int fail_write_csw;     /* WRITE(10): data accepted, CSW status 1 */
	int timeout_xfers;      /* next bulk transfers get no answer */

	/* counters */
	u32 cbws, csws, bot_resets, clear_halts, port_resets, dev_resets;
	u32 writes_ok, reads_ok;

	/* time */
	u64 now_us, limit_us;
	bool hung;
};

extern struct sim sim;

void sim_reset(void);           /* a fresh drive, no faults, time 0 */
const char *sim_summary(void);

/* ums_unit.c: the module code under test */
int ums_plug(void);             /* mount the drive as USBStorage_Init() does */
void ums_unplug(void);          /* forget it, without talking to it */
u32 ums_sector_size(void);
s32 USBStorage_Read_Sectors(u32 sector, u32 numSectors, void *buffer);
s32 USBStorage_Write_Sectors(u32 sector, u32 numSectors, const void *buffer);

#endif
