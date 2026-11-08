/*
 * Device enumeration after a port reset (ehci_init_port), with limits.
 * Included by ehci.c, and by the host tests (tests/host/ums/ums_unit.c) over
 * a simulated device. It needs ehci_control_message(), ehci_reset_usb_port(),
 * ehci_adquire_port(), USB_ClearHalt(), ehci_msleep(), get_timer(), ehci->devices
 * and ehci_recover.h (EHCI_TICKS_PER_MS, D2XDBG_EV).
 *
 * A device that never answers (a drive left stuck by a console reset in the
 * middle of a transfer) costs one transfer timeout, about 1 s, per request.
 * Nothing here may retry without a limit while the port shows a drive
 * (docs/usb-storage.md, rule 1): every loop has a count, and the callers that
 * answer a loader (UMS_INIT, the open of /dev/usb2) also arm an overall time
 * budget, so the call returns an error in bounded time.
 */
#ifndef EHCI_ENUM_H
#define EHCI_ENUM_H

/* Overall budgets of the callers that answer a loader. A healthy drive
   enumerates in well under a second; a slow USB-SATA bridge gets several tries. */
#define EHCI_ENUM_INIT_MS	8000	/* UMS_INIT: reset, enumerate, mount */
#define EHCI_ENUM_DISCOVER_MS	4000	/* the open of /dev/usb2, per port */

/* SET_ADDRESS is tried this many times (the loop had no way out before) */
#define EHCI_SETADDR_TRIES	3

static u32 ehci_enum_deadline;
static int ehci_enum_on;

void ehci_enum_arm(int ms)
{
	ehci_enum_deadline = get_timer() + ms * EHCI_TICKS_PER_MS;
	ehci_enum_on = 1;
}

void ehci_enum_disarm(void)
{
	ehci_enum_on = 0;
}

/* 1 when an armed budget is used up: no new attempt, no new reset */
int ehci_enum_expired(void)
{
	return ehci_enum_on && (s32)(get_timer() - ehci_enum_deadline) >= 0;
}

void ehci_adquire_port(int port);
s32 USB_ClearHalt(struct ehci_device *fd, u8 endpoint);

/* 'e' records why enumeration gave up: a = phase, b = result, x 'e' */
#define EHCI_ENUM_GIVE_UP(phase, ret)	do { \
		D2XDBG_EV(D2XDBG_M_EHCI, D2XDBG_E_RESET, 'e', phase, ret); \
		return -2201; } while (0)

int ehci_init_port(int port)
{
	struct ehci_device *dev = &ehci->devices[port];
	int retval = 0;
	int i, tries;

	dev->id = 0;
	ehci_msleep(50);
	for(i=0;i<3;i++)
	{
		// now the device has the default device id
		retval = ehci_control_message(dev,USB_CTRLTYPE_DIR_DEVICE2HOST,
				USB_REQ_GETDESCRIPTOR,USB_DT_DEVICE<<8,0,sizeof(dev->desc),&dev->desc);
		if (retval < 0) {
			retval=-2201;
			ehci_msleep(100);
			if (ehci_enum_expired())
				EHCI_ENUM_GIVE_UP(1, i);
		}
		else break;
	}
	if (retval < 0)
	{
		for(i=0;i<3;i++)
		{
			ehci_reset_usb_port(port);
			ehci_msleep(100);
			retval = ehci_control_message(dev,USB_CTRLTYPE_DIR_DEVICE2HOST,
					USB_REQ_GETDESCRIPTOR,USB_DT_DEVICE<<8,0,sizeof(dev->desc),&dev->desc);
			if (retval < 0) {
				retval=-2201;
				if (ehci_enum_expired())
					EHCI_ENUM_GIVE_UP(2, i);
			}
			else break;
		}
	}

	if (retval < 0)
	{
		for(i=0;i<3;i++)
		{
			ehci_adquire_port(port);
			ehci_msleep(100);
			ehci_reset_usb_port(port);
			ehci_msleep(100);
			retval = ehci_control_message(dev,USB_CTRLTYPE_DIR_DEVICE2HOST,
					USB_REQ_GETDESCRIPTOR,USB_DT_DEVICE<<8,0,sizeof(dev->desc),&dev->desc);
			if (retval < 0) {
				retval=-2201;
				if (ehci_enum_expired())
					EHCI_ENUM_GIVE_UP(3, i);
			}
			else break;
		}
	}

	if (retval < 0)
		EHCI_ENUM_GIVE_UP(4, retval);

	int cnt=2;
	tries=0;
	do{
		ehci_msleep(100);
		retval = ehci_control_message(dev,USB_CTRLTYPE_DIR_HOST2DEVICE,
					      USB_REQ_SETADDRESS,cnt,0,0,0);
		if (retval < 0) {
			ehci_dbg ( "unable to set device addr...\n");
			retval=-8000-cnt;
		}

		dev->toggles = 0;

		dev->id = cnt;

		USB_ClearHalt(dev, 0);
		ehci_msleep(100);
		retval = ehci_control_message(dev,USB_CTRLTYPE_DIR_DEVICE2HOST,
				USB_REQ_GETDESCRIPTOR,USB_DT_DEVICE<<8,0,sizeof(dev->desc),&dev->desc);

		if (retval < 0) {
			ehci_dbg ( "unable to get device desc...\n");
			retval=-2242;
			dev->id =0;
			/* The address never changed here: this loop had no limit, and a
			   device that answers the descriptor request but not SET_ADDRESS
			   kept it for ever. */
			if (++tries >= EHCI_SETADDR_TRIES || ehci_enum_expired()) {
				D2XDBG_EV(D2XDBG_M_EHCI, D2XDBG_E_RESET, 'e', 5, tries);
				return retval;
			}
		}

	}while(retval<0);

	return retval;
}

#endif
