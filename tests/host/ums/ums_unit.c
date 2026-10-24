/*
 * source/ehci-module/tinyehci/usbstorage.c, built for the host the way
 * tinyehci.c builds it for the module: one translation unit, with the EHCI
 * layer below it replaced by sim.c.
 */
#include <stdlib.h>
#include "sim.h"
#include "usb.h"

/* ehci.h, reduced to what usbstorage.c uses. */
#define __LINUX_EHCI_HCD_H
struct ehci_device {
	usb_devdesc desc;
	int id;
	int port;
	int fd;
	u32 toggles;
};
struct ehci_regs { u32 status; u32 port_status[4]; };
struct ehci_hcd { struct ehci_regs *regs; int num_port; struct ehci_device devices[4]; };
extern struct ehci_hcd *ehci;
void ehci_usleep(int time);
void ehci_msleep(int time);
int ehci_reset_port(int port);
int ehci_reset_port2(int port);
int ehci_reset_device(struct ehci_device *dev);
#define ehci_readl(a) (*(volatile u32 *)(a))

/* Globals that ehci.c and the module define. */
extern int unplug_device, handshake_mode;
extern u32 usb_timeout;
int try_status;
struct ehci_hcd *ehci;

#include "usbstorage.h"
#include "usbstorage.c"

static struct ehci_device sim_dev = { .desc = { .idVendor = 0x1234, .idProduct = 0x5678 }, .id = 1 };

/* What USBStorage_Init() does once a port has a device. */
int ums_plug(void)
{
	handshake_mode = 1;
	if (USBStorage_Try_Device(&sim_dev) != 0)
		return -1;
	first_access = true;
	handshake_mode = 0;
	ums_init_done = 1;
	unplug_device = 0;
	return 0;
}

/* Forget the device without talking to it, for the next test. */
void ums_unplug(void)
{
	if (__usbfd.buffer)
		USB_Free(__usbfd.buffer);
	memset(&__usbfd, 0, sizeof(__usbfd));
	memset(&_old_udd, 0, sizeof(_old_udd));
	__lun = 16;
	__mounted = 0;
	ums_init_done = 0;
	unplug_device = 0;
	handshake_mode = 0;
	usb_timeout = 1000 * 1000;
}

u32 ums_sector_size(void)
{
	u32 size = 0;
	USBStorage_Get_Capacity(&size);
	return size;
}
