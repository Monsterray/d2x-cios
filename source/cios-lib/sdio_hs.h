/*
 * SD high speed (50 MHz class) helpers for sdio.c. Pure functions with no
 * hardware access, so the host tests can run them (docs/sd-high-speed.md).
 * Included only in builds with -DSDIO_HIGH_SPEED.
 *
 * CMD6 (SWITCH_FUNC) answers with a 512-bit (64-byte) status block, sent
 * most significant bit first: bit N of the status is bit (N & 7) of byte
 * 63 - N / 8.
 */
#ifndef _SDIO_HS_H_
#define _SDIO_HS_H_

#include "types.h"

#define SDHS_CMD6			0x06
#define SDHS_CMD6_CHECK			0x00FFFFF1	/* mode 0, group 1 function 1: ask only */
#define SDHS_CMD6_SWITCH		0x80FFFFF1	/* mode 1, group 1 function 1: switch */
#define SDHS_STATUS_LEN			64
#define SDHS_HOSTCTL_HISPEED		0x04		/* host control (0x28) bit 2 */

/* Function group 1 support: status bits 415:400, bit 401 is function 1 (high speed) */
static int sdhs_group1_supported(const u8 *status)
{
	return (status[13] & 0x02) != 0;
}

/* Function group 1 selected: status bits 379:376; 1 means high speed was chosen */
static int sdhs_group1_selected(const u8 *status)
{
	return (status[16] & 0x0F) == 1;
}

static u32 sdhs_hostctl_enable(u32 hc)
{
	return (hc & 0xff) | SDHS_HOSTCTL_HISPEED;
}

static u32 sdhs_hostctl_disable(u32 hc)
{
	return (hc & 0xff) & ~SDHS_HOSTCTL_HISPEED;
}

#endif
