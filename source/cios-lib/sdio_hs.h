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
static inline int sdhs_group1_supported(const u8 *status)
{
	return (status[13] & 0x02) != 0;
}

/* Function group 1 selected: status bits 379:376; 1 means high speed was chosen */
static inline int sdhs_group1_selected(const u8 *status)
{
	return (status[16] & 0x0F) == 1;
}

static inline u32 sdhs_hostctl_enable(u32 hc)
{
	return (hc & 0xff) | SDHS_HOSTCTL_HISPEED;
}

static inline u32 sdhs_hostctl_disable(u32 hc)
{
	return (hc & 0xff) & ~SDHS_HOSTCTL_HISPEED;
}

/*
 * Outcome of the last __sd0_hs_try(), for the debug ioctlv 0x22 on
 * /dev/sdio/sdhc. One output vector of at least sizeof(struct sdhs_record)
 * (32) bytes; all fields big-endian (IOS and the PPC agree):
 *
 *   offset  size  field
 *   0       4     calls   times __sd0_hs_try() ran since the module started
 *   4       4     step    where the last call stopped (SDHS_STEP_*)
 *   8       4     rc      IOS return code of that step (0 when the step did
 *                         not fail on a return code: dead, unsupported, ...)
 *   12      20    status  bytes 0..19 of the last CMD6 answer (check or
 *                         switch): group 1 support is byte 13 (bit 1),
 *                         group 1 selected is byte 16 (low nibble)
 */
enum {
	SDHS_STEP_NOT_CALLED = 0,	/* __sd0_hs_try() has not run */
	SDHS_STEP_DEAD,			/* skipped: a fallback marked high speed dead */
	SDHS_STEP_CHECK_RC,		/* CMD6 check: IOS returned rc < 0 */
	SDHS_STEP_CHECK_UNSUPPORTED,	/* CMD6 check ok, group 1 function 1 not offered */
	SDHS_STEP_SWITCH_RC,		/* CMD6 switch: IOS returned rc < 0 */
	SDHS_STEP_SWITCH_NOT_SELECTED,	/* CMD6 switch ok, card did not select function 1 */
	SDHS_STEP_HOSTCTL_READ,		/* host control read failed */
	SDHS_STEP_HOSTCTL_WRITE,	/* host control write failed */
	SDHS_STEP_SETCLK,		/* SETCLK(0) failed; fell back */
	SDHS_STEP_ON			/* high speed is on */
};

#define SDHS_RECORD_STATUS_LEN		20

struct sdhs_record {
	u32 calls;
	s32 step;
	s32 rc;
	u8  status[SDHS_RECORD_STATUS_LEN];
} __attribute__((packed));

/* status may be NULL (the step had no CMD6 answer): the old bytes stay */
static inline void sdhs_record_step(struct sdhs_record *r, s32 step, s32 rc, const u8 *status)
{
	u32 i;

	r->step = step;
	r->rc = rc;
	if (status)
		for (i = 0; i < SDHS_RECORD_STATUS_LEN; i++)
			r->status[i] = status[i];
}

#endif
