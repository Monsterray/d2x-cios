#ifndef RAMDISK_H
#define RAMDISK_H

#define RAMDISK_SECTOR 512

/* The storage is sparse: 64 KB chunks, allocated at the first write, read as
   zeros before that. So a disk of several GB costs the memory of what was
   written to it (a few MB), and a test can make an exFAT volume with a file of
   more than 4 GB. */
#define RAMDISK_CHUNK_SECTORS 128

struct ramdisk {
	unsigned char **chunks;
	unsigned int sectors;
	int inited;
	unsigned int reads, writes;
	int fail_writes;        /* nonzero: every write fails (a pulled card) */
};

extern struct ramdisk ramdisk[2];

void ramdisk_reset(unsigned int sectors);       /* two blank disks */
int ramdisk_format(int drive);                  /* ff_unit.c: f_mkfs, FAT12/16/32 */

/* ff_unit.c: format with f_mkfs. fmt is FM_FAT32, FM_EXFAT, ... (ff.h) with
   FM_SFD for a drive without a partition table; part 0 makes a new partition
   table (or none with FM_SFD), 1 to 4 formats that partition of the table
   already on the disk (see ramdisk_set_partition). */
int ramdisk_format_ex(int drive, unsigned int fmt, int part);

/* Put a partition (1 to 4) of the given start and size in the disk's MBR. */
void ramdisk_set_partition(int drive, int part, unsigned int start, unsigned int sectors);

/* Raw access to a sector, for a test that looks at or damages the disk. */
void ramdisk_read_raw(int drive, unsigned int sector, void *buf);
void ramdisk_write_raw(int drive, unsigned int sector, const void *buf);
unsigned int ramdisk_chunks_used(int drive);

#endif
