/*
 * The USB port of each client fd of /dev/usb2 (docs/usb-ports.md). The loader,
 * the FAT module and the DIP plugin each have an fd of their own, and each may
 * use a different drive. An fd that never sent USB_IOCTL_UMS_SET_PORT is on
 * port 0, as before. Included by loop.c, and by the host tests
 * (tests/host/ums/ums_unit.c), which exercise it.
 *
 * Slots hold fd + 1, so that 0 is a free slot. Port 0 needs no slot, except for
 * an fd that sent UMS_INIT (fdport_init, EHCI module only), which is recorded
 * in its slot too.
 */
#ifndef FDPORT_H
#define FDPORT_H

/* Slots for the fds that have a port or sent UMS_INIT; 4 more, for the fds
   whose SET_PORT was refused (PORT_BAD), so that a table that is full can still
   say so */
#define PORT_FDS_USE 12
#define PORT_FDS 16

/* In port_of: fdport_get() answers PORT_BAD for an fd whose SET_PORT could not
   be honoured (see fdport_set); its drive requests must fail. Not a port number. */
#define PORT_BAD 0x7F
#define PORT_INIT 0x80			/* the fd sent UMS_INIT */

static s32 port_fd[PORT_FDS];
static u8  port_of[PORT_FDS];		/* the port or PORT_BAD, and PORT_INIT */

/* The index of key (an fd + 1) in a slot table, or -1. Key 0 finds a free slot. */
static int fd_find(const s32 *tab, int n, s32 key)
{
	int i;

	for (i = 0; i < n; i++)
		if (tab[i] == key)
			return i;

	return -1;
}

/* The slot of an fd; when it has none, a free one among the first n, cleared;
   -1 when there is none */
static int fdport_slot(s32 fd, int n)
{
	int i = fd_find(port_fd, PORT_FDS, fd + 1);

	if (i < 0) {
		i = fd_find(port_fd, n, 0);
		if (i >= 0) {
			port_fd[i] = fd + 1;
			port_of[i] = 0;
		}
	}

	return i;
}

/* The port of an fd, or PORT_BAD */
static u32 fdport_get(s32 fd)
{
	int i = fd_find(port_fd, PORT_FDS, fd + 1);

	return i < 0 ? 0 : port_of[i] & ~PORT_INIT;
}

/*
 * SET_PORT: 0, or -1 when it cannot be honoured: a port that does not exist, or
 * all slots taken. The caller ignores that result more often than not (USB
 * Loader GX does), and an fd left on port 0 would then read and write the
 * drive of port 0 while it means port 1. So the fd is marked PORT_BAD: its
 * requests fail until it sends SET_PORT 0, or SET_PORT for a port that works,
 * or is closed. (With all 16 slots taken even the mark has no room: that is
 * more fds than the module will ever have.)
 */
static s32 fdport_set(s32 fd, u32 port, u32 nports)
{
	int i = -1;

	if (port < nports) {
		/* Port 0 is the default: it needs no slot */
		i = port ? fdport_slot(fd, PORT_FDS_USE) : fd_find(port_fd, PORT_FDS, fd + 1);
		if (i >= 0) {
			port_of[i] = (port_of[i] & PORT_INIT) | port;
			if (!port_of[i])        /* port 0, and no UMS_INIT: no slot needed */
				port_fd[i] = 0;
		}
		if (!port || i >= 0)
			return 0;
	}

	i = fdport_slot(fd, PORT_FDS);
	if (i >= 0)
		port_of[i] = (port_of[i] & PORT_INIT) | PORT_BAD;

	return -1;
}

/* UMS_INIT came from this fd (the EHCI module's keepalive follows the fds that
   sent it). With the slots taken the fd is not recorded: the keepalive can then
   stop early, never run on. */
__attribute__((unused)) static void fdport_init(s32 fd)
{
	int i = fdport_slot(fd, PORT_FDS_USE);

	if (i >= 0)
		port_of[i] |= PORT_INIT;
}

/* How many open fds sent UMS_INIT */
__attribute__((unused)) static u32 fdport_init_count(void)
{
	u32 i, n = 0;

	for (i = 0; i < PORT_FDS; i++)
		n += port_of[i] >> 7;

	return n;
}

/* IOS_CLOSE, and a new open of the device (the fd number may be reused):
   1 when the fd had sent UMS_INIT */
static int fdport_forget(s32 fd)
{
	int i = fd_find(port_fd, PORT_FDS, fd + 1), init = 0;

	if (i >= 0) {
		init = port_of[i] >> 7;
		port_fd[i] = port_of[i] = 0;
	}

	return init;
}

#endif
