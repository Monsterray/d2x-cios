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
#define SIM_DRIVES      2       /* drive 0 on root port 0, drive 1 on SIM_PORT1 */
#define SIM_PORT1       1

struct ehci_device;

struct sim {
	/* the drive */
	u8 data[SIM_SECTORS * SIM_SECTOR_SIZE];
	u32 sectors;                    /* READ CAPACITY: set by sim_reset(), drive 1 is smaller */
	u8 sense_key, asc;
	struct ehci_device *dev;        /* the host's view of it (toggles) */

	/* BOT state */
	enum { SIM_CBW, SIM_DATA_OUT, SIM_DATA_IN, SIM_CSW } phase;
	u32 tag, residue;
	u8 status;
	u8 cmd[16];
	u32 lba, xfer_len, xfer_done;
	u8 in_buf[256 * 1024];   /* the largest READ(10): 65535 sectors is more, tests stay below */
	bool halt_in, halt_out;
	u8 toggle_in, toggle_out;       /* the drive's data toggles */

	/* faults: each counts down once per event */
	int stall_write_data;   /* WRITE(10): STALL bulk-out at the data phase */
	int fail_write_csw;     /* WRITE(10): data accepted, CSW status 1 */
	int timeout_xfers;      /* next bulk transfers get no answer */
	int lose_write_csw;     /* WRITE(10): data taken, the CSW read STALLs, and
	                           after the clear-halt the CSW never comes, until
	                           a reset (the SanDisk on the bench, 2026-10-24) */
	bool csw_lost;          /* the drive is in that state now */
	int stall_read_data;    /* READ(10): STALL bulk-in at the data phase, status 1
	                           (BOT 6.7.2, Hi > Di) */
	int stall_csw;          /* the next CSW read STALLs; after the clear-halt the
	                           CSW is there (BOT 6.7.2: the host reads it again) */

	/* the port (PORTSC): the drive can be unplugged and come back */
	bool disconnected;      /* CCS = 0 */
	bool owner;             /* PORT_OWNER: the port is routed to the OHCI companion */
	u32 acquires;           /* PORT_OWNER written 1 -> 0: the port taken back */
	bool csc;               /* PORTSC.CSC, write-1-to-clear */
	u64 replug_at_us;       /* when it comes back (0: never by itself) */
	u32 portsc_writes, port_xfers_gone;
	bool ppc;               /* HCSPARAMS.PPC: the ports switch power */
	bool unpowered;         /* port power is off (PORT_POWER 0, no drive seen) */
	u32 power_offs, power_cycles;   /* PORT_POWER 1 -> 0, and back to 1 */
	int stuck;              /* the drive answers no bulk transfer: 1 until a port
	                           reset, 2 until a power cycle (a drive left mid-
	                           transfer by an earlier reset) */
	bool stuck_io_only;     /* ... only READ(10) and WRITE(10) commands */
	bool silent;            /* the drive answers no transfer at all, control or
	                           bulk, and a reset does not help (a drive left
	                           stuck by a console reset, devwii 2026-11-08) */
	bool no_setaddr;        /* the first descriptor request is answered, but
	                           SET_ADDRESS and everything after it are not */
	u32 silent_xfers;       /* transfers that got no answer for those two */

	/* the controller: USBCMD and USBSTS as ehci_recover.h uses them */
	u32 hc_cmd;             /* USBCMD */
	bool hc_ass;            /* USBSTS.ASS follows CMD_ASE unless ass_stuck */
	bool ass_stuck;         /* the schedule does not stop when CMD_ASE clears */
	bool iaa_never;         /* the doorbell is never answered */
	bool iaa_set;           /* USBSTS.IAA, write-1-to-clear */
	bool iaa_pending;       /* a doorbell was rung and not answered yet */
	u64 iaa_at_us;
	u32 doorbells, ase_clears, ase_sets, async_next;

	/* counters */
	u32 cbws, csws, bot_resets, clear_halts, port_resets, dev_resets;
	u32 writes_ok, reads_ok, senses, toggle_losses;
	u8 last_cb_len, last_cb_op;     /* bCBWCBLength and opcode of the last CBW */

	/* time */
	u64 now_us, limit_us;
	bool hung;
};

/*
 * Two drives share one controller and one clock. `sim` is the drive that is
 * selected: the simulation selects by itself the drive a call is meant for
 * (by the device's or the register's port), and a test selects with
 * sim_sel() before it sets up or looks at the other one. Selecting copies the
 * clock (now_us, limit_us, hung) over, so the selected drive always has the
 * time. The controller's registers (hc_*, doorbells) live in drive 0.
 */
typedef struct sim sim_t;
extern sim_t sims[SIM_DRIVES];
extern sim_t *sim_p;
#define sim (*sim_p)
void sim_sel(int drive);

/* sim.c calls this from ehci_reset_port() when sim.silent or sim.no_setaddr:
   the enumeration that ehci_reset_port() does after the bus reset (ums_unit.c) */
extern int (*sim_enum_hook)(int port);

void sim_reset(void);           /* a fresh drive, no faults, time 0 */
const char *sim_summary(void);
void sim_unplug_for(u64 us);    /* unplug now; it is back (CSC set) after us */
u32 sim_portsc_read(int port);
void sim_portsc_write(int port, u32 v);
enum { SIM_HC_CMD, SIM_HC_STS, SIM_HC_ASYNC_NEXT };
u32 sim_hc_read(int reg);
void sim_hc_write(int reg, u32 v);

/* ums_unit.c: the module code under test */
int ums_plug(void);             /* mount the drive as USBStorage_Init() does */
void ums_unplug(void);          /* forget it, without talking to it */
u32 ums_sector_size(void);
u32 ums_sectors(void);          /* the capacity of the selected drive, in sectors */
int ums_enum_port(void);        /* ehci_init_port(0), as ehci_reset_port() ends */
void ums_set_dev_id(int id);    /* the id of the device USBStorage_Init() sees on port 0 */
void ums_set_dev_id_port(int port, int id);     /* the same on root port 0 or 1 */
int ums_select(int port);      /* USBStorage_Select(): the logical port the next calls go to */
int ums_selected(void);
int ums_fd_port(int fd);        /* loop.c's fd -> port table */
int ums_fd_set_port(int fd, unsigned port);
void ums_fd_forget(int fd);
int ums_mounted(void);
int ums_dev_id(int port);       /* the id of the device on root port 0 or 1: 0 = not known */
void ums_umount(void);          /* USBStorage_Umount(): UMS_UMOUNT on the selected port */
extern const unsigned ums_request_ms;  /* the driver's USBSTORAGE_REQUEST_MS */
s32 ums_reset(void);            /* USBStorage_Reset(): Bulk-Only reset recovery */
s32 ums_init(void);             /* USBStorage_Init(): UMS_INIT on a mounted drive */
int ums_keepalive_ready(void);  /* USBStorage_Keepalive_Ready() */
int ums_is_dead(void);
void ums_startup(void);         /* ehci_startup_recover(), as EHCI_Init() calls it */
int ums_power_cycle(void);      /* ehci_port_power_cycle(0) */
int ums_quiesce(void);          /* ehci_async_quiesce(), then ehci_async_resume() if it asks */
s32 USBStorage_Read_Sectors(u32 sector, u32 numSectors, void *buffer);
s32 USBStorage_Write_Sectors(u32 sector, u32 numSectors, const void *buffer);

#endif
