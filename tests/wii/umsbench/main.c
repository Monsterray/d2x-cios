/*
 * umsbench: time USB mass-storage sector reads and writes through a cIOS.
 *
 *     hbc.py run umsbench.dol [--ios=249] [--write-back [--writes=N]]
 *
 * Prints the cIOS in the slot (its TMD version), reloads to it, opens the
 * EHCI module's /dev/usb2, and times reads of 1 and 64 sectors in the middle
 * of the drive. With --write-back it also times writes that put back the
 * bytes it just read, then reads them again to check nothing changed: the
 * drive's data stays as it was. Output goes to the PC through hbc_netlog;
 * the last line starts with "RESULT" for scripts.
 */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hbc_netlog.h"

#define UMS_BASE                    (('U' << 24) | ('M' << 16) | ('S' << 8))
#define USB_IOCTL_UMS_INIT          (UMS_BASE + 0x1)
#define USB_IOCTL_UMS_GET_CAPACITY  (UMS_BASE + 0x2)
#define USB_IOCTL_UMS_READ_SECTORS  (UMS_BASE + 0x3)
#define USB_IOCTL_UMS_WRITE_SECTORS (UMS_BASE + 0x4)
#define USB_IOCTL_SET_PORT          (UMS_BASE + 0x83)

#define RUNS      20
#define BIG       64            /* sectors: the EHCI module's 32 KB transfer */

static char dev_usb2[] ATTRIBUTE_ALIGN(32) = "/dev/usb2";
static s32 heap = -1, fd = -1;

struct timing {
	u32 n, failed;
	u64 total_us, max_us;
};

static void add(struct timing *t, s64 start, s32 ret)
{
	u64 us = ticks_to_microsecs(diff_ticks(start, gettime()));
	t->n++;
	t->total_us += us;
	if (us > t->max_us)
		t->max_us = us;
	if (ret != 0)
		t->failed++;
}

static u32 avg(const struct timing *t)
{
	return t->n ? (u32) (t->total_us / t->n) : 0;
}

static s32 rw(s32 ioctl, u32 lba, u32 count, void *buf, u32 size)
{
	return IOS_IoctlvFormat(heap, fd, ioctl, "ii:d", lba, count, buf, size);
}

static u16 slot_version(u32 slot)
{
	static u8 tmd_buf[MAX_SIGNED_TMD_SIZE] ATTRIBUTE_ALIGN(32);
	u64 tid = 0x0000000100000000ULL | slot;
	u32 size = 0;
	if (ES_GetStoredTMDSize(tid, &size) < 0 || size > sizeof(tmd_buf))
		return 0;
	if (ES_GetStoredTMD(tid, (signed_blob *) tmd_buf, size) < 0)
		return 0;
	return ((tmd *) SIGNATURE_PAYLOAD((signed_blob *) tmd_buf))->title_version;
}

int main(int argc, char **argv)
{
	u32 ios = 249, sector_size = 0, sectors, lba;
	bool write_back = false;
	int writes = RUNS;
	struct timing r1 = {0}, r64 = {0}, w1 = {0}, w64 = {0};
	u32 changed = 0;

	for (int i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--ios=", 6))
			ios = atoi(argv[i] + 6);
		else if (!strcmp(argv[i], "--write-back"))
			write_back = true;
		else if (!strncmp(argv[i], "--writes=", 9))
			writes = atoi(argv[i] + 9);
	}

	hbc_netlog_init();
	printf("umsbench: running on IOS%d v%d; slot %u holds v%u\n",
	       IOS_GetVersion(), IOS_GetRevision(), ios, slot_version(ios));

	/* Reloading IOS takes the network down: close the log first. */
	hbc_netlog_close();
	s32 ret = IOS_ReloadIOS(ios);
	hbc_netlog_init();
	printf("umsbench: IOS_ReloadIOS(%u) = %d; now IOS%d v%d\n", ios, ret,
	       IOS_GetVersion(), IOS_GetRevision());
	if (ret < 0 || IOS_GetVersion() != (s32) ios) {
		printf("RESULT ios=%u error=reload\n", ios);
		return 1;
	}

	/* An IOS heap is in MEM2: room for the ioctl vectors and two 32 KB buffers. */
	heap = iosCreateHeap(2 * BIG * 512 + 4096);
	fd = IOS_Open(dev_usb2, 0);
	if (heap < 0 || fd < 0) {
		printf("RESULT ios=%u error=open heap=%d fd=%d\n", ios, heap, fd);
		return 1;
	}
	IOS_IoctlvFormat(heap, fd, USB_IOCTL_SET_PORT, "i:", 0);
	s64 t0 = gettime();
	ret = IOS_IoctlvFormat(heap, fd, USB_IOCTL_UMS_INIT, ":");
	u32 init_ms = ticks_to_millisecs(diff_ticks(t0, gettime()));
	sectors = IOS_IoctlvFormat(heap, fd, USB_IOCTL_UMS_GET_CAPACITY, ":i", &sector_size);
	printf("umsbench: UMS_INIT = %d in %u ms; %u sectors of %u bytes\n", ret, init_ms,
	       sectors, sector_size);
	if ((s32) sectors <= 0 || (sector_size != 512 && sector_size != 4096)) {
		printf("RESULT ios=%u error=nodrive init=%d\n", ios, ret);
		IOS_Close(fd);
		return 1;
	}

	/* From the IOS heap: MEM2, 32-byte aligned, as the module's DMA wants it. */
	u32 big = BIG * 512 / sector_size;
	u8 *buf = iosAlloc(heap, big * sector_size);
	u8 *orig = iosAlloc(heap, big * sector_size);
	if (!buf || !orig) {
		printf("RESULT ios=%u error=nomem\n", ios);
		IOS_Close(fd);
		return 1;
	}
	lba = (sectors / 2) & ~63u;

	for (int i = 0; i < RUNS; i++) {
		s64 s = gettime();
		add(&r1, s, rw(USB_IOCTL_UMS_READ_SECTORS, lba + i, 1, buf, sector_size));
	}
	for (int i = 0; i < RUNS; i++) {
		s64 s = gettime();
		add(&r64, s, rw(USB_IOCTL_UMS_READ_SECTORS, lba, big, buf, big * sector_size));
	}
	printf("umsbench: read 1 sector: avg %u us, max %llu us, %u failed\n", avg(&r1),
	       r1.max_us, r1.failed);
	printf("umsbench: read %u sectors: avg %u us, max %llu us, %u failed\n", big, avg(&r64),
	       r64.max_us, r64.failed);

	if (write_back) {
		/* Put back exactly what is there: read, write the same bytes. */
		if (rw(USB_IOCTL_UMS_READ_SECTORS, lba, big, orig, big * sector_size) != 0) {
			printf("RESULT ios=%u error=readorig\n", ios);
			IOS_Close(fd);
			return 1;
		}
		for (int i = 0; i < writes; i++) {
			s64 s = gettime();
			add(&w1, s, rw(USB_IOCTL_UMS_WRITE_SECTORS, lba + (i % big), 1,
				       orig + (i % big) * sector_size, sector_size));
			printf("umsbench: write 1 sector #%d: %llu us\n", i,
			       ticks_to_microsecs(diff_ticks(s, gettime())));
		}
		for (int i = 0; i < (writes < 5 ? writes : 5); i++) {
			s64 s = gettime();
			add(&w64, s, rw(USB_IOCTL_UMS_WRITE_SECTORS, lba, big, orig, big * sector_size));
		}
		if (rw(USB_IOCTL_UMS_READ_SECTORS, lba, big, buf, big * sector_size) != 0 ||
		    memcmp(buf, orig, big * sector_size) != 0)
			changed = 1;
		printf("umsbench: write 1 sector: avg %u us, max %llu us, %u failed\n", avg(&w1),
		       w1.max_us, w1.failed);
		printf("umsbench: write %u sectors: avg %u us, max %llu us, %u failed\n", big,
		       avg(&w64), w64.max_us, w64.failed);
		printf("umsbench: data after write-back %s\n", changed ? "DIFFERS" : "unchanged");
	}

	IOS_Close(fd);
	printf("RESULT ios=%u rev=%d init_ms=%u r1_us=%u r1_max=%llu r64_us=%u w1_us=%u "
	       "w1_max=%llu w64_us=%u failed=%u changed=%u\n", ios, IOS_GetRevision(), init_ms,
	       avg(&r1), r1.max_us, avg(&r64), avg(&w1), w1.max_us, avg(&w64),
	       r1.failed + r64.failed + w1.failed + w64.failed, changed);
	return 0;
}
