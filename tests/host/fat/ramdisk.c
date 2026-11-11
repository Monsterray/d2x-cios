/* Two RAM disks in place of the FAT module's diskio.c (SD is 0, USB is 1). */
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"
#include "ramdisk.h"

struct ramdisk ramdisk[2];

static unsigned int chunk_count(int drv)
{
	return (ramdisk[drv].sectors + RAMDISK_CHUNK_SECTORS - 1) / RAMDISK_CHUNK_SECTORS;
}

void ramdisk_reset(unsigned int sectors)
{
	for (int i = 0; i < 2; i++) {
		if (ramdisk[i].chunks) {
			for (unsigned int c = 0; c < chunk_count(i); c++)
				free(ramdisk[i].chunks[c]);
			free(ramdisk[i].chunks);
		}
		memset(&ramdisk[i], 0, sizeof(ramdisk[i]));
		ramdisk[i].sectors = sectors;
		ramdisk[i].chunks = calloc(chunk_count(i), sizeof(unsigned char *));
	}
}

/* The bytes of a sector; allocates its chunk when make is set, else NULL for a hole */
static unsigned char *sector_at(int drv, unsigned int sector, int make)
{
	unsigned char **slot = &ramdisk[drv].chunks[sector / RAMDISK_CHUNK_SECTORS];

	if (!*slot) {
		if (!make)
			return NULL;
		*slot = calloc(RAMDISK_CHUNK_SECTORS, RAMDISK_SECTOR);
	}
	return *slot + (size_t)(sector % RAMDISK_CHUNK_SECTORS) * RAMDISK_SECTOR;
}

void ramdisk_read_raw(int drv, unsigned int sector, void *buf)
{
	unsigned char *p = sector_at(drv, sector, 0);

	if (p)
		memcpy(buf, p, RAMDISK_SECTOR);
	else
		memset(buf, 0, RAMDISK_SECTOR);
}

void ramdisk_write_raw(int drv, unsigned int sector, const void *buf)
{
	memcpy(sector_at(drv, sector, 1), buf, RAMDISK_SECTOR);
}

unsigned int ramdisk_chunks_used(int drv)
{
	unsigned int used = 0;

	for (unsigned int c = 0; c < chunk_count(drv); c++)
		used += ramdisk[drv].chunks[c] != NULL;
	return used;
}

void ramdisk_set_partition(int drv, int part, unsigned int start, unsigned int sectors)
{
	unsigned char mbr[RAMDISK_SECTOR];
	unsigned char *e = mbr + 446 + (part - 1) * 16;

	ramdisk_read_raw(drv, 0, mbr);
	memset(e, 0, 16);
	e[4] = 0x0C;                    /* a type FatFs accepts; f_mkfs sets the real one */
	for (int i = 0; i < 4; i++) {
		e[8 + i] = (unsigned char)(start >> (8 * i));
		e[12 + i] = (unsigned char)(sectors >> (8 * i));
	}
	mbr[510] = 0x55;
	mbr[511] = 0xAA;
	ramdisk_write_raw(drv, 0, mbr);
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

DRESULT disk_read(BYTE drv, BYTE *buf, LBA_t sector, UINT count)
{
	if (drv > 1 || (unsigned long long)sector + count > ramdisk[drv].sectors)
		return RES_PARERR;
	if (ramdisk[drv].fail_reads && ramdisk[drv].read_budget-- <= 0)
		return RES_ERROR;
	for (UINT i = 0; i < count; i++)
		ramdisk_read_raw(drv, sector + i, buf + (size_t)i * RAMDISK_SECTOR);
	ramdisk[drv].reads++;
	return RES_OK;
}

DRESULT disk_write(BYTE drv, const BYTE *buf, LBA_t sector, UINT count)
{
	if (drv > 1 || (unsigned long long)sector + count > ramdisk[drv].sectors)
		return RES_PARERR;
	if (ramdisk[drv].fail_writes)
		return RES_ERROR;
	for (UINT i = 0; i < count; i++)
		ramdisk_write_raw(drv, sector + i, buf + (size_t)i * RAMDISK_SECTOR);
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
		*(LBA_t *) buf = ramdisk[drv].sectors;
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

/* The module's stamp (diskio.c): 1980-01-01 00:00:00 */
DWORD get_fattime(void)
{
	return 0x00210000;
}
