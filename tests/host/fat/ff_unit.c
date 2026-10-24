/*
 * The FAT module's FatFs (ff.c, ccsbcs.c) for the host, with f_mkfs turned
 * on so the tests can format their RAM disks with the same code.
 */
#include "fat_types.h"
#include "ffconf.h"
#undef _USE_MKFS
#define _USE_MKFS 1
#include "ff.c"
#include "ccsbcs.c"

/* Format a RAM disk: one FAT partition (FDISK rule), default cluster size. */
int ramdisk_format(int drive)
{
	static FATFS fs;
	FRESULT r = f_mount((BYTE) drive, &fs);
	if (r == FR_OK)
		r = f_mkfs((BYTE) drive, 0, 0);
	f_mount((BYTE) drive, NULL);
	return r;
}
