/* The parts of tinyehci's ehci.h that usb.c, usbstorage.c and the simulation
 * share: the device structure and its per-endpoint data toggles. */
#ifndef EHCI_DEV_H
#define EHCI_DEV_H

#include "types.h"
#include "usb.h"

#define __LINUX_EHCI_HCD_H      /* keep the real ehci.h out */

struct ehci_device {
	usb_devdesc desc;
	int id;
	int port;
	int fd;
	u32 toggles;            /* the host's DATA0/DATA1 toggle per endpoint */
};

#define ep_bit(ep) (((ep)&0xf)+(((ep)>>7)?16:0))
#define get_toggle(dev,ep) (((dev)->toggles>>ep_bit(ep))&1)
#define set_toggle(dev,ep,v) (dev)->toggles = ((dev)->toggles &(~(1<<ep_bit(ep)))) | ((v)<<ep_bit(ep))

/* The EHCI layer under usb.c, simulated in sim.c. */
s32 ehci_control_message(struct ehci_device *dev, u8 bmRequestType, u8 bmRequest, u16 wValue,
			 u16 wIndex, u16 wLength, void *buf);
s32 ehci_bulk_message(struct ehci_device *dev, u8 bEndpoint, u16 wLength, void *rpData);
void ehci_usleep(int time);
void ehci_msleep(int time);
int ehci_reset_port(int port);
int ehci_reset_port2(int port);
int ehci_reset_device(struct ehci_device *dev);

#endif
