/* The simulated drive and EHCI layer; see sim.h. */
#include <stdio.h>
#include <stdlib.h>
#include "sim.h"
#include "ehci_dev.h"
#include "hostcheck.h"

struct sim sim;
int unplug_device, handshake_mode;
u32 usb_timeout = 1000 * 1000;

#define XFER_US 125     /* one bulk transfer: about a USB 2.0 microframe */
#define EP_IN   0x81
#define EP_OUT  0x02

static void tick(u64 us)
{
	sim.now_us += us;
	if (sim.limit_us && sim.now_us > sim.limit_us) {
		sim.hung = true;
		hc_hang("hung: %s", sim_summary());
	}
}

void ehci_usleep(int us) { tick(us); }
void ehci_msleep(int ms) { tick((u64)ms * 1000); }

/* A bus reset: the drive forgets its state and its toggles; tinyehci zeroes
 * the host's toggles when it resets the port or the device (ehci.c). */
static void bus_reset(void)
{
	sim.phase = SIM_CBW;
	sim.halt_in = sim.halt_out = false;
	sim.toggle_in = sim.toggle_out = 0;
	if (sim.dev)
		sim.dev->toggles = 0;
}

int ehci_reset_port(int port) { (void)port; sim.port_resets++; tick(60000); bus_reset(); return 0; }
int ehci_reset_port2(int port) { (void)port; sim.port_resets++; tick(60000); bus_reset(); return 0; }
int ehci_reset_device(struct ehci_device *dev) { (void)dev; sim.dev_resets++; tick(60000); bus_reset(); return 0; }

void *USB_Alloc(int size) { return calloc(1, size + 32); }
void USB_Free(void *ptr) { free(ptr); }

/* No answer within usb_timeout, as tinyehci's handshake() reports it. */
static s32 no_answer(void)
{
	tick(usb_timeout);
	if (handshake_mode)
		return -ETIMEDOUT;
	unplug_device = 1;
	return -ENODEV;
}

/*
 * One mass-storage interface, SCSI over Bulk-Only, two bulk endpoints.
 *
 * Static storage, not the heap: after a reconnect, USBStorage_Open() checks
 * that it is the same drive by comparing sizeof(usb_devdesc) - 4 bytes of
 * the old and new descriptors, which leaves out the configurations pointer
 * on the 32-bit Wii but includes half of it on a 64-bit host. Fixed
 * addresses keep the comparison meaningful here.
 */
static usb_endpointdesc sim_eps[2];
static usb_interfacedesc sim_if;
static usb_configurationdesc sim_conf;

s32 USB_GetDescriptors(struct ehci_device *fd, usb_devdesc *udd)
{
	sim.dev = fd;
	memset(udd, 0, sizeof(*udd));
	udd->bLength = USB_DT_DEVICE_SIZE;
	udd->bDescriptorType = USB_DT_DEVICE;
	udd->bcdUSB = 0x0200;
	udd->idVendor = 0x1234;
	udd->idProduct = 0x5678;
	udd->bNumConfigurations = 1;
	udd->configurations = &sim_conf;
	memset(&sim_conf, 0, sizeof(sim_conf));
	sim_conf.bNumInterfaces = 1;
	sim_conf.bConfigurationValue = 1;
	sim_conf.interfaces = &sim_if;
	memset(&sim_if, 0, sizeof(sim_if));
	sim_if.bNumEndpoints = 2;
	sim_if.bInterfaceClass = 0x08;          /* mass storage */
	sim_if.bInterfaceSubClass = 0x06;       /* SCSI */
	sim_if.bInterfaceProtocol = 0x50;       /* Bulk-Only */
	sim_if.endpoints = sim_eps;
	memset(sim_eps, 0, sizeof(sim_eps));
	sim_eps[0].bEndpointAddress = EP_IN;
	sim_eps[0].bmAttributes = 2;
	sim_eps[0].wMaxPacketSize = SIM_MAXPACKET;
	sim_eps[1].bEndpointAddress = EP_OUT;
	sim_eps[1].bmAttributes = 2;
	sim_eps[1].wMaxPacketSize = SIM_MAXPACKET;
	return 0;
}

void USB_FreeDescriptors(usb_devdesc *udd)
{
	udd->configurations = NULL;
}

/* Control transfers on endpoint 0 (toggles there are not modelled). */
s32 ehci_control_message(struct ehci_device *dev, u8 type, u8 req, u16 value, u16 index,
			 u16 len, void *buf)
{
	sim.dev = dev;
	tick(XFER_US);
	if (type == 0x02 && req == USB_REQ_CLEARFEATURE && value == USB_FEATURE_ENDPOINT_HALT) {
		/* USB 2.0 9.4.5: the halt clears and the toggle goes to DATA0 */
		sim.clear_halts++;
		if (index & 0x80) {
			sim.halt_in = false;
			sim.toggle_in = 0;
		} else {
			sim.halt_out = false;
			sim.toggle_out = 0;
		}
		return 0;
	}
	if (type == 0x80 && req == USB_REQ_GETCONFIG && len >= 1) {
		*(u8 *)buf = 1;
		return 1;
	}
	if (type == 0x00 && req == USB_REQ_SETCONFIG) {
		/* USB 2.0 9.1.1.5: every endpoint back to DATA0 */
		sim.toggle_in = sim.toggle_out = 0;
		sim.halt_in = sim.halt_out = false;
		return 0;
	}
	if (type == 0x21 && req == 0xFF) {      /* Bulk-Only Mass Storage Reset */
		sim.bot_resets++;
		sim.phase = SIM_CBW;            /* halts and toggles stay (BOT 3.1) */
		return 0;
	}
	if (type == 0xA1 && req == 0xFE && len >= 1) {  /* Get Max LUN */
		*(u8 *)buf = 0;
		return 1;
	}
	return 0;
}

static void set_sense(u8 key, u8 asc) { sim.sense_key = key; sim.asc = asc; }

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void put_be32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* A CBW arrived: decode the SCSI command and set up its data phase. */
static void start_command(const u8 *cbw)
{
	u32 len = cbw[8] | cbw[9] << 8 | cbw[10] << 16 | (u32)cbw[11] << 24;
	memcpy(sim.cmd, cbw + 15, 16);
	sim.tag = cbw[4] | cbw[5] << 8 | cbw[6] << 16 | (u32)cbw[7] << 24;
	sim.status = 0;
	sim.residue = 0;
	sim.xfer_len = len;
	sim.xfer_done = 0;
	sim.cbws++;
	sim.last_cb_len = cbw[14];
	sim.last_cb_op = cbw[15];
	u8 *c = sim.cmd;
	switch (c[0]) {
	case 0x00:                      /* TEST UNIT READY */
		sim.phase = SIM_CSW;
		break;
	case 0x03:                      /* REQUEST SENSE */
		memset(sim.in_buf, 0, 18);
		sim.in_buf[0] = 0x70;
		sim.in_buf[2] = sim.sense_key;
		sim.in_buf[7] = 10;
		sim.in_buf[12] = sim.asc;
		set_sense(0, 0);
		sim.senses++;
		sim.phase = SIM_DATA_IN;
		break;
	case 0x12:                      /* INQUIRY */
		memset(sim.in_buf, 0, 36);
		sim.in_buf[4] = 31;
		memcpy(sim.in_buf + 8, "SIM     DRIVE           1.00", 28);
		sim.phase = SIM_DATA_IN;
		break;
	case 0x25:                      /* READ CAPACITY(10) */
		put_be32(sim.in_buf, SIM_SECTORS - 1);
		put_be32(sim.in_buf + 4, SIM_SECTOR_SIZE);
		sim.phase = SIM_DATA_IN;
		break;
	case 0x28:                      /* READ(10) */
		sim.lba = be32(c + 2);
		memcpy(sim.in_buf, sim.data + (size_t)sim.lba * SIM_SECTOR_SIZE, len);
		sim.phase = SIM_DATA_IN;
		break;
	case 0x2A:                      /* WRITE(10) */
		sim.lba = be32(c + 2);
		sim.phase = SIM_DATA_OUT;
		if (sim.stall_write_data > 0) {
			/* Like a write-protected or failing drive: refuse the
			 * data, STALL bulk-out, answer status 1 in the CSW
			 * (BOT 6.7.3, case Ho < Do). */
			sim.stall_write_data--;
			sim.halt_out = true;
			sim.status = 1;
			sim.residue = len;
			set_sense(0x07, 0x27);  /* DATA PROTECT, WRITE PROTECTED */
			sim.phase = SIM_CSW;
		}
		break;
	case 0x1B:                      /* START STOP UNIT */
		sim.phase = SIM_CSW;
		break;
	default:
		sim.status = 1;
		set_sense(0x05, 0x20);  /* ILLEGAL REQUEST, INVALID COMMAND */
		sim.phase = len ? (cbw[12] & 0x80 ? SIM_DATA_IN : SIM_DATA_OUT) : SIM_CSW;
		if (sim.phase == SIM_DATA_IN)
			memset(sim.in_buf, 0, len);
		break;
	}
}

static s32 bulk_out(u32 len, const u8 *p)
{
	if (sim.halt_out)
		return 0;       /* STALL: tinyehci returns the 0 bytes moved */
	if (sim.phase == SIM_CBW) {
		if (len != 31 || memcmp(p, "USBC", 4) != 0) {
			/* invalid CBW: stall both until a reset recovery */
			sim.halt_in = sim.halt_out = true;
			return 0;
		}
		start_command(p);
		return len;
	}
	if (sim.phase == SIM_DATA_OUT) {
		u32 n = len;
		if (n > sim.xfer_len - sim.xfer_done)
			n = sim.xfer_len - sim.xfer_done;
		if (sim.cmd[0] == 0x2A)
			memcpy(sim.data + (size_t)sim.lba * SIM_SECTOR_SIZE + sim.xfer_done, p, n);
		sim.xfer_done += n;
		if (sim.xfer_done == sim.xfer_len) {
			sim.phase = SIM_CSW;
			if (sim.cmd[0] == 0x2A && sim.fail_write_csw > 0) {
				sim.fail_write_csw--;
				sim.status = 1;
				set_sense(0x03, 0x0C);  /* MEDIUM ERROR, WRITE ERROR */
			} else if (sim.cmd[0] == 0x2A) {
				sim.writes_ok++;
			}
		}
		return n;
	}
	/* data the drive did not expect */
	sim.halt_out = true;
	return 0;
}

/* What the drive has to send now: 0 when it would NAK. */
static u32 in_pending(void)
{
	if (sim.phase == SIM_DATA_IN)
		return sim.xfer_len - sim.xfer_done;
	if (sim.phase == SIM_CSW)
		return 13;
	return 0;
}

static s32 bulk_in(u32 len, u8 *p)
{
	if (sim.halt_in)
		return 0;
	if (sim.phase == SIM_DATA_IN) {
		u32 n = len;
		if (n > sim.xfer_len - sim.xfer_done)
			n = sim.xfer_len - sim.xfer_done;
		memcpy(p, sim.in_buf + sim.xfer_done, n);
		sim.xfer_done += n;
		if (sim.xfer_done == sim.xfer_len) {
			sim.phase = SIM_CSW;
			if (sim.cmd[0] == 0x28)
				sim.reads_ok++;
		}
		return n;
	}
	if (sim.phase == SIM_CSW && len == 13) {
		memset(p, 0, 13);
		memcpy(p, "USBS", 4);
		memcpy(p + 4, &sim.tag, 4);
		memcpy(p + 8, &sim.residue, 4);
		p[12] = sim.status;
		sim.csws++;
		sim.phase = SIM_CBW;
		return 13;
	}
	/* The host reads when the drive has nothing to send: it NAKs until the
	 * transfer times out. */
	return no_answer();
}

static u32 packets(u32 bytes)
{
	return bytes ? (bytes + SIM_MAXPACKET - 1) / SIM_MAXPACKET : 1;
}

s32 ehci_bulk_message(struct ehci_device *dev, u8 ep, u16 len, void *data)
{
	bool in = ep & 0x80;
	u8 *drive_toggle = in ? &sim.toggle_in : &sim.toggle_out;
	u8 *p = data;
	sim.dev = dev;
	if (sim.timeout_xfers > 0) {
		sim.timeout_xfers--;
		return no_answer();
	}
	tick(XFER_US + len / 64);       /* about 40 MB/s */
	if (in ? sim.halt_in : sim.halt_out)
		return 0;               /* STALL handshake: no toggle changes */

	u8 host = get_toggle(dev, ep);
	if (host != *drive_toggle) {
		/* USB 2.0 8.6.4: the receiver acknowledges the first packet and
		 * discards it as a retransmission. The sender flips its toggle;
		 * the receiver does not. */
		sim.toggle_losses++;
		if (!in) {
			u32 first = len < SIM_MAXPACKET ? len : SIM_MAXPACKET;
			u32 n = packets(len);
			set_toggle(dev, ep, host ^ (n & 1));
			if (len > first) {      /* the rest arrives, in step again */
				*drive_toggle ^= (n - 1) & 1;
				bulk_out(len - first, p + first);
			}
			return len;     /* the host saw every packet acknowledged */
		}
		u32 avail = in_pending();
		if (!avail)
			return no_answer();
		/* The drive sent its first packet and counts it delivered. */
		*drive_toggle ^= 1;
		if (sim.phase == SIM_CSW) {
			sim.phase = SIM_CBW;
		} else {
			sim.xfer_done += avail < SIM_MAXPACKET ? avail : SIM_MAXPACKET;
			if (sim.xfer_done == sim.xfer_len)
				sim.phase = SIM_CSW;
		}
		return no_answer();     /* the host is still waiting for it */
	}

	s32 r = in ? bulk_in(len, p) : bulk_out(len, p);
	if (r > 0 || (r == 0 && len == 0)) {
		u32 n = packets(r);
		set_toggle(dev, ep, host ^ (n & 1));
		*drive_toggle ^= n & 1;
	}
	return r;
}

void sim_reset(void)
{
	memset(&sim, 0, sizeof(sim));
	for (size_t i = 0; i < sizeof(sim.data); i++)
		sim.data[i] = (u8)(i * 7 + 3);
	unplug_device = 0;
	handshake_mode = 0;
	usb_timeout = 1000 * 1000;
}

const char *sim_summary(void)
{
	static char s[320];
	snprintf(s, sizeof(s), "%.3f s, %u CBWs, %u CSWs, %u BOT resets, %u clear-halts, "
		 "%u port resets, %u device resets, %u toggle losses%s",
		 sim.now_us / 1e6, sim.cbws, sim.csws, sim.bot_resets, sim.clear_halts,
		 sim.port_resets, sim.dev_resets, sim.toggle_losses, sim.hung ? ", HUNG" : "");
	return s;
}
