/*
 * source/ehci-module/tinyehci/usb.c and usbstorage.c, built for the host the
 * way tinyehci.c builds them for the module: one translation unit, with the
 * EHCI layer below them (ehci_control_message, ehci_bulk_message, port
 * resets) replaced by sim.c.
 */
#include <stdlib.h>
#include "sim.h"
#include "ehci_dev.h"

struct ehci_regs { u32 status; u32 port_status[4]; };
struct ehci_hcd { struct ehci_regs *regs; int num_port; struct ehci_device devices[4]; };
struct ehci_hcd *ehci;
#define ehci_readl(a) (*(volatile u32 *)(a))
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

#include "usbstorage.h"
#include "usbstorage.c"

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
	sim_dev.toggles = 0;
}

u32 ums_sector_size(void)
{
	u32 size = 0;
	USBStorage_Get_Capacity(&size);
	return size;
}

/* A Bulk-Only reset recovery as the driver does it (USBStorage_Reset). */
s32 ums_reset(void)
{
	return USBStorage_Reset(&__usbfd);
}

/* For test_ums.c: the driver's limit on recovering a failing drive. */
const unsigned ums_request_ms = USBSTORAGE_REQUEST_MS;
