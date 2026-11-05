/*
 * sdbench: time SD card reads through d2x's /dev/sdio/sdhc, and read the SD
 * high-speed state. READ ONLY: it never writes a sector of the card.
 *
 *     hbc.py run sdbench.dol [--ios=249] [--mb=32]
 *
 * Reloads to the cIOS, prints its identity, opens /dev/sdio/sdhc, sends INIT,
 * asks the high-speed state (ioctl 0x20; -101, IPC_EINVAL, is a release build
 * without SDIO_HIGH_SPEED: the default case of the ioctlv switch) and the
 * record of the last high-speed attempt (0x22), then times --mb MB of sequential reads from sector 0 in
 * 64-sector calls and 2 MB in 1-sector calls. On a build that answers 0x20 it
 * then sends 0x21 (forced fallback), prints the state (bit 1 set, when high
 * speed was on before) and reads the same area again: the CRC32 must match, and the speed shows the cost of
 * the slow path. Output goes to the TV and, until the IOS reload, to the PC
 * through hbc_netlog; the whole report is saved to sd:/sdbench.txt
 * (tests/wii/sdbench.py fetches it). The last line starts with "RESULT".
 */
#include <gccore.h>
#include <malloc.h>
#include <fat.h>
#include <sdcard/wiisd_io.h>
#include <stdarg.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "hbc_netlog.h"

/* libogc exports it without a header: reload HBC after a crash */
extern void __exception_setreload(int t);

/* ioctlv numbers, source/cios-lib/sdhc_server.c */
#define SDHC_IOCTL_INIT        0x01
#define SDHC_IOCTL_READ        0x02
#define SDHC_IOCTL_HS_STATE    0x20   /* SDIO_HIGH_SPEED builds: bit 0 active, bit 1 fell back */
#define SDHC_IOCTL_HS_FALLBACK 0x21   /* same builds: fall back now, answer the state */
#define SDHC_IOCTL_HS_RECORD   0x22   /* same builds: output vector, struct hs_record */

/* docs/sd-high-speed.md, source/cios-lib/sdio_hs.h: where the last attempt stopped */
struct hs_record {
	u32 calls;
	s32 step;
	s32 rc;
	u8  status[20];     /* CMD6 answer bytes 0..19: [13] bit 1 supported, [16] & 0xF selected */
} __attribute__((packed));

static const char *const hs_step_name[] = {
	"high speed code was never called",
	"skipped: an earlier fallback marked high speed dead",
	"CMD6 check command failed",
	"CMD6 check answered: the card does not offer high speed",
	"CMD6 switch command failed",
	"CMD6 switch answered: the card did not select high speed",
	"reading the host control register failed",
	"writing the host control register failed",
	"setting the 50 MHz clock failed (fell back)",
	"high speed is on",
};

#define BIG    64                     /* sectors per call */
#define SECTOR 512

static char dev_sdhc[] ATTRIBUTE_ALIGN(32) = "/dev/sdio/sdhc";
static s32 heap = -1, fd = -1;
static char report[16384];
static size_t report_len;

/* printf, and keep a copy for sd:/sdbench.txt */
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

/* Written after /dev/sdio/sdhc is closed, by libogc's own SD driver. The only
   write this program makes. */
static void save_report(void)
{
	if (!fatMountSimple("sd", &__io_wiisd))
		return;
	FILE *f = fopen("sd:/sdbench.txt", "wb");
	if (f) {
		fwrite(report, 1, report_len, f);
		fclose(f);
	}
	fatUnmount("sd:");
}

static u32 crc_table[256];

static void crc_init(void)
{
	for (u32 i = 0; i < 256; i++) {
		u32 c = i;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_table[i] = c;
	}
}

static u32 crc_update(u32 crc, const u8 *p, u32 n)
{
	while (n--)
		crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return crc;
}

static s32 sd_read(u32 lba, u32 count, void *buf)
{
	return IOS_IoctlvFormat(heap, fd, SDHC_IOCTL_READ, "ii:d", lba, count, buf, count * SECTOR);
}

struct readres {
	u32 crc, calls, failed;
	u64 us;
	s32 first_err;
};

/* Read `sectors` from sector 0 in `per`-sector calls. Only the ioctl is timed;
   the CRC is not. */
static void read_run(struct readres *r, u32 sectors, u32 per, void *buf, bool crc)
{
	memset(r, 0, sizeof(*r));
	r->crc = 0xFFFFFFFFu;
	u64 ticks = 0;
	for (u32 lba = 0; lba + per <= sectors; lba += per) {
		s64 t = gettime();
		s32 ret = sd_read(lba, per, buf);
		ticks += diff_ticks(t, gettime());
		r->calls++;
		if (ret != 0) {
			if (!r->failed++)
				r->first_err = ret;
			continue;
		}
		if (crc)
			r->crc = crc_update(r->crc, buf, per * SECTOR);
	}
	r->crc ^= 0xFFFFFFFFu;
	r->us = ticks_to_microsecs(ticks);
}

static void report_run(const char *name, const struct readres *r, u32 sectors, u32 per)
{
	u32 kb = sectors / 2;
	u32 kbps = r->us ? (u32) ((u64) kb * 1000000 / r->us) : 0;
	say("sdbench: %s: %u KB in %u-sector calls (%u calls), %llu us: %u.%02u MB/s, %u failed",
	    name, kb, per, r->calls, r->us, kbps / 1024, (kbps % 1024) * 100 / 1024, r->failed);
	if (r->failed)
		say(" (first error %d)", (int) r->first_err);
	say("\n");
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

/* The cIOS identity block, read the way USB Loader GX does: the first content
 * of the IOS's stored TMD starts with magic 0x1ee7c105, version, base IOS,
 * name[16] and versionstring[16]. Shows which build is running. */
static void show_cios_identity(u32 ios)
{
	u64 tid = (1ULL << 32) | ios;
	u32 tmd_len = 0;
	char path[ISFS_MAXPATH] ATTRIBUTE_ALIGN(32);
	struct { u32 magic, magicver, version, base; char name[16], vstr[16]; } *info = NULL;
	signed_blob *tmd = NULL;
	fstats *st = NULL;
	s32 ifd = -1;

	if (ISFS_Initialize() < 0 || ES_GetStoredTMDSize(tid, &tmd_len) < 0 || tmd_len <= 0x1E7 ||
	    !(tmd = memalign(32, (tmd_len + 31) & ~31u)) || ES_GetStoredTMD(tid, tmd, tmd_len) < 0)
		goto fail;
	snprintf(path, sizeof(path), "/title/00000001/%08x/content/%08x.app", ios,
	         (u32) *(u8 *) ((u32) tmd + 0x1E7));
	ifd = ISFS_Open(path, ISFS_OPEN_READ);
	if (ifd < 0 || !(st = memalign(32, sizeof(*st))) || ISFS_GetFileStats(ifd, st) < 0 ||
	    st->file_length < 0x30 || !(info = memalign(32, 64)) || ISFS_Read(ifd, info, 64) < 0x30)
		goto fail;
	if (info->magic != 0x1ee7c105) {
		say("cIOS %u: no identity block (magic %08x): stock IOS?\n", ios, info->magic);
		goto done;
	}
	info->name[15] = info->vstr[15] = 0;
	say("cIOS %u: %s v%u %s base %u rev %d\n", ios, info->name, info->version, info->vstr,
	    info->base, IOS_GetRevision());
	goto done;
fail:
	say("cIOS %u: identity not readable (tmd %d, file %d)\n", ios, (int) tmd_len, (int) ifd);
done:
	if (ifd >= 0)
		ISFS_Close(ifd);
	free(info);
	free(st);
	free(tmd);
}

static int bench(int argc, char **argv)
{
	u32 ios = 249, mb = 32;
	char why[96] = "";
	bool bad = false;

	for (int i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--ios=", 6))
			ios = atoi(argv[i] + 6);
		else if (!strncmp(argv[i], "--mb=", 5))
			mb = atoi(argv[i] + 5);
	}
	if (mb < 1)
		mb = 1;

	console_setup();
	hbc_netlog_init();
	crc_init();
	say("sdbench: running on IOS%d v%d; slot %u holds v%u; reads %u MB (read only)\n",
	    IOS_GetVersion(), IOS_GetRevision(), ios, slot_version(ios), mb);

	/* Reloading IOS takes the network down: close the log, and tell libogc
	 * its network is gone, so a new hbc_netlog_init() starts it again. */
	hbc_netlog_close();
	net_deinit();
	printf("sdbench: reloading to IOS%u...\n", ios);
	s32 ret = IOS_ReloadIOS(ios);
	hbc_netlog_init();
	say("sdbench: IOS_ReloadIOS(%u) = %d; now IOS%d v%d\n", ios, ret, IOS_GetVersion(),
	    IOS_GetRevision());
	if (ret < 0 || IOS_GetVersion() != (s32) ios) {
		say("RESULT fail reload ios=%u\n", ios);
		return 1;
	}
	show_cios_identity(ios);

	/* An IOS heap is in MEM2: room for the ioctlv vectors and one 32 KB buffer. */
	heap = iosCreateHeap(BIG * SECTOR + 4096);
	fd = IOS_Open(dev_sdhc, 0);
	if (heap < 0 || fd < 0) {
		say("sdbench: open /dev/sdio/sdhc: heap %d, fd %d (no such device?)\n", heap, fd);
		say("RESULT fail open /dev/sdio/sdhc heap=%d fd=%d\n", heap, fd);
		return 1;
	}
	u8 *buf = iosAlloc(heap, BIG * SECTOR);       /* MEM2, 32-byte aligned */
	if (!buf) {
		IOS_Close(fd);
		say("RESULT fail no memory for the buffer\n");
		return 1;
	}

	ret = IOS_IoctlvFormat(heap, fd, SDHC_IOCTL_INIT, ":");
	say("sdbench: INIT (0x%02x) = %d\n", SDHC_IOCTL_INIT, ret);
	if (ret != 0) {
		say("RESULT fail INIT returned %d\n", ret);
		IOS_Close(fd);
		return 1;
	}
	/* The device has no capacity ioctl: the sector count is not available. */
	say("sdbench: sector count: not offered by /dev/sdio/sdhc\n");

	s32 hs = IOS_IoctlvFormat(heap, fd, SDHC_IOCTL_HS_STATE, ":");
	bool has_hs = hs >= 0;
	bool hs_was_on = hs >= 0 && (hs & 1);
	if (hs == -101)
		say("sdbench: release build: no high-speed state (0x20 = -101, unknown command)\n");
	else if (hs < 0)
		say("sdbench: HS_STATE (0x20) = %d (error)\n", hs);
	else
		say("sdbench: HS_STATE (0x20) = %d (active %d, fell back %d)\n", hs, hs & 1,
		    (hs >> 1) & 1);

	if (has_hs) {
		static struct hs_record rec ATTRIBUTE_ALIGN(32);
		memset(&rec, 0, sizeof(rec));
		s32 rr = IOS_IoctlvFormat(heap, fd, SDHC_IOCTL_HS_RECORD, ":d", &rec, sizeof(rec));
		if (rr < 0) {
			say("sdbench: HS_RECORD (0x22) = %d (not answered)\n", rr);
		} else {
			say("sdbench: HS_RECORD (0x22): high speed code ran %u time(s); last call: %s (step %d, IOS rc %d)\n",
			    (unsigned) rec.calls, (rec.step >= 0 && rec.step < 10) ? hs_step_name[rec.step] : "unknown step",
			    (int) rec.step, (int) rec.rc);
			say("sdbench: CMD6 answer, first 20 bytes:");
			for (int i = 0; i < 20; i++)
				say(" %02x", rec.status[i]);
			say("\n");
			say("sdbench: group 1 function 1 (high speed) offered: %s; group 1 selected: %u\n",
			    (rec.status[13] & 2) ? "yes" : "no", (unsigned) (rec.status[16] & 0xF));
		}
	}

	u32 sectors = mb * 2048;
	struct readres r64, r1, rfb;
	printf("sdbench: reading %u MB in %u-sector calls...\n", mb, BIG);
	read_run(&r64, sectors, BIG, buf, true);
	report_run("read", &r64, sectors, BIG);
	say("sdbench: CRC32 of the first %u MB: %08x\n", mb, (unsigned) r64.crc);
	if (r64.failed) {
		bad = true;
		snprintf(why, sizeof(why), "%u failed %u-sector reads, first error %d",
		         (unsigned) r64.failed, BIG, (int) r64.first_err);
	}

	printf("sdbench: reading 2 MB in 1-sector calls...\n");
	read_run(&r1, 2 * 2048, 1, buf, false);
	report_run("read", &r1, 2 * 2048, 1);
	if (r1.failed && !bad) {
		bad = true;
		snprintf(why, sizeof(why), "%u failed 1-sector reads, first error %d",
		         (unsigned) r1.failed, (int) r1.first_err);
	}

	if (has_hs) {
		s32 st = IOS_IoctlvFormat(heap, fd, SDHC_IOCTL_HS_FALLBACK, ":");
		if (!hs_was_on) {
			say("sdbench: HS_FALLBACK (0x21) = %d; fallback check skipped: high speed was not on\n", st);
		} else {
			say("sdbench: HS_FALLBACK (0x21) = %d%s\n", st,
			    st >= 0 ? ((st & 2) ? " (bit 1 set: fell back)" : " (bit 1 NOT set)") : " (error)");
			if (st < 0 || !(st & 2)) {
				if (!bad)
					snprintf(why, sizeof(why), "fallback state %d lacks bit 1", st);
				bad = true;
			}
		}
		printf("sdbench: reading %u MB again after the fallback...\n", mb);
		read_run(&rfb, sectors, BIG, buf, true);
		report_run("read after fallback", &rfb, sectors, BIG);
		bool same = rfb.crc == r64.crc;
		say("sdbench: CRC32 after fallback %08x: %s\n", (unsigned) rfb.crc,
		    same ? "matches" : "DIFFERS");
		if (!same && !bad) {
			bad = true;
			snprintf(why, sizeof(why), "CRC after fallback differs (%08x vs %08x)",
			         (unsigned) rfb.crc, (unsigned) r64.crc);
		}
		if (rfb.failed && !bad) {
			bad = true;
			snprintf(why, sizeof(why), "%u failed reads after the fallback",
			         (unsigned) rfb.failed);
		}
	}

	IOS_Close(fd);
	if (bad) {
		say("RESULT fail %s\n", why);
		return 1;
	}
	say("RESULT ok\n");
	return 0;
}

int main(int argc, char **argv)
{
	__exception_setreload(5);
	int rc = bench(argc, argv);
	save_report();
	/* Leave the result on the TV for a moment before HBC comes back */
	printf("sdbench: done (report saved to sd:/sdbench.txt); back to HBC in 3 s\n");
	sleep(3);
	return rc;
}
