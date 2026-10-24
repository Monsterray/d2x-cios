/*
 * A simulated USB mass-storage drive for host tests of the EHCI module's
 * usb.c and usbstorage.c. It stands in for the EHCI layer under usb.c
 * (ehci_bulk_message, ehci_control_message, the port resets) and answers the
 * Bulk-Only Transport protocol: CBW, data, CSW.
 *
 * Errors are reported the way tinyehci reports them on a Wii:
 *  - a STALLed transfer comes back as a short count (qh_end_transfer()
 *    returns the bytes moved before the halt), not as an error code;
 *  - a transfer that gets no answer in usb_timeout returns -ENODEV and sets
 *    unplug_device (handshake_mode 0) or returns -ETIMEDOUT (handshake_mode 1).
 *
 * Data toggles (USB 2.0 8.6) are kept on both sides. The host's are in
 * ehci_device.toggles, as tinyehci keeps them: each bulk transfer starts from
 * the stored toggle, and only a port or device reset zeroes them. The drive's
 * reset to DATA0 on CLEAR_FEATURE(ENDPOINT_HALT) and SET_CONFIGURATION
 * (USB 2.0 9.4.5, 9.1.1.5) and on a bus reset. On a mismatch the receiver
 * acknowledges and discards the packet as a retransmission (8.6.4): a lost
 * OUT packet leaves the drive waiting, a lost IN packet leaves the host
 * waiting, and the transfer times out.
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
#define SIM_MAXPACKET   512

struct ehci_device;

struct sim {
	/* the drive */
	u8 data[SIM_SECTORS * SIM_SECTOR_SIZE];
	u8 sense_key, asc;
	struct ehci_device *dev;        /* the host's view of it (toggles) */

	/* BOT state */
	enum { SIM_CBW, SIM_DATA_OUT, SIM_DATA_IN, SIM_CSW } phase;
	u32 tag, residue;
	u8 status;
	u8 cmd[16];
	u32 lba, xfer_len, xfer_done;
	u8 in_buf[64 * 1024];
	bool halt_in, halt_out;
	u8 toggle_in, toggle_out;       /* the drive's data toggles */

	/* faults: each counts down once per event */
	int stall_write_data;   /* WRITE(10): STALL bulk-out at the data phase */
	int fail_write_csw;     /* WRITE(10): data accepted, CSW status 1 */
	int timeout_xfers;      /* next bulk transfers get no answer */

	/* counters */
	u32 cbws, csws, bot_resets, clear_halts, port_resets, dev_resets;
	u32 writes_ok, reads_ok, senses, toggle_losses;
	u8 last_cb_len, last_cb_op;     /* bCBWCBLength and opcode of the last CBW */

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
s32 ums_reset(void);            /* USBStorage_Reset(): Bulk-Only reset recovery */
s32 USBStorage_Read_Sectors(u32 sector, u32 numSectors, void *buffer);
s32 USBStorage_Write_Sectors(u32 sector, u32 numSectors, const void *buffer);

#endif
