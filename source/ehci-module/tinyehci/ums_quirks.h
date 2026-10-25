/* Bench experiments in EHCI_DEBUG_LOG builds (USB_IOCTL_UMS_SET_QUIRKS):
   UMS_QUIRK_CBW12 sends every CBW with the padded length 12, as this driver
   did before it sent the command's own length (usbstorage.c);
   UMS_QUIRK_KEEP_TOGGLE leaves the host's data toggle as it was when a halt
   is cleared, as this driver did before (usb.c);
   UMS_QUIRK_PING starts each bulk-out transfer with a PING (ehci.c), the
   overlay state a NYET leaves and ehci_do_urb() clears. */
#ifndef UMS_QUIRKS_H
#define UMS_QUIRKS_H

#define UMS_QUIRK_CBW12		1
#define UMS_QUIRK_KEEP_TOGGLE	2
#define UMS_QUIRK_PING		4
#define UMS_QUIRK_FLIP_DATA	8	/* data-out phase starts on the other toggle */

#ifdef EHCI_DEBUG_LOG
static u32 ums_quirks;
#else
#define ums_quirks 0
#endif

#endif
