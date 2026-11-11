/*
 * The FAT module's FatFs (ff.c, ffunicode.c) for the host, with f_mkfs and
 * f_expand turned on so the tests can format their RAM disks with the same
 * code and make a file of several GB without writing it.
 */
#include "ffconf.h"
#undef FF_USE_MKFS
#define FF_USE_MKFS 1
#undef FF_USE_EXPAND
#define FF_USE_EXPAND 1
#include "ff.c"
#include "ffunicode.c"
#include "ramdisk.h"

_Static_assert(RD_FAT == FM_FAT && RD_FAT32 == FM_FAT32 && RD_EXFAT == FM_EXFAT && RD_SFD == FM_SFD,
	       "ramdisk.h and ff.h disagree on the formats");

/* Format a RAM disk (see ramdisk.h). VolToPart is the module's (fat_wrapper.c). */
int ramdisk_format_ex(int drive, unsigned int fmt, int part)
{
	static FATFS fs;
	static BYTE work[FF_MAX_SS];
	MKFS_PARM opt = { (BYTE) fmt, 0, 0, 0, 0 };
	char path[3] = { (char)('0' + drive), ':', 0 };
	BYTE saved = VolToPart[drive].pt;
	FRESULT r;

	VolToPart[drive].pt = (BYTE) part;
	r = f_mount(&fs, path, 0);
	if (r == FR_OK)
		r = f_mkfs(path, &opt, work, sizeof(work));
	f_mount(NULL, path, 0);
	VolToPart[drive].pt = saved;
	return r;
}

/* FAT12/16/32 by the volume's size, in a new partition table as before */
int ramdisk_format(int drive)
{
	return ramdisk_format_ex(drive, RD_FAT | RD_FAT32, 0);
}

/* Make an open file (a descriptor of the module: its FIL) bytes long without
   writing it: the clusters are allocated, their data is whatever is on the disk. */
int fatw_expand(unsigned int fd, unsigned long long bytes)
{
	return f_expand((FIL *)(uintptr_t)fd, (FSIZE_t)bytes, 1);
}

/* f_lseek on the file, past the wrapper's FAT_Seek limits */
int fatw_raw_seek(unsigned int fd, unsigned long long pos)
{
	return f_lseek((FIL *)(uintptr_t)fd, (FSIZE_t)pos);
}

unsigned long long fatw_raw_size(unsigned int fd)
{
	return f_size((FIL *)(uintptr_t)fd);
}

/* Free space of a drive in bytes by f_getfree (the cached count when valid, a scan if not);
   ~0 on an error. forget != 0 first drops the cached count, so it is the bitmap's answer. */
unsigned long long fatw_free_bytes(int drive, int forget)
{
	char path[3] = { (char)('0' + drive), ':', 0 };
	FATFS *fsp;
	DWORD n;

	if (f_getfree(path, &n, &fsp) != FR_OK)
		return ~0ULL;
	if (forget) {
		fsp->free_clst = 0xFFFFFFFF;
		if (f_getfree(path, &n, &fsp) != FR_OK)
			return ~0ULL;
	}
	return (unsigned long long)n * fsp->csize * SS(fsp);
}
