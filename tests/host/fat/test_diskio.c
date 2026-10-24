/*
 * Host tests of the FAT module's diskio.c: the drivers are stubbed on a RAM
 * array that refuses unaligned buffers, as the SD and EHCI drivers need
 * 32-byte alignment for DMA. Run with `make -C tests/host`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fat_types.h"
#include "types.h"
#include "sdio.h"
#include "usbstorage.h"
#include "mem.h"

#define DISK_SECTORS 1024

static u8 disk[2][DISK_SECTORS * 4096];
static u32 usb_secsz;
static int calls, unaligned, out_of_range;
static u32 largest;             /* sectors in the largest single transfer */
static int alloc_live;
static u32 alloc_largest, alloc_limit;

/* Mem_Alloc: malloc with a size limit, standing in for the 112 KB heap */
void *Mem_Alloc(u32 size)
{
	if (size > alloc_limit)
		return NULL;
	if (size > alloc_largest)
		alloc_largest = size;
	alloc_live++;
	return aligned_alloc(32, (size + 31) & ~31u);
}

void Mem_Free(void *ptr)
{
	if (ptr)
		alloc_live--;
	free(ptr);
}

s32 Mem_Init(u32 *heapspace, u32 heapspaceSize) { (void)heapspace; (void)heapspaceSize; return 0; }

/* cios-lib's bool is an int: the stubs and diskio.c come before hostcheck.h */
static int ram_io(int drv, u32 secsz, u32 sector, u32 count, void *buffer, int write)
{
	calls++;
	if (count > largest)
		largest = count;
	if ((uintptr_t)buffer & 31) {
		unaligned++;
		return 0;
	}
	if (sector + count > DISK_SECTORS) {
		out_of_range++;
		return 0;
	}
	u8 *at = disk[drv] + (size_t)sector * secsz;
	if (write)
		memcpy(at, buffer, (size_t)count * secsz);
	else
		memcpy(buffer, at, (size_t)count * secsz);
	return 1;
}

bool sdio_ReadSectors(sec_t s, sec_t n, void *b) { return ram_io(0, 512, s, n, b, 0); }
bool sdio_WriteSectors(sec_t s, sec_t n, const void *b) { return ram_io(0, 512, s, n, (void *)b, 1); }
bool sdio_IsInserted(void) { return 1; }
bool usbstorage_ReadSectors(u32 s, u32 n, void *b) { return ram_io(1, usb_secsz, s, n, b, 0); }
bool usbstorage_WriteSectors(u32 s, u32 n, void *b) { return ram_io(1, usb_secsz, s, n, b, 1); }
bool usbstorage_IsInserted(void) { return 1; }
u32 usbstorage_GetSectorSize(void) { return usb_secsz; }

#include "diskio.c"

#define HOSTCHECK_MAIN
#include "hostcheck.h"

static void setup(void)
{
	usb_secsz = 512;
	calls = unaligned = out_of_range = 0;
	largest = 0;
	alloc_live = 0;
	alloc_largest = 0;
	alloc_limit = 112 * 1024;
	for (int d = 0; d < 2; d++)
		for (size_t i = 0; i < sizeof(disk[d]); i++)
			disk[d][i] = (u8)(i * 7 + d);
}

/* A buffer at a given misalignment from 32 bytes */
static u8 *buffer_at(u8 *base, int off)
{
	return (u8 *)(((uintptr_t)base + 31) & ~(uintptr_t)31) + off;
}

static void test_aligned_read_is_direct(void)
{
	static u8 base[255 * 512 + 64];
	u8 *buf = buffer_at(base, 0);
	CHECK(disk_read(0, buf, 10, 255) == RES_OK, "aligned read failed");
	CHECK(calls == 1 && alloc_largest == 0, "%d calls, %u bytes allocated", calls, alloc_largest);
	CHECK(memcmp(buf, disk[0] + 10 * 512, 255 * 512) == 0, "wrong data");
}

static void test_unaligned_read_large(void)
{
	static u8 base[255 * 512 + 64];
	u8 *buf = buffer_at(base, 3);
	CHECK(disk_read(0, buf, 100, 255) == RES_OK, "unaligned 255-sector read failed");
	CHECK(unaligned == 0, "a driver got an unaligned buffer");
	CHECK(alloc_largest <= 16 * 1024, "bounce buffer of %u bytes", alloc_largest);
	CHECK(memcmp(buf, disk[0] + 100 * 512, 255 * 512) == 0, "wrong data");
	CHECK(alloc_live == 0, "%d blocks leaked", alloc_live);
}

/* The heap need not have room for the whole transfer */
static void test_unaligned_read_small_heap(void)
{
	static u8 base[200 * 4096 + 64];
	usb_secsz = 4096;
	alloc_limit = 32 * 1024;
	u8 *buf = buffer_at(base, 1);
	CHECK(disk_read(1, buf, 5, 200) == RES_OK, "unaligned 800 KB read failed");
	CHECK(memcmp(buf, disk[1] + 5 * 4096, 200 * 4096) == 0, "wrong data");
	CHECK(alloc_live == 0, "%d blocks leaked", alloc_live);
}

static void test_unaligned_write_large(void)
{
	static u8 base[255 * 512 + 64];
	u8 *buf = buffer_at(base, 17);
	for (int i = 0; i < 255 * 512; i++)
		buf[i] = (u8)(i ^ 0x5A);
	CHECK(disk_write(1, buf, 300, 255) == RES_OK, "unaligned 255-sector write failed");
	CHECK(unaligned == 0, "a driver got an unaligned buffer");
	CHECK(alloc_largest <= 16 * 1024, "bounce buffer of %u bytes", alloc_largest);
	CHECK(memcmp(buf, disk[1] + 300 * 512, 255 * 512) == 0, "wrong data on disk");
	CHECK(alloc_live == 0, "%d blocks leaked", alloc_live);
}

/* A sector larger than the bounce limit still moves one sector at a time */
static void test_unaligned_huge_sector(void)
{
	static u8 base[3 * 32768 + 64];
	usb_secsz = 32768;
	u8 *buf = buffer_at(base, 8);
	CHECK(disk_read(1, buf, 0, 3) == RES_OK, "read of 32 KB sectors failed");
	CHECK(largest == 1 && calls == 3, "%d calls, largest %u sectors", calls, largest);
	CHECK(memcmp(buf, disk[1], 3 * 32768) == 0, "wrong data");
}

/* A failing chunk fails the call, frees the buffer and stops there */
static void test_unaligned_failure_stops(void)
{
	static u8 base[64 * 512 + 64];
	u8 *buf = buffer_at(base, 4);
	CHECK(disk_read(0, buf, DISK_SECTORS - 40, 64) == RES_ERROR, "read past the end succeeded");
	CHECK(out_of_range == 1, "%d failing transfers, expected one", out_of_range);
	CHECK(alloc_live == 0, "%d blocks leaked", alloc_live);
}

static void test_no_bounce_memory(void)
{
	static u8 base[8 * 512 + 64];
	alloc_limit = 0;
	CHECK(disk_read(0, buffer_at(base, 2), 0, 8) == RES_ERROR, "read without memory succeeded");
	CHECK(calls == 0, "the driver ran without a bounce buffer");
}

static void test_bad_drive(void)
{
	static u8 base[512 + 64];
	CHECK(disk_read(7, buffer_at(base, 0), 0, 1) == RES_ERROR, "drive 7 read succeeded");
	CHECK(disk_write(7, buffer_at(base, 0), 0, 1) == RES_ERROR, "drive 7 write succeeded");
	CHECK(disk_read(0, buffer_at(base, 0), 0, 0) == RES_ERROR, "zero-sector read succeeded");
}

static const struct hc_test tests[] = {
	{ "aligned_read_is_direct", test_aligned_read_is_direct, false },
	{ "unaligned_read_large", test_unaligned_read_large, false },
	{ "unaligned_read_small_heap", test_unaligned_read_small_heap, false },
	{ "unaligned_write_large", test_unaligned_write_large, false },
	{ "unaligned_huge_sector", test_unaligned_huge_sector, false },
	{ "unaligned_failure_stops", test_unaligned_failure_stops, false },
	{ "no_bounce_memory", test_no_bounce_memory, false },
	{ "bad_drive", test_bad_drive, false },
};

int main(int argc, char **argv)
{
	return hc_main(tests, HC_COUNT(tests), argc, argv, setup);
}
