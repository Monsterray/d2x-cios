/*-------------------------------------------------------------

usbstorage.c -- Bulk-only USB mass storage support

Copyright (C) 2008
Sven Peter (svpe) <svpe@gmx.net>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1.	The origin of this software must not be misrepresented; you
must not claim that you wrote the original software. If you use
this software in a product, an acknowledgment in the product
documentation would be appreciated but is not required.

2.	Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3.	This notice may not be removed or altered from any source
distribution.

-------------------------------------------------------------*/

#include <string.h>

#include "hollywood.h"
#include "ipc.h"
#include "mem.h"
#include "timer.h"
#include "usb2.h"
#include "usbstorage.h"

#ifdef D2X_DEBUG
#include "d2xdbg.h"
#else
#define D2XDBG_EV(mod, ev, x, a, b)	do {} while (0)
#endif

#define ROUNDDOWN32(v)			(((u32)(v)-0x1f)&~0x1f)

#define	HEAP_SIZE			(32*1024)
#define	TAG_START			0x0BADC0DE

#define	CBW_SIZE			31
#define	CBW_SIGNATURE			0x43425355
#define	CBW_IN				(1 << 7)
#define	CBW_OUT				0

#define	CSW_SIZE			13
#define	CSW_SIGNATURE			0x53425355

#define	SCSI_TEST_UNIT_READY		0x00
#define	SCSI_REQUEST_SENSE		0x03
#define	SCSI_READ_CAPACITY		0x25
#define	SCSI_READ_10			0x28
#define	SCSI_WRITE_10			0x2A

#define	SCSI_SENSE_REPLY_SIZE		18
#define	SCSI_SENSE_NOT_READY		0x02
#define	SCSI_SENSE_MEDIUM_ERROR		0x03
#define	SCSI_SENSE_HARDWARE_ERROR	0x04

#define	MASS_STORAGE_SCSI_COMMANDS	0x06
#define	MASS_STORAGE_BULK_ONLY		0x50

#define USBSTORAGE_GET_MAX_LUN		0xFE
#define USBSTORAGE_RESET		0xFF

#define	USB_ENDPOINT_BULK		0x02

#define USBSTORAGE_CYCLE_RETRIES	3

/* Bytes per bulk transfer, for both paths. IOS's /dev/usb/ven takes about 1 ms
   per transfer whatever its size (base 58 on a Wii: 3 ms for a 1-sector read,
   10 ms for 64 sectors in 4 KB pieces, 4 ms in 16 KB pieces), so fewer, bigger
   transfers are faster.

   USBS_MAX_TRANSFER is the piece size. A caller buffer that is unaligned or
   not in MEM2 goes through the bounce buffer (from the 32 KB module heap); a
   32-byte aligned MEM2 buffer goes to IOS as it is, in pieces of the same size.

   Do not raise it above 16384. The bulk wLength in usb2.c is a u16, but IOS58's
   /dev/usb/ven does not take that much: a 64-sector (32 KB) read in one direct
   transfer into an aligned MEM2 buffer never returned on a real Wii (base 58,
   IOS58 v6175); the console froze and needed a power cycle. The 8 s deadline
   cannot help, because the IPC call itself blocks. 16 KB works (64 sectors in
   4051 us, no failures), and libogc's MAX_TRANSFER_SIZE_V5 is 16 KB too, so
   the limit looks like 16 KB. Do not try a bigger size again without a timeout
   that IOS itself enforces. It can be set with -D (a variant build). */
#ifndef USBS_MAX_TRANSFER
#define USBS_MAX_TRANSFER			16384
#endif
#define MAX_TRANSFER_SIZE			USBS_MAX_TRANSFER

/* A caller buffer IOS can use as it is: 32-byte aligned and in MEM2 (0x10000000
   up). A test build replaces this, because host addresses are 64 bits wide. */
#ifndef usbs_buffer_direct
#define usbs_buffer_direct(p)		((((u32)(p)) & 0x1F) == 0 && (((u32)(p)) & 0x10000000))
#endif

/* IOS's USB driver reports a STALLed transfer as -7003 or -7004 (WiiBrew,
   /dev/usb/oh0, from gc-linux; not checked on a Wii). Any other negative
   result is a transfer error that needs a reset recovery. */
#define USBS_IS_STALL(r)		((r) == -7003 || (r) == -7004)

/* How many times a sector read or write is tried while the drive answers. */
#define USBSTORAGE_SECTOR_TRIES		4

/* How long one sector read or write may go on recovering a drive that keeps
   failing, in ms. After that no new attempt or reset recovery starts, and the
   caller gets an error instead of a wait that looks like a hang. A transfer
   that IOS's driver never ends still blocks (docs/usb-storage.md, USBS).
   Same value and rule as the EHCI module. */
#define USBSTORAGE_REQUEST_MS		8000

/* The Hollywood timer, about 1898 ticks per ms; it wraps after about 37
   minutes, which the signed difference below allows for. */
#ifndef usbs_get_timer
#define usbs_get_timer()		(*(vu32 *)HW_TIMER)
#endif
#define USBS_TICKS_PER_MS		1898

/* The deadline of the sector request in progress. Off outside a sector
   request, so mounting keeps its own limits. */
static u32 usbs_deadline;
static int usbs_deadline_on;

static void __deadline_start(void)
{
	usbs_deadline = usbs_get_timer() + USBSTORAGE_REQUEST_MS * USBS_TICKS_PER_MS;
	usbs_deadline_on = 1;
}

static int __deadline_passed(void)
{
	return usbs_deadline_on && (int)(usbs_get_timer() - usbs_deadline) >= 0;
}


static inline s32 Swap32(s32 val)
{
	return ((val & 0x000000FF) << 24) | ((val & 0x0000FF00) << 8) |
	       ((val & 0xFF000000) >> 24) | ((val & 0x00FF0000) >> 8);
}


static s32 __usbstorage_reset(usbstorage_handle *dev);
static s32 __usbstorage_clearerrors(usbstorage_handle *dev, u8 lun);


#ifdef D2X_DEBUG
/* A fault set through /dev/d2xdbg for this point (d2xdbg.h): the transfer's
   result in its place, or 1 to run the transfer. A STALL is IOS's -7004; a
   timeout or a lost CSW is USBSTORAGE_ETIMEDOUT; an error is -1. Delays and
   hangs happen in Dbg_Point() itself. *big is set for D2XDBG_F_BIG. */
static s32 __usbs_fault(u32 point, u32 arg, int *big)
{
	u32 f = D2XDBG_POINT(D2XDBG_M_USBS, point, arg);

	switch (D2XDBG_FAULT_ACTION(f)) {
	case 0:			return 1;
	case D2XDBG_F_BIG:	if (big) *big = 1; return 1;
	case D2XDBG_F_STALL:	return -7004;
	case D2XDBG_F_ERROR:	return -1;
	default:		return USBSTORAGE_ETIMEDOUT;
	}
}
#endif

static s32 __USB_BlkMsgTimeout(usbstorage_handle *dev, u8 bEndpoint, u16 wLength, void *rpData)
{
	s32 retval;

	retval = USB_WriteBlkMsg(dev->usb_fd, bEndpoint, wLength, rpData);
	if (retval < 0)
		USB_ClearHalt(dev->usb_fd, bEndpoint);

	return retval;
}

static s32 __USB_CtrlMsgTimeout(usbstorage_handle *dev, u8 bmRequestType, u8 bmRequest, u16 wValue, u16 wIndex, u16 wLength, void *rpData)
{
	return USB_WriteCtrlMsg(dev->usb_fd, bmRequestType, bmRequest, wValue, wIndex, wLength, rpData);
}

s32 USBStorage_Initialize()
{
	return 0;
}

static s32 __send_cbw(usbstorage_handle *dev, u8 lun, u32 len, u8 flags, const u8 *cb, u8 cbLen)
{
	u32 *buf = (u32 *)dev->buffer;
	s32 retval = USBSTORAGE_OK;

	if(cbLen == 0 || cbLen > 16)
		return IPC_EINVAL;

	memset(dev->buffer, 0, CBW_SIZE);

	buf[0] = Swap32(CBW_SIGNATURE);
	buf[1] = Swap32(dev->tag);
	buf[2] = Swap32(len);

	dev->buffer[12] = flags;
	dev->buffer[13] = lun;
	dev->buffer[14] = (cbLen > 6 ? 10 : 6);

	memcpy(dev->buffer + 15, cb, cbLen);

	if(dev->suspended == 1)
	{
		USB_ResumeDevice(dev->usb_fd);
		dev->suspended = 0;
	}

#ifdef D2X_DEBUG
	retval = __usbs_fault(D2XDBG_P_CMD, cb[0], NULL);
	if(retval == 1)
#endif
	retval = __USB_BlkMsgTimeout(dev, dev->ep_out, CBW_SIZE, (void *)dev->buffer);

	if(retval == CBW_SIZE) return USBSTORAGE_OK;
	else if(retval > 0) return USBSTORAGE_ESHORTWRITE;

	return retval;
}

static s32 __read_csw(usbstorage_handle *dev, u8 *status, u32 *dataResidue)
{
	u32 *buf = (u32 *)dev->buffer;
	s32 retval = USBSTORAGE_OK;
	u32 signature, tag, _dataResidue, _status;

	memset(dev->buffer, 0, CSW_SIZE);

#ifdef D2X_DEBUG
	retval = __usbs_fault(D2XDBG_P_CSW, 0, NULL);
	if(retval == 1)
#endif
	retval = __USB_BlkMsgTimeout(dev, dev->ep_in, CSW_SIZE, dev->buffer);
	if(retval > 0 && retval != CSW_SIZE) return USBSTORAGE_ESHORTREAD;
	else if(retval < 0) return retval;

	signature = Swap32(buf[0]);
	tag = Swap32(buf[1]);
	_dataResidue = Swap32(buf[2]);
	_status = dev->buffer[12];

	if(signature != CSW_SIGNATURE) return USBSTORAGE_ESIGNATURE;

	if(dataResidue != NULL)
		*dataResidue = _dataResidue;
	if(status != NULL)
		*status = _status;

	if(tag != dev->tag) return USBSTORAGE_ETAG;
	dev->tag++;

	return USBSTORAGE_OK;
}

/* The data phase of a read or a write, in pieces of MAX_TRANSFER_SIZE. Sent
   straight from or to the caller's buffer when IOS can take it as it is,
   else through dev->buffer. Returns < 0 when the transfer failed and the
   command needs a recovery (the caller resets the drive); a STALL ends the
   phase without an error (the CSW says why: BOT 6.7.2 and 6.7.3). A short
   read goes on to the CSW; a short write does not (EDATARESIDUE). */
static s32 __data_phase(usbstorage_handle *dev, u8 *buffer, u32 len, u8 write)
{
	s32 retval = USBSTORAGE_OK;
	u8 ep = write ? dev->ep_out : dev->ep_in;
	u32 thisLen;

	while(len > 0)
	{
		thisLen = len > MAX_TRANSFER_SIZE ? MAX_TRANSFER_SIZE : len;
#ifdef D2X_DEBUG
		{
			int big = 0;

			retval = __usbs_fault(D2XDBG_P_DATA, thisLen, &big);
			/* the whole data phase as one transfer, past IOS58's 16 KB
			   limit: the real /dev/usb/ven hang (REVIEW.md) */
			if (big && usbs_buffer_direct(buffer))
				thisLen = len;
		}
		if (retval != 1) {
			/* the fault's result stands in for the transfer */
		} else
#endif
		if (usbs_buffer_direct(buffer)) {
			retval = __USB_BlkMsgTimeout(dev, ep, thisLen, buffer);
		} else {
			if (write)
				memcpy(dev->buffer, buffer, thisLen);
			retval = __USB_BlkMsgTimeout(dev, ep, thisLen, dev->buffer);
			if (!write && retval > 0)
				memcpy(buffer, dev->buffer, retval);
		}

		if (retval == USBSTORAGE_ETIMEDOUT)
			break;

		if(USBS_IS_STALL(retval))
			return USBSTORAGE_OK;

		if(retval < 0)
			return write ? USBSTORAGE_EDATARESIDUE : retval;

		if(write && retval != thisLen)
			return USBSTORAGE_EDATARESIDUE;

		len -= retval;
		buffer += retval;

		if(!write && retval != thisLen)
			break;
	}

	return retval;
}

static s32 __cycle(usbstorage_handle *dev, u8 lun, u8 *buffer, u32 len, u8 *cb, u8 cbLen, u8 write, u8 *_status, u32 *_dataResidue)
{
	s32 retval = USBSTORAGE_OK;

	u8 status = 0;
	u32 dataResidue = 0;

	s8 retries = USBSTORAGE_CYCLE_RETRIES + 1;

	do
	{
		retries--;

		if(retval == USBSTORAGE_ETIMEDOUT)
			break;

		/* the sector request's time is used up: no more attempts */
		if(retval < 0 && __deadline_passed())
			break;

		/* each attempt sends the whole command again */
		retval = __send_cbw(dev, lun, len, write ? CBW_OUT : CBW_IN, cb, cbLen);
		if(retval >= 0)
			retval = __data_phase(dev, buffer, len, write);
		if(retval < 0)
			goto recover;

		retval = __read_csw(dev, &status, &dataResidue);

		/* The CSW read STALLed: clear the halt and read it once more (BOT
		   5.3.3). A drive that still sends none needs a reset recovery. */
		if(retval == USBSTORAGE_ESHORTREAD || USBS_IS_STALL(retval))
		{
			if(retval == USBSTORAGE_ESHORTREAD)
				USB_ClearHalt(dev->usb_fd, dev->ep_in);
			retval = __read_csw(dev, &status, &dataResidue);
		}

		if(retval < 0)
			goto recover;

		/* phase error: the drive needs a reset recovery (BOT 5.3.3.1) */
		if(status == 2)
		{
			retval = USBSTORAGE_ESTATUS;
			goto recover;
		}

		retval = USBSTORAGE_OK;
		continue;

recover:
		D2XDBG_EV(D2XDBG_M_USBS, D2XDBG_E_RESET, 'b', retval, retries);
		if(__usbstorage_reset(dev) == USBSTORAGE_ETIMEDOUT)
			retval = USBSTORAGE_ETIMEDOUT;
	} while(retval < 0 && retries > 0);

	if(retval < 0 && retval != USBSTORAGE_ETIMEDOUT)
	{
		if(__usbstorage_reset(dev) == USBSTORAGE_ETIMEDOUT)
			retval = USBSTORAGE_ETIMEDOUT;
	}

	if(_status != NULL)
		*_status = status;
	if(_dataResidue != NULL)
		*_dataResidue = dataResidue;

	return retval;
}

static s32 __usbstorage_clearerrors(usbstorage_handle *dev, u8 lun)
{
	s32 retval;
	u8 cmd[16];
	u8 sense[SCSI_SENSE_REPLY_SIZE];
	u8 status = 0;

	memset(cmd, 0, sizeof(cmd));
	cmd[0] = SCSI_TEST_UNIT_READY;

	retval = __cycle(dev, lun, NULL, 0, cmd, 1, 0, &status, NULL);
	if (retval < 0) return retval;

	if (status)
	{
		cmd[0] = SCSI_REQUEST_SENSE;
		cmd[1] = lun << 5;
		cmd[4] = SCSI_SENSE_REPLY_SIZE;
		cmd[5] = 0;
		memset(sense, 0, SCSI_SENSE_REPLY_SIZE);
		retval = __cycle(dev, lun, sense, SCSI_SENSE_REPLY_SIZE, cmd, 6, 0, NULL, NULL);
		if (retval>=0) {
			switch (sense[2]&0xF) {
				case SCSI_SENSE_NOT_READY:
					return USBSTORAGE_EINIT;
				case SCSI_SENSE_MEDIUM_ERROR:
				case SCSI_SENSE_HARDWARE_ERROR:
					return USBSTORAGE_ESENSE;
			}
		}
	}

	return retval;
}

static s32 __usbstorage_reset(usbstorage_handle *dev)
{
	s32 retval = __USB_CtrlMsgTimeout(dev, (USB_CTRLTYPE_DIR_HOST2DEVICE | USB_CTRLTYPE_TYPE_CLASS | USB_CTRLTYPE_REC_INTERFACE), USBSTORAGE_RESET, 0, dev->interface, 0, NULL);

	if (retval<0 && retval != -7004)
		goto end;

	//  don't clear the endpoints, it makes too many devices die

	//retval = USB_ClearHalt(dev->usb_fd, dev->ep_in);
	//if (retval < 0)	goto end;
	//retval = USB_ClearHalt(dev->usb_fd, dev->ep_out);

	usleep(100);

end:
	return retval;
}

/* The bounce buffer, shared by the drives of both ports and kept while one is
   open (docs/usb-ports.md). The +dbg build has one port, and no room to spare
   (module.h, USBS_PORTS): there the handle has its own, as it always had. */
#ifndef D2X_DEBUG
static u8 *usbs_bounce;
static u8 usbs_bounce_users;
#endif

s32 USBStorage_Open(usbstorage_handle *dev, s32 device_id, u16 vid, u16 pid)
{
	s32 retval = -1;
	u8 conf;
	u8 *max_lun;
	u32 iConf, iInterface, iEp;
	usb_devdesc udd;
	usb_configurationdesc *ucd;
	usb_interfacedesc *uid;
	usb_endpointdesc *ued;

	max_lun = Mem_Alloc(1);
	if (!max_lun)
		return IPC_ENOMEM;

	memset(dev, 0, sizeof(*dev));
	dev->usb_fd = -1;

	dev->tag = TAG_START;

	retval = USB_OpenDevice(device_id);
	if (retval < 0)
		goto free_and_return;

	dev->usb_fd = device_id;

	retval = USB_GetDescriptors(dev->usb_fd, &udd);
	if (retval < 0)
		goto free_and_return;

	for (iConf = 0; iConf < udd.bNumConfigurations; iConf++) {
		ucd = &udd.configurations[iConf];
		for (iInterface = 0; iInterface < ucd->bNumInterfaces; iInterface++) {
			uid = &ucd->interfaces[iInterface];
			if (uid->bInterfaceClass    == USB_CLASS_MASS_STORAGE &&
				uid->bInterfaceSubClass == MASS_STORAGE_SCSI_COMMANDS &&
				uid->bInterfaceProtocol == MASS_STORAGE_BULK_ONLY)
			{
				if (uid->bNumEndpoints < 2)
					continue;

				dev->ep_in = dev->ep_out = 0;
				for (iEp = 0; iEp < uid->bNumEndpoints; iEp++) {
					ued = &uid->endpoints[iEp];
					if (ued->bmAttributes != USB_ENDPOINT_BULK)
						continue;

					if (ued->bEndpointAddress & USB_ENDPOINT_IN)
						dev->ep_in = ued->bEndpointAddress;
					else
						dev->ep_out = ued->bEndpointAddress;
				}

				if (dev->ep_in != 0 && dev->ep_out != 0) {
					dev->configuration = ucd->bConfigurationValue;
					dev->interface = uid->bInterfaceNumber;
					dev->altInterface = uid->bAlternateSetting;
					goto found;
				}
			}
		}
	}

	USB_FreeDescriptors(&udd);
	retval = USBSTORAGE_ENOINTERFACE;
	goto free_and_return;

found:
	USB_FreeDescriptors(&udd);

	retval = USBSTORAGE_EINIT;
	// some devices return an error, ignore it
	USB_GetConfiguration(dev->usb_fd, &conf);

	if (conf != dev->configuration && (retval = USB_SetConfiguration(dev->usb_fd, dev->configuration)) < 0)
		goto free_and_return;

	if (dev->altInterface != 0 && USB_SetAlternativeInterface(dev->usb_fd, dev->interface, dev->altInterface) < 0)
		goto free_and_return;

	dev->suspended = 0;

	retval = USBStorage_Reset(dev);
	if (retval < 0)
		goto free_and_return;

	retval = __USB_CtrlMsgTimeout(dev, (USB_CTRLTYPE_DIR_DEVICE2HOST | USB_CTRLTYPE_TYPE_CLASS | USB_CTRLTYPE_REC_INTERFACE), USBSTORAGE_GET_MAX_LUN, 0, dev->interface, 1, max_lun);

	if (retval < 0)
		dev->max_lun = 1;
	else
		dev->max_lun = *max_lun + 1;

	if (retval == USBSTORAGE_ETIMEDOUT)
		goto free_and_return;

	retval = USBSTORAGE_OK;
	dev->sector_size = (u32 *)Mem_Alloc(dev->max_lun * sizeof(u32));
	if(!dev->sector_size) {
		retval = IPC_ENOMEM;
		goto free_and_return;
	}

	/* taken from linux usbstorage module (drivers/usb/storage/transport.c)
	 *
	 * Some devices (i.e. Iomega Zip100) need this -- apparently
	 * the bulk pipes get STALLed when the GetMaxLUN request is
	 * processed.   This is, in theory, harmless to all other devices
	 * (regardless of if they stall or not).
	 *
	 * 8/9/10: If anyone wants to actually use a Zip100, they can add this back.
	 * But for now, it seems to be breaking things more than it is helping.
	 */
	//USB_ClearHalt(dev->usb_fd, dev->ep_in);
	//USB_ClearHalt(dev->usb_fd, dev->ep_out);

#ifndef D2X_DEBUG
	/* The module serves one request at a time, and the heap has room for one
	   buffer: every open drive uses it. */
	if(!usbs_bounce_users)
		usbs_bounce = Mem_Alloc(MAX_TRANSFER_SIZE);
	dev->buffer = usbs_bounce;
#else
	dev->buffer = Mem_Alloc(MAX_TRANSFER_SIZE);
#endif

	if(!dev->buffer) {
		retval = IPC_ENOMEM;
	} else {
#ifndef D2X_DEBUG
		usbs_bounce_users++;
#endif
// 		USB_DeviceRemovalNotify(dev->usb_fd,__usb_deviceremoved_cb,dev);
		retval = USBSTORAGE_OK;
	}

free_and_return:
	if (max_lun)
		Mem_Free( max_lun);

	if (retval < 0) {
		USBStorage_Close(dev);
		return retval;
	}

	return 0;
}

s32 USBStorage_Close(usbstorage_handle *dev)
{
	if (dev->usb_fd != -1)
		USB_CloseDevice(&dev->usb_fd);

	Mem_Free(dev->sector_size);

#ifndef D2X_DEBUG
	if (dev->buffer && !--usbs_bounce_users)
#else
	if (dev->buffer)
#endif
		Mem_Free(dev->buffer);

	memset(dev, 0, sizeof(*dev));
	dev->usb_fd = -1;

	return 0;
}

s32 USBStorage_Reset(usbstorage_handle *dev)
{
	return __usbstorage_reset(dev);
}

s32 USBStorage_GetMaxLUN(usbstorage_handle *dev)
{
	return dev->max_lun;
}

s32 USBStorage_MountLUN(usbstorage_handle *dev, u8 lun)
{
	s32 retval;

	if(lun >= dev->max_lun)
		return IPC_EINVAL;

	retval = __usbstorage_clearerrors(dev, lun);
	if (retval<0)
		return retval;

	retval = USBStorage_ReadCapacity(dev, lun, &dev->sector_size[lun], NULL);
	return retval;
}

s32 USBStorage_ReadCapacity(usbstorage_handle *dev, u8 lun, u32 *sector_size, u32 *n_sectors)
{
	s32 retval;
	u8 cmd[10] = {SCSI_READ_CAPACITY, lun<<5};
	u8 response[8];

	retval = __cycle(dev, lun, response, sizeof(response), cmd, sizeof(cmd), 0, NULL, NULL);
	if(retval >= 0)
	{
		if(n_sectors != NULL)
			memcpy(n_sectors, response, 4);
		if(sector_size != NULL)
			memcpy(sector_size, response + 4, 4);
		retval = USBSTORAGE_OK;
	}

	return retval;
}

/* REQUEST SENSE after a failed command clears the condition (a drive may
   refuse everything until it is asked) */
static void __usbstorage_sense(usbstorage_handle *dev, u8 lun)
{
	u8 cmd[16];
	u8 sense[SCSI_SENSE_REPLY_SIZE];

	memset(cmd, 0, sizeof(cmd));
	cmd[0] = SCSI_REQUEST_SENSE;
	cmd[1] = lun << 5;
	cmd[4] = SCSI_SENSE_REPLY_SIZE;
	memset(sense, 0, SCSI_SENSE_REPLY_SIZE);
	__cycle(dev, lun, sense, SCSI_SENSE_REPLY_SIZE, cmd, 6, 0, NULL, NULL);
}

static s32 __USBStorage_Read(usbstorage_handle *dev, u8 lun, u32 sector, u16 n_sectors, u8 *buffer)
{
	u8 status = 0;
	s32 retval;
	u8 cmd[] = {
		SCSI_READ_10,
		lun << 5,
		sector >> 24,
		sector >> 16,
		sector >>  8,
		sector,
		0,
		n_sectors >> 8,
		n_sectors,
		0
	};

	retval = __cycle(dev, lun, buffer, n_sectors * dev->sector_size[lun], cmd, sizeof(cmd), 0, &status, NULL);
	// CSW status 1 or 2: the command failed (__cycle() returns 0 on success)
	if(retval >= 0 && status != 0)
	{
		__usbstorage_sense(dev, lun);
		retval = USBSTORAGE_ESTATUS;
	}

	return retval;
}

static s32 __USBStorage_Write(usbstorage_handle *dev, u8 lun, u32 sector, u16 n_sectors, const u8 *buffer)
{
	u8 status = 0;
	s32 retval;
	u8 cmd[] = {
		SCSI_WRITE_10,
		lun << 5,
		sector >> 24,
		sector >> 16,
		sector >> 8,
		sector,
		0,
		n_sectors >> 8,
		n_sectors,
		0
	};

	retval = __cycle(dev, lun, (u8 *)buffer, n_sectors * dev->sector_size[lun], cmd, sizeof(cmd), 1, &status, NULL);
	// CSW status 1 or 2: the command failed (__cycle() returns 0 on success)
	if(retval >= 0 && status != 0)
	{
		__usbstorage_sense(dev, lun);
		retval = USBSTORAGE_ESTATUS;
	}
	return retval;
}

/*
 * One sector read or write gives up on a failing drive after
 * USBSTORAGE_SECTOR_TRIES attempts or USBSTORAGE_REQUEST_MS, whichever comes
 * first. (The EHCI module's __USBStorage_Sectors() also waits, without limit,
 * for a drive that is unplugged; here IOS's driver owns the port, and a
 * request on a removed drive simply fails.)
 */
static s32 __USBStorage_Sectors(usbstorage_handle *dev, u8 lun, u32 sector, u16 n_sectors, u8 *buffer, u8 write)
{
	s32 retval;
	int tries = 0;

	if(lun >= dev->max_lun || dev->sector_size[lun] == 0)
		return IPC_EINVAL;

	__deadline_start();
	D2XDBG_EV(D2XDBG_M_USBS, D2XDBG_E_BEGIN, write ? 0x2A : 0x28, sector, n_sectors);

	for(;;)
	{
		if(write)
			retval = __USBStorage_Write(dev, lun, sector, n_sectors, buffer);
		else
			retval = __USBStorage_Read(dev, lun, sector, n_sectors, buffer);

		if(retval >= 0)
			break;

		if(retval == USBSTORAGE_ETIMEDOUT)
			D2XDBG_EV(D2XDBG_M_USBS, D2XDBG_E_TIMEOUT, 0, sector, tries);

		if(++tries >= USBSTORAGE_SECTOR_TRIES || __deadline_passed())
			break;
		D2XDBG_EV(D2XDBG_M_USBS, D2XDBG_E_RETRY, 0, tries, retval);
	}

	D2XDBG_EV(D2XDBG_M_USBS, D2XDBG_E_END, 0, retval, tries);
	usbs_deadline_on = 0;
	return retval;
}

s32 USBStorage_Read(usbstorage_handle *dev, u8 lun, u32 sector, u16 n_sectors, u8 *buffer)
{
	return __USBStorage_Sectors(dev, lun, sector, n_sectors, buffer, 0);
}

s32 USBStorage_Write(usbstorage_handle *dev, u8 lun, u32 sector, u16 n_sectors, const u8 *buffer)
{
	return __USBStorage_Sectors(dev, lun, sector, n_sectors, (u8 *)buffer, 1);
}

s32 USBStorage_Suspend(usbstorage_handle *dev)
{
	if(dev->suspended == 1)
		return USBSTORAGE_OK;

	USB_SuspendDevice(dev->usb_fd);
	dev->suspended = 1;

	return USBSTORAGE_OK;
}
