/*   
	Custom IOS Module (FAT)

	Copyright (C) 2008 neimod.
	Copyright (C) 2009 WiiGator.
	Copyright (C) 2009 Waninkoko.
	Copyright (C) 2011 davebaol.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>

#include "ff.h"
#include "fs.h"
#include "fat_tools.h"
#include "fat_wrapper.h"
#include "ipc.h"
#include "main.h"
#include "mem.h"
#include "types.h"
#include "sdio.h"
#include "usbstorage.h"

/* Variables */
/* One work area per volume: FatFs keeps a pointer to it for each drive. */
static FATFS fatFs[FF_VOLUMES] ATTRIBUTE_ALIGN(32);

/* The window of a work area is the DMA target of the sector reads (see ff.h) */
typedef char __fatfs_win_is_aligned[(offsetof(FATFS, win) % 32) ? -1 : 1];
/* ... and so is fatFs[1].win, so the work area's size is a multiple of 32.
   (Checked for the module only: the host tests have 8-byte pointers.) */
#ifdef __arm__
typedef char __fatfs_size_is_aligned[(sizeof(FATFS) % 32) ? -1 : 1];
#endif

/* Which partition each volume mounts: 0 is none, 1 to 4 the partition (FatFs
   counts from 1; the IPC calls and GETPARTITION count from 0). */
#define PART_NONE	0

PARTITION VolToPart[FF_VOLUMES]  ATTRIBUTE_ALIGN(32) = {
	{0,PART_NONE}, {1,PART_NONE}
};

/* The directory entry the walks below read into. One for the module: it handles
   one request at a time, and a recursive walk has built its path from an entry's
   name before it goes into the next directory. (A FILINFO is over 300 bytes: on
   the stack of each level it would not fit the thread's 12 KB.) */
static FILINFO fno ATTRIBUTE_ALIGN(32);

/* Drive prefixes of the paths */
static const char *driveRoot[FF_VOLUMES] = { "0:/", "1:/" };

/* The sizes and positions the IPC calls return are 32 bits (struct stats, struct
   fstats): an exFAT file of 4 GB or more reports 0xFFFFFFFF, the most they hold.
   FAT_GetUsage adds up the real sizes in 64 bits. */
static u32 FAT_Size32(FSIZE_t size)
{
	return ((u64)size > 0xFFFFFFFFULL) ? 0xFFFFFFFF : (u32)size;
}


s32 __FAT_OpenDir(DIR *dir, const char *dirpath)
{
	s32 ret;

	/* Open directory */
	ret = f_opendir(dir, dirpath);

	/* Check result */
	switch (ret) {
	case FR_OK:
		return FS_SUCCESS;

	case FR_INVALID_NAME:

	// FIX:
	// Case uncommented in d2x v3beta6 for error code compatibility
	// to fix the message "corrupted data" in The Tower of Druaga.
	case FR_NO_PATH:
		return FS_ENOENT;
	}

	return FS_EFATAL;
}

/* Read the next entry that is not . or .. (or hidden by a leading dot) into fno.
   Returns FS_SUCCESS with an entry, FS_ENOENT at the end of the directory and
   FS_EFATAL when the read failed (the callers must not take that for the end). */
s32 __FAT_ReadDirEntry(DIR *dir)
{
	s32 ret;

	/* Loop */
	while (1) {
		/* Read entry */
		ret = f_readdir(dir, &fno);
		if (ret != FR_OK)
			return FS_EFATAL;

		/* Read end */
		if (fno.fname[0] == '\0')
			return FS_ENOENT;

		/* Check entry */
		if (fno.fname[0] != '.')
			break;
	}

	return FS_SUCCESS;
}


s32 __FAT_MountPartition(u8 device, u8 partition)
{
	s32 ret;

	/* Set partition */
	VolToPart[device].pt = partition + 1;

	/* Mount device (FatFs mounts it on the first access, the root directory below) */
	ret = f_mount(&fatFs[device], driveRoot[device], 0);
	if (!ret) {
		DIR d;

		/* Open root directory */
		if (f_opendir(&d, driveRoot[device]) == FR_OK)
			return FS_SUCCESS;

		/* Unmount device */
		f_mount(NULL, driveRoot[device], 0);
	}

	/* Reset partition */
	VolToPart[device].pt = PART_NONE;

	return FS_EFATAL;
}


s32 FAT_Mount(u8 device, s32 partition)
{
	s32 ret, i;

	/* Initialize device */
	switch (device) {
	case 0:
		/* Initialize SDIO */
		ret = sdio_Startup();
		break;

	case 1:
		/* Initialize EHCI */
		ret = usbstorage_Init();
		break;

	default:
		/* Unknown device */
		ret = 0;
		break;
	}

	/* Device not found */
	if (!ret)
		return IPC_EINVAL;

	/* Mount required partition */
	if (partition >= 0 && partition < 4)
		return __FAT_MountPartition(device, partition);

	/* Find partition */
	for (i = 0; i < 4; i++) {
		ret = __FAT_MountPartition(device, i);
		if (!ret)
			return FS_SUCCESS;
	}

	return FS_EFATAL;
}

s32 FAT_Unmount(u8 device)
{
	s32 ret;

	if (device >= FF_VOLUMES)
		return FS_EFATAL;

	/* Unmount device */
	ret = f_mount(NULL, driveRoot[device], 0);
	if (ret)
		return FS_EFATAL;

	/* Reset partition */
	VolToPart[device].pt = PART_NONE;

	return FS_SUCCESS;
}

s32 FAT_GetPartition(u8 device, u32 *partition)
{
	if (device >= FF_VOLUMES)
		return IPC_EINVAL;

	if (VolToPart[device].pt == PART_NONE)
		return -1;

	*partition = VolToPart[device].pt - 1;
   
	return 0;
}

s32 FAT_Open(const char *path, u32 mode)
{
	FIL *fil = NULL;
	s32  ret;

	/* Allocate entry */
	fil = Mem_Alloc(sizeof(FIL));
	if (!fil)
		return IPC_ENOMEM;

	/* Open file */
	ret = f_open(fil, path, mode);

	/* This fix is required by No More Heroes 2 to create the save */
	if (ret == FR_NO_FILE && (mode & FA_WRITE)) {
		// Most games don't seem to mind this fix
		// but it breaks Monster Hunter Tri so we need a hack.
		u32 isNMH2 = ((*(vu32*)0 >> 8) == 0x525559);
		if (isNMH2)
			ret = f_open(fil, path, mode | FA_CREATE_ALWAYS); 
	}

	/* Error */
	if (ret) {
		dbg_printf("FAT_Open: fat error %d\n", ret);

		/* Free entry */
		Mem_Free(fil);

		/* File sharing control disabled. See issue 11. */
		if (FF_FS_LOCK == 0)
			return FS_ENOENT;

		/* File sharing control enabled */
		return ret == FR_TOO_MANY_OPEN_FILES ? FS_ENFILE : FS_ENOENT;
	}

	return (u32)fil;
}

s32 FAT_Close(s32 fd)
{
	FIL *fil = (FIL *)fd;
	s32  ret;

	/* Close file */
	ret = f_close(fil);

	/* A close that failed for a reason a next try could get past (anything but
	   a disk or mount error: the size and cluster update is still pending)
	   keeps the file object, so a later close can write it again. Whether the
	   caller's descriptor survives an error reply is IOS's rule; if it does
	   not, the object stays allocated: one FIL leaked per such failure. */
	if (ret != FR_OK && ret != FR_DISK_ERR && ret != FR_NOT_READY &&
	    ret != FR_INVALID_OBJECT && ret != FR_INVALID_DRIVE &&
	    ret != FR_NOT_ENABLED && ret != FR_NO_FILESYSTEM)
		return FS_EFATAL;

	/* Free memory */
	Mem_Free(fil);

	return ret ? FS_EFATAL : FS_SUCCESS;
}

s32 FAT_Read(s32 fd, void *buffer, u32 len)
{
	FIL *fil = (FIL *)fd;

	UINT read;
	s32 ret;

	/* Read file */
	ret = f_read(fil, buffer, len, &read);

	/* Check result */
	switch (ret) {
	case FR_OK:
		return read;

	case FR_DENIED:
		return FS_EACCESS;
	}

	return FS_EFATAL;
}

s32 FAT_Write(s32 fd, void *buffer, u32 len)
{
	FIL *fil = (FIL *)fd;

	UINT wrote;
	s32 ret;

	/* Write file */
	ret = f_write(fil, buffer, len, &wrote);

	/* Check result */
	switch (ret) {
	case FR_OK:
		return wrote;

	case FR_DENIED:
		return FS_EACCESS;
	}

	return FS_EFATAL;
}

s32 FAT_Seek(s32 fd, s32 where, s32 whence)
{
	FIL *fil = (FIL *)fd;
	long long offset;

	/* Calculate offset (in 64 bits: a file can be over 4 GB on exFAT) */
	switch (whence) {
	case SEEK_SET:
		offset = where;
		break;

	case SEEK_CUR:
		offset = (long long)f_tell(fil) + where;
		break;

	case SEEK_END:
		offset = (long long)f_size(fil) + where;
		break;

	default:
		// FIX:
 		// Modified in d2x v4beta2 to improve
 		// error code compatibility.
		return FS_EFATAL; //FS_EINVAL;
	}

	// FIX:
 	// Check added in d2x v4beta2 to prevent from increasing
 	// the file size when seeking out of the file.
	// The position is the return value, a signed 32 bits: a position of 2 GB
	// or more cannot be told from an error, so a seek there fails (with the
	// file position unchanged) instead of returning a negative number.
	if (offset >= 0 && offset <= (long long)f_size(fil) && offset <= 0x7FFFFFFF) {
		s32 ret;

		/* Seek file */
		ret = f_lseek(fil, (FSIZE_t)offset);

		/* No error */
		if (!ret)
			return (s32)offset;
	}

	return FS_EFATAL;
}

s32 FAT_CreateDir(const char *dirpath)
{
	s32 ret;

	/* Create directory */
	ret = f_mkdir(dirpath);

	/* Check result */
	switch (ret) {
	case FR_OK:
		return FS_SUCCESS;

	case FR_DENIED:
		return FS_EACCESS;

	case FR_EXIST:
		return FS_EEXIST;

	case FR_NO_FILE:
	case FR_NO_PATH:
		return FS_ENOENT;
	}

	return FS_EFATAL;
}

s32 FAT_CreateFile(const char *filepath)
{
	FIL fil;
	s32 ret;

	/* Create file */
	ret = f_open(&fil, filepath, FA_CREATE_NEW);

	/* Check result */
	switch (ret) {
	case FR_OK:
		f_close(&fil);
		return FS_SUCCESS;

	case FR_EXIST:
		return FS_EEXIST;

	case FR_NO_FILE:
	case FR_NO_PATH:
		return FS_ENOENT;
	}

	return FS_EFATAL;
}

s32 FAT_ReadDir(const char *dirpath, char *outbuf, u32 buflen, u32 *outlen, u32 entries, u8 forFS)
{
	DIR     dir;

	char *buffer = NULL;
	u32   cnt = 0, pos = 0;
	s32   ret, err = FS_SUCCESS;
	FIL   fil;
	
	// FIX:
	// Check added in d2x v4beta2 to improve error code compatibility.
	// Now FS_EFATAL is returned when the requested folder is an existing file.
	// With this fix some games like all Strong Bad episodes are working now.
	ret = f_open(&fil, dirpath, FA_OPEN_EXISTING);
	if (ret == FR_OK) {
		f_close(&fil);
		return FS_EFATAL;
	}

	/* Open directory */
	ret = __FAT_OpenDir(&dir, dirpath);
	if (ret)
		return ret;

	/* Allocate memory */
	if (buflen) {
		buffer = Mem_Alloc(buflen);
		if (!buffer)
			return IPC_ENOMEM;
	}

	/* Read directory */
	while (!entries || (entries > cnt)) {
		u32 len;
		char *name;

		/* Read entry */
		ret = __FAT_ReadDirEntry(&dir);
		if (ret) {
			if (ret != FS_ENOENT)
				err = ret;
			break;
		}

		/* Get name (the long name when there is one) */
		name = fno.fname;

		/* Request coming from FS */
		if (forFS) {
			/* Unescape invalid FAT characters in place */
			len = FAT_Unescape(name);

			/* Skip entries too long for FS */
			if (len > 12)
				continue;
		}
		else {
			/* Get length */
			len = strnlen(name, FF_MAX_LFN);
		}

		/* Copy entry */
		if (buffer) {
			char *ptr = buffer + pos;

			/* The caller sized the buffer: stop when the next name does not fit */
			if (pos + len + 1 > buflen)
				break;

			/* Copy filename */
			memcpy(ptr, name, len);
			ptr[len] = 0;

			/* Update position */
			pos += len + 1;
		}

		/* Increase counter */
		cnt++;
	}

	if (buffer) {
		/* Copy the names, not the heap bytes after them */
		if (outbuf)
			memcpy(outbuf, buffer, pos);

		/* Free buffer */
		Mem_Free(buffer);
	}

	/* A failed directory read is an error, not a short list */
	if (err)
		return err;

	/* Set value */
	*outlen = cnt;

	return FS_SUCCESS;
}

s32 FAT_Delete(const char *path)
{
	s32 ret;

	/* Delete object */
	ret = f_unlink(path);

	/* Check result */
	switch (ret) {
	case FR_OK:
		return FS_SUCCESS;

	case FR_DENIED:
		return FS_EACCESS;

	case FR_EXIST:
		return FS_EEXIST;

	case FR_NO_FILE:
	case FR_NO_PATH:
		return FS_ENOENT;
	}

	return FS_EFATAL;
}

/*
 * DELETEDIR and GETUSAGE recurse with about 350 bytes of stack per level on
 * a 12 KB thread stack, and a tree of short names can be over 100 levels
 * deep within the path limit. IOS's NAND allows 8 levels; an emulated NAND
 * folder adds a few: past FAT_MAX_DEPTH the walk refuses with FS_EINVAL.
 */
#define FAT_MAX_DEPTH	16

static s32 __FAT_DeleteDir(const char *dirpath, u32 depth)
{
	DIR     dir;

	s32 ret;

	if (depth > FAT_MAX_DEPTH)
		return FS_EINVAL;

	/* Open directory */
	ret = __FAT_OpenDir(&dir, dirpath);
	if (ret)
		return ret;

	/* Read directory */
	for (;;) {
		char  path[FF_MAX_LFN + 1];
		char *name;

		/* Read entry */
		ret = __FAT_ReadDirEntry(&dir);
		if (ret == FS_ENOENT)
			break;
		if (ret)
			return ret;

		/* Get name */
		name = fno.fname;

		/* The entry's path must fit: a tree can be deeper than _MAX_LFN */
		if (strlen(dirpath) + 1 + strlen(name) >= sizeof(path))
			return FS_ENAMETOOLONG;

		/* Generate path */
		strcpy(path, dirpath);
		strcat(path, "/");
		strcat(path, name);

		/* Entry is directory: empty it first, and stop if that fails */
		if (fno.fattrib & AM_DIR) {
			ret = __FAT_DeleteDir(path, depth + 1);
			if (ret)
				return ret;
		}

		/* Delete */
		ret = FAT_Delete(path);
		if (ret)
			return ret;
	}

	return FS_SUCCESS;
}

s32 FAT_DeleteDir(const char *dirpath)
{
	return __FAT_DeleteDir(dirpath, 0);
}

s32 FAT_Rename(const char *oldname, const char *newname)
{
	char *ptr;
	s32   ret;

	/* Skip prefix */
	ptr = strchr(newname, ':');
	if (ptr)
		newname = ptr + 1;

	/* Rename object */
	ret = f_rename(oldname, newname);

	/* Check result */
	switch (ret) {
	case FR_OK:
		return FS_SUCCESS;

	case FR_EXIST:
		return FS_EEXIST;

	case FR_NO_PATH:
	case FR_NO_FILE:
		return FS_ENOENT;
	}

	return FS_EFATAL;
} 

s32 FAT_GetStats(const char *path, struct stats *stats)
{
	s32     ret;

	/* Get stats */
	ret = f_stat(path, &fno);
	
	// FIX:
	// Error code compatibility improved in d2x v3beta6.
	// This way LIT doesn't stall anymore
	switch(ret) {
		case FR_OK:
			/* Fill info */
			if (stats) {
				stats->size   = FAT_Size32(fno.fsize);
				stats->date   = fno.fdate;
				stats->time   = fno.ftime;
				stats->attrib = fno.fattrib;
			}
			return FS_SUCCESS;
			
		case FR_NO_FILE:
		case FR_NO_PATH:
		case FR_INVALID_NAME:
			return FS_ENOENT;
	}

	return FS_EFATAL;
}

s32 FAT_GetFileStats(s32 fd, struct fstats *stats)
{
	FIL *fil = (FIL *)fd;

	/* No file */
	if (!fil->obj.fs)
		return FS_EFATAL;

	/* Fill stats */
	if (stats) {
		stats->length = FAT_Size32(f_size(fil));
		stats->pos    = FAT_Size32(f_tell(fil));
	}

	return FS_SUCCESS;
}

static s32 __FAT_GetUsage(const char *dirpath, u64 *size, u32 *files, u32 *dirs, u8 forFS, u32 depth)
{
	DIR     dir;
	s32 ret;

	/* See FAT_MAX_DEPTH */
	if (depth > FAT_MAX_DEPTH)
		return FS_EINVAL;

	/* Open directory */
	ret = __FAT_OpenDir(&dir, dirpath);
	if (ret)
		return ret;

	/* Read directory */
	for (;;) {
		char *name;
		u32 len;

		/* Read entry */
		ret = __FAT_ReadDirEntry(&dir);
		if (ret == FS_ENOENT)
			break;
		if (ret)
			return ret;

		/* Get name */
		name = fno.fname;

		/* Request coming from FS */
		if (forFS) {

			/* Unescape invalid FAT characters in place */
			len = FAT_Unescape(name);

			/* Skip entries too long for FS */
			if (len > 12)
				continue;
		}

		/* Entry is subdirectory */
		if (fno.fattrib & AM_DIR) {
			char subdirpath[FF_MAX_LFN + 1];

			/* The subdirectory's path must fit: a tree can be deeper than _MAX_LFN */
			len = strlen(dirpath);
			if (len + 1 + strlen(name) >= sizeof(subdirpath))
				return FS_ENAMETOOLONG;

			/* Generate path */
			strcpy(subdirpath, dirpath);
			subdirpath[len] = '/';
			strcpy(subdirpath + len + 1, name);

			/* Update directory counter */
			*dirs = *dirs + 1;

			/* Get subdirectory usage */
			ret = __FAT_GetUsage(subdirpath, size, files, dirs, forFS, depth + 1);
			if (ret)
				return ret;

		} else {
			/* Update file counters */
			*size  = *size + fno.fsize;
			*files = *files + 1;
		}
	}

	return FS_SUCCESS;
}

s32 FAT_GetUsage(const char *dirpath, u64 *size, u32 *files, u32 *dirs, u8 forFS)
{
	return __FAT_GetUsage(dirpath, size, files, dirs, forFS, 0);
}

