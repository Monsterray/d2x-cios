/*
 * Recovery helpers for a drive that stops answering (docs/usb-storage.md,
 * "The recovery ladder"). Included by ehci.c, and by the host tests
 * (tests/host/ums/ums_unit.c) over a simulated controller, so it needs only
 * ehci_readl/ehci_writel, ehci_usleep/ehci_msleep, get_timer() and ehci->regs,
 * ehci->hcs_params, ehci->num_port, ehci->async->qh_dma.
 *
 * Everything here waits with a limit. None of it runs while a transfer is in
 * flight: the module's loop is single-threaded, so it runs between transfers.
 */
#ifndef EHCI_RECOVER_H
#define EHCI_RECOVER_H

#ifdef D2X_DEBUG
#include "d2xdbg.h"
#else
#define D2XDBG_EV(mod, ev, x, a, b)	do {} while (0)
#endif

#define EHCI_TICKS_PER_MS	1898	/* get_timer(): Hollywood timer ticks per ms */

/* How long the controller gets to let go of an unlinked QH (the async advance
   doorbell), and then to stop the async schedule when it does not. */
#define EHCI_IAA_US		20000
#define EHCI_ASS_US		20000

/* Port power cycle: off for this long (the drive's bus power must decay), then
   the wait for it to reconnect, then a settle time (USB 2.0 7.1.7.3 debounce). */
#define EHCI_POWER_OFF_MS	300
#define EHCI_POWER_ON_WAIT_MS	1000
#define EHCI_POWER_SETTLE_MS	100

int ehci_reset_usb_port(int port);

/* Poll a register until (value & mask) == want. 0: done, -1: timed out, -2: the
   register reads all ones (the controller is gone). */
static int ehci_wait_bits(volatile u32 *reg, u32 mask, u32 want, u32 us)
{
	u32 t0 = get_timer();
	u32 limit = us / 1000 * EHCI_TICKS_PER_MS;

	for (;;) {
		u32 v = ehci_readl(reg);
		if (v == ~(u32)0)
			return -2;
		if ((v & mask) == want)
			return 0;
		if (get_timer() - t0 >= limit)
			return -1;
		ehci_usleep(50);
	}
}

/*
 * After the QH of a transfer is unlinked from the async list the controller
 * may still hold it (it caches the QH it is working on), so it must not be
 * freed or reused until the controller has moved on (EHCI 4.8.2: the async
 * advance doorbell). Ring the doorbell and wait for STS_IAA, W1C. If the
 * doorbell never comes, stop the async schedule (clear CMD_ASE, wait for
 * STS_ASS to clear): with the schedule stopped nothing holds the QH.
 *
 * Returns 0 when the QH is free to reuse (IAA came, or the controller is not
 * walking the list), 1 when the schedule was stopped: call ehci_async_resume()
 * after the qTDs are freed. Waits at most EHCI_IAA_US + EHCI_ASS_US.
 */
int ehci_async_quiesce(void)
{
	volatile u32 *cmd = &ehci->regs->command;
	volatile u32 *sts = &ehci->regs->status;
	u32 s = ehci_readl(sts);
	int r;

	/* not walking the list (schedule off, controller halted or gone) */
	if (s == ~(u32)0 || !(s & STS_ASS) || (s & STS_HALT))
		return 0;

	ehci_writel(STS_IAA, sts);			/* forget an old IAA (W1C) */
	ehci_writel(ehci_readl(cmd) | CMD_IAAD, cmd);
	r = ehci_wait_bits(sts, STS_IAA, STS_IAA, EHCI_IAA_US);
	ehci_writel(STS_IAA, sts);
	ehci_writel(ehci_readl(cmd) & ~CMD_IAAD, cmd);	/* as handshake() does after an IAA */
	D2XDBG_EV(D2XDBG_M_EHCI, D2XDBG_E_RESET, 'q', r, 0);
	if (r == 0)
		return 0;

	/* no doorbell: halt the async schedule */
	ehci_writel(ehci_readl(cmd) & ~CMD_ASE, cmd);
	r = ehci_wait_bits(sts, STS_ASS, 0, EHCI_ASS_US);
	D2XDBG_EV(D2XDBG_M_EHCI, D2XDBG_E_RESET, 'Q', r, 0);
	return 1;
}

/* Start the async schedule again after ehci_async_quiesce() returned 1. The
   list address register still points at the QH the controller stopped on:
   point it at the list head first (allowed while the schedule is off). */
void ehci_async_resume(void)
{
	volatile u32 *cmd = &ehci->regs->command;

	if (!(ehci_readl(&ehci->regs->status) & STS_ASS))
		ehci_writel(ehci->async->qh_dma, &ehci->regs->async_next);
	ehci_writel(ehci_readl(cmd) | CMD_ASE, cmd);
}

/* HCSPARAMS.PPC: the ports have power switches. Without it PORT_POWER is
   read-only and a power cycle is impossible. */
int ehci_ports_have_ppc(void)
{
	return HCS_PPC(ehci->hcs_params) != 0;
}

/*
 * Power a port off, wait, power it on, and wait for the drive to reconnect.
 * Only with PPC. PORTSC's change bits are write-1-to-clear, so every write
 * here masks them out; the connect change the power cycle itself causes is
 * cleared on purpose at the end, so that it is not taken for a replug by
 * someone else (usbstorage.c's dead state treats CSC as one).
 * Returns 0 when the drive is connected again, -1 when it is not, -2 without PPC.
 */
int ehci_port_power_cycle(int port)
{
	volatile u32 *reg = &ehci->regs->port_status[port];
	u32 s = ehci_readl(reg);
	int r;

	if (!ehci_ports_have_ppc())
		return -2;
	if (s & PORT_OWNER)
		return -1;
	ehci_writel(s & ~(PORT_RWC_BITS | PORT_POWER | PORT_PE | PORT_RESET), reg);
	ehci_msleep(EHCI_POWER_OFF_MS);
	s = ehci_readl(reg);
	ehci_writel((s & ~PORT_RWC_BITS) | PORT_POWER, reg);
	r = ehci_wait_bits(reg, PORT_CONNECT, PORT_CONNECT, EHCI_POWER_ON_WAIT_MS * 1000);
	ehci_msleep(EHCI_POWER_SETTLE_MS);
	s = ehci_readl(reg);
	if (s & PORT_CSC)
		ehci_writel((s & ~PORT_RWC_BITS) | PORT_CSC, reg);
	D2XDBG_EV(D2XDBG_M_EHCI, D2XDBG_E_RESET, 'C', r, s);
	return r == 0 ? 0 : -1;
}

/*
 * Leave the drive clean for this boot. A drive that a previous IOS or console
 * reset left in the middle of a transfer can sit in a state that enumeration
 * does not clear. Before the ports are enumerated: reset every port that has a
 * drive for EHCI, and with port power switches, power cycle it first. Runs once
 * at module start (EHCI_Init, after the non-USB2 ports are released).
 * Logs HCSPARAMS and PPC (D2X_DEBUG builds), so a bench run shows whether
 * Hollywood's EHCI can switch port power.
 */
void ehci_startup_recover(void)
{
	int i, ppc = ehci_ports_have_ppc();

	D2XDBG_EV(D2XDBG_M_EHCI, D2XDBG_E_RESET, 'c', ehci->hcs_params, ppc);
	for (i = 0; i < ehci->num_port; i++) {
		u32 s = ehci_readl(&ehci->regs->port_status[i]);
		if (!(s & PORT_CONNECT) || (s & PORT_OWNER))
			continue;
		if (ppc)
			ehci_port_power_cycle(i);
		ehci_reset_usb_port(i);
	}
}

#endif
