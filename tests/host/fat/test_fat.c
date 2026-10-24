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
int fatw_corruptions(void);
int fatw_deletedir(const char *path);
int fatw_usage(const char *path, unsigned long long *size, unsigned int *files, unsigned int *dirs);
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

static void make_dir_of_files(const char *dir, int n)
{
	char path[64];
	CHECK(fatw_mkdir(dir) >= 0, "mkdir %s failed", dir);
	for (int i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "%s/a_long_file_name_%02d.txt", dir, i);
		CHECK(fatw_create(path) >= 0, "create %s failed", path);
	}
}

/*
 * READDIR with a buffer the caller sized too small: names must stop at the
 * buffer's end. Before, FAT_ReadDir() copied every name into
 * Mem_Alloc(buflen) without a bound.
 */
static void test_readdir_small_buffer(void)
{
	char out[256];
	unsigned int n = 0;
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	make_dir_of_files("0:/many", 8);
	CHECK_OK();
	memset(out, 0x77, sizeof(out));
	CHECK(fatw_readdir("0:/many", out, 40, &n, 0) == 0, "readdir failed");
	CHECK(fatw_corruptions() == 0, "the module heap was overwritten past the buffer");
	CHECK(out[40] == 0x77, "wrote past the caller's %d-byte buffer", 40);
	CHECK(n >= 1 && n < 8, "%u entries reported for a 40-byte buffer", n);
}

/* The names come back whole; bytes past the last name are not heap leftovers. */
static void test_readdir_returns_names(void)
{
	char out[512];
	unsigned int n = 0;
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	make_dir_of_files("0:/two", 2);
	CHECK_OK();
	memset(out, 0, sizeof(out));
	CHECK(fatw_readdir("0:/two", out, sizeof(out), &n, 0) == 0, "readdir failed");
	CHECK(n == 2, "%u entries", n);
	size_t first = strlen(out);
	CHECK(first > 0 && !strncmp(out, "a_long_file_name_0", 18), "first name \"%.30s\"", out);
	const char *second = out + first + 1;
	CHECK(!strncmp(second, "a_long_file_name_0", 18), "second name \"%.30s\"", second);
	const char *end = second + strlen(second) + 1;
	for (const char *p = end; p < out + sizeof(out); p++)
		CHECK(*p == 0, "heap bytes copied out after the names (offset %td)", p - out);
}

/* READDIR for the count only (no output buffer) frees what it allocated. */
static void test_readdir_count_only_frees(void)
{
	unsigned int n = 0;
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	make_dir_of_files("0:/cnt", 3);
	CHECK_OK();
	int live = fatw_live_blocks();
	CHECK(fatw_readdir("0:/cnt", NULL, 128, &n, 0) == 0, "readdir failed");
	CHECK(n == 3, "%u entries", n);
	CHECK(fatw_live_blocks() == live, "%d blocks leaked", fatw_live_blocks() - live);
}

/* A directory path over 256 characters: FatFs allows it (255 per name). */
static void make_long_tree(char *top, size_t toplen)
{
	char path[400];
	snprintf(top, toplen, "0:/%0200d", 0);
	CHECK(fatw_mkdir(top) >= 0, "mkdir top failed");
	snprintf(path, sizeof(path), "%s/%0100d", top, 1);
	CHECK(fatw_mkdir(path) >= 0, "mkdir inner failed");
	snprintf(path, sizeof(path), "%s/%0100d/f.txt", top, 1);
	CHECK(fatw_create(path) >= 0, "create in inner failed");
}

/*
 * DELETEDIR and GETUSAGE build each entry's path in a 256-byte stack buffer.
 * A path that does not fit must fail cleanly (FS_ENAMETOOLONG), not run
 * past the buffer: before, strcpy/strcat overflowed it (ASan reports a
 * stack-buffer-overflow).
 */
static void test_deletedir_long_path(void)
{
	char top[256];
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	make_long_tree(top, sizeof(top));
	CHECK_OK();
	int r = fatw_deletedir(top);
	CHECK(r == -110, "DELETEDIR of a too-long tree returned %d, expected FS_ENAMETOOLONG", r);
}

static void test_getusage_long_path(void)
{
	char top[256];
	unsigned long long size = 0;
	unsigned int files = 0, dirs = 0;
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	make_long_tree(top, sizeof(top));
	CHECK_OK();
	int r = fatw_usage(top, &size, &files, &dirs);
	CHECK(r == -110, "GETUSAGE of a too-long tree returned %d, expected FS_ENAMETOOLONG", r);
}

/* A chain of one-letter directories, levels deep, with a 4-byte file at the bottom. */
static void make_deep_tree(const char *top, int levels)
{
	char path[256];
	snprintf(path, sizeof(path), "%s", top);
	CHECK(fatw_mkdir(path) >= 0, "mkdir %s failed", path);
	for (int i = 1; i < levels; i++) {
		strcat(path, "/a");
		CHECK(fatw_mkdir(path) >= 0, "mkdir level %d failed", i);
	}
	strcat(path, "/f.bin");
	put_file(path, "data");
}

/*
 * DELETEDIR and GETUSAGE recurse with about 350 bytes of stack per level on
 * a 12 KB thread stack. Within the 255-character path limit a tree of
 * one-letter directories can be over 100 levels deep, so the recursion is
 * capped (IOS's own NAND allows 8 levels): a deeper tree is refused with
 * FS_EINVAL, a tree within the cap works as before.
 */
static void test_deletedir_deep_tree_refused(void)
{
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	make_deep_tree("0:/deep", 24);
	CHECK_OK();
	int r = fatw_deletedir("0:/deep");
	CHECK(r == -4, "DELETEDIR of a 24-level tree returned %d, expected FS_EINVAL", r);
}

static void test_deletedir_tree_within_cap(void)
{
	char out[64];
	unsigned int n = 99;
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	make_deep_tree("0:/ten", 10);
	CHECK_OK();
	int r = fatw_deletedir("0:/ten");
	CHECK(r == 0, "DELETEDIR of a 10-level tree returned %d", r);
	CHECK(fatw_readdir("0:/ten", out, sizeof(out), &n, 0) == 0 && n == 0,
	      "0:/ten still holds %u entries", n);
}

static void test_getusage_deep_tree_refused(void)
{
	unsigned long long size = 0;
	unsigned int files = 0, dirs = 0;
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	make_deep_tree("0:/deep", 24);
	CHECK_OK();
	int r = fatw_usage("0:/deep", &size, &files, &dirs);
	CHECK(r == -4, "GETUSAGE of a 24-level tree returned %d, expected FS_EINVAL", r);
}

static void test_getusage_tree_within_cap(void)
{
	unsigned long long size = 0;
	unsigned int files = 0, dirs = 0;
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	make_deep_tree("0:/ten", 10);
	CHECK_OK();
	int r = fatw_usage("0:/ten", &size, &files, &dirs);
	CHECK(r == 0 && files == 1 && dirs == 9 && size == 4,
	      "GETUSAGE returned %d: %u files, %u dirs, %llu bytes", r, files, dirs, size);
}

/* A close that fails (the card went away) still frees the file's FIL. */
static void test_close_failure_frees(void)
{
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	CHECK(fatw_create("0:/c.txt") >= 0, "create failed");
	int live = fatw_live_blocks();
	long fd = fatw_open("0:/c.txt", FA_WRITE);
	CHECK(fd > 0, "open failed");
	CHECK(fatw_write(fd, "data", 4) == 4, "write failed");
	ramdisk[0].fail_writes = 1;
	int r = fatw_close(fd);
	ramdisk[0].fail_writes = 0;
	CHECK(r < 0, "close returned %d with the disk failing", r);
	CHECK(fatw_live_blocks() == live, "%d FIL block leaked", fatw_live_blocks() - live);
}

static const struct hc_test tests[] = {
	{ "one_volume", test_one_volume, false },
	{ "sd_and_usb_together", test_sd_and_usb_together, false },
	{ "readdir_small_buffer", test_readdir_small_buffer, false },
	{ "readdir_returns_names", test_readdir_returns_names, false },
	{ "readdir_count_only_frees", test_readdir_count_only_frees, false },
	{ "deletedir_long_path", test_deletedir_long_path, false },
	{ "getusage_long_path", test_getusage_long_path, false },
	{ "close_failure_frees", test_close_failure_frees, false },
	{ "deletedir_deep_tree_refused", test_deletedir_deep_tree_refused, false },
	{ "deletedir_tree_within_cap", test_deletedir_tree_within_cap, false },
	{ "getusage_deep_tree_refused", test_getusage_deep_tree_refused, false },
	{ "getusage_tree_within_cap", test_getusage_tree_within_cap, false },
};

int main(int argc, char **argv)
{
	return hc_main(tests, HC_COUNT(tests), argc, argv, setup);
}
