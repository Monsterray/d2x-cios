/*
 * Host tests of the FAT module on exFAT: fat_wrapper.c, main.c's handlers and FatFs
 * R0.16 on RAM disks (ramdisk.c, sparse, so a disk can be several GB). test_fat.c
 * runs its generic cases on exFAT too; these are the ones that need exFAT, a big
 * file, a partition table or a damaged disk. Run with `make -C tests/host`.
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

/* FatFs's fs_type values */
#define T_FAT1216 (-2)          /* FatFs picks FAT12 or FAT16 by the volume's size: either */
#define T_FAT32  3
#define T_EXFAT  4

#define EFATAL   -101
#define EACCESS  -102
#define EEXIST   -105
#define ENOENT   -106

void fatw_reset(void);
int fatw_live_blocks(void);
int fatw_mount(int dev);
int fatw_mount_part(int dev, int part);
int fatw_unmount(int dev);
int fatw_getpartition(int dev, unsigned int *part);
int fatw_fstype(int dev);
int fatw_create(const char *path);
int fatw_mkdir(const char *path);
long fatw_open(const char *path, unsigned int mode);
int fatw_close(long fd);
int fatw_read(long fd, void *buf, unsigned int len);
int fatw_write(long fd, const void *buf, unsigned int len);
long long fatw_seek(long fd, int where, int whence);
int fatw_rename(const char *from, const char *to);
int fatw_delete(const char *path);
int fatw_deletedir(const char *path);
int fatw_stats(const char *path, unsigned int *size, unsigned int *date, unsigned int *time,
	       unsigned int *attrib);
int fatw_filestats(long fd, unsigned int *length, unsigned int *pos);
int fatw_readdir(const char *path, char *out, unsigned int buflen, unsigned int *outlen,
		 unsigned int entries);
int fatw_readdir_fs(const char *path, char *out, unsigned int buflen, unsigned int *outlen);
int fatw_usage(const char *path, unsigned long long *size, unsigned int *files, unsigned int *dirs);
int fatw_usage_fs(const char *path, unsigned long long *size, unsigned int *files, unsigned int *dirs);
int fatw_ioctl(long fd, unsigned int cmd, void *in, unsigned int inlen, void *io, unsigned int iolen);
const char *fatw_nandpath(void);
void fatw_nand_path(char *dst, const char *nand, const char *name);
int fatw_corruptions(void);
int fatw_expand(unsigned int fd, unsigned long long bytes);
unsigned long long fatw_free_bytes(int drive, int forget);
int fatw_raw_seek(unsigned int fd, unsigned long long pos);
unsigned long long fatw_raw_size(unsigned int fd);

/* --- helpers ------------------------------------------------------------ */

static int same_type(int got, int want)
{
	return want == T_FAT1216 ? (got == 1 || got == 2) : got == want;
}

/* Two blank disks of this size, formatted as given (RD_*), a partition table on each */
static void fresh(unsigned int sectors, unsigned int fmt0, unsigned int fmt1)
{
	fatw_reset();
	ramdisk_reset(sectors);
	if (ramdisk_format_ex(0, fmt0, 0) || ramdisk_format_ex(1, fmt1, 0))
		hc_hang("formatting the RAM disks failed");
}

static void setup(void)
{
	fresh(16384, RD_EXFAT, RD_EXFAT);       /* 8 MB each */
}

static void put_file(const char *path, const void *data, unsigned int len)
{
	CHECK(fatw_create(path) >= 0, "create %s failed", path);
	long fd = fatw_open(path, FA_WRITE);
	CHECK(fd > 0, "open %s for writing: %ld", path, fd);
	CHECK(fatw_write(fd, data, len) == (int) len, "write %s failed", path);
	CHECK(fatw_close(fd) >= 0, "close %s failed", path);
}

static void put_text(const char *path, const char *text) { put_file(path, text, strlen(text)); }

static int file_has(const char *path, const char *text)
{
	char buf[64] = { 0 };
	long fd = fatw_open(path, FA_READ);
	if (fd <= 0)
		return 0;
	fatw_read(fd, buf, sizeof(buf) - 1);
	fatw_close(fd);
	return !strcmp(buf, text);
}

/* The byte at a position of a test file: a different stream for each seed */
static unsigned char pattern(unsigned int seed, unsigned int pos)
{
	return (unsigned char)((pos * 2654435761u >> 13) ^ (seed * 97u) ^ pos);
}

/* The names a READDIR returned, as one string "a,b,c" in the order returned */
static void names(const char *out, unsigned int n, char *dst, size_t dstlen)
{
	dst[0] = 0;
	for (unsigned int i = 0; i < n; i++) {
		if (i)
			strncat(dst, ",", dstlen - strlen(dst) - 1);
		strncat(dst, out, dstlen - strlen(dst) - 1);
		out += strlen(out) + 1;
	}
}

/* --- tests -------------------------------------------------------------- */

static void test_mounts_as_exfat(void)
{
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	CHECK(fatw_fstype(0) == T_EXFAT, "fs type %d, expected exFAT", fatw_fstype(0));
	unsigned int part = 9;
	CHECK(fatw_getpartition(0, &part) == 0 && part == 0, "GETPARTITION %u", part);
}

/* An SD card in FAT and a USB drive in exFAT, mounted together, each in its own FATFS */
static void test_fat_and_exfat_together(void)
{
	fresh(16384, RD_FAT, RD_EXFAT);
	CHECK(fatw_mount(0) >= 0 && fatw_mount(1) >= 0, "mount failed");
	CHECK(same_type(fatw_fstype(0), T_FAT1216) && fatw_fstype(1) == T_EXFAT, "types %d, %d", fatw_fstype(0),
	      fatw_fstype(1));
	put_text("0:/which.txt", "sd card (FAT)");
	CHECK_OK();
	put_text("1:/which.txt", "usb drive (exFAT)");
	CHECK_OK();
	CHECK(file_has("0:/which.txt", "sd card (FAT)"), "SD file wrong");
	CHECK(file_has("1:/which.txt", "usb drive (exFAT)"), "USB file wrong");
}

/*
 * Two files grown in turn cannot stay contiguous: exFAT then needs a FAT chain
 * where a contiguous file has none. Both read back whole, in odd chunk sizes.
 */
static void test_interleaved_files_roundtrip(void)
{
	enum { TOTAL = 200000 };
	static unsigned char buf[8192];
	unsigned int posa = 0, posb = 0;

	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	CHECK(fatw_create("0:/a.bin") >= 0 && fatw_create("0:/b.bin") >= 0, "create failed");
	long fa = fatw_open("0:/a.bin", FA_WRITE), fb = fatw_open("0:/b.bin", FA_WRITE);
	CHECK(fa > 0 && fb > 0, "open failed");
	while (posa < TOTAL || posb < TOTAL) {
		unsigned int n = posa < TOTAL ? (TOTAL - posa < 3001 ? TOTAL - posa : 3001) : 0;
		for (unsigned int i = 0; i < n; i++)
			buf[i] = pattern(1, posa + i);
		CHECK(n == 0 || fatw_write(fa, buf, n) == (int) n, "write a at %u failed", posa);
		posa += n;
		n = posb < TOTAL ? (TOTAL - posb < 5003 ? TOTAL - posb : 5003) : 0;
		for (unsigned int i = 0; i < n; i++)
			buf[i] = pattern(2, posb + i);
		CHECK(n == 0 || fatw_write(fb, buf, n) == (int) n, "write b at %u failed", posb);
		posb += n;
	}
	CHECK(fatw_close(fa) >= 0 && fatw_close(fb) >= 0, "close failed");

	for (int which = 1; which <= 2; which++) {
		long fd = fatw_open(which == 1 ? "0:/a.bin" : "0:/b.bin", FA_READ);
		unsigned int pos = 0;
		CHECK(fd > 0, "reopen failed");
		while (pos < TOTAL) {
			unsigned int want = 4099;
			int got = fatw_read(fd, buf, want);
			CHECK(got > 0, "read %d at %u returned %d", which, pos, got);
			for (int i = 0; i < got; i++)
				CHECK(buf[i] == pattern(which, pos + i), "file %d differs at %u", which, pos + i);
			pos += got;
		}
		CHECK(pos == TOTAL && fatw_read(fd, buf, 10) == 0, "file %d length wrong", which);
		fatw_close(fd);
	}
	CHECK(fatw_live_blocks() == 0, "%d FIL blocks leaked", fatw_live_blocks());
}

static void test_rename_delete(void)
{
	unsigned int size, date, time, attrib;
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	CHECK(fatw_mkdir("0:/dir") >= 0 && fatw_mkdir("0:/dir") == EEXIST, "mkdir / mkdir again");
	put_text("0:/dir/f.txt", "one");
	CHECK_OK();
	put_text("0:/dir/g.txt", "two");
	CHECK_OK();
	CHECK(fatw_rename("0:/dir/f.txt", "0:/dir/h.txt") == 0, "rename a file failed");
	CHECK(fatw_stats("0:/dir/f.txt", &size, &date, &time, &attrib) == ENOENT, "old name still there");
	CHECK(file_has("0:/dir/h.txt", "one"), "renamed file lost its data");
	CHECK(fatw_rename("0:/dir/h.txt", "0:/dir/g.txt") == EEXIST, "rename onto a file succeeded");
	CHECK(fatw_rename("0:/dir", "0:/folder") == 0, "rename a directory failed");
	CHECK(file_has("0:/folder/g.txt", "two"), "file in the renamed directory lost");
	CHECK(fatw_delete("0:/folder") == EACCESS, "DELETE of a non-empty directory succeeded");
	CHECK(fatw_delete("0:/folder/g.txt") == 0, "delete a file failed");
	CHECK(fatw_deletedir("0:/folder") == 0, "DELETEDIR failed");
	CHECK(fatw_stats("0:/folder", &size, &date, &time, &attrib) == 0 && attrib == 0x10,
	      "DELETEDIR removed the directory itself (it empties it)");
	CHECK(fatw_delete("0:/folder") == 0 && fatw_delete("0:/folder") == ENOENT, "the directory is still there");
}

/* The time stamp is get_fattime(): 1980-01-01 00:00:00 */
static void test_stats_and_timestamps(void)
{
	unsigned int size, date, time, attrib;
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	put_text("0:/s.txt", "12345");
	CHECK_OK();
	CHECK(fatw_mkdir("0:/d") >= 0, "mkdir failed");
	CHECK(fatw_stats("0:/s.txt", &size, &date, &time, &attrib) == 0, "stats of a file failed");
	CHECK(size == 5 && attrib == 0x20 && date == 0x0021 && time == 0,
	      "file: size %u attrib %02x date %04x time %04x", size, attrib, date, time);
	CHECK(fatw_stats("0:/d", &size, &date, &time, &attrib) == 0, "stats of a directory failed");
	CHECK(size == 0 && attrib == 0x10 && date == 0x0021 && time == 0,
	      "directory: size %u attrib %02x date %04x time %04x", size, attrib, date, time);
	CHECK(fatw_stats("0:/nothing", &size, &date, &time, &attrib) == ENOENT, "missing file not ENOENT");
}

/* FAT_Seek: positions, errors, and the file position left alone after an error */
static void test_seek(void)
{
	unsigned int len, pos;
	char data[100];
	for (int i = 0; i < 100; i++)
		data[i] = (char)i;
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	put_file("0:/seek.bin", data, 100);
	CHECK_OK();
	long fd = fatw_open("0:/seek.bin", FA_READ);
	CHECK(fd > 0, "open failed");
	CHECK(fatw_seek(fd, 10, 0) == 10, "SEEK_SET 10");
	CHECK(fatw_seek(fd, 5, 1) == 15, "SEEK_CUR +5");
	CHECK(fatw_seek(fd, -10, 2) == 90, "SEEK_END -10");
	CHECK(fatw_seek(fd, 100, 0) == 100, "SEEK_SET to the end");
	CHECK(fatw_seek(fd, 101, 0) == EFATAL, "SEEK_SET past the end");
	CHECK(fatw_seek(fd, -1, 0) == EFATAL, "SEEK_SET -1");
	CHECK(fatw_seek(fd, 7, 3) == EFATAL, "whence 3");
	CHECK(fatw_filestats(fd, &len, &pos) == 0 && len == 100 && pos == 100, "length %u, position %u", len, pos);
	CHECK(fatw_seek(fd, 50, 0) == 50, "SEEK_SET 50");
	CHECK(fatw_seek(fd, 60, 1) == EFATAL, "SEEK_CUR past the end");
	CHECK(fatw_filestats(fd, &len, &pos) == 0 && pos == 50, "position %u after a refused seek", pos);
	char c = 0;
	CHECK(fatw_read(fd, &c, 1) == 1 && c == 50, "read after the seeks: %d", c);
	fatw_close(fd);
}

/*
 * A file of more than 4 GB, on a 6 GB exFAT volume (RAM disk of a few MB: only
 * the sectors written are kept). The IPC structures hold 32 bits, so sizes and
 * positions saturate at 0xFFFFFFFF; GETUSAGE adds up the real sizes in 64 bits;
 * a seek that would have to answer with a position of 2 GB or more fails.
 */
static void test_file_over_4gb(void)
{
	const unsigned long long size = 5ULL * 1024 * 1024 * 1024 + 1000;
	const unsigned long long tail = 4ULL * 1024 * 1024 * 1024 + 16;
	unsigned int len, pos, st_size, date, time, attrib;
	unsigned long long usage = 0;
	unsigned int files = 0, dirs = 0;
	char buf[8] = { 0 };

	fresh(12582912, RD_EXFAT, RD_EXFAT);            /* 6 GB */
	CHECK(fatw_mount(0) >= 0 && fatw_fstype(0) == T_EXFAT, "mount of the 6 GB volume failed");
	CHECK(fatw_create("0:/big.bin") >= 0, "create failed");
	unsigned long long free0 = fatw_free_bytes(0, 1);
	long fd = fatw_open("0:/big.bin", FA_WRITE);
	CHECK(fd > 0, "open failed");
	CHECK(fatw_expand((unsigned int) fd, size) == 0, "making a 5 GB file failed");
	CHECK(fatw_raw_size((unsigned int) fd) == size, "size %llu", fatw_raw_size((unsigned int) fd));

	/* a marker past 4 GB, written through the module's FAT_Write */
	CHECK(fatw_raw_seek((unsigned int) fd, tail) == 0, "seek to 4 GB + 16");
	CHECK(fatw_write(fd, "TAIL", 4) == 4, "write past 4 GB failed");
	CHECK(fatw_filestats(fd, &len, &pos) == 0 && len == 0xFFFFFFFF && pos == 0xFFFFFFFF,
	      "FILESTATS: length %08x, position %08x", len, pos);

	/* FAT_Seek: nothing at 2 GB or more, and the position stays */
	CHECK(fatw_seek(fd, 0, 0) == 0, "SEEK_SET 0");
	CHECK(fatw_seek(fd, 0x7FFFFFFF, 0) == 0x7FFFFFFF, "SEEK_SET 0x7FFFFFFF");
	CHECK(fatw_seek(fd, 1, 1) == EFATAL, "SEEK_CUR to 0x80000000");
	CHECK(fatw_filestats(fd, &len, &pos) == 0 && pos == 0x7FFFFFFF, "position %08x after a refused seek", pos);
	CHECK(fatw_seek(fd, 0, 2) == EFATAL, "SEEK_END of a 5 GB file");
	CHECK(fatw_seek(fd, -0x7FFFFFFF, 2) == EFATAL, "SEEK_END -0x7FFFFFFF of a 5 GB file");
	CHECK(fatw_seek(fd, 0, 0) == 0, "SEEK_SET 0 after the refusals");
	CHECK(fatw_close(fd) >= 0, "close failed");

	/* what the other calls say about it */
	CHECK(fatw_stats("0:/big.bin", &st_size, &date, &time, &attrib) == 0 && st_size == 0xFFFFFFFF
	      && attrib == 0x20, "stats: size %08x attrib %02x", st_size, attrib);
	CHECK(fatw_usage("0:/", &usage, &files, &dirs) == 0 && usage == size && files == 1 && dirs == 0,
	      "usage: %llu bytes, %u files, %u dirs", usage, files, dirs);
	usage = 0;
	files = dirs = 0;
	CHECK(fatw_usage_fs("0:/", &usage, &files, &dirs) == 0 && usage == size, "usage for FS: %llu", usage);

	/* the marker is there, and the data before it reads as the (empty) disk */
	fd = fatw_open("0:/big.bin", FA_READ);
	CHECK(fd > 0, "reopen failed");
	CHECK(fatw_raw_seek((unsigned int) fd, tail) == 0 && fatw_read(fd, buf, 4) == 4 && !memcmp(buf, "TAIL", 4),
	      "the marker past 4 GB reads back \"%s\"", buf);
	CHECK(fatw_raw_seek((unsigned int) fd, size - 4) == 0 && fatw_read(fd, buf, 8) == 4,
	      "the read at the end got a wrong length");
	CHECK(fatw_read(fd, buf, 8) == 0, "a read at the end returned data");
	fatw_close(fd);

	/* the disk is mostly holes, and deleting the file frees the clusters for another */
	CHECK(ramdisk_chunks_used(0) < 4096, "%u chunks of the RAM disk in use", ramdisk_chunks_used(0));
	CHECK(fatw_delete("0:/big.bin") == 0, "delete failed");
	/*
	 * Free space after the delete. The module only allocates cluster by cluster (FAT_Write), so the
	 * question is whether the space comes back, by the cached count and by a scan of the bitmap, and
	 * whether the next big file can be written normally. (f_expand, which the module does not call,
	 * can refuse a second file of the same size: its search for one contiguous block starts at the
	 * end of the last allocation and does not join the run that contains its start. docs/fatfs.md.)
	 */
	CHECK(fatw_free_bytes(0, 0) == free0 && fatw_free_bytes(0, 1) == free0,
	      "free space after the delete: %llu cached, %llu scanned, %llu before",
	      fatw_free_bytes(0, 0), fatw_free_bytes(0, 1), free0);
	CHECK(fatw_create("0:/big2.bin") >= 0, "create the second file failed");
	fd = fatw_open("0:/big2.bin", FA_WRITE);
	CHECK(fd > 0, "open of the second file failed");
	CHECK(fatw_raw_seek((unsigned int) fd, size) == 0 && fatw_raw_size((unsigned int) fd) == size,
	      "a second 5 GB file does not fit after the first was deleted");
	CHECK(fatw_write(fd, "TWO", 3) == 3, "write at the end of the second file failed");
	fatw_close(fd);
	CHECK(fatw_free_bytes(0, 0) < free0 - size && fatw_free_bytes(0, 1) == fatw_free_bytes(0, 0),
	      "free space with the second file: %llu cached, %llu scanned", fatw_free_bytes(0, 0), fatw_free_bytes(0, 1));
	CHECK(fatw_delete("0:/big2.bin") == 0, "delete of the second file failed");
	CHECK(fatw_free_bytes(0, 0) == free0 && fatw_free_bytes(0, 1) == free0, "free space not back after the second delete");

	/* f_expand of 1 GB still works after all that */
	CHECK(fatw_create("0:/big3.bin") >= 0, "create the third file failed");
	fd = fatw_open("0:/big3.bin", FA_WRITE);
	int r = fd > 0 ? fatw_expand((unsigned int) fd, 1024ULL * 1024 * 1024) : -1;
	CHECK(r == 0, "a 1 GB file does not fit after the 5 GB ones were deleted (%d)", r);
	fatw_close(fd);
	CHECK(fatw_live_blocks() == 0, "%d FIL blocks leaked", fatw_live_blocks());
}

static void test_long_names(void)
{
	char name255[300], name256[304], path[400], out[1024], list[1024];
	unsigned int n = 0;

	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	memset(name255, 'n', 251);
	strcpy(name255 + 251, ".txt");                      /* 255 characters */
	snprintf(path, sizeof(path), "0:/%s", name255);
	put_text(path, "long");
	CHECK_OK();
	snprintf(name256, sizeof(name256), "%sx", name255);
	snprintf(path, sizeof(path), "0:/%s", name256);
	CHECK(fatw_create(path) < 0, "a 256-character name was created");

	put_text("0:/A name with spaces.and.dots.txt", "spaces");
	CHECK_OK();
	put_text("0:/Hello.TXT", "case");
	CHECK_OK();
	CHECK(file_has("0:/hello.txt", "case"), "names are not case-insensitive");

	CHECK(fatw_readdir("0:/", out, sizeof(out), &n, 0) == 0 && n == 3, "readdir: %u entries", n);
	names(out, n, list, sizeof(list));
	CHECK(strstr(list, name255) && strstr(list, "A name with spaces.and.dots.txt") && strstr(list, "Hello.TXT"),
	      "the names are not as created: %.100s", list);

	/* FS-side listing (forFS) skips names over 12 characters, as IOS's own NAND does */
	CHECK(fatw_readdir_fs("0:/", out, sizeof(out), &n) == 0 && n == 1, "readdir for FS: %u entries", n);
	CHECK(!strcmp(out, "Hello.TXT"), "readdir for FS: \"%.100s\"", out);

	snprintf(path, sizeof(path), "0:/%s", name255);
	CHECK(fatw_delete(path) == 0, "delete of the long name failed");
}

/* CP437 letters outside ASCII: stored as Unicode, found without regard to case (exFAT's up-case table) */
static void test_oem_names(void)
{
	char out[256], list[256];
	unsigned int n = 0;
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	put_text("0:/\x84-\x80.txt", "umlaut");          /* a-umlaut, C-cedilla in CP437 */
	CHECK_OK();
	CHECK(file_has("0:/\x8E-\x80.txt", "umlaut"), "A-umlaut does not find a-umlaut");
	CHECK(fatw_readdir("0:/", out, sizeof(out), &n, 0) == 0 && n == 1, "readdir failed");
	names(out, n, list, sizeof(list));
	CHECK(!strcmp(list, "\x84-\x80.txt"), "the name came back as \"%s\"", list);
}

/* Find the directory entry of an object by a test on its 32 bytes (the first match on the disk) */
static unsigned char *find_entry(int drive, int (*match)(const unsigned char *e), unsigned char *sector,
				 unsigned int *sector_no)
{
	for (unsigned int s = 0; s < 16384; s++) {
		ramdisk_read_raw(drive, s, sector);
		for (int off = 0; off < RAMDISK_SECTOR; off += 32)
			if (match(sector + off)) {
				*sector_no = s;
				return sector + off;
			}
	}
	return NULL;
}

static int is_ab_name_entry(const unsigned char *e)     /* exFAT file name entry holding "ab" */
{
	return e[0] == 0xC1 && e[2] == 'a' && e[3] == 0 && e[4] == 'b' && e[5] == 0 && e[6] == 0;
}

static int is_mixed_lfn(const unsigned char *e)         /* FAT long name entry starting "Mi" */
{
	return e[0] == 0x41 && e[11] == 0x0F && e[1] == 'M' && e[3] == 'i';
}

/*
 * A name with a character that is not in code page 437 (here HIRAGANA A) cannot
 * be given to a caller that gets 8-bit characters. FatFs lists an exFAT object
 * as "?" (the whole name); a FAT object by its short name.
 */
static void test_name_outside_the_code_page(void)
{
	unsigned char sector[RAMDISK_SECTOR];
	unsigned int sno, n = 0;
	char out[64], list[64];
	unsigned char *e;

	/* exFAT */
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	put_text("0:/ab", "x");
	CHECK_OK();
	CHECK(fatw_unmount(0) == 0, "unmount failed");
	e = find_entry(0, is_ab_name_entry, sector, &sno);
	CHECK(e != NULL, "the file's name entry is not on the disk");
	CHECK(e[-64] == 0x85 && e[-63] == 2, "the entry set is not where expected");
	e[2] = 0x42;                                    /* U+3042 in place of "a" */
	e[3] = 0x30;
	{       /* the entry set's checksum, over its 3 entries, skipping bytes 2 and 3 of the first */
		unsigned int sum = 0;
		for (int i = 0; i < 96; i++) {
			if (i == 2 || i == 3)
				continue;
			sum = ((sum & 1) ? 0x8000 : 0) + (sum >> 1) + (e - 64)[i];
			sum &= 0xFFFF;
		}
		(e - 64)[2] = (unsigned char) sum;
		(e - 64)[3] = (unsigned char)(sum >> 8);
	}
	ramdisk_write_raw(0, sno, sector);
	CHECK(fatw_mount(0) >= 0, "mount after the edit failed");
	CHECK(fatw_readdir("0:/", out, sizeof(out), &n, 0) == 0 && n == 1, "exFAT: readdir returned %u entries", n);
	names(out, n, list, sizeof(list));
	CHECK(!strcmp(list, "?"), "exFAT: the name is \"%s\"", list);

	/* FAT */
	fresh(16384, RD_FAT, RD_FAT);
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	put_text("0:/Mixed.txt", "x");
	CHECK_OK();
	CHECK(fatw_unmount(0) == 0, "unmount failed");
	e = find_entry(0, is_mixed_lfn, sector, &sno);
	CHECK(e != NULL, "the file's long name entry is not on the disk");
	e[1] = 0x42;
	e[2] = 0x30;
	ramdisk_write_raw(0, sno, sector);
	CHECK(fatw_mount(0) >= 0, "mount after the edit failed");
	CHECK(fatw_readdir("0:/", out, sizeof(out), &n, 0) == 0 && n == 1, "FAT: readdir returned %u entries", n);
	names(out, n, list, sizeof(list));
	CHECK(!strcmp(list, "MIXED.TXT"), "FAT: the name is \"%s\"", list);
}

/* READDIR of a file is an error (the games that look for a folder), exFAT as FAT */
static void test_readdir_of_a_file(void)
{
	unsigned int n = 0;
	char out[64];
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	put_text("0:/file.txt", "x");
	CHECK_OK();
	CHECK(fatw_readdir("0:/file.txt", out, sizeof(out), &n, 0) == EFATAL, "READDIR of a file");
	CHECK(fatw_readdir("0:/nothing", out, sizeof(out), &n, 0) == ENOENT, "READDIR of a missing folder");
}

/* The save files of an emulated NAND: IOS names with characters FAT does not allow, escaped */
static void nand_test(unsigned int fmt, int dev)
{
	char nand[16], dir[200], path[300], out[512], list[512];
	unsigned int n = 0;
	unsigned long long usage = 0;
	unsigned int files = 0, dirs = 0;
	static const char *folders[] = { "title", "title/00010000", "title/00010000/48414141",
					 "title/00010000/48414141/data" };

	fresh(16384, fmt, fmt);
	CHECK(fatw_mount(dev) >= 0, "mount failed");
	snprintf(nand, sizeof(nand), "%d:/nand", dev);
	CHECK(fatw_ioctl(0, IOCTL_FAT_SETNANDPATH, nand, strlen(nand) + 1, NULL, 0) == 0, "SETNANDPATH failed");
	CHECK(fatw_mkdir(nand) >= 0, "mkdir of the NAND folder failed");
	for (unsigned int i = 0; i < 4; i++) {
		fatw_nand_path(path, fatw_nandpath(), folders[i]);
		CHECK(fatw_mkdir(path) >= 0, "mkdir %s failed", path);
	}
	snprintf(dir, sizeof(dir), "%s%s", fatw_nandpath(), folders[3]);

	static const char *saves[] = { "a:b*c?.bin", "x<y>z|q\"r.dat", "plain.sav" };
	for (unsigned int i = 0; i < 3; i++) {
		char ios[100];
		snprintf(ios, sizeof(ios), "%s/%s", folders[3], saves[i]);
		fatw_nand_path(path, fatw_nandpath(), ios);
		CHECK(strchr(path + 3, ':') == NULL && strchr(path, '*') == NULL, "the path is not escaped: %s", path);
		put_text(path, saves[i]);
		CHECK_OK();
		CHECK(file_has(path, saves[i]), "%s does not read back", path);
	}

	/* on the volume the escaped names; for FS the real ones, those of 12 characters or less */
	CHECK(fatw_readdir(dir, out, sizeof(out), &n, 0) == 0 && n == 3, "readdir: %u entries", n);
	names(out, n, list, sizeof(list));
	CHECK(strstr(list, "a&cl;b&st;c&qm;.bin") && strstr(list, "plain.sav"), "escaped names: %.200s", list);
	CHECK(fatw_readdir_fs(dir, out, sizeof(out), &n) == 0 && n == 2, "readdir for FS: %u entries", n);
	names(out, n, list, sizeof(list));
	CHECK(strstr(list, "a:b*c?.bin") && strstr(list, "plain.sav"), "names for FS: %.200s", list);
	CHECK(fatw_usage_fs(dir, &usage, &files, &dirs) == 0 && files == 2 && dirs == 0, "usage for FS: %u files", files);
	CHECK(fatw_deletedir(nand) == 0, "DELETEDIR of the NAND failed");
	CHECK(fatw_readdir(nand, out, sizeof(out), &n, 0) == 0 && n == 0, "the NAND folder is not empty");
}

static void test_nand_escaped_paths_exfat(void) { nand_test(RD_EXFAT, 0); }
static void test_nand_escaped_paths_exfat_usb(void) { nand_test(RD_EXFAT, 1); }
static void test_nand_escaped_paths_fat(void) { nand_test(RD_FAT, 0); }

static void test_usage(void)
{
	unsigned long long size = 0;
	unsigned int files = 0, dirs = 0;
	CHECK(fatw_mount(0) >= 0, "mount SD failed");
	CHECK(fatw_mkdir("0:/a") >= 0 && fatw_mkdir("0:/a/b") >= 0 && fatw_mkdir("0:/c") >= 0, "mkdir failed");
	put_text("0:/top.txt", "12345");
	put_text("0:/a/one.txt", "1234567890");
	put_text("0:/a/b/two.txt", "x");
	put_text("0:/c/three.txt", "123");
	CHECK_OK();
	CHECK(fatw_usage("0:/", &size, &files, &dirs) == 0, "usage failed");
	CHECK(size == 19 && files == 4 && dirs == 3, "%llu bytes, %u files, %u dirs", size, files, dirs);
}

/* A directory that grows over several clusters */
static void test_many_entries(void)
{
	enum { N = 300 };
	static char out[N * 8];
	char path[32];
	unsigned int n = 0;
	CHECK(fatw_mount(0) >= 0 && fatw_mkdir("0:/many") >= 0, "mount or mkdir failed");
	for (int i = 0; i < N; i++) {
		snprintf(path, sizeof(path), "0:/many/f%03d", i);
		CHECK(fatw_create(path) >= 0, "create %s failed", path);
	}
	CHECK(fatw_readdir("0:/many", out, sizeof(out), &n, 0) == 0 && n == N, "readdir: %u entries", n);
	CHECK(fatw_deletedir("0:/many") == 0, "DELETEDIR failed");
	CHECK(fatw_readdir("0:/many", out, sizeof(out), &n, 0) == 0 && n == 0, "entries left: %u", n);
}

/* Four partitions: FAT, exFAT, exFAT, none. Index 0 to 3 as the IPC calls count them. */
static void partitioned_disk(void)
{
	fatw_reset();
	ramdisk_reset(16384);
	ramdisk_set_partition(0, 1, 64, 4096);
	ramdisk_set_partition(0, 2, 4160, 6144);
	ramdisk_set_partition(0, 3, 10304, 6000);
	if (ramdisk_format_ex(0, RD_FAT, 1) || ramdisk_format_ex(0, RD_EXFAT, 2) || ramdisk_format_ex(0, RD_EXFAT, 3))
		hc_hang("formatting the partitions failed");
	if (ramdisk_format_ex(1, RD_EXFAT, 0))
		hc_hang("formatting USB failed");
	CHECK_OK();
}

static void test_forced_partitions(void)
{
	unsigned int part = 9;
	partitioned_disk();
	CHECK_OK();
	static const char *names_[3] = { "0:/p1.txt", "0:/p2.txt", "0:/p3.txt" };
	static const int types[3] = { T_FAT1216, T_EXFAT, T_EXFAT };

	for (int p = 0; p < 3; p++) {
		CHECK(fatw_mount_part(0, p) == 0, "mount of partition %d failed", p);
		CHECK(same_type(fatw_fstype(0), types[p]), "partition %d: fs type %d", p, fatw_fstype(0));
		CHECK(fatw_getpartition(0, &part) == 0 && part == (unsigned int) p, "GETPARTITION %u, expected %d", part, p);
		put_text(names_[p], "mine");
		CHECK_OK();
		CHECK(fatw_unmount(0) == 0, "unmount failed");
	}
	for (int p = 0; p < 3; p++) {
		CHECK(fatw_mount_part(0, p) == 0, "second mount of partition %d failed", p);
		for (int q = 0; q < 3; q++) {
			unsigned int sz, d, t, a;
			CHECK((fatw_stats(names_[q], &sz, &d, &t, &a) == 0) == (p == q),
			      "partition %d shows the file of partition %d", p, q);
		}
		CHECK(fatw_unmount(0) == 0, "unmount failed");
	}
	CHECK(fatw_mount_part(0, 3) == EFATAL, "mount of the empty fourth partition did not fail");
	CHECK(fatw_getpartition(0, &part) < 0, "GETPARTITION after a failed mount");
	CHECK(fatw_mount(0) == 0 && fatw_getpartition(0, &part) == 0 && part == 0 && same_type(fatw_fstype(0), T_FAT1216),
	      "automatic mount did not take partition 0");
}

/* Partition 0 has no file system: the automatic mount takes the next one that has */
static void test_automatic_partition_search(void)
{
	unsigned char zero[RAMDISK_SECTOR] = { 0 };
	unsigned int part = 9;
	partitioned_disk();
	CHECK_OK();
	ramdisk_write_raw(0, 64, zero);                  /* the first VBR */
	CHECK(fatw_mount(0) == 0, "automatic mount failed");
	CHECK(fatw_getpartition(0, &part) == 0 && part == 1 && fatw_fstype(0) == T_EXFAT,
	      "took partition %u, type %d", part, fatw_fstype(0));
}

/* A drive formatted without a partition table: its VBR is sector 0, whatever partition is asked for */
static void superfloppy(unsigned int sectors, unsigned int fmt, int type)
{
	unsigned int part = 9;
	fatw_reset();
	ramdisk_reset(sectors);
	CHECK(ramdisk_format_ex(0, fmt | RD_SFD, 0) == 0 && ramdisk_format_ex(1, fmt | RD_SFD, 0) == 0, "format failed");
	CHECK(fatw_mount(0) == 0 && same_type(fatw_fstype(0), type), "automatic mount failed (type %d)", fatw_fstype(0));
	CHECK(fatw_getpartition(0, &part) == 0 && part == 0, "GETPARTITION %u", part);
	put_text("0:/sfd.txt", "super");
	CHECK_OK();
	CHECK(fatw_unmount(0) == 0, "unmount failed");
	for (int p = 0; p < 4; p++) {
		CHECK(fatw_mount_part(0, p) == 0, "mount as partition %d failed", p);
		CHECK(fatw_getpartition(0, &part) == 0 && part == (unsigned int) p, "GETPARTITION %u, expected %d", part, p);
		CHECK(file_has("0:/sfd.txt", "super"), "the file is not there through partition %d", p);
		CHECK(fatw_unmount(0) == 0, "unmount failed");
	}
	CHECK(fatw_mount(1) == 0, "USB mount failed");
}

static void test_superfloppy_exfat(void) { superfloppy(16384, RD_EXFAT, T_EXFAT); }
static void test_superfloppy_fat(void) { superfloppy(16384, RD_FAT, T_FAT1216); }
static void test_superfloppy_fat32(void) { superfloppy(262144, RD_FAT32, T_FAT32); }

/* UStealth drives sign sector 0 with 0xAB55: taken as 0xAA55, in a partition table and in a VBR */
static void test_signature_ab55(void)
{
	unsigned char s[RAMDISK_SECTOR];
	unsigned int part = 9;

	/* partition table */
	fresh(16384, RD_EXFAT, RD_FAT);
	ramdisk_read_raw(0, 0, s);
	CHECK(s[510] == 0x55 && s[511] == 0xAA, "the MBR has signature %02x%02x", s[511], s[510]);
	s[511] = 0xAB;
	ramdisk_write_raw(0, 0, s);
	CHECK(fatw_mount(0) == 0 && fatw_fstype(0) == T_EXFAT, "mount with 0xAB55 in the MBR failed");
	CHECK(fatw_getpartition(0, &part) == 0 && part == 0, "GETPARTITION %u", part);
	fatw_unmount(0);

	/* an exFAT VBR in sector 0, and a FAT one */
	for (int d = 0; d < 2; d++) {
		fatw_reset();
		ramdisk_reset(16384);
		CHECK(ramdisk_format_ex(0, (d ? RD_FAT : RD_EXFAT) | RD_SFD, 0) == 0, "format failed");
		ramdisk_read_raw(0, 0, s);
		CHECK(s[510] == 0x55 && s[511] == 0xAA, "the VBR has signature %02x%02x", s[511], s[510]);
		s[511] = 0xAB;
		ramdisk_write_raw(0, 0, s);
		CHECK(fatw_mount(0) == 0, "mount of a %s VBR with 0xAB55 failed", d ? "FAT" : "exFAT");
		fatw_unmount(0);
		s[511] = 0x12;                          /* and not a signature of any kind */
		s[510] = 0x34;
		ramdisk_write_raw(0, 0, s);
		if (d)  /* FatFs R0.16 takes a FAT12/16 VBR without a signature (early MS-DOS volumes) */
			CHECK(fatw_mount(0) == 0, "mount of a FAT VBR with signature 0x1234 failed");
		else
			CHECK(fatw_mount(0) == EFATAL, "mount of an exFAT VBR with signature 0x1234 succeeded");
		fatw_unmount(0);
	}

	/* a partition table with no signature at all is not a drive */
	fresh(16384, RD_EXFAT, RD_FAT);
	ramdisk_read_raw(0, 0, s);
	s[510] = 0x34;
	s[511] = 0x12;
	ramdisk_write_raw(0, 0, s);
	CHECK(fatw_mount(0) == EFATAL, "mount with signature 0x1234 in the MBR succeeded");
}

/* FAT32 as before: a volume big enough for it (128 MB, cluster 1 KB or more), data over many clusters */
static void test_fat32_unchanged(void)
{
	static unsigned char buf[10000];
	enum { TOTAL = 1000000 };
	unsigned int part = 9, pos = 0;

	fresh(262144, RD_FAT32, RD_FAT32);
	CHECK(fatw_mount(0) == 0 && fatw_fstype(0) == T_FAT32, "mount failed (type %d)", fatw_fstype(0));
	CHECK(fatw_getpartition(0, &part) == 0 && part == 0, "GETPARTITION %u", part);
	CHECK(fatw_mkdir("0:/dir") >= 0 && fatw_create("0:/dir/big.bin") >= 0, "create failed");
	long fd = fatw_open("0:/dir/big.bin", FA_WRITE);
	CHECK(fd > 0, "open failed");
	while (pos < TOTAL) {
		for (unsigned int i = 0; i < sizeof(buf); i++)
			buf[i] = pattern(3, pos + i);
		CHECK(fatw_write(fd, buf, sizeof(buf)) == (int) sizeof(buf), "write at %u failed", pos);
		pos += sizeof(buf);
	}
	CHECK(fatw_close(fd) >= 0, "close failed");
	unsigned int size, date, time, attrib;
	CHECK(fatw_stats("0:/dir/big.bin", &size, &date, &time, &attrib) == 0 && size == pos && date == 0x0021,
	      "stats: size %u date %04x", size, date);
	fd = fatw_open("0:/dir/big.bin", FA_READ);
	CHECK(fd > 0 && fatw_seek(fd, 654321, 0) == 654321, "seek failed");
	CHECK(fatw_read(fd, buf, 100) == 100, "read failed");
	for (int i = 0; i < 100; i++)
		CHECK(buf[i] == pattern(3, 654321 + i), "data differs at %d", 654321 + i);
	CHECK(fatw_seek(fd, 0, 2) == (long long) pos, "SEEK_END");
	fatw_close(fd);
	CHECK(fatw_deletedir("0:/dir") == 0, "DELETEDIR failed");
}

static const struct hc_test tests[] = {
	{ "mounts_as_exfat", test_mounts_as_exfat, false },
	{ "fat_and_exfat_together", test_fat_and_exfat_together, false },
	{ "interleaved_files_roundtrip", test_interleaved_files_roundtrip, false },
	{ "rename_delete", test_rename_delete, false },
	{ "stats_and_timestamps", test_stats_and_timestamps, false },
	{ "seek", test_seek, false },
	{ "file_over_4gb", test_file_over_4gb, false },
	{ "long_names", test_long_names, false },
	{ "oem_names", test_oem_names, false },
	{ "name_outside_the_code_page", test_name_outside_the_code_page, false },
	{ "readdir_of_a_file", test_readdir_of_a_file, false },
	{ "nand_escaped_paths_exfat", test_nand_escaped_paths_exfat, false },
	{ "nand_escaped_paths_exfat_usb", test_nand_escaped_paths_exfat_usb, false },
	{ "nand_escaped_paths_fat", test_nand_escaped_paths_fat, false },
	{ "usage", test_usage, false },
	{ "many_entries", test_many_entries, false },
	{ "forced_partitions", test_forced_partitions, false },
	{ "automatic_partition_search", test_automatic_partition_search, false },
	{ "superfloppy_exfat", test_superfloppy_exfat, false },
	{ "superfloppy_fat", test_superfloppy_fat, false },
	{ "superfloppy_fat32", test_superfloppy_fat32, false },
	{ "signature_ab55", test_signature_ab55, false },
	{ "fat32_unchanged", test_fat32_unchanged, false },
};

int main(int argc, char **argv)
{
	return hc_main(tests, HC_COUNT(tests), argc, argv, setup);
}
