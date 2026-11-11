#include <string.h>

#include "mem.h"
#include "module.h"
#include "syscalls.h"
#include "timer.h"
#include "types.h"
#include "usb2.h"
#include "usbglue.h"
#include "usbstorage.h"

/* Constants */
#define DEVLIST_MAXSIZE		8

/*
 * One drive per logical port (docs/usb-ports.md). IOS58 does not say which
 * socket a device is in, so port 0 is the first mass-storage device that
 * mounts and port 1 the second. Port 1 is looked at only after a client has
 * asked for it (usbstorage_Select(1)): a loader that never sends SET_PORT gets
 * what it always got.
 */
static struct {
	usbstorage_handle fd;	/* device handler */
#if USBS_PORTS > 1
	s32  id;		/* IOS's id of the device: two drives can share VID/PID */
#endif
	u16  vid, pid;		/* device info; 0 when the port has no drive */
	u8   lun;
	bool mounted;
} __d[USBS_PORTS];

/* Variables */
static bool __inited  = false;

/* With one port the drive is found by VID/PID alone, as it always was (the
   +dbg build has no room for more). */
#if USBS_PORTS > 1
#define USBS_SAME_ID(a, b)	((a) == (b))
#else
#define USBS_SAME_ID(a, b)	1
#endif

#if USBS_PORTS > 1
static __typeof__(__d[0]) *__c = __d;	/* the drive of the request in progress */
static u8   __ports   = 1;

void usbstorage_Select(u32 port)
{
	__c = &__d[port];

	if (port)
		__ports = USBS_PORTS;
}
#else
#define __ports	1
#define __c	__d
#endif

bool usbstorage_Startup(void)
{
	s32 ret;

	if (!__inited) {
		/* Initialize USB */
		ret = USB_Init();
		if (ret < 0)
			return false;

		/* Initialize USB Storage */
		ret = USBStorage_Initialize();
		if (ret < 0)
			return false;

		/* Set as initialized */
		__inited = true;
	}

	msleep(1);

	/* Setup message */
	os_message_queue_send(queuehandle, MESSAGE_MOUNT, 0);

	return true;
}

bool usbstorage_IsInserted(void)
{
	usb_device_entry *buffer;

	u8  device_count = 0, i, j, k;
	u16 vid, pid;
	s32 maxLun;
	s32 retval;

	/* USB not inited */
	if (!__inited)
		return false;

	/* Reset flags */
	for (k = 0; k < USBS_PORTS; k++)
		__d[k].mounted = false;

	/* Allocate memory */
	buffer = Mem_Alloc(DEVLIST_MAXSIZE * sizeof(usb_device_entry));
	if (!buffer)
		return false;

	memset(buffer, 0, DEVLIST_MAXSIZE * sizeof(usb_device_entry));

	/* Get device list (none, when that fails) */
	retval = USB_GetDeviceList(buffer, DEVLIST_MAXSIZE, USB_CLASS_MASS_STORAGE, &device_count);
	if (retval < 0)
		device_count = 0;
	else
		usleep(100);

	for (k = 0; k < __ports; k++) {
		/* Port k's drive, which this port had */
		if (__d[k].vid || __d[k].pid) {
			/* Search device */
			for (i = 0; i < device_count; i++) {
				if (USBS_SAME_ID(buffer[i].device_id, __d[k].id) && buffer[i].vid == __d[k].vid &&
				    buffer[i].pid == __d[k].pid) {
					/* Mounted */
					__d[k].mounted = true;
					break;
				}
			}

			if (!__d[k].mounted) {
				/* Gone: close it and forget it */
				USBStorage_Close(&__d[k].fd);

				__d[k].lun = 0;
				__d[k].vid = 0;
				__d[k].pid = 0;
			}

			continue;
		}

		for (i = 0; i < device_count; i++) {
			vid = buffer[i].vid;
			pid = buffer[i].pid;

			/* Wrong VID/PID */
			if (!vid || !pid)
				continue;

#if USBS_PORTS > 1
			/* The other port's drive */
			if (__d[k ^ 1].vid && __d[k ^ 1].id == buffer[i].device_id)
				continue;
#endif

			/* Open device */
			retval = USBStorage_Open(&__d[k].fd, buffer[i].device_id, vid, pid);
			if (retval < 0)
				continue;

			/* Get LUN */
			maxLun = USBStorage_GetMaxLUN(&__d[k].fd);

			/* Mount LUN */
			for (j = 0; j < maxLun; j++) {
				retval = USBStorage_MountLUN(&__d[k].fd, j);

				if (retval == USBSTORAGE_ETIMEDOUT)
					break;

				if (retval < 0)
					continue;

				/* Ignore encrypted Wii U drives */
				u8 *mbr = Mem_Alloc(512);
				if (mbr)
				{
					USBStorage_Read(&__d[k].fd, j, 0, 1, mbr);
					if (*(u32 *)mbr != 0x57424653 && *(u16 *)(mbr + 510) != 0x55AA && *(u16 *)(mbr + 510) != 0x55AB)
					{
						Mem_Free(mbr);
						continue;
					}
					Mem_Free(mbr);
				}

				/* Set parameters */
				__d[k].mounted = true;
				__d[k].lun = j;
				__d[k].vid = vid;
				__d[k].pid = pid;
#if USBS_PORTS > 1
				__d[k].id = buffer[i].device_id;
#endif

				break;
			}

			/* Device mounted */
			if (__d[k].mounted)
				break;

			/* Close device */
			USBStorage_Close(&__d[k].fd);
		}
	}

	/* Free memory */
	Mem_Free(buffer);

	return __c->mounted;
}

bool usbstorage_ReadSectors(u32 sector, u32 numSectors, void *buffer)
{
	s32 retval;

	if (!__c->mounted)
		return false;

	/* Read sectors */
	retval = USBStorage_Read(&__c->fd, __c->lun, sector, numSectors, buffer);

	return retval >= 0;
}

bool usbstorage_WriteSectors(u32 sector, u32 numSectors, const void *buffer)
{
	s32 retval;

	if (!__c->mounted)
		return false;

	/* Write sectors */
	retval = USBStorage_Write(&__c->fd, __c->lun, sector, numSectors, buffer);

	return retval >= 0;
}

bool usbstorage_ReadCapacity(u32 *sectorSz, u32 *numSectors)
{
	s32 retval;

	if (!__c->mounted)
		return false;

	/* Read capacity */
	retval = USBStorage_ReadCapacity(&__c->fd, __c->lun, sectorSz, numSectors);

	return retval >= 0;
}

bool usbstorage_Shutdown(void)
{
	/* Close device */
	if (__c->vid || __c->pid)
		USBStorage_Close(&__c->fd);

	return true;
}
