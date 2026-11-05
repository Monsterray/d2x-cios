/*
	Hardware routines for reading and writing to the Wii's internal
	SD slot.

	Copyright (c) 2008
	Michael Wiedenbauer (shagkur)
	Dave Murphy (WinterMute)
	Sven Peter <svpe@gmx.net>

	Redistribution and use in source and binary forms, with or without modification,
	are permitted provided that the following conditions are met:

	1. Redistributions of source code must retain the above copyright notice,
	   this list of conditions and the following disclaimer.
	2. Redistributions in binary form must reproduce the above copyright notice,
	   this list of conditions and the following disclaimer in the documentation and/or
	   other materials provided with the distribution.
	3. The name of the author may not be used to endorse or promote products derived
	   from this software without specific prior written permission.

	THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
	WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
	AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE
	LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
	DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
	LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
	THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
	(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
	EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include <stdlib.h>
#include <string.h>

#include "syscalls.h"
#include "timer.h"
#ifdef SDIO_HIGH_SPEED
#include "sdio_hs.h"
#endif


/* Page size */
#define PAGE_SIZE512			512

/* Commands */
#define SDIOHCR_RESPONSE		0x10
#define SDIOHCR_HOSTCONTROL		0x28
#define SDIOHCR_POWERCONTROL		0x29
#define SDIOHCR_CLOCKCONTROL		0x2c
#define SDIOHCR_TIMEOUTCONTROL		0x2e
#define SDIOHCR_SOFTWARERESET		0x2f
#define SDIOHCR_HOSTCONTROL_4BIT	0x02

#define SDIO_DEFAULT_TIMEOUT		0xe
 
#define IOCTL_SDIO_WRITEHCREG		0x01
#define IOCTL_SDIO_READHCREG		0x02
#define IOCTL_SDIO_READCREG		0x03
#define IOCTL_SDIO_RESETCARD		0x04
#define IOCTL_SDIO_WRITECREG		0x05
#define IOCTL_SDIO_SETCLK		0x06
#define IOCTL_SDIO_SENDCMD		0x07
#define IOCTL_SDIO_SETBUSWIDTH		0x08
#define IOCTL_SDIO_READMCREG		0x09
#define IOCTL_SDIO_WRITEMCREG		0x0A
#define IOCTL_SDIO_GETSTATUS		0x0B
#define IOCTL_SDIO_GETOCR		0x0C
#define IOCTL_SDIO_READDATA		0x0D
#define IOCTL_SDIO_WRITEDATA		0x0E
 
#define SDIOCMD_TYPE_BC			1
#define SDIOCMD_TYPE_BCR		2
#define SDIOCMD_TYPE_AC			3
#define SDIOCMD_TYPE_ADTC		4
 
#define SDIO_RESPONSE_NONE		0
#define SDIO_RESPONSE_R1		1
#define SDIO_RESPONSE_R1B		2
#define SDIO_RESPOSNE_R2		3
#define SDIO_RESPONSE_R3		4
#define SDIO_RESPONSE_R4		5
#define SDIO_RESPONSE_R5		6
#define SDIO_RESPONSE_R6		7
 
#define SDIO_CMD_GOIDLE			0x00
#define	SDIO_CMD_ALL_SENDCID		0x02
#define SDIO_CMD_SENDRCA		0x03
#define SDIO_CMD_SELECT			0x07
#define SDIO_CMD_DESELECT		0x07
#define	SDIO_CMD_SENDIFCOND		0x08
#define SDIO_CMD_SENDCSD		0x09
#define SDIO_CMD_SENDCID		0x0A
#define SDIO_CMD_SENDSTATUS		0x0D
#define SDIO_CMD_SETBLOCKLEN		0x10
#define SDIO_CMD_READBLOCK		0x11
#define SDIO_CMD_READMULTIBLOCK		0x12
#define SDIO_CMD_WRITEBLOCK		0x18
#define SDIO_CMD_WRITEMULTIBLOCK	0x19
#define SDIO_CMD_APPCMD			0x37
 
#define SDIO_ACMD_SETBUSWIDTH		0x06
#define SDIO_ACMD_SENDSCR		0x33
#define	SDIO_ACMD_SENDOPCOND		0x29

#define	SDIO_STATUS_CARD_INSERTED	0x1
#define	SDIO_STATUS_CARD_INITIALIZED	0x10000
#define SDIO_STATUS_CARD_SDHC		0x100000

/* SDIO structures */
struct _sdiorequest
{
	u32 cmd;
	u32 cmd_type;
	u32 rsp_type;
	u32 arg;
	u32 blk_cnt;
	u32 blk_size;
	void *dma_addr;
	u32 isdma;
	u32 pad0;
};
 
struct _sdioresponse
{
	u32 rsp_fields[3];
	u32 acmd12_response;
};

/* Variables */
static s32 __sd0_fd = -1;
static u16 __sd0_rca = 0;
static s32 __sd0_initialized = 0;
static s32 __sd0_sdhc = 0;
static u8  __sd0_cid[16];
static s32 __sdio_initialized = 0;
#ifdef SDIO_HIGH_SPEED
static u8  __sd0_swstatus[SDHS_STATUS_LEN] ATTRIBUTE_ALIGN(32);
static s32 __sd0_hs_on = 0;	/* host clock is at the high speed rate */
static s32 __sd0_hs_dead = 0;	/* a transfer failed at high speed: never again this session */
#endif
#ifdef SDIO_HS_DEBUG
static struct sdhs_record __sd0_hs_rec;	/* outcome of the last __sd0_hs_try() */
#define HS_STEP(step, rc, st)	sdhs_record_step(&__sd0_hs_rec, step, rc, st)
#else
#define HS_STEP(step, rc, st)	do { } while (0)
#endif

/* Device */
static char _sd0_fs[] ATTRIBUTE_ALIGN(32) = "/dev/sdio/slot0";

/* Buffers */
static struct _sdiorequest __request ATTRIBUTE_ALIGN(32);
static struct _sdioresponse __response ATTRIBUTE_ALIGN(32);
static ioctlv __iovec[3] ATTRIBUTE_ALIGN(32);
static u8 __buffer1[32]  ATTRIBUTE_ALIGN(32);
static u8 __buffer2[32]  ATTRIBUTE_ALIGN(32);


static s32 __sdio_sendcommand(u32 cmd, u32 cmd_type, u32 rsp_type, u32 arg, u32 blk_cnt, u32 blk_size, void *buffer, void *reply, u32 rlen)
{
	ioctlv *iovec = __iovec;
	struct _sdiorequest *request = &__request;
	struct _sdioresponse *response = &__response;

	s32 ret;

	/* Prepare request */
	request->cmd = cmd;
	request->cmd_type = cmd_type;
	request->rsp_type = rsp_type;
	request->arg = arg;
	request->blk_cnt = blk_cnt;
	request->blk_size = blk_size;
	request->dma_addr = buffer;
	request->isdma = ((buffer!=NULL)?1:0);
	request->pad0 = 0;

	/* Flush cache */
	os_sync_after_write(request,  sizeof(struct _sdiorequest));
	os_sync_after_write(response, sizeof(struct _sdioresponse));

	if (buffer)
		os_sync_after_write(buffer, blk_size * blk_cnt);
 
	/* DMA request */
	if(request->isdma || __sd0_sdhc == 1) {
		/* Prepare vector */
		iovec[0].data = request;
		iovec[0].len  = sizeof(struct _sdiorequest);
		iovec[1].data = buffer;
		iovec[1].len  = (blk_size*blk_cnt);
		iovec[2].data = response;
		iovec[2].len  = sizeof(struct _sdioresponse);

		os_sync_after_write(iovec, sizeof(ioctlv) * 3);

		/* Send command */
		ret = os_ioctlv(__sd0_fd, IOCTL_SDIO_SENDCMD, 2, 1, iovec);
	}else
		ret = os_ioctl(__sd0_fd,  IOCTL_SDIO_SENDCMD, request, sizeof(struct _sdiorequest), response, sizeof(struct _sdioresponse));

	/* Invalidate cache */
	os_sync_before_read(response, sizeof(struct _sdioresponse));

	if (buffer)
		os_sync_before_read(buffer, blk_size * blk_cnt);

	/* Copy response */
	if (reply && (rlen <= 16))
		memcpy(reply, response, rlen);

	return ret;
}
 
static s32 __sdio_setclock(u32 set)
{
	u32 *clock = (u32 *)__buffer1;

	*clock = set;
	os_sync_after_write(clock, 4);

	/* Send command */
	return os_ioctl(__sd0_fd, IOCTL_SDIO_SETCLK, clock, sizeof(u32), NULL, 0);
}

static s32 __sdio_getstatus(void)
{
	u32 *status = (u32 *)__buffer1;
	s32 ret;
 
	os_sync_after_write(status, 4);

	/* Send command */
	ret = os_ioctl(__sd0_fd, IOCTL_SDIO_GETSTATUS, NULL, 0, status, sizeof(u32));
	if (ret < 0)
		return ret;

	return *status;
}
 
static s32 __sdio_resetcard(void)
{
	u32 *status = (u32 *)__buffer1;
	s32 ret;
    
	os_sync_after_write(status, 4);

	__sd0_rca = 0;

	/* Send command */
	ret = os_ioctl(__sd0_fd, IOCTL_SDIO_RESETCARD, NULL, 0, status, sizeof(u32));
	if (ret < 0)
		return ret;
 
	__sd0_rca = (u16)(*status >> 16);

	return (*status & 0xffff);
}
 
static s32 __sdio_gethcr(u8 reg, u8 size, u32 *val)
{
	u32 *hcr_value = (u32 *)__buffer1;
	u32 *hcr_query = (u32 *)__buffer2;

	s32 ret;

	if (!val)
		return -1;

	*hcr_value = 0;
	*val = 0;

	/* Setup query */
	hcr_query[0] = reg;
	hcr_query[1] = 0;
	hcr_query[2] = 0;
	hcr_query[3] = size;
	hcr_query[4] = 0;
	hcr_query[5] = 0;

	os_sync_after_write(hcr_value, sizeof(u32));
	os_sync_after_write(hcr_query, sizeof(u32) * 6);

	/* Send command */
	ret = os_ioctl(__sd0_fd, IOCTL_SDIO_READHCREG, (void *)hcr_query, 24, hcr_value, sizeof(u32));

	*val = *hcr_value;

	return ret;
}
 
static s32 __sdio_sethcr(u8 reg, u8 size, u32 data)
{
	u32 *hcr_query = (u32 *)__buffer1;

	/* Setup query */
	hcr_query[0] = reg;
	hcr_query[1] = 0;
	hcr_query[2] = 0;
	hcr_query[3] = size;
	hcr_query[4] = data;
	hcr_query[5] = 0;
    
	os_sync_after_write(hcr_query, sizeof(u32) * 6);

	/* Send command */
	return os_ioctl(__sd0_fd, IOCTL_SDIO_WRITEHCREG, (void *)hcr_query, 24, NULL, 0);
}

static s32 __sdio_waithcr(u8 reg, u8 size, u8 unset, u32 mask)
{
	u32 val;
	s32 ret;
	s32 tries = 10;

	while(tries-- > 0)
	{
		ret = __sdio_gethcr(reg, size, &val);
		if (ret < 0)
			return ret;

		if ((unset && !(val & mask)) || (!unset && (val & mask)))
			return 0;

		usleep(10000);
	}

	return -1;
}
 
static s32 __sdio_setbuswidth(u32 bus_width)
{
	s32 ret;
	u32 hc_reg = 0;
 
	/* Get HCR */
	ret = __sdio_gethcr(SDIOHCR_HOSTCONTROL, 1, &hc_reg);
	if (ret < 0)
		return ret;
 
	hc_reg &= 0xff;
	hc_reg &= ~SDIOHCR_HOSTCONTROL_4BIT;
	if (bus_width == 4)
		hc_reg |= SDIOHCR_HOSTCONTROL_4BIT;
 
	/* Set HCR */
	return __sdio_sethcr(SDIOHCR_HOSTCONTROL, 1, hc_reg);		
}
 
static s32 __sd0_getrca(void)
{
	s32 ret;
	u32 rca;
 
	/* Send command */
	ret = __sdio_sendcommand(SDIO_CMD_SENDRCA, 0, SDIO_RESPONSE_R5, 0, 0, 0, NULL, &rca, sizeof(rca));	
	if (ret < 0)
		return ret;

	/* Get RCA */
	__sd0_rca = (u16)(rca >> 16);

	return (rca & 0xffff);
}
 
static s32 __sd0_select(void)
{
	/* Send command */
	return __sdio_sendcommand(SDIO_CMD_SELECT, SDIOCMD_TYPE_AC, SDIO_RESPONSE_R1B, (__sd0_rca << 16), 0, 0, NULL, NULL, 0);
}
 
static s32 __sd0_deselect(void)
{
	/* Send command */
	return __sdio_sendcommand(SDIO_CMD_DESELECT, SDIOCMD_TYPE_AC, SDIO_RESPONSE_R1B, 0, 0, 0, NULL, NULL, 0);
}
 
static s32 __sd0_setblocklength(u32 blk_len)
{
	/* Send command */
	return __sdio_sendcommand(SDIO_CMD_SETBLOCKLEN, SDIOCMD_TYPE_AC, SDIO_RESPONSE_R1, blk_len, 0, 0, NULL, NULL, 0);
}
 
static s32 __sd0_setbuswidth(u32 bus_width)
{
	u16 val;
	s32 ret;

	/* Set value */
	val = (bus_width == 4) ? 0x0002 : 0x0000;

	/* Send command */
	ret = __sdio_sendcommand(SDIO_CMD_APPCMD, SDIOCMD_TYPE_AC, SDIO_RESPONSE_R1, (__sd0_rca << 16), 0, 0, NULL, NULL, 0);
	if (ret < 0)
		return ret;

	/* Send command */
	return __sdio_sendcommand(SDIO_ACMD_SETBUSWIDTH, SDIOCMD_TYPE_AC, SDIO_RESPONSE_R1, val, 0, 0, NULL, NULL, 0);
}
 
static s32 __sd0_getcid(void)
{
	/* Send command */
	return __sdio_sendcommand(SDIO_CMD_ALL_SENDCID, 0, SDIO_RESPOSNE_R2, (__sd0_rca << 16), 0, 0, NULL, __sd0_cid, 16);
}

#ifdef SDIO_HIGH_SPEED
/* CMD6 with a 64-byte data block; the card must be selected. IOS wants
 * cmd_type 1 (BC) here, as libogc's wiisd.c sends it: sent as an ADTC (4) it
 * returns rc 0 but never delivers the data stage (the status stays zeroed). */
static s32 __sd0_switchfunc(u32 arg)
{
	memset(__sd0_swstatus, 0, sizeof(__sd0_swstatus));

	return __sdio_sendcommand(SDHS_CMD6, SDIOCMD_TYPE_BC, SDIO_RESPONSE_R1, arg, 1, SDHS_STATUS_LEN, __sd0_swstatus, NULL, 0);
}

/* Back to default speed for the rest of the session. Safe at any speed. */
static void __sd0_hs_fallback(void)
{
	u32 hc;

	if (!__sd0_hs_on)
		return;

	__sd0_hs_on = 0;
	__sd0_hs_dead = 1;

	if (__sdio_gethcr(SDIOHCR_HOSTCONTROL, 1, &hc) >= 0)
		__sdio_sethcr(SDIOHCR_HOSTCONTROL, 1, sdhs_hostctl_disable(hc));
	__sdio_setclock(1);
}

/* Card selected, 4-bit mode on. Any failure leaves default speed, silently;
 * where it stopped is kept in __sd0_hs_rec (ioctlv 0x22). */
static void __sd0_hs_try(void)
{
	u32 hc;
	s32 ret;

#ifdef SDIO_HS_DEBUG
	__sd0_hs_rec.calls++;
#endif

	if (__sd0_hs_dead) {
		HS_STEP(SDHS_STEP_DEAD, 0, NULL);
		return;
	}

	/* CMD6 check, then switch; an SD 1.0 card fails or reports nothing */
	ret = __sd0_switchfunc(SDHS_CMD6_CHECK);
	if (ret < 0) {
		HS_STEP(SDHS_STEP_CHECK_RC, ret, __sd0_swstatus);
		return;
	}
	if (!sdhs_group1_supported(__sd0_swstatus)) {
		HS_STEP(SDHS_STEP_CHECK_UNSUPPORTED, ret, __sd0_swstatus);
		return;
	}
	ret = __sd0_switchfunc(SDHS_CMD6_SWITCH);
	if (ret < 0) {
		HS_STEP(SDHS_STEP_SWITCH_RC, ret, __sd0_swstatus);
		return;
	}
	if (!sdhs_group1_selected(__sd0_swstatus)) {
		HS_STEP(SDHS_STEP_SWITCH_NOT_SELECTED, ret, __sd0_swstatus);
		return;
	}

	/* Host side: high speed enable, then the clock up */
	ret = __sdio_gethcr(SDIOHCR_HOSTCONTROL, 1, &hc);
	if (ret < 0) {
		HS_STEP(SDHS_STEP_HOSTCTL_READ, ret, __sd0_swstatus);
		return;
	}
	ret = __sdio_sethcr(SDIOHCR_HOSTCONTROL, 1, sdhs_hostctl_enable(hc));
	if (ret < 0) {
		HS_STEP(SDHS_STEP_HOSTCTL_WRITE, ret, __sd0_swstatus);
		return;
	}

	__sd0_hs_on = 1;
	ret = __sdio_setclock(0);
	if (ret < 0) {
		HS_STEP(SDHS_STEP_SETCLK, ret, __sd0_swstatus);
		__sd0_hs_fallback();
		return;
	}
	HS_STEP(SDHS_STEP_ON, 0, __sd0_swstatus);
}

#ifdef SDIO_HS_DEBUG
/* The record of the last __sd0_hs_try() */
void sdio_HighSpeedRecord(struct sdhs_record *out)
{
	*out = __sd0_hs_rec;
}
#endif
#endif

static	bool __sd0_initio(void)
{
	struct _sdioresponse resp;

	s32 ret;
	s32 tries;
	u32 status;

#ifdef SDIO_HIGH_SPEED
	/* The reset below clears the host's high speed bit */
	__sd0_hs_on = 0;
#endif

	/* Reset card */
	__sdio_resetcard();

	/* Get card status */
	status = __sdio_getstatus();

	/* Card not inserted */
	if (!(status & SDIO_STATUS_CARD_INSERTED))
		return false;

	/* Card not initialized */
	if (!(status & SDIO_STATUS_CARD_INITIALIZED)) {
		/* Close device */
		os_close(__sd0_fd);

		/* Open device */
		__sd0_fd = os_open(_sd0_fs, 1);
		if (__sd0_fd < 0)
			return false; 

		/* Reset the host controller */
		if (__sdio_sethcr(SDIOHCR_SOFTWARERESET, 1, 7) < 0)
			goto fail;
		if (__sdio_waithcr(SDIOHCR_SOFTWARERESET, 1, 1, 7) < 0)
			goto fail;

		/* Initialize interrupts */
		__sdio_sethcr(0x34, 4, 0x13f00c3);
		__sdio_sethcr(0x38, 4, 0x13f00c3);

		/* Set SDHC flag */
		__sd0_sdhc = 1;

		/* Enable power */
		ret = __sdio_sethcr(SDIOHCR_POWERCONTROL, 1, 0xe);
		if (ret < 0)
			goto fail;

		ret = __sdio_sethcr(SDIOHCR_POWERCONTROL, 1, 0xf);
		if (ret < 0)
			goto fail;

		/* Enable internal clock, wait until it gets stable and enable sd clock */
		ret = __sdio_sethcr(SDIOHCR_CLOCKCONTROL, 2, 0);
		if (ret < 0)
			goto fail;

		ret = __sdio_sethcr(SDIOHCR_CLOCKCONTROL, 2, 0x101);
		if (ret < 0)
			goto fail;

		ret = __sdio_waithcr(SDIOHCR_CLOCKCONTROL, 2, 0, 2);
		if (ret < 0)
			goto fail;

		ret = __sdio_sethcr(SDIOHCR_CLOCKCONTROL, 2, 0x107);
		if (ret < 0)
			goto fail;

		/* Setup timeout */
		ret = __sdio_sethcr(SDIOHCR_TIMEOUTCONTROL, 1, SDIO_DEFAULT_TIMEOUT);
		if (ret < 0)
			goto fail;

		/* Standard SDHC initialization process */
		ret = __sdio_sendcommand(SDIO_CMD_GOIDLE, 0, 0, 0, 0, 0, NULL, NULL, 0);
		if (ret < 0)
			goto fail;

		ret = __sdio_sendcommand(SDIO_CMD_SENDIFCOND, 0, SDIO_RESPONSE_R6, 0x1aa, 0, 0, NULL, &resp, sizeof(resp));
		if (ret < 0)
			goto fail;

		if ((resp.rsp_fields[0] & 0xff) != 0xaa)
			goto fail;

		/* Do tries */
		for (tries = 10; tries; tries--) {
			ret = __sdio_sendcommand(SDIO_CMD_APPCMD, SDIOCMD_TYPE_AC, SDIO_RESPONSE_R1, 0, 0, 0, NULL, NULL, 0);
			if (ret < 0)
				goto fail;

			ret = __sdio_sendcommand(SDIO_ACMD_SENDOPCOND, 0, SDIO_RESPONSE_R3, 0x40300000, 0, 0, NULL, &resp, sizeof(resp));
			if (ret < 0)
				goto fail;

			if (resp.rsp_fields[0] & (1 << 31))
				break;

			usleep(10000);
		}

		if (tries <= 0)
			goto fail;

		if (resp.rsp_fields[0] & (1 << 30))
			__sd0_sdhc = 1;
		else
			__sd0_sdhc = 0;

		/* Get CID */
		ret = __sd0_getcid();
		if (ret < 0)
			goto fail;

		/* Get RCA */
		ret = __sd0_getrca();
		if (ret < 0)
			goto fail;
	}
	else if (status & SDIO_STATUS_CARD_SDHC)
		__sd0_sdhc = 1;
	else
		__sd0_sdhc = 0;
 
	/* Set bus bandwidth */
	ret = __sdio_setbuswidth(4);
	if (ret < 0)
		return false;
 
	/* Set clock */
	ret = __sdio_setclock(1);
	if (ret < 0)
		return false;
 
	/* Select card */
	ret = __sd0_select();
	if (ret < 0)
		return false;
 
	/* Set block length */
	ret = __sd0_setblocklength(PAGE_SIZE512);
	if (ret < 0) {
		ret = __sd0_deselect();
		return false;
	}

	/* Set bus bandwidth */
	ret = __sd0_setbuswidth(4);
	if (ret < 0) {
		ret = __sd0_deselect();
		return false;
	}

#ifdef SDIO_HIGH_SPEED
	__sd0_hs_try();
#endif

	/* Deselect card */
	__sd0_deselect();

	/* Set initialized */
	__sd0_initialized = 1;

	return true;

fail:
	/* Software reset */
	__sdio_sethcr(SDIOHCR_SOFTWARERESET, 1, 7);
	__sdio_waithcr(SDIOHCR_SOFTWARERESET, 1, 1, 7);

	/* Close device */
	os_close(__sd0_fd);

	/* Open device */
	__sd0_fd = os_open(_sd0_fs, 1);

	return false;
}

bool sdio_Deinitialize(void)
{
	/* Close device */
	if (__sd0_fd >= 0)
		os_close(__sd0_fd);

	/* Reset variables */
	__sd0_fd = -1;
	__sdio_initialized = 0;

	return true;
}

bool sdio_Startup(void)
{
	/* Already initialized */
	if (__sdio_initialized)
		return true;

	/* Open device */
	__sd0_fd = os_open(_sd0_fs, 1);

	if (__sd0_fd < 0) {
		sdio_Deinitialize();
		return false;
	}
 
	if (!__sd0_initio()) {
		sdio_Deinitialize();
		return false;
	}

	/* Set initialized */
	__sdio_initialized = 1;

	return true;
}

bool sdio_Shutdown(void)
{
	/* Already deinitialized */
	if (!__sd0_initialized)
		return false;

	/* Deinitialize */
	sdio_Deinitialize();
 
	/* Set deinitialized */
	__sd0_initialized = 0;

	return true;
}
 
bool sdio_ReadSectors(sec_t sector, sec_t numSectors,void* buffer)
{
	u32 i;
	s32 ret;

	/* No buffer */
	if (!buffer)
		return false;

	/* Read loop */
	for (i = 0; i < 10; i++) {
		/* Select card */
		ret = __sd0_select();
		if (ret < 0) {
#ifdef SDIO_HIGH_SPEED
			__sd0_hs_fallback();
#endif
			continue;
		}

		/* Check if SDHC */
		if (!__sd0_sdhc)
			sector *= PAGE_SIZE512;

		/* Send command */
		ret = __sdio_sendcommand(SDIO_CMD_READMULTIBLOCK, SDIOCMD_TYPE_AC, SDIO_RESPONSE_R1, sector, numSectors, PAGE_SIZE512, buffer, NULL, 0);

		/* Deselect card */
		__sd0_deselect();

		if (ret >= 0)
			return true;

#ifdef SDIO_HIGH_SPEED
		__sd0_hs_fallback();
#endif
	}

	return false;
}
 
bool sdio_WriteSectors(sec_t sector, sec_t numSectors,const void* buffer)
{
	u32 i;
	s32 ret;

	/* No buffer */
	if (!buffer)
		return false;

	/* Write loop */
	for (i = 0; i < 10; i++) {
		/* Select card */
		ret = __sd0_select();
		if (ret < 0) {
#ifdef SDIO_HIGH_SPEED
			__sd0_hs_fallback();
#endif
			continue;
		}

		/* Check if SDHC */
		if (!__sd0_sdhc)
			sector *= PAGE_SIZE512;

		/* Send command */
		ret = __sdio_sendcommand(SDIO_CMD_WRITEMULTIBLOCK, SDIOCMD_TYPE_AC, SDIO_RESPONSE_R1, sector, numSectors, PAGE_SIZE512, (char *)buffer, NULL, 0);

		/* Deselect card */
		__sd0_deselect();

		if (ret >= 0)
			return true;

#ifdef SDIO_HIGH_SPEED
		__sd0_hs_fallback();
#endif
	}
 
	return false;
}
 
bool sdio_ClearStatus(void)
{
	return true;
}
 
bool sdio_IsInserted(void)
{
	/* Check if card is inserted */
	return ((__sdio_getstatus() & SDIO_STATUS_CARD_INSERTED) ==
			SDIO_STATUS_CARD_INSERTED);
}

bool sdio_IsInitialized(void)
{
	/* Check if card is initialized */
	return ((__sdio_getstatus() & SDIO_STATUS_CARD_INITIALIZED) ==
			SDIO_STATUS_CARD_INITIALIZED);
}

#ifdef SDIO_HIGH_SPEED
/* bit 0: high speed active; bit 1: fell back to default speed this session */
u32 sdio_HighSpeedState(void)
{
	return (__sd0_hs_on ? 1 : 0) | (__sd0_hs_dead ? 2 : 0);
}

#ifdef SDIO_HS_DEBUG
/* Debug: drop to default speed now, as a transfer error would */
void sdio_HighSpeedForceFallback(void)
{
	__sd0_hs_fallback();
}
#endif
#endif
