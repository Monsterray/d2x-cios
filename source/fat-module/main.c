/*   
	Custom IOS Module (FAT)

	Copyright (C) 2008 neimod.
	Copyright (C) 2009 WiiGator.
	Copyright (C) 2010 Waninkoko.
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

#include <string.h>

#include "fat_tools.h"
#include "fat_wrapper.h"
#include "fs.h"
#include "ipc.h"
#include "led.h"
#include "main.h"
#include "mem.h"
#include "module.h"
#include "sdio.h"
#include "syscalls.h"
#include "timer.h"
#include "types.h"
#include "usbstorage.h"


char *moduleName = "FAT";

static char emuNandPath[192] = "/";
static u32  emuNandPathLen   = 1;

/* A file descriptor from IOS_OPEN of a file is its FIL pointer, 32-byte
   aligned; the module itself is fd 0 (see IOS_OPEN and IOS_CLOSE). */
static inline int __FAT_IsFileFd(s32 fd)
{
	return fd != 0 && (fd % 32) == 0;
}

/* A path argument must end with a NUL inside its vector. */
static int __FAT_IsPath(const ioctlv *v)
{
	return v->data && v->len && strnlen((const char *)v->data, v->len) < v->len;
}

/* The vectors an IOCTLV command reads must have been sent, its paths must
   be terminated, and its outputs must be big enough for what is written. */
static int __FAT_CheckVectors(u32 cmd, ioctlv *vector, u32 inlen, u32 iolen)
{
	u32 count = inlen + iolen;

	switch (cmd) {
	case IOCTL_FAT_MKDIR:
	case IOCTL_FAT_MKFILE:
	case IOCTL_FAT_DELETE:
	case IOCTL_FAT_DELETEDIR:
		return count >= 1 && __FAT_IsPath(&vector[0]);

	case IOCTL_FAT_READDIR_FS:
	case IOCTL_FAT_READDIR:
		if (iolen > 1)		/* path, entries, names, count */
			return count >= 4 && __FAT_IsPath(&vector[0]) &&
			       vector[1].len >= sizeof(u32) && vector[3].len >= sizeof(u32);
		return count >= 2 && __FAT_IsPath(&vector[0]) && vector[1].len >= sizeof(u32);

	case IOCTL_FAT_RENAME:
		return count >= 2 && __FAT_IsPath(&vector[0]) && __FAT_IsPath(&vector[1]);

	case IOCTL_FAT_STATS:
		return count >= 2 && __FAT_IsPath(&vector[0]) && vector[1].len >= sizeof(struct stats);

	case IOCTL_FAT_GETUSAGE_FS:
	case IOCTL_FAT_GETUSAGE:	/* path, size, then files and dirs if sent */
		return count >= 2 && __FAT_IsPath(&vector[0]) && vector[1].len >= sizeof(u64) &&
		       (iolen < 2 || (count >= 3 && vector[2].len >= sizeof(u32))) &&
		       (iolen < 3 || (count >= 4 && vector[3].len >= sizeof(u32)));

	case IOCTL_FAT_MOUNT_SD:
	case IOCTL_FAT_MOUNT_USB:
		return inlen == 0 || vector[0].len >= sizeof(s32);

	case IOCTL_FAT_GETPARTITION:
		return count >= 2 && vector[0].len >= sizeof(u32) && vector[1].len >= sizeof(u32);
	}
	return 1;
}

s32 __FAT_Ioctl(s32 fd, u32 cmd, void *inbuf, u32 inlen, void *iobuf, u32 iolen)
{
	s32 ret = IPC_EINVAL;

	/* Invalidate cache */
	if (inbuf)
		os_sync_before_read(inbuf, inlen);

	/* Parse command */
	switch (cmd) {
	/** Get file stats **/
	case IOCTL_FAT_FILESTATS: {
		void *stats = iobuf;
		dbg_printf("FAT: IOCTL_FAT_FILESTATS: fd = %d\n", fd);

		if (!__FAT_IsFileFd(fd) || !stats || iolen < sizeof(struct fstats))
			break;

		/* Get file stats */
		ret = FAT_GetFileStats(fd, stats);
		dbg_printf("FAT: IOCTL_FAT_FILESTATS: ret = %d, length = %d, pos = %d\n", ret, ((struct fstats *)stats)->length, ((struct fstats *)stats)->pos);

		break;
	}

	case IOCTL_FAT_SETNANDPATH: {
		char *path = inbuf;
		dbg_printf("FAT: IOCTL_FAT_SETNANDPATH: path = %d\n", path);
		ret = -4;
		/* The path must end with a NUL inside the caller's buffer */
		if (path != NULL && inlen > 0 && strnlen(path, inlen) < inlen) {
			u32 len = strnlen(path, inlen);
			/* Leave room for a trailing '/' and the NUL */
			if (len < sizeof(emuNandPath) - 1) {
				strcpy(emuNandPath, path);
				emuNandPathLen = len;
				if (len == 0 || emuNandPath[len-1] != '/') {
					emuNandPath[len]   = '/';
					emuNandPath[len+1] = '\0';
					emuNandPathLen++;
				}
				ret = 0;
			}
		}
		dbg_printf("FAT: IOCTL_FAT_SETNANDPATH: ret = %d\n", ret);
		break;
	}

	default:
		break;
	}

	/* Flush cache */
	if (iobuf)
		os_sync_after_write(iobuf, iolen);

	return ret;
}

s32 __FAT_Ioctlv(s32 fd, u32 cmd, ioctlv *vector, u32 inlen, u32 iolen)
{
	s32 ret = IPC_EINVAL;

	/* Invalidate cache */
	InvalidateVector(vector, inlen, iolen);

	/* Missing vectors, unterminated paths, short outputs: IPC_EINVAL */
	if (!__FAT_CheckVectors(cmd, vector, inlen, iolen))
		goto out;

	/* Parse command */
	switch (cmd) {
	/** Create directory **/
	case IOCTL_FAT_MKDIR: {
		char *dirpath = (char *)vector[0].data;

		dbg_printf("FAT: IOCTL_FAT_MKDIR: fd = %d, dirpath = %s\n", fd, dirpath);

		/* Sart blinking */
		Led_BlinkOn();

		/* Create directory */
		ret = FAT_CreateDir(dirpath);

		/* Stop blinking */
		Led_BlinkOff();

		dbg_printf("FAT: IOCTL_FAT_MKDIR: ret = %d\n", ret);

		break;
	}

	/** Create file **/
	case IOCTL_FAT_MKFILE: {
		char *filepath = (char *)vector[0].data;

		dbg_printf("FAT: IOCTL_FAT_MKFILE: fd = %d, filepath = %s\n", fd, filepath);

		/* Sart blinking */
		Led_BlinkOn();

		/* Create file */
		ret = FAT_CreateFile(filepath);

		/* Stop blinking */
		Led_BlinkOff();

		dbg_printf("FAT: IOCTL_FAT_MKFILE: ret = %d\n", ret);

		break;
	}

	/** Read directory **/
	case IOCTL_FAT_READDIR_FS:
	case IOCTL_FAT_READDIR: {
		char *dirpath = (char *)vector[0].data;
		char *outbuf  = NULL;
		u32  *outlen  = NULL;

		u32 buflen  = 0;
		u32 entries = 0, forFS;

		/* Set FS flag */
		forFS = (cmd == IOCTL_FAT_READDIR_FS);

		dbg_printf("FAT: IOCTL_FAT_READDIR%s: fd = %d, dirpath = %s\n", forFS? "_FS" : "", fd, dirpath);

		/* Input values */
		if (iolen > 1) {
			entries = *(u32 *)vector[1].data;
			outbuf  = (char *)vector[2].data;
			outlen  =  (u32 *)vector[3].data;
			buflen  =         vector[2].len;
		} else
			outlen =  (u32 *)vector[1].data;

		/* Read directory */
		ret = FAT_ReadDir(dirpath, outbuf, buflen, outlen, entries, forFS);

		dbg_printf("FAT: IOCTL_FAT_READDIR%s: ret = %d\n", forFS? "_FS" : "", ret);

		break;
	}

	/** Delete object **/
	case IOCTL_FAT_DELETE: {
		char *path = (char *)vector[0].data;

 		dbg_printf("FAT: IOCTL_FAT_DELETE: fd = %d, path = %s\n", fd, path);

		/* Sart blinking */
		Led_BlinkOn();

		/* Delete file/directory */
		ret = FAT_Delete(path);

		/* Stop blinking */
		Led_BlinkOff();

 		dbg_printf("FAT: IOCTL_FAT_DELETE: ret = %d\n", ret);

		break;
	}

	/** Delete directory **/
	case IOCTL_FAT_DELETEDIR: {
		char *dirpath = (char *)vector[0].data;
 		dbg_printf("FAT: IOCTL_FAT_DELETEDIR: fd = %d, path = %s\n", fd, dirpath);

		/* Sart blinking */
		Led_BlinkOn();

		/* Delete directory */
		ret = FAT_DeleteDir(dirpath);

		/* Stop blinking */
		Led_BlinkOff();

 		dbg_printf("FAT: IOCTL_FAT_DELETEDIR: ret = %d\n", ret);

		break;
	}

	/** Rename object **/
	case IOCTL_FAT_RENAME: {
		char *oldname = (char *)vector[0].data;
		char *newname = (char *)vector[1].data;
 		dbg_printf("FAT: IOCTL_FAT_RENAME: fd = %d, from = %s, to = %s\n", fd, oldname, newname);

		/* Sart blinking */
		Led_BlinkOn();

		/* Rename object */
		ret = FAT_Rename(oldname, newname);

		/* Stop blinking */
		Led_BlinkOff();

 		dbg_printf("FAT: IOCTL_FAT_RENAME: ret = %d\n", ret);

		break;
	}

	/** Get stats **/
	case IOCTL_FAT_STATS: {
		char *path  = (char *)vector[0].data;
		void *stats = (void *)vector[1].data;
 		dbg_printf("FAT: IOCTL_FAT_STATS: fd = %d, path = %s\n", fd, path);

		/* Get stats */
		ret = FAT_GetStats(path, stats);
 		dbg_printf("FAT: IOCTL_FAT_STATS: ret = %d, size = %d\n", ret, ((struct stats *)stats)->size);

		break;
	}

	/** Get usage **/
	case IOCTL_FAT_GETUSAGE_FS:
	case IOCTL_FAT_GETUSAGE: {
		char *path  = (char *)vector[0].data;
		u64  *size  =  (u64 *)vector[1].data;
		u32  fakedirs, fakefiles, forFS;

		/* The file and directory counts are optional outputs */
		u32  *files = (iolen >= 2) ? (u32 *)vector[2].data : &fakefiles;
		u32  *dirs  = (iolen >= 3) ? (u32 *)vector[3].data : &fakedirs;

		/* Set initial values */
		*size  = 0;
		*files = 0;
		*dirs  = 0;
			
		/* Set FS flag */
		forFS = (cmd == IOCTL_FAT_GETUSAGE_FS);
 		dbg_printf("FAT: IOCTL_FAT_GETUSAGE%s: fd = %d, path = %s\n", forFS?"_FS":"", fd, path);

		/* Get usage */
		ret = FAT_GetUsage(path, size, files, dirs, forFS);
 		dbg_printf("FAT: IOCTL_FAT_GETUSAGE: ret = %d, size = %dl, files = %d, dirs = %d\n", ret, *size, *files, *dirs);

		break;
	}

	/** Mount SD card **/
	case IOCTL_FAT_MOUNT_SD: {
		s32 partition = inlen > 0 ? *(s32 *)vector[0].data : -1;

		/* Mount SD card */
		ret = FAT_Mount(0, partition);

		break;
	}

	/** Unmount SD card **/
	case IOCTL_FAT_UMOUNT_SD: {
		/* Unmount SD card */
		ret = FAT_Unmount(0);
		if (ret < 0)
			break;

		/* Deinitialize SDIO */
		ret = sdio_Shutdown();
		if (!ret)
			ret = IPC_EINVAL;

		break;
	}

	/** Mount USB device **/
	case IOCTL_FAT_MOUNT_USB: {
		s32 partition = inlen > 0 ? *(s32 *)vector[0].data : -1;

		/* Mount USB device */
		ret = FAT_Mount(1, partition);

		break;
	}

	/** Unmount USB card **/
	case IOCTL_FAT_UMOUNT_USB: {
		/* Unmount USB */
		ret = FAT_Unmount(1);
		if (ret < 0)
			break;

		/* Deinitialize USB */
		ret = usbstorage_Shutdown();
		if (!ret)
			ret = IPC_EINVAL;

		break;
	}

	/** Get mounted partition of the given device **/
	case IOCTL_FAT_GETPARTITION: {
		u32 device = *(u32 *)vector[0].data;
		u32 *partition = (u32 *)vector[1].data;

		/* Device 0 is the SD card, 1 is USB */
		if (device > 1)
			break;

		/* Get partition */
		ret = FAT_GetPartition(device, partition);

		break;
	}

	default:
		break;
	}

out:
	/* Flush cache */
	FlushVector(vector, inlen, iolen);

	return ret;
}

s32 __FAT_Initialize(u32 *queuehandle)
{
	/* Heap space */
	//static u32 heapspace[0x8000] ATTRIBUTE_ALIGN(32);
	static u32 heapspace[0x7000] ATTRIBUTE_ALIGN(32);

	void *buffer = NULL;
	s32   ret;

	/* Initialize memory heap */
	ret = Mem_Init(heapspace, sizeof(heapspace));
	if (ret < 0)
		return ret;

	/* Initialize timer subsystem */
	ret = Timer_Init();
	if (ret < 0)
		return ret;

	/* Allocate queue buffer */
	buffer = Mem_Alloc(0x80);
	if (!buffer)
		return IPC_ENOMEM;

	/* Create message queue */
	ret = os_message_queue_create(buffer, 32);
	if (ret < 0)
		return ret;

	/* Register device */
	os_device_register(DEVICE_FAT, ret);
	os_device_register("$", ret);

	/* Copy queue handler */
	*queuehandle = ret;

	return 0;
}

int main(void)
{
	u32 queuehandle;
	s32 ret;

	/* Print info */
	svc_write("$IOSVersion: FAT: " __DATE__ " " __TIME__ " 64M$\n");

	/* Create blinker thread */
	Led_CreateBlinkThread();

	/* Initialize module */
	ret = __FAT_Initialize(&queuehandle);
	if (ret < 0)
		return ret;

	/* Main loop */
	while (1) {
		ipcmessage *message = NULL;

		/* Wait for message */
		ret = os_message_queue_receive(queuehandle, (void *)&message, 0);
		if (ret)
			continue;

		switch (message->command) {
		case IOS_OPEN: {
			char *devpath = message->open.device;
			u32   mode    = message->open.mode;

			if (*devpath == '$') {
				/* The emu NAND path plus the escaped name: FAT_Escape() turns
				   some characters into 4, and IOS paths are under 64 bytes */
				char path[sizeof(emuNandPath) + 4 * 64];

				if (strnlen(devpath + 1, 64) >= 64)
					ret = FS_ENAMETOOLONG;
				else {
					/* Build absolute path to emu nand */
					strcpy(path, emuNandPath);
					FAT_Escape(path+emuNandPathLen, devpath+1);

					dbg_printf("FAT: IOS_OPEN: Opening relative file %s\n", devpath);
					/* Open file */
					ret = FAT_Open(path, mode);
					dbg_printf("FAT: IOS_OPEN: ret = %d\n", ret);
				}
			}
			else if (!strcmp(devpath, DEVICE_FAT)) {
				/* Open module */
				dbg_printf("FAT: IOS_OPEN: Opening FAT module\n");
				ret = 0;
				dbg_printf("FAT: IOS_OPEN: ret = %d\n", ret);
			}
			else if (!strncmp(devpath, DEVICE_FAT, DEVICE_FAT_LEN)) {
				/* Open file */
				dbg_printf("FAT: IOS_OPEN: Opening absolute file %s\n", devpath + DEVICE_FAT_LEN);
				ret = FAT_Open(devpath + DEVICE_FAT_LEN, mode);
				dbg_printf("FAT: IOS_OPEN: ret = %d\n", ret);
			}
			else {
				dbg_printf("FAT: IOS_OPEN: Unknown device path %s\n", devpath);
				/* Error */
				ret = IPC_EINVAL;
			}

			break;
		}

		case IOS_CLOSE: {
			ret = 0;

			// FIX d2x v7 beta1
			// Check added because the fd might represent the module,
			// see IOS_OPEN.
			// Tipically a fd representing a module is far lower than 32, 
			// while a fd representing a file is an address 32 byte aligned.
			if (__FAT_IsFileFd(message->fd)) {
				dbg_printf("FAT: IOS_CLOSE: Closing file... fd = %d\n", message->fd);
				/* Close file */
				ret = FAT_Close(message->fd);
			}
#ifdef DEBUG
			else {
				dbg_printf("FAT: IOS_CLOSE: Closing FAT module...\n");
			}
#endif

			dbg_printf("FAT: IOS_CLOSE: ret = %d\n", ret);

			break;
		}

		case IOS_READ: {
			void *buffer = message->read.data;
			u32   len    = message->read.length;

			dbg_printf("FAT: IOS_READ: fd = %d, buffer = %x, len = %d\n", message->fd, buffer, len);

			/* Read file */
			ret = __FAT_IsFileFd(message->fd) ? FAT_Read(message->fd, buffer, len) : IPC_EINVAL;

			/* Flush cache */
			os_sync_after_write(buffer, len);

			dbg_printf("FAT: IOS_READ: ret = %d\n", ret);

			break;
		}

		case IOS_WRITE: {
			void *buffer = message->write.data;
			u32   len    = message->write.length;

			dbg_printf("FAT: IOS_WRITE: fd = %d, buffer = %x, len = %d\n", message->fd, buffer, len);

			/* Invalidate cache */
			os_sync_before_read(buffer, len);

			/* Sart blinking */
			Led_BlinkOn();

			/* Write file */
			ret = __FAT_IsFileFd(message->fd) ? FAT_Write(message->fd, buffer, len) : IPC_EINVAL;

			dbg_printf("FAT: IOS_WRITE: ret = %d\n", ret);

			/* Stop blinking */
			Led_BlinkOff();

			break;
		}

		case IOS_SEEK: {
			s32 where  = message->seek.offset;
			s32 whence = message->seek.origin;

			dbg_printf("FAT: IOS_SEEK: fd = %d, where = %d, whence = %d\n", message->fd, where, whence);
      
			/* Seek file */
			ret = __FAT_IsFileFd(message->fd) ? FAT_Seek(message->fd, where, whence) : IPC_EINVAL;

			dbg_printf("FAT: IOS_SEEK: ret = %d\n", ret);

			break;
		}

		case IOS_IOCTL: {
			void *inbuf = message->ioctl.buffer_in;
			void *iobuf = message->ioctl.buffer_io;
			u32   inlen = message->ioctl.length_in;
			u32   iolen = message->ioctl.length_io;
			u32   cmd   = message->ioctl.command;

			/* Parse IOCTL */
			ret = __FAT_Ioctl(message->fd, cmd, inbuf, inlen, iobuf, iolen);

			break;
		}

		case IOS_IOCTLV: {
			ioctlv *vector = message->ioctlv.vector;
			u32     inlen  = message->ioctlv.num_in;
			u32     iolen  = message->ioctlv.num_io;
			u32     cmd    = message->ioctlv.command;

			/* Parse IOCTLV */
			ret = __FAT_Ioctlv(message->fd, cmd, vector, inlen, iolen);

			break;
		}

		default:
			/* Unknown command */
			ret = IPC_EINVAL;
		}

		/* Acknowledge message */
		os_message_queue_ack((void *)message, ret);
	}
   
	return 0;
}
