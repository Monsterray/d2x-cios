/*
 * The USB port of each client fd of /dev/usb2 (docs/usb-ports.md). The loader,
 * the FAT module and the DIP plugin each have an fd of their own, and each may
 * use a different drive. An fd that never sent USB_IOCTL_UMS_SET_PORT is on
 * port 0, as before. Included by loop.c, and by the host tests
 * (tests/host/ums/ums_unit.c), which exercise it.
 *
 * Slots hold fd + 1, so that 0 is a free slot. Port 0 needs no slot.
 */
#ifndef FDPORT_H
#define FDPORT_H

#define PORT_FDS 8

static s32 port_fd[PORT_FDS];
static u8  port_of[PORT_FDS];

/* The port of an fd */
static u32 fdport_get(s32 fd)
{
	int i;

	for (i = 0; i < PORT_FDS; i++)
		if (port_fd[i] == fd + 1)
			return port_of[i];

	return 0;
}

/* SET_PORT: 0, or -1 for a port that does not exist or when all slots are taken */
static s32 fdport_set(s32 fd, u32 port, u32 nports)
{
	int i, hit = -1, spare = -1;

	if (port >= nports)
		return -1;

	for (i = 0; i < PORT_FDS; i++) {
		if (port_fd[i] == fd + 1)
			hit = i;
		else if (!port_fd[i])
			spare = i;
	}

	if (hit < 0) {
		/* Port 0 is the default: it needs no slot */
		if (!port)
			return 0;
		if (spare < 0)
			return -1;

		hit = spare;
		port_fd[hit] = fd + 1;
	}

	port_of[hit] = port;

	return 0;
}

/* IOS_CLOSE */
static void fdport_forget(s32 fd)
{
	int i;

	for (i = 0; i < PORT_FDS; i++)
		if (port_fd[i] == fd + 1)
			port_fd[i] = 0;
}

#endif
