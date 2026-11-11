/*
 * Host tests of the FAT module's fat_wrapper.c and FatFs on two RAM disks
 * (SD is drive 0, USB is drive 1). Run with `make -C tests/host`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ramdisk.h"
#include "module.h"
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
int fatw_ioctlv(unsigned int cmd, void **data, unsigned int *len, unsigned int inlen,
		unsigned int iolen);
int fatw_ioctl(long fd, unsigned int cmd, void *in, unsigned int inlen, void *io, unsigned int iolen);
const char *fatw_nandpath(void);
unsigned int fatw_usb_port(void);
unsigned int fatw_usb_closes(void);
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

/* --- main.c's IPC handlers: what a caller can send ---------------------- */

#define IPC_EINVAL -101

/* A vector buffer of exactly len bytes (ASan sees any read past it). */
static void *exact(const void *data, size_t len)
{
	void *p = malloc(len ? len : 1);
	memcpy(p, data, len);
	return p;
}

/*
 * READDIR with one input and two outputs: the handler read vector[3] when
 * iolen > 1 whatever inlen was, one past the three vectors sent.
 */
static void test_ioctlv_readdir_vector_count(void)
{
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	unsigned int entries = 0, outlen = 0;
	char out[64];
	void *data[3] = { exact("0:/", 4), &entries, out };
	unsigned int len[3] = { 4, 4, sizeof(out) };
	(void) outlen;
	int r = fatw_ioctlv(IOCTL_FAT_READDIR, data, len, 1, 2);
	free(data[0]);
	CHECK(r == IPC_EINVAL, "READDIR with 3 vectors returned %d, expected IPC_EINVAL", r);
}

/* GETUSAGE with only the size output reads no vector past the second. */
static void test_ioctlv_getusage_size_only(void)
{
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	put_file("0:/u.bin", "12345");
	CHECK_OK();
	unsigned long long size = 0;
	void *data[2] = { exact("0:/", 4), &size };
	unsigned int len[2] = { 4, sizeof(size) };
	int r = fatw_ioctlv(IOCTL_FAT_GETUSAGE, data, len, 1, 1);
	free(data[0]);
	CHECK(r == 0 && size == 5, "GETUSAGE returned %d, size %llu", r, size);
}

/* A path with no NUL inside its vector is refused, not read past. */
static void test_ioctlv_path_without_nul(void)
{
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	void *data[1] = { exact("0:/abc", 6) };          /* no terminating NUL */
	unsigned int len[1] = { 6 };
	int r = fatw_ioctlv(IOCTL_FAT_MKDIR, data, len, 1, 0);
	free(data[0]);
	CHECK(r == IPC_EINVAL, "MKDIR with an unterminated path returned %d", r);
}

/* GETPARTITION of a device that does not exist (VolToPart has 2 entries). */
static void test_ioctlv_getpartition_bad_device(void)
{
	unsigned int device = 7, partition = 0;
	void *data[2] = { &device, &partition };
	unsigned int len[2] = { 4, 4 };
	int r = fatw_ioctlv(IOCTL_FAT_GETPARTITION, data, len, 1, 1);
	CHECK(r == IPC_EINVAL, "GETPARTITION of device 7 returned %d", r);
}

/* MOUNT_USB: the optional second vector is the USB port. None means port 0;
   a port other than the open client's closes it first; a port that does not
   exist, or a vector too short to hold one, is refused. */
static void test_ioctlv_mount_usb_port(void)
{
	int part = -1;
	unsigned int port = 1, closes;
	void *data[2] = { &part, &port };
	unsigned int len[2] = { 4, 4 };
	int r;

	r = fatw_ioctlv(IOCTL_FAT_MOUNT_USB, data, len, 1, 0);
	CHECK(r == 0 && fatw_usb_port() == 0, "no port vector: ret %d, port %u", r, fatw_usb_port());

	closes = fatw_usb_closes();
	r = fatw_ioctlv(IOCTL_FAT_MOUNT_USB, data, len, 2, 0);
	CHECK(r == 0 && fatw_usb_port() == 1, "port 1: ret %d, port %u", r, fatw_usb_port());
	CHECK(fatw_usb_closes() == closes + 1, "a change of port closes the client once, not %u times",
	      fatw_usb_closes() - closes);

	r = fatw_ioctlv(IOCTL_FAT_MOUNT_USB, data, len, 2, 0);
	CHECK(r == 0 && fatw_usb_closes() == closes + 1, "the same port again keeps the client open");

	port = 2;
	r = fatw_ioctlv(IOCTL_FAT_MOUNT_USB, data, len, 2, 0);
	CHECK(r == IPC_EINVAL && fatw_usb_port() == 1, "port 2 returned %d (port now %u)", r, fatw_usb_port());

	port = 0;
	len[1] = 2;
	r = fatw_ioctlv(IOCTL_FAT_MOUNT_USB, data, len, 2, 0);
	CHECK(r != 0 && fatw_usb_port() == 1, "a 2-byte port vector returned %d", r);

	len[1] = 4;
	r = fatw_ioctlv(IOCTL_FAT_MOUNT_USB, data, len, 1, 0);
	CHECK(r == 0 && fatw_usb_port() == 0, "back to the default: ret %d, port %u", r, fatw_usb_port());
}

/* FILESTATS on the module's own descriptor (0) is not a file. */
static void test_ioctl_filestats_on_module_fd(void)
{
	unsigned char stats[64];
	int r = fatw_ioctl(0, IOCTL_FAT_FILESTATS, NULL, 0, stats, sizeof(stats));
	CHECK(r == IPC_EINVAL, "FILESTATS on fd 0 returned %d", r);
}

/* SETNANDPATH: one trailing '/', a NUL inside the buffer, room for the '/'. */
static void test_ioctl_setnandpath(void)
{
	char *p;
	p = exact("/nands/a", 9);
	CHECK(fatw_ioctl(0, IOCTL_FAT_SETNANDPATH, p, 9, NULL, 0) == 0, "set /nands/a failed");
	free(p);
	CHECK(!strcmp(fatw_nandpath(), "/nands/a/"), "path is \"%s\"", fatw_nandpath());
	p = exact("/nands/b/", 10);
	CHECK(fatw_ioctl(0, IOCTL_FAT_SETNANDPATH, p, 10, NULL, 0) == 0, "set /nands/b/ failed");
	free(p);
	CHECK(!strcmp(fatw_nandpath(), "/nands/b/"), "path is \"%s\" (one '/' expected)", fatw_nandpath());
	p = exact("/nands/c", 8);                      /* no NUL in the buffer */
	int r = fatw_ioctl(0, IOCTL_FAT_SETNANDPATH, p, 8, NULL, 0);
	free(p);
	CHECK(r != 0 && !strcmp(fatw_nandpath(), "/nands/b/"), "an unterminated path was taken (%d)", r);
	char longpath[200];
	memset(longpath, 'x', 191);
	longpath[0] = '/';
	longpath[191] = 0;
	r = fatw_ioctl(0, IOCTL_FAT_SETNANDPATH, longpath, 192, NULL, 0);
	CHECK(r != 0, "a 191-character path was taken (no room for the '/')");
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

/* FatFs's name buffers are static: a close (which writes the directory entry)
   works with the module heap exhausted, and the size and start cluster land. */
extern int fatw_fail_alloc;
static void test_close_with_heap_exhausted(void)
{
	char buf[16];
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	CHECK(fatw_create("0:/h.txt") >= 0, "create failed");
	long fd = fatw_open("0:/h.txt", FA_WRITE);
	CHECK(fd > 0, "open failed");
	CHECK(fatw_write(fd, "heapfull", 8) == 8, "write failed");
	fatw_fail_alloc = 1;
	int r = fatw_close(fd);
	fatw_fail_alloc = 0;
	CHECK(r == 0, "close returned %d with the heap exhausted", r);
	/* Remount: what the directory entry holds, not the cache */
	CHECK(fatw_unmount(0) == 0 && fatw_mount(0) >= 0, "remount failed");
	get_file("0:/h.txt", buf, sizeof(buf));
	CHECK(strcmp(buf, "heapfull") == 0, "file holds '%s' after the close", buf);
}

/* Reads of the card start failing after n reads: a directory call either gives
   the whole answer or an error, never a short list reported as success. */
#define NFILES 40
static void fail_reads_after(int n)
{
	ramdisk[0].fail_reads = 1;
	ramdisk[0].read_budget = n;
}

static void reads_ok(void)
{
	ramdisk[0].fail_reads = 0;
}

static void test_readdir_read_error(void)
{
	static char out[NFILES * 40];
	int errors = 0;
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	make_dir_of_files("0:/rd", NFILES);
	CHECK_OK();
	for (int n = 0; n < 12; n++) {
		unsigned int cnt = 0;
		fail_reads_after(n);
		int r = fatw_readdir("0:/rd", out, sizeof(out), &cnt, 0);
		reads_ok();
		if (r != 0)
			errors++;
		else
			CHECK(cnt == NFILES, "budget %d: success with %u of %d entries", n, cnt, NFILES);
	}
	CHECK(errors > 0, "no read failure reached the listing (test is blind)");
}

static void test_deletedir_read_error(void)
{
	int errors = 0;
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	for (int n = 0; n < 12; n++) {
		char dir[24];
		unsigned int cnt = 0;
		snprintf(dir, sizeof(dir), "0:/dd%d", n);
		make_dir_of_files(dir, NFILES);
		CHECK_OK();
		fail_reads_after(n);
		int r = fatw_deletedir(dir);
		reads_ok();
		CHECK(fatw_readdir(dir, NULL, 0, &cnt, 0) == 0, "listing after delete failed");
		if (r != 0)
			errors++;
		else
			CHECK(cnt == 0, "budget %d: DELETEDIR succeeded with %u entries left", n, cnt);
	}
	CHECK(errors > 0, "no read failure reached DELETEDIR (test is blind)");
}

static void test_getusage_read_error(void)
{
	int errors = 0;
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	make_dir_of_files("0:/gu", NFILES);
	CHECK_OK();
	for (int n = 0; n < 12; n++) {
		unsigned long long size = 0;
		unsigned int files = 0, dirs = 0;
		fail_reads_after(n);
		int r = fatw_usage("0:/gu", &size, &files, &dirs);
		reads_ok();
		if (r != 0)
			errors++;
		else
			CHECK(files == NFILES, "budget %d: success with %u of %d files", n, files, NFILES);
	}
	CHECK(errors > 0, "no read failure reached GETUSAGE (test is blind)");
}

/*
 * Cluster runs (docs/sd-speed.md): f_read and f_write hand the disk one request
 * for a whole run of contiguous clusters, not one per cluster. Each SD request
 * costs about 1.3 ms whatever its size, so the number of requests is the speed.
 */
int fatw_raw_seek(unsigned int fd, unsigned long long pos);

static unsigned char pat(unsigned int salt, unsigned long long i)
{
	return (unsigned char) ((i * 131 + (i >> 9) * 7 + salt) ^ (i >> 16));
}

static void fill_pat(unsigned char *p, unsigned int salt, unsigned long long from, unsigned int n)
{
	for (unsigned int i = 0; i < n; i++)
		p[i] = pat(salt, from + i);
}

static int same_pat(const unsigned char *p, unsigned int salt, unsigned long long from, unsigned int n)
{
	for (unsigned int i = 0; i < n; i++)
		if (p[i] != pat(salt, from + i))
			return 0;
	return 1;
}

/* A new file of len bytes of pattern salt, written in chunk-byte calls */
static void make_pat_file(const char *path, unsigned int salt, unsigned int len, unsigned int chunk)
{
	static unsigned char buf[1 << 20];
	CHECK(fatw_create(path) >= 0, "create %s failed", path);
	long fd = fatw_open(path, FA_WRITE);
	CHECK(fd > 0, "open %s: %ld", path, fd);
	for (unsigned int at = 0; at < len; at += chunk) {
		unsigned int n = len - at < chunk ? len - at : chunk;
		fill_pat(buf, salt, at, n);
		CHECK(fatw_write(fd, buf, n) == (int) n, "write %s at %u failed", path, at);
	}
	CHECK(fatw_close(fd) >= 0, "close %s failed", path);
}

/* A file of 768 KB is read back with one call: the disk sees a few requests, not one per cluster */
static void test_run_read_one_request(void)
{
	static unsigned char buf[768 * 1024];
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	make_pat_file("0:/run.bin", 3, sizeof(buf), sizeof(buf));
	CHECK_OK();
	long fd = fatw_open("0:/run.bin", FA_READ);
	CHECK(fd > 0, "open failed");
	ramdisk[0].reads = 0;
	CHECK(fatw_read(fd, buf, sizeof(buf)) == (int) sizeof(buf), "short read");
	unsigned int reads = ramdisk[0].reads;
	CHECK(fatw_close(fd) >= 0, "close failed");
	CHECK(same_pat(buf, 3, 0, sizeof(buf)), "data differs");
	/* the data in one request, and the FAT sectors the chain walk loads; one per cluster is 190 or more */
	CHECK(reads <= 8, "%u disk reads for 768 KB (one per cluster)", reads);
}

/* The same for writing a new file in one call. FAT volumes only: on exFAT a write goes one
   cluster at a time, because the file's size, which its chain lookups depend on, grows with each. */
static void test_run_write_few_requests(void)
{
	static unsigned char buf[768 * 1024];
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	CHECK(fatw_create("0:/w.bin") >= 0, "create failed");
	long fd = fatw_open("0:/w.bin", FA_WRITE);
	CHECK(fd > 0, "open failed");
	fill_pat(buf, 5, 0, sizeof(buf));
	ramdisk[0].writes = 0;
	CHECK(fatw_write(fd, buf, sizeof(buf)) == (int) sizeof(buf), "short write");
	unsigned int writes = ramdisk[0].writes;
	CHECK(fatw_close(fd) >= 0, "close failed");
	/* the data in a few requests (the 255-sector cap of diskio.c is not in this stub), the tables around them;
	   one per cluster would be 190 or more */
	CHECK(writes <= 12, "%u disk writes for 768 KB", writes);
	CHECK(fatw_unmount(0) == 0 && fatw_mount(0) >= 0, "remount failed");
	fd = fatw_open("0:/w.bin", FA_READ);
	CHECK(fd > 0, "reopen failed");
	static unsigned char back[sizeof(buf)];
	CHECK(fatw_read(fd, back, sizeof(back)) == (int) sizeof(back), "short read back");
	fatw_close(fd);
	CHECK(!memcmp(back, buf, sizeof(buf)), "data differs after the write");
}

/* Two files grown in turns are in pieces. Reads of any offset and length across the
   pieces give the right bytes. */
static void test_run_fragmented_reads(void)
{
	static unsigned char buf[300 * 1024], copy[300 * 1024];
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	CHECK(fatw_create("0:/fa.bin") >= 0 && fatw_create("0:/fb.bin") >= 0, "create failed");
	long fa = fatw_open("0:/fa.bin", FA_WRITE), fb = fatw_open("0:/fb.bin", FA_WRITE);
	CHECK(fa > 0 && fb > 0, "open failed");
	for (unsigned int at = 0; at < sizeof(buf); at += 24 * 1000) {
		unsigned int n = sizeof(buf) - at < 24 * 1000 ? sizeof(buf) - at : 24 * 1000;
		fill_pat(copy, 1, at, n);
		CHECK(fatw_write(fa, copy, n) == (int) n, "write a failed");
		fill_pat(copy, 2, at, n);
		CHECK(fatw_write(fb, copy, n) == (int) n, "write b failed");
	}
	CHECK(fatw_close(fa) >= 0 && fatw_close(fb) >= 0, "close failed");
	for (int f = 0; f < 2; f++) {
		long fd = fatw_open(f ? "0:/fb.bin" : "0:/fa.bin", FA_READ);
		CHECK(fd > 0, "reopen failed");
		unsigned int rnd = 12345 + f;
		for (int k = 0; k < 60; k++) {
			rnd = rnd * 1103515245u + 12345u;
			unsigned int off = (rnd >> 8) % sizeof(buf);
			rnd = rnd * 1103515245u + 12345u;
			unsigned int len = 1 + (rnd >> 8) % (sizeof(buf) - off);
			if (k == 0) { off = 0; len = sizeof(buf); }
			memset(copy, 0xEE, sizeof(copy));
			CHECK(fatw_raw_seek(fd, off) == 0, "seek %u failed", off);
			CHECK(fatw_read(fd, copy, len) == (int) len, "short read at %u len %u", off, len);
			CHECK(same_pat(copy, f + 1, off, len), "file %d differs at %u len %u", f, off, len);
			CHECK(len == sizeof(copy) || copy[len] == 0xEE, "read wrote past the end");
		}
		fatw_close(fd);
	}
}

/* A write that runs into the end of the disk keeps what it wrote: the file reads back as long as the count says */
static void test_run_write_disk_full(void)
{
	static unsigned char buf[1 << 20], back[1 << 20];
	unsigned long long total = 0;
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	CHECK(fatw_create("0:/full.bin") >= 0, "create failed");
	long fd = fatw_open("0:/full.bin", FA_WRITE);
	CHECK(fd > 0, "open failed");
	for (int i = 0; i < 12; i++) {                      /* 8 MB disk: it fills in the 8th */
		fill_pat(buf, 9, total, sizeof(buf));
		int n = fatw_write(fd, buf, sizeof(buf));
		if (n < 0)
			break;
		total += n;
		if (n < (int) sizeof(buf))
			break;
	}
	CHECK(total > 4u << 20 && total < 8u << 20, "wrote %llu bytes", total);
	CHECK(fatw_close(fd) >= 0, "close failed");
	CHECK(fatw_unmount(0) == 0 && fatw_mount(0) >= 0, "remount failed");
	fd = fatw_open("0:/full.bin", FA_READ);
	CHECK(fd > 0, "reopen failed");
	unsigned long long got = 0;
	for (;;) {
		int n = fatw_read(fd, back, sizeof(back));
		CHECK(n >= 0, "read failed");
		if (n == 0)
			break;
		CHECK(same_pat(back, 9, got, n), "data differs at %llu", got);
		got += n;
	}
	fatw_close(fd);
	CHECK(got == total, "file holds %llu bytes, the writes said %llu", got, total);
}

/*
 * Every test above runs twice: on FAT (the RAM disks setup() formats) and on
 * exFAT (the same disks formatted again). The cases that are only exFAT's are
 * in test_exfat.c.
 */
#define GENERIC_TESTS(X) \
	X(one_volume) X(sd_and_usb_together) X(readdir_small_buffer) X(readdir_returns_names) \
	X(readdir_count_only_frees) X(deletedir_long_path) X(getusage_long_path) \
	X(close_failure_frees) X(deletedir_deep_tree_refused) X(deletedir_tree_within_cap) \
	X(getusage_deep_tree_refused) X(getusage_tree_within_cap) X(ioctlv_readdir_vector_count) \
	X(ioctlv_getusage_size_only) X(ioctlv_path_without_nul) X(ioctlv_getpartition_bad_device) \
	X(ioctl_filestats_on_module_fd) X(ioctl_setnandpath) X(close_with_heap_exhausted) \
	X(readdir_read_error) X(deletedir_read_error) X(getusage_read_error) \
	X(run_read_one_request) X(run_fragmented_reads) X(run_write_disk_full)

#define EXFAT_WRAPPER(name) \
	static void exfat_##name(void) \
	{ \
		if (ramdisk_format_ex(0, RD_EXFAT, 0) || ramdisk_format_ex(1, RD_EXFAT, 0)) \
			hc_hang("formatting the RAM disks as exFAT failed"); \
		test_##name(); \
	}
GENERIC_TESTS(EXFAT_WRAPPER)

#define AS_FAT(name) { #name, test_##name, false },
#define AS_EXFAT(name) { "exfat_" #name, exfat_##name, false },

static const struct hc_test tests[] = {
	GENERIC_TESTS(AS_FAT)
	GENERIC_TESTS(AS_EXFAT)
	{ "run_write_few_requests", test_run_write_few_requests, false },
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
	{ "ioctlv_readdir_vector_count", test_ioctlv_readdir_vector_count, false },
	{ "ioctlv_getusage_size_only", test_ioctlv_getusage_size_only, false },
	{ "ioctlv_path_without_nul", test_ioctlv_path_without_nul, false },
	{ "ioctlv_getpartition_bad_device", test_ioctlv_getpartition_bad_device, false },
	{ "ioctlv_mount_usb_port", test_ioctlv_mount_usb_port, false },
	{ "ioctl_filestats_on_module_fd", test_ioctl_filestats_on_module_fd, false },
	{ "ioctl_setnandpath", test_ioctl_setnandpath, false },
};

int main(int argc, char **argv)
{
	return hc_main(tests, HC_COUNT(tests), argc, argv, setup);
}
