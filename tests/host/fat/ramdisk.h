#ifndef RAMDISK_H
#define RAMDISK_H

#define RAMDISK_SECTOR 512

struct ramdisk {
	unsigned char *data;
	unsigned int sectors;
	int inited;
	unsigned int reads, writes;
	int fail_writes;        /* nonzero: every write fails (a pulled card) */
};

extern struct ramdisk ramdisk[2];

void ramdisk_reset(unsigned int sectors);       /* two blank disks */
int ramdisk_format(int drive);                  /* ff_unit.c: f_mkfs */

#endif
