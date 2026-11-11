/*
 * Taking an EHCI root port back from the OHCI companion controller, for the
 * second USB socket (docs/usb-ports.md). Included by ehci_enum.h, so it is in
 * ehci.c's unit (and the host tests') without a change to ehci.c.
 *
 * ehci_release_ports() hands the ports with nothing on them, and port 1, to
 * OHCI at boot and "we can't kick OHCI drivers later". The way back is the one
 * of d2x v9 (source/ehci-module/tinyehci/ehci.c, ehci_acquire_port): route the
 * port to the companion, route it back to EHCI with the port disabled, and
 * power and enable it. A full-speed device then stays with OHCI by itself
 * (ehci_reset_usb_port() releases a port that does not enable).
 *
 * Needs ehci->regs, ehci_readl/ehci_writel, ehci_msleep and the PORT_* bits.
 */
#ifndef EHCI_ACQUIRE_H
#define EHCI_ACQUIRE_H

void ehci_acquire_port(int port)
{
	volatile u32 *status_reg = &ehci->regs->port_status[port];
	u32 status = ehci_readl(status_reg);

	/* to the companion, port disabled */
	status |= PORT_OWNER;
	status &= ~(PORT_PE | PORT_RWC_BITS);
	ehci_writel(status, status_reg);
	ehci_msleep(5);

	/* and back to EHCI */
	status = ehci_readl(status_reg);
	status &= ~(PORT_OWNER | PORT_PE | PORT_RWC_BITS);
	ehci_writel(status, status_reg);
	ehci_msleep(5);

	/* power on and let a connected device show itself */
	ehci_writel(PORT_POWER | PORT_CONNECT, status_reg);
	ehci_msleep(60);
}

#endif
