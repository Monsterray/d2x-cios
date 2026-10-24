/*
 * The FAT module's fat_wrapper.c (and cios-lib's fat_tools.c) for the host,
 * on the RAM disks in ramdisk.c, with cios-lib's types.
 *
 * FAT_Open() returns the FIL pointer itself as the file descriptor, a u32 on
 * the Wii; so Mem_Alloc() hands out blocks from a static arena, which a
 * non-PIE host binary keeps below 4 GB.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "fat_types.h"

#include "types.h"
#include "sdio.h"
#include "usbstorage.h"

bool sdio_Startup(void) { return true; }
bool usbstorage_Init(void) { return true; }

/* Mem_Alloc: a bump allocator with a live-block count, for leak checks. */
static uint8_t arena[1 << 20] __attribute__((aligned(32)));
static size_t arena_used;
static int arena_live;

void *Mem_Alloc(u32 size)
{
	size = (size + 31) & ~31u;
	if (arena_used + size > sizeof(arena))
		return NULL;
	void *p = arena + arena_used;
	arena_used += size;
	arena_live++;
	memset(p, 0xA5, size);          /* not zeroed, as on the Wii */
	return p;
}

void Mem_Free(void *ptr)
{
	if (ptr)
		arena_live--;
}

#include "fat_tools.c"
#include "fat_wrapper.c"

/* For test_fat.c, in plain C types. */
void fatw_reset(void)
{
	arena_used = 0;
	arena_live = 0;
	f_mount(0, NULL);
	f_mount(1, NULL);
}

int fatw_live_blocks(void) { return arena_live; }
int fatw_mount(int dev) { return FAT_Mount((u8) dev, -1); }
int fatw_unmount(int dev) { return FAT_Unmount((u8) dev); }
int fatw_create(const char *path) { return FAT_CreateFile(path); }
int fatw_mkdir(const char *path) { return FAT_CreateDir(path); }
long fatw_open(const char *path, unsigned int mode) { return FAT_Open(path, mode); }
int fatw_close(long fd) { return FAT_Close((s32) fd); }
int fatw_read(long fd, void *buf, unsigned int len) { return FAT_Read((s32) fd, buf, len); }
int fatw_write(long fd, const void *buf, unsigned int len) { return FAT_Write((s32) fd, (void *) buf, len); }

int fatw_readdir(const char *path, char *out, unsigned int buflen, unsigned int *outlen,
		 unsigned int entries)
{
	u32 n = 0;
	s32 r = FAT_ReadDir(path, out, buflen, &n, entries, 0);
	*outlen = n;
	return r;
}

int fatw_deletedir(const char *path) { return FAT_DeleteDir(path); }

int fatw_usage(const char *path, unsigned long long *size, unsigned int *files, unsigned int *dirs)
{
	u64 s = 0;
	u32 f = 0, d = 0;
	s32 r = FAT_GetUsage(path, &s, &f, &d, 0);
	*size = s;
	*files = f;
	*dirs = d;
	return r;
}
