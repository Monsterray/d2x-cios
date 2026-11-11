/*
 * source/ehci-module/tinyehci/usb.c and usbstorage.c, built for the host the
 * way tinyehci.c builds them for the module: one translation unit, with the
 * EHCI layer below them (ehci_control_message, ehci_bulk_message, port
 * resets) replaced by sim.c.
 */
#include <stdlib.h>
#include "sim.h"
#include "ehci_dev.h"

struct ehci_regs { u32 command; u32 status; u32 async_next; u32 port_status[4]; };
struct ehci_qh_head { u32 qh_dma; };
struct ehci_hcd {
	struct ehci_regs *regs;
	int num_port;
	u32 hcs_params;
	struct ehci_qh_head *async;
	struct ehci_device devices[4];
};
static struct ehci_regs sim_regs;
static struct ehci_qh_head sim_async = { .qh_dma = 0x1000 };
static struct ehci_hcd sim_hcd = { .regs = &sim_regs, .num_port = 1, .async = &sim_async };
struct ehci_hcd *ehci = &sim_hcd;
/* PORTSC (ports 0 and 1), USBCMD and USBSTS are simulated: sim.c keeps them */
static u32 sim_rd(volatile u32 *a)
{
	if (a >= &sim_regs.port_status[0] && a < &sim_regs.port_status[4])
		return sim_portsc_read(a - &sim_regs.port_status[0]);
	if (a == &sim_regs.command)
		return sim_hc_read(SIM_HC_CMD);
	if (a == &sim_regs.status)
		return sim_hc_read(SIM_HC_STS);
	return *a;
}
static void sim_wr(u32 v, volatile u32 *a)
{
	if (a >= &sim_regs.port_status[0] && a < &sim_regs.port_status[4])
		sim_portsc_write(a - &sim_regs.port_status[0], v);
	else if (a == &sim_regs.command)
		sim_hc_write(SIM_HC_CMD, v);
	else if (a == &sim_regs.status)
		sim_hc_write(SIM_HC_STS, v);
	else if (a == &sim_regs.async_next)
		sim_hc_write(SIM_HC_ASYNC_NEXT, v);
	else
		*a = v;
}
#define ehci_readl(a) sim_rd((volatile u32 *)(a))
#define ehci_writel(v, a) sim_wr((v), (volatile u32 *)(a))
#define PORT_CONNECT (1 << 0)
#define PORT_CSC     (1 << 1)
#define PORT_PE      (1 << 2)
#define PORT_PEC     (1 << 3)
#define PORT_OCC     (1 << 5)
#define PORT_OWNER   (1 << 13)
#define PORT_POWER   (1 << 12)
#define PORT_RESET   (1 << 8)
#define PORT_RWC_BITS (PORT_CSC | PORT_PEC | PORT_OCC)
#define PORT_USB11(x) (((x)&(3<<10)) == (1<<10))
#define CMD_IAAD     (1 << 6)
#define CMD_ASE      (1 << 5)
#define STS_ASS      (1 << 15)
#define STS_HALT     (1 << 12)
#define STS_IAA      (1 << 5)
#define HCS_PPC(p)   ((void)(p), sim.ppc)   /* the simulated HCSPARAMS.PPC */
#define printk(...) ((void)0)

/* Globals that ehci.c and the module define. */
extern int unplug_device, handshake_mode;
extern u32 usb_timeout;
int try_status;

/* usb.c, except the descriptor parser: sim.c serves parsed descriptors from
 * static storage (see USB_GetDescriptors there). */
#define USB_GetDescriptors usbc_GetDescriptors_unused
#define USB_FreeDescriptors usbc_FreeDescriptors_unused
s32 USB_GetDescriptors(struct ehci_device *fd, usb_devdesc *udd);
void USB_FreeDescriptors(usb_devdesc *udd);
#include "usb.c"
#undef USB_GetDescriptors
#undef USB_FreeDescriptors
s32 USB_GetDescriptors(struct ehci_device *fd, usb_devdesc *udd);
void USB_FreeDescriptors(usb_devdesc *udd);

/* Hollywood's timer, from the simulated clock (ehci.c defines it on the Wii) */
#define get_timer() ((u32)(sim.now_us * 1898 / 1000))

#include "ehci_recover.h"
#define ehci_dbg(...) ((void)0)
#include "ehci_enum.h"
#include "usbstorage.h"
#include "usbstorage.c"
#include "fdport.h"

static struct ehci_device sim_dev = { .desc = { .idVendor = 0x1234, .idProduct = 0x5678 }, .id = 1 };

/* What USBStorage_Init() does once a port has a device. */
int ums_plug(void)
{
	sim.dev = &sim_dev;
	handshake_mode = 1;
	if (USBStorage_Try_Device(&sim_dev) != 0)
		return -1;
	first_access = true;
	handshake_mode = 0;
	ums_init_done = 1;
	unplug_device = 0;
	return 0;
}

/* The enumeration ehci_reset_port() does, with the real ehci_init_port() */
static int enum_hook(int port) { return ehci_init_port(port); }
int ums_enum_port(void)
{
	sim.dev = &ehci->devices[0];
	return ehci_init_port(0);
}
void ums_set_dev_id(int id) { ehci->devices[0].id = id; }
void ums_set_dev_id_port(int port, int id) { ehci->devices[port].id = id; }
int ums_select(int port) { return USBStorage_Select(port); }
int ums_selected(void) { return ums_cur; }
int ums_fd_port(int fd) { return fdport_get(fd); }
int ums_fd_set_port(int fd, unsigned port) { return fdport_set(fd, port, UMS_PORTS); }
void ums_fd_forget(int fd) { fdport_forget(fd); }
int ums_mounted(void) { return __mounted == 1; }
int ums_dev_id(int port) { return ehci->devices[port].id; }
void ums_umount(void) { USBStorage_Umount(); }

/* Forget the device without talking to it, for the next test. */
void ums_unplug(void)
{
	/* both logical ports, then back on port 0 */
	for (int p = UMS_PORTS - 1; p >= 0; p--) {
		USBStorage_Select(p);
		if (__usbfd.buffer)
			USB_Free(__usbfd.buffer);
		memset(&__usbfd, 0, sizeof(__usbfd));
		memset(&_old_udd, 0, sizeof(_old_udd));
		__lun = 16;
		__mounted = 0;
		ums_init_done = 0;
		unplug_device = 0;
		ums_dead = 0;
		ums_fail_n = 0;
		ums_gone_failed = 0;
		ehci->devices[p].id = 0;
		ehci->devices[p].toggles = 0;
	}
	for (int i = 0; i < PORT_FDS; i++)
		port_fd[i] = 0;
	handshake_mode = 0;
	usb_timeout = 1000 * 1000;
	sim_dev.toggles = 0;
	ehci_enum_disarm();
	sim_enum_hook = enum_hook;
}

/* The recovery helpers of ehci_recover.h, on the simulated controller */
void ums_startup(void)
{
	ehci_startup_recover();
}
int ums_power_cycle(void) { return ehci_port_power_cycle(0); }
int ums_quiesce(void)
{
	int r = ehci_async_quiesce();
	if (r)
		ehci_async_resume();
	return r;
}

/* UMS_INIT, the keepalive's check and the dead state, for the tests */
s32 ums_init(void) { return USBStorage_Init(); }
int ums_keepalive_ready(void) { return USBStorage_Keepalive_Ready(); }
int ums_is_dead(void) { return ums_dead; }

u32 ums_sector_size(void)
{
	u32 size = 0;
	USBStorage_Get_Capacity(&size);
	return size;
}

u32 ums_sectors(void)
{
	return USBStorage_Get_Capacity(NULL);
}

/* A Bulk-Only reset recovery as the driver does it (USBStorage_Reset). */
s32 ums_reset(void)
{
	return USBStorage_Reset(&__usbfd);
}

/* For test_ums.c: the driver's limit on recovering a failing drive. */
const unsigned ums_request_ms = USBSTORAGE_REQUEST_MS;
