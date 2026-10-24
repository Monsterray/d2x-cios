/*
 * source/usb-module/usbstorage.c (the USBS module's Bulk-Only driver) built
 * for the host, over IOS's USB API (usb2.h) implemented here on the simulated
 * drive (glue_sim.c). IOS's own USB stack sits between them on a Wii; here
 * the requests go straight to the drive.
 */
#include <stdlib.h>
#include <string.h>

/* sim.c already defines these names for the EHCI build's types */
#define USB_GetDescriptors usbsglue_GetDescriptors
#define USB_FreeDescriptors usbsglue_FreeDescriptors

#include "types.h"
#include "usb2.h"
#include "usbstorage.h"

int glue_bulk(unsigned char ep, unsigned short len, void *buf);
int glue_ctrl(unsigned char type, unsigned char req, unsigned short value, unsigned short index,
	      unsigned short len, void *buf);
void glue_reset(void);

void *Mem_Alloc(u32 size) { return calloc(1, size + 32); }
void Mem_Free(void *ptr) { free(ptr); }

s32 USB_OpenDevice(s32 devId) { (void)devId; return 1; }
s32 USB_CloseDevice(s32 *devId) { *devId = -1; return 0; }
s32 USB_DeviceRemovalNotify(s32 devId, usbcallback cb, void *userdata)
{
	(void)devId; (void)cb; (void)userdata;
	return 0;
}
s32 USB_SuspendResume(s32 devId, s32 resumed) { (void)devId; (void)resumed; return 0; }

/* IOS's request cancels the endpoint's pending transfers. Whether IOS also
 * clears a halt there is not modelled: the drive's halts stay. */
s32 USB_CancelEndpoint(s32 devId, u8 endpoint) { (void)devId; (void)endpoint; return 0; }

s32 USB_WriteBlkMsg(s32 devId, u8 ep, u16 len, void *data)
{
	(void)devId;
	return glue_bulk(ep, len, data);
}

s32 USB_WriteCtrlMsg(s32 devId, u8 type, u8 req, u16 value, u16 index, u16 len, void *data)
{
	(void)devId;
	return glue_ctrl(type, req, value, index, len, data);
}

s32 USB_GetConfiguration(s32 fd, u8 *configuration)
{
	(void)fd;
	return glue_ctrl(0x80, USB_REQ_GETCONFIG, 0, 0, 1, configuration);
}

s32 USB_SetConfiguration(s32 fd, u8 configuration)
{
	(void)fd;
	return glue_ctrl(0x00, USB_REQ_SETCONFIG, configuration, 0, 0, NULL);
}

s32 USB_SetAlternativeInterface(s32 fd, u8 interface, u8 alternate)
{
	(void)fd; (void)interface; (void)alternate;
	return 0;
}

/* The same drive as ../ums/sim.c describes: SCSI over Bulk-Only. */
static usb_endpointdesc eps[2];
static usb_interfacedesc intf;
static usb_configurationdesc conf;

s32 usbsglue_GetDescriptors(s32 devId, usb_devdesc *udd)
{
	(void)devId;
	memset(udd, 0, sizeof(*udd));
	udd->bLength = USB_DT_DEVICE_SIZE;
	udd->bDescriptorType = USB_DT_DEVICE;
	udd->idVendor = 0x1234;
	udd->idProduct = 0x5678;
	udd->bNumConfigurations = 1;
	udd->configurations = &conf;
	memset(&conf, 0, sizeof(conf));
	conf.bNumInterfaces = 1;
	conf.bConfigurationValue = 1;
	conf.interfaces = &intf;
	memset(&intf, 0, sizeof(intf));
	intf.bNumEndpoints = 2;
	intf.bInterfaceClass = 0x08;
	intf.bInterfaceSubClass = 0x06;
	intf.bInterfaceProtocol = 0x50;
	intf.endpoints = eps;
	memset(eps, 0, sizeof(eps));
	eps[0].bEndpointAddress = 0x81;
	eps[0].bmAttributes = 2;
	eps[0].wMaxPacketSize = 512;
	eps[1].bEndpointAddress = 0x02;
	eps[1].bmAttributes = 2;
	eps[1].wMaxPacketSize = 512;
	return 0;
}

void usbsglue_FreeDescriptors(usb_devdesc *udd)
{
	udd->configurations = NULL;
}

#include "usbstorage.c"

/* For test_usbs.c, in plain C types. */
static usbstorage_handle handle;

int usbs_mount(void)
{
	glue_reset();
	memset(&handle, 0, sizeof(handle));
	if (USBStorage_Open(&handle, 0, 0x1234, 0x5678) < 0)
		return -1;
	return USBStorage_MountLUN(&handle, 0);
}

int usbs_read(unsigned int sector, unsigned int n, void *buf)
{
	return USBStorage_Read(&handle, 0, sector, n, buf);
}

int usbs_write(unsigned int sector, unsigned int n, const void *buf)
{
	return USBStorage_Write(&handle, 0, sector, n, buf);
}

void usbs_close(void)
{
	USBStorage_Close(&handle);
}
