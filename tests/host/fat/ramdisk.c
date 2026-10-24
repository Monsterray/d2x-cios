/* Two RAM disks in place of the FAT module's diskio.c (SD is 0, USB is 1). */
#include <stdlib.h>
#include <string.h>
#include "fat_types.h"
#include "diskio.h"
#include "ramdisk.h"

struct ramdisk ramdisk[2];

void ramdisk_reset(unsigned int sectors)
{
	for (int i = 0; i < 2; i++) {
		free(ramdisk[i].data);
		memset(&ramdisk[i], 0, sizeof(ramdisk[i]));
		ramdisk[i].sectors = sectors;
		ramdisk[i].data = calloc(sectors, RAMDISK_SECTOR);
	}
}

DSTATUS disk_initialize(BYTE drv)
{
	if (drv > 1)
		return STA_NOINIT;
	ramdisk[drv].inited = 1;
	return 0;
}

DSTATUS disk_status(BYTE drv)
{
	return (drv < 2 && ramdisk[drv].inited) ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE drv, BYTE *buf, DWORD sector, BYTE count)
{
	if (drv > 1 || sector + count > ramdisk[drv].sectors)
		return RES_PARERR;
	memcpy(buf, ramdisk[drv].data + (size_t) sector * RAMDISK_SECTOR, (size_t) count * RAMDISK_SECTOR);
	ramdisk[drv].reads++;
	return RES_OK;
}

DRESULT disk_write(BYTE drv, const BYTE *buf, DWORD sector, BYTE count)
{
	if (drv > 1 || sector + count > ramdisk[drv].sectors)
		return RES_PARERR;
	if (ramdisk[drv].fail_writes)
		return RES_ERROR;
	memcpy(ramdisk[drv].data + (size_t) sector * RAMDISK_SECTOR, buf, (size_t) count * RAMDISK_SECTOR);
	ramdisk[drv].writes++;
	return RES_OK;
}

DRESULT disk_ioctl(BYTE drv, BYTE cmd, void *buf)
{
	if (drv > 1)
		return RES_PARERR;
	switch (cmd) {
	case CTRL_SYNC:
		return RES_OK;
	case GET_SECTOR_COUNT:
		*(DWORD *) buf = ramdisk[drv].sectors;
		return RES_OK;
	case GET_SECTOR_SIZE:
		*(WORD *) buf = RAMDISK_SECTOR;
		return RES_OK;
	case GET_BLOCK_SIZE:
		*(DWORD *) buf = 1;
		return RES_OK;
	}
	return RES_PARERR;
}

DWORD get_fattime(void)
{
	return (31u << 25) | (1u << 21) | (1u << 16);     /* 2011-01-01 */
}

void *ff_memalloc(UINT size) { return malloc(size); }
void ff_memfree(void *ptr) { free(ptr); }
