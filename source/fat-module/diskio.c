/*-----------------------------------------------------------------------*/
/* Low level disk I/O module skeleton for FatFs     (C)ChaN, 2007        */
/*-----------------------------------------------------------------------*/
/* This is a stub disk I/O module that acts as front end of the existing */
/* disk I/O modules and attach it to FatFs module with common interface. */
/*-----------------------------------------------------------------------*/

#include <string.h>

#include "ff.h"
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

/* Most sectors handed to a driver in one request. FatFs R0.08b asked for 255 at
   most (its count was a BYTE); R0.16 asks for a whole run inside one cluster, and
   an exFAT cluster can be 32 MB. */
#define DISK_MAX_SECTORS	255


static s32 __disk_io(BYTE drv, LBA_t sector, u32 count, void *buffer, int write)
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
static s32 __disk_bounce(BYTE drv, BYTE *buff, LBA_t sector, u32 count, u32 secsz, int write)
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

/* Read or write count sectors, DISK_MAX_SECTORS at a time */
static DRESULT __disk_transfer(BYTE drv, BYTE *buff, LBA_t sector, UINT count, int write)
{
	u32 secsz = SECTOR_SZ(drv);

	if (!secsz || !count)
		return RES_ERROR;

	while (count) {
		u32 n = (count < DISK_MAX_SECTORS) ? count : DISK_MAX_SECTORS;
		s32 ret;

		/* An aligned buffer is used directly, an unaligned one bounced */
		if ((u32)buff & 31)
			ret = __disk_bounce(drv, buff, sector, n, secsz, write);
		else
			ret = __disk_io(drv, sector, n, buff, write);

		if (!ret)
			return RES_ERROR;

		buff   += n * secsz;
		sector += n;
		count  -= n;
	}

	return RES_OK;
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

DRESULT disk_read(BYTE drv, BYTE *buff, LBA_t sector, UINT count)
{
	return __disk_transfer(drv, buff, sector, count, 0);
}

#if !FF_FS_READONLY
DRESULT disk_write(BYTE drv, const BYTE *buff, LBA_t sector, UINT count)
{
	return __disk_transfer(drv, (BYTE *)buff, sector, count, 1);
}
#endif /* !FF_FS_READONLY */

DRESULT disk_ioctl(BYTE drv, BYTE ctrl, void *buff)
{
	if (ctrl == GET_SECTOR_SIZE) {
		s32 ret = *((WORD *) buff) = (WORD) SECTOR_SZ(drv);

		return (ret) ? RES_OK : RES_ERROR;
	}

	return RES_OK;
}


/* The module has no clock: every time stamp is 1980-01-01 00:00:00 (the DOS
   epoch). A zero stamp is not a valid FAT date, and exFAT readers reject it. */
DWORD get_fattime(void)
{
	return 0x00210000;
}
