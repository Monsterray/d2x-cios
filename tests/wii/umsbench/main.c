/*
 * umsbench: time USB mass-storage sector reads and writes through a cIOS.
 *
 *     hbc.py run umsbench.dol [--ios=249] [--write-back [--writes=N]]
 *                             [--big-writes=N] [--timeout=N] [--quirks=N] [--log]
 *
 * --timeout and --log need an EHCI module built with EHCI_DEBUG_LOG:
 * --timeout sets its sector I/O timeout (usb_timeout units; 1000000 is
 * about 1.05 s), --quirks sets its UMS_QUIRK_* bits before UMS_INIT (1: pad
 * every CBW to 12 bytes), --log prints its Bulk-Only event log after the
 * writes. --big-writes is the number of 64-sector writes (default 5, at
 * most --writes).
 *
 * Prints the cIOS in the slot (its TMD version), reloads to it, opens the
 * EHCI module's /dev/usb2, and times reads of 1 and 64 sectors in the middle
 * of the drive. With --write-back it also times writes that put back the
 * bytes it just read, then reads them again to check nothing changed: the
 * drive's data stays as it was. Output goes to the PC through hbc_netlog
 * until the IOS reload; the whole report is also saved to sd:/umsbench.txt
 * (tests/wii/umsbench.py fetches it). Its last line starts with "RESULT".
 */
#include <gccore.h>
#include <fat.h>
#include <sdcard/wiisd_io.h>
#include <stdarg.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "hbc_netlog.h"

#define UMS_BASE                    (('U' << 24) | ('M' << 16) | ('S' << 8))
#define USB_IOCTL_UMS_INIT          (UMS_BASE + 0x1)
#define USB_IOCTL_UMS_GET_CAPACITY  (UMS_BASE + 0x2)
#define USB_IOCTL_UMS_READ_SECTORS  (UMS_BASE + 0x3)
#define USB_IOCTL_UMS_WRITE_SECTORS (UMS_BASE + 0x4)
#define USB_IOCTL_SET_PORT          (UMS_BASE + 0x83)
/* EHCI_DEBUG_LOG builds of the EHCI module only */
#define USB_IOCTL_UMS_DEBUG_LOG     (UMS_BASE + 0x90)
#define USB_IOCTL_UMS_SET_TIMEOUT   (UMS_BASE + 0x91)
#define USB_IOCTL_UMS_SET_QUIRKS    (UMS_BASE + 0x92)

#define RUNS      20
#define BIG       64            /* sectors: the EHCI module's 32 KB transfer */

static char dev_usb2[] ATTRIBUTE_ALIGN(32) = "/dev/usb2";
static s32 heap = -1, fd = -1;
static char report[16384];
static size_t report_len;

/* printf, and keep a copy for sd:/umsbench.txt */
static void __attribute__((format(printf, 1, 2))) say(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fputs(line, stdout);
	if (n > 0 && (size_t) n < sizeof(line) && report_len + n < sizeof(report)) {
		memcpy(report + report_len, line, n);
		report_len += n;
	}
}

/* The SD card only: the USB drive under test stays untouched. */
static void save_report(void)
{
	if (!fatMountSimple("sd", &__io_wiisd))
		return;
	FILE *f = fopen("sd:/umsbench.txt", "wb");
	if (f) {
		fwrite(report, 1, report_len, f);
		fclose(f);
	}
	fatUnmount("sd:");
}

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

/* The EHCI module's event ring: u32 count, then 12-byte events. */
static void print_log(void)
{
	static u8 raw[4 + 256 * 12] ATTRIBUTE_ALIGN(32);
	s32 n = IOS_IoctlvFormat(heap, fd, USB_IOCTL_UMS_DEBUG_LOG, ":d", raw, sizeof(raw));
	if (n < 4) {
		say("umsbench: DEBUG_LOG = %d (not an EHCI_DEBUG_LOG build?)\n", n);
		return;
	}
	u32 total, t0 = 0;
	memcpy(&total, raw, 4);
	say("umsbench: event log, %u events in all, last %d:\n", total, (n - 4) / 12);
	for (s32 off = 4; off + 12 <= n; off += 12) {
		u32 ticks;
		s32 ret;
		u16 len;
		memcpy(&ticks, raw + off, 4);
		memcpy(&len, raw + off + 6, 2);
		memcpy(&ret, raw + off + 8, 4);
		if (off == 4)
			t0 = ticks;
		/* Hollywood's timer: about 1898 ticks per ms */
		say("  %8u ms  %c ep=%02x len=%u ret=%d\n", (ticks - t0) / 1898, raw[off + 4],
		    raw[off + 5], len, ret);
	}
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

/* A text console on the TV, so the bench capture shows the run. The VI
   belongs to the PPC, so it stays up across the IOS reload. */
static void console_setup(void)
{
	VIDEO_Init();
	GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
	void *xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
		     rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
}

static int bench(int argc, char **argv)
{
	u32 ios = 249, sector_size = 0, sectors, lba;
	bool write_back = false;
	int writes = RUNS;
	int big_writes = 5;
	u32 io_timeout = 0, quirks = 0;
	bool log = false;
	int dump = 0;
	struct timing r1 = {0}, r64 = {0}, w1 = {0}, w64 = {0};
	u32 changed = 0;

	for (int i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--ios=", 6))
			ios = atoi(argv[i] + 6);
		else if (!strcmp(argv[i], "--write-back"))
			write_back = true;
		else if (!strncmp(argv[i], "--writes=", 9))
			writes = atoi(argv[i] + 9);
		else if (!strncmp(argv[i], "--timeout=", 10))
			io_timeout = strtoul(argv[i] + 10, NULL, 10);
		else if (!strncmp(argv[i], "--big-writes=", 13))
			big_writes = atoi(argv[i] + 13);
		else if (!strncmp(argv[i], "--quirks=", 9))
			quirks = strtoul(argv[i] + 9, NULL, 0);
		else if (!strcmp(argv[i], "--log"))
			log = true;
		else if (!strncmp(argv[i], "--dump=", 7))
			dump = atoi(argv[i] + 7);
	}

	console_setup();
	hbc_netlog_init();
	say("umsbench: running on IOS%d v%d; slot %u holds v%u\n",
	       IOS_GetVersion(), IOS_GetRevision(), ios, slot_version(ios));

	/* Reloading IOS takes the network down: close the log, and tell libogc
	 * its network is gone, so a new hbc_netlog_init() starts it again. */
	hbc_netlog_close();
	net_deinit();
	printf("umsbench: reloading to IOS%u...\n", ios);
	s32 ret = IOS_ReloadIOS(ios);
	hbc_netlog_init();
	say("umsbench: IOS_ReloadIOS(%u) = %d; now IOS%d v%d\n", ios, ret,
	       IOS_GetVersion(), IOS_GetRevision());
	if (ret < 0 || IOS_GetVersion() != (s32) ios) {
		say("RESULT ios=%u error=reload\n", ios);
		return 1;
	}

	/* An IOS heap is in MEM2: room for the ioctl vectors and two 32 KB buffers. */
	heap = iosCreateHeap(2 * BIG * 512 + 4096);
	fd = IOS_Open(dev_usb2, 0);
	if (heap < 0 || fd < 0) {
		say("RESULT ios=%u error=open heap=%d fd=%d\n", ios, heap, fd);
		return 1;
	}
	IOS_IoctlvFormat(heap, fd, USB_IOCTL_SET_PORT, "i:", 0);
	if (io_timeout)
		say("umsbench: SET_TIMEOUT(%u) = %d\n", io_timeout,
		    IOS_IoctlvFormat(heap, fd, USB_IOCTL_UMS_SET_TIMEOUT, "i:", io_timeout));
	if (quirks)
		say("umsbench: SET_QUIRKS(%u) = %d\n", quirks,
		    IOS_IoctlvFormat(heap, fd, USB_IOCTL_UMS_SET_QUIRKS, "i:", quirks));
	printf("umsbench: starting the USB drive...\n");
	s64 t0 = gettime();
	ret = IOS_IoctlvFormat(heap, fd, USB_IOCTL_UMS_INIT, ":");
	u32 init_ms = ticks_to_millisecs(diff_ticks(t0, gettime()));
	sectors = IOS_IoctlvFormat(heap, fd, USB_IOCTL_UMS_GET_CAPACITY, ":i", &sector_size);
	say("umsbench: UMS_INIT = %d in %u ms; %u sectors of %u bytes\n", ret, init_ms,
	       sectors, sector_size);
	if ((s32) sectors <= 0 || (sector_size != 512 && sector_size != 4096)) {
		say("RESULT ios=%u error=nodrive init=%d\n", ios, ret);
		IOS_Close(fd);
		return 1;
	}

	/* From the IOS heap: MEM2, 32-byte aligned, as the module's DMA wants it. */
	u32 big = BIG * 512 / sector_size;
	u8 *buf = iosAlloc(heap, big * sector_size);
	u8 *orig = iosAlloc(heap, big * sector_size);
	if (!buf || !orig) {
		say("RESULT ios=%u error=nomem\n", ios);
		IOS_Close(fd);
		return 1;
	}
	lba = (sectors / 2) & ~63u;

	/* --dump=N: print the first N sectors of the test area, read only */
	for (int s = 0; s < dump && s < (int) big; s++) {
		s32 r = rw(USB_IOCTL_UMS_READ_SECTORS, lba + s, 1, buf, sector_size);
		say("umsbench: dump sector %u (read %d):\n", lba + s, r);
		for (u32 o = 0; o < sector_size; o += 32) {
			char line[96];
			int n = 0;
			for (int b = 0; b < 32; b++)
				n += snprintf(line + n, sizeof(line) - n, "%02x", buf[o + b]);
			say("  %03x %s\n", o, line);
		}
	}

	printf("umsbench: timing %d reads of 1 and %u sectors at sector %u...\n", RUNS, big, lba);
	for (int i = 0; i < RUNS; i++) {
		s64 s = gettime();
		add(&r1, s, rw(USB_IOCTL_UMS_READ_SECTORS, lba + i, 1, buf, sector_size));
	}
	for (int i = 0; i < RUNS; i++) {
		s64 s = gettime();
		add(&r64, s, rw(USB_IOCTL_UMS_READ_SECTORS, lba, big, buf, big * sector_size));
	}
	say("umsbench: read 1 sector: avg %u us, max %llu us, %u failed\n", avg(&r1),
	       r1.max_us, r1.failed);
	say("umsbench: read %u sectors: avg %u us, max %llu us, %u failed\n", big, avg(&r64),
	       r64.max_us, r64.failed);

	if (write_back) {
		/* Put back exactly what is there: read, write the same bytes. */
		if (rw(USB_IOCTL_UMS_READ_SECTORS, lba, big, orig, big * sector_size) != 0) {
			say("RESULT ios=%u error=readorig\n", ios);
			IOS_Close(fd);
			return 1;
		}
		for (int i = 0; i < writes; i++) {
			printf("umsbench: writing sector %u (#%d of %d), waiting for the drive...\n",
			       lba + (i % big), i + 1, writes);
			s64 s = gettime();
			add(&w1, s, rw(USB_IOCTL_UMS_WRITE_SECTORS, lba + (i % big), 1,
				       orig + (i % big) * sector_size, sector_size));
			say("umsbench: write 1 sector #%d: %llu us\n", i,
			       ticks_to_microsecs(diff_ticks(s, gettime())));
		}
		for (int i = 0; i < (writes < big_writes ? writes : big_writes); i++) {
			printf("umsbench: writing %u sectors at %u (#%d), waiting for the drive...\n",
			       big, lba, i + 1);
			s64 s = gettime();
			add(&w64, s, rw(USB_IOCTL_UMS_WRITE_SECTORS, lba, big, orig, big * sector_size));
		}
		if (rw(USB_IOCTL_UMS_READ_SECTORS, lba, big, buf, big * sector_size) != 0 ||
		    memcmp(buf, orig, big * sector_size) != 0)
			changed = 1;
		say("umsbench: write 1 sector: avg %u us, max %llu us, %u failed\n", avg(&w1),
		       w1.max_us, w1.failed);
		say("umsbench: write %u sectors: avg %u us, max %llu us, %u failed\n", big,
		       avg(&w64), w64.max_us, w64.failed);
		say("umsbench: data after write-back %s\n", changed ? "DIFFERS" : "unchanged");
	}
	if (log)
		print_log();

	IOS_Close(fd);
	say("RESULT ios=%u rev=%d init_ms=%u r1_us=%u r1_max=%llu r64_us=%u w1_us=%u "
	       "w1_max=%llu w64_us=%u failed=%u changed=%u\n", ios, IOS_GetRevision(), init_ms,
	       avg(&r1), r1.max_us, avg(&r64), avg(&w1), w1.max_us, avg(&w64),
	       r1.failed + r64.failed + w1.failed + w64.failed, changed);
	return 0;
}

int main(int argc, char **argv)
{
	int rc = bench(argc, argv);
	save_report();
	/* Leave the result on the TV for a moment before HBC comes back */
	printf("umsbench: done (report saved to sd:/umsbench.txt); back to HBC in 3 s\n");
	sleep(3);
	return rc;
}
