/*
 * Host tests of the FAT module's fat_wrapper.c and FatFs on two RAM disks
 * (SD is drive 0, USB is drive 1). Run with `make -C tests/host`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ramdisk.h"
#define HOSTCHECK_MAIN
#include "hostcheck.h"

#define FA_READ  0x01
#define FA_WRITE 0x02

void fatw_reset(void);
int fatw_live_blocks(void);
int fatw_mount(int dev);
int fatw_unmount(int dev);
int fatw_create(const char *path);
int fatw_mkdir(const char *path);
long fatw_open(const char *path, unsigned int mode);
int fatw_close(long fd);
int fatw_read(long fd, void *buf, unsigned int len);
int fatw_write(long fd, const void *buf, unsigned int len);
int fatw_readdir(const char *path, char *out, unsigned int buflen, unsigned int *outlen,
		 unsigned int entries);

static void put_file(const char *path, const char *text)
{
	CHECK(fatw_create(path) >= 0, "create %s failed", path);
	long fd = fatw_open(path, FA_WRITE);
	CHECK(fd > 0, "open %s for writing: %ld", path, fd);
	CHECK(fatw_write(fd, text, strlen(text)) == (int) strlen(text), "write %s failed", path);
	CHECK(fatw_close(fd) >= 0, "close %s failed", path);
}

static void get_file(const char *path, char *buf, unsigned int len)
{
	memset(buf, 0, len);
	long fd = fatw_open(path, FA_READ);
	CHECK(fd > 0, "open %s for reading: %ld", path, fd);
	CHECK(fatw_read(fd, buf, len - 1) >= 0, "read %s failed", path);
	CHECK(fatw_close(fd) >= 0, "close %s failed", path);
}

static void setup(void)
{
	fatw_reset();
	ramdisk_reset(16384);                   /* 8 MB each */
	if (ramdisk_format(0) || ramdisk_format(1))
		hc_hang("formatting the RAM disks failed");
}

static void test_one_volume(void)
{
	char buf[64];
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	put_file("0:/hello.txt", "hello");
	CHECK_OK();
	get_file("0:/hello.txt", buf, sizeof(buf));
	CHECK_OK();
	CHECK(!strcmp(buf, "hello"), "read back %s", buf);
	CHECK(fatw_live_blocks() == 0, "%d FIL blocks still allocated", fatw_live_blocks());
}

/*
 * SD and USB mounted together each keep their own file system. Before, one
 * static FATFS served both volumes, so whichever was used last answered for
 * either drive number.
 */
static void test_sd_and_usb_together(void)
{
	char buf[64];
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	CHECK(fatw_mount(1) >= 0, "mount USB failed");
	put_file("0:/which.txt", "sd card");
	CHECK_OK();
	put_file("1:/which.txt", "usb drive");
	CHECK_OK();
	get_file("0:/which.txt", buf, sizeof(buf));
	CHECK_OK();
	CHECK(!strcmp(buf, "sd card"), "0:/which.txt holds \"%s\"", buf);
	get_file("1:/which.txt", buf, sizeof(buf));
	CHECK_OK();
	CHECK(!strcmp(buf, "usb drive"), "1:/which.txt holds \"%s\"", buf);
}

static const struct hc_test tests[] = {
	{ "one_volume", test_one_volume, false },
	{ "sd_and_usb_together", test_sd_and_usb_together, false },
};

int main(int argc, char **argv)
{
	return hc_main(tests, HC_COUNT(tests), argc, argv, setup);
}
