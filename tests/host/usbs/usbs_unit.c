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

int glue_bulk(int id, unsigned char ep, unsigned short len, void *buf);
int glue_ctrl(int id, unsigned char type, unsigned char req, unsigned short value, unsigned short index,
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
 * clears a halt there is not known: glue_cancel_clears_halt picks. */
extern int glue_cancel_clears_halt;
unsigned int glue_timer(void);
s32 USB_CancelEndpoint(s32 devId, u8 endpoint)
{
	(void)devId;
	if (glue_cancel_clears_halt)
		glue_ctrl(devId, 0x02, USB_REQ_CLEARFEATURE, 0, endpoint, 0, NULL);
	return 0;
}

s32 USB_WriteBlkMsg(s32 devId, u8 ep, u16 len, void *data)
{
	return glue_bulk(devId, ep, len, data);
}

s32 USB_WriteCtrlMsg(s32 devId, u8 type, u8 req, u16 value, u16 index, u16 len, void *data)
{
	return glue_ctrl(devId, type, req, value, index, len, data);
}

s32 USB_GetConfiguration(s32 fd, u8 *configuration)
{
	return glue_ctrl(fd, 0x80, USB_REQ_GETCONFIG, 0, 0, 1, configuration);
}

s32 USB_SetConfiguration(s32 fd, u8 configuration)
{
	return glue_ctrl(fd, 0x00, USB_REQ_SETCONFIG, configuration, 0, 0, NULL);
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

/* Host addresses are 64 bits wide, so the module's MEM2 test (address bit
 * 0x10000000) cannot be true here. A test says which buffers count as aligned
 * MEM2; the alignment rule is the module's. */
extern int glue_mem2;
#define usbs_buffer_direct(p) (glue_mem2 && (((unsigned long)(p)) & 0x1F) == 0)

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

#define DEVLIST_MAX 8

/*
 * usbglue.c, the module's mount and port logic (docs/usb-ports.md), over a list
 * of devices a test sets and the simulated drives behind them. What main.c
 * does around it: usbstorage_Select() before each request of an fd, and the
 * mount message that UMS_INIT leaves in the queue, run here by usbg_init().
 * module.h is replaced by the three things the glue takes from it.
 */
#define _MODULE_H_
#define MESSAGE_MOUNT	0x10000001
#ifdef D2X_DEBUG
#define USBS_PORTS	1
#else
#define USBS_PORTS	2
#endif
s32 queuehandle = 1;
static int mount_pending;
s32 os_message_queue_send(s32 queue, u32 message, s32 flags)
{
	(void)queue; (void)flags;
	mount_pending = message == MESSAGE_MOUNT;
	return 0;
}
static usb_device_entry dev_list[DEVLIST_MAX];
static int dev_count;
s32 USB_Init(void) { return 0; }
s32 USB_GetDeviceList(usb_device_entry *buffer, u8 num_descr, u8 interface, u8 *cnt_descr)
{
	int n = dev_count < num_descr ? dev_count : num_descr;
	(void)interface;
	memcpy(buffer, dev_list, n * sizeof(*buffer));
	*cnt_descr = n;
	return 0;
}

#include "usbglue.c"

/* Forget everything: close what is open, no devices, one port selected */
void usbg_reset(void)
{
	for (int k = 0; k < USBS_PORTS; k++)
		if (__d[k].vid || __d[k].pid)
			USBStorage_Close(&__d[k].fd);
	memset(__d, 0, sizeof(__d));
	__inited = false;
	dev_count = 0;
	mount_pending = 0;
	usbstorage_Select(0);
#if USBS_PORTS > 1
	__ports = 1;
#endif
	glue_reset();
}

/* The devices IOS lists, in order; 0 is the first drive's. */
void usbg_clear_devices(void) { dev_count = 0; }
void usbg_add_device(int id, unsigned vid, unsigned pid)
{
	dev_list[dev_count].device_id = id;
	dev_list[dev_count].vid = vid;
	dev_list[dev_count].pid = pid;
	dev_count++;
}
void usbg_remove_device(int id)
{
	for (int i = 0; i < dev_count; i++)
		if (dev_list[i].device_id == id) {
			memmove(&dev_list[i], &dev_list[i + 1], (dev_count - i - 1) * sizeof(*dev_list));
			dev_count--;
			return;
		}
}

/* UMS_INIT of an fd on that port, and the mount message the module's loop then runs */
int usbg_init(int port)
{
	int ok;
	usbstorage_Select(port);
	ok = usbstorage_Startup();
	if (ok && mount_pending) {
		mount_pending = 0;
		usbstorage_IsInserted();
	}
	return ok;
}
int usbg_inserted(int port) { usbstorage_Select(port); return usbstorage_IsInserted(); }
int usbg_mounted(int port) { return __d[port].mounted; }
int usbg_read(int port, unsigned sector, unsigned n, void *buf)
{
	usbstorage_Select(port);
	return usbstorage_ReadSectors(sector, n, buf);
}
int usbg_write(int port, unsigned sector, unsigned n, const void *buf)
{
	usbstorage_Select(port);
	return usbstorage_WriteSectors(sector, n, buf);
}
/* GET_CAPACITY: the last LBA as the drive reports it, 0 when there is no drive */
unsigned usbg_capacity(int port)
{
	u32 size = 0, n = 0;
	usbstorage_Select(port);
	if (!usbstorage_ReadCapacity(&size, &n) || size != 512)
		return 0;
	return n;
}
int usbg_shutdown(int port) { usbstorage_Select(port); return usbstorage_Shutdown(); }
/* The distinct bounce buffers the open drives hold: 1 however many are open (0 in the +dbg build, which has one port) */
int usbg_buffers(void)
{
#if USBS_PORTS > 1
	int n = 0;
	for (int k = 0; k < USBS_PORTS; k++)
		if (__d[k].fd.buffer && (k == 0 || __d[k].fd.buffer != __d[0].fd.buffer))
			n++;
	return n;
#else
	return 0;
#endif
}
