/*-----------------------------------------------------------------------*/
/* Low level disk I/O module skeleton for FatFs     (C)ChaN, 2007        */
/*-----------------------------------------------------------------------*/
/* This is a stub disk I/O module that acts as front end of the existing */
/* disk I/O modules and attach it to FatFs module with common interface. */
/*-----------------------------------------------------------------------*/

#include <string.h>

#include "diskio.h"
#include "mem.h"
#include "sdio.h"
#include "syscalls.h"
#include "types.h"
#include "usbstorage.h"

/* Disk drives */
#define DRIVE_SDHC	0
#define DRIVE_EHCI	1

/* Disk constants */
#define SECTOR_SZ(drv)		((drv)==DRIVE_SDHC ? 512 :	\
	((drv)==DRIVE_EHCI ? usbstorage_GetSectorSize() : 0))

/* Largest bounce buffer for an unaligned transfer (the heap is 112 KB) */
#define BOUNCE_MAX		(16 * 1024)


static s32 __disk_io(BYTE drv, DWORD sector, u32 count, void *buffer, int write)
{
	switch (drv) {
	case DRIVE_SDHC:
		return write ? sdio_WriteSectors(sector, count, buffer)
			     : sdio_ReadSectors(sector, count, buffer);

	case DRIVE_EHCI:
		return write ? usbstorage_WriteSectors(sector, count, buffer)
			     : usbstorage_ReadSectors(sector, count, buffer);
	}

	return 0;
}

/* Transfer sectors to or from an unaligned buffer, BOUNCE_MAX at a time */
static s32 __disk_bounce(BYTE drv, BYTE *buff, DWORD sector, u32 count, u32 secsz, int write)
{
	u32 chunk = BOUNCE_MAX / secsz;
	void *buffer;
	s32 ret = 1;

	if (!chunk)
		chunk = 1;
	if (chunk > count)
		chunk = count;

	buffer = Mem_Alloc(chunk * secsz);
	if (!buffer)
		return 0;

	while (count && ret) {
		u32 n = (count < chunk) ? count : chunk;
		u32 len = n * secsz;

		if (write)
			memcpy(buffer, buff, len);
		ret = __disk_io(drv, sector, n, buffer, write);
		if (ret && !write)
			memcpy(buff, buffer, len);

		buff   += len;
		sector += n;
		count  -= n;
	}

	Mem_Free(buffer);

	return ret;
}


DSTATUS disk_initialize(BYTE drv)
{
	/* Initialize drive */
	switch (drv) {
	case DRIVE_SDHC:
	case DRIVE_EHCI:
		break;

	default:
		return RES_PARERR;
	}

	return RES_OK;
}

DSTATUS disk_status(BYTE drv)
{
	s32 ret = 1;

	/* Check drive status */
	switch (drv) {
	case DRIVE_SDHC:
		/* Check SD card status */
		ret = sdio_IsInserted();
		break;

	case DRIVE_EHCI:
		/* Check USB device status */
		ret = usbstorage_IsInserted();
		break;
	}

	return ret ? RES_OK : STA_NODISK;
}

DRESULT disk_read(BYTE drv, BYTE *buff, DWORD sector, BYTE count)
{
	u32 secsz = SECTOR_SZ(drv);
	s32 ret;

	if (!secsz || !count)
		return RES_ERROR;

	/* An aligned buffer is read directly, an unaligned one bounced */
	if ((u32)buff & 31)
		ret = __disk_bounce(drv, buff, sector, count, secsz, 0);
	else
		ret = __disk_io(drv, sector, count, buff, 0);

	return (ret) ? RES_OK : RES_ERROR;
}

#if _READONLY == 0
DRESULT disk_write(BYTE drv, const BYTE *buff, DWORD sector, BYTE count)
{
	u32 secsz = SECTOR_SZ(drv);
	s32 ret;

	if (!secsz || !count)
		return RES_ERROR;

	/* An aligned buffer is written directly, an unaligned one bounced */
	if ((u32)buff & 31)
		ret = __disk_bounce(drv, (BYTE *)buff, sector, count, secsz, 1);
	else
		ret = __disk_io(drv, sector, count, (void *)buff, 1);

	return (ret) ? RES_OK : RES_ERROR;
}
#endif /* _READONLY */

DRESULT disk_ioctl(BYTE drv, BYTE ctrl, void *buff)
{
	if (ctrl == GET_SECTOR_SIZE) {
		s32 ret = *((WORD *) buff) = (WORD) SECTOR_SZ(drv);

		return (ret) ? RES_OK : RES_ERROR;
	}

	return RES_OK;
}


u32 get_fattime(void)
{
	return 0;
}
