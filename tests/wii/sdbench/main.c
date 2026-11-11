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
 * the slow path. Init diagnostics (2026-11, base 38): --wait=MS waits before
 * INIT, --inits=N repeats INIT while it fails, --probe=N prints IOS's own
 * RESETCARD and GETSTATUS N times first, --libogc runs libogc's own SD
 * start-up first.
 *
 * Also (docs/sd-speed.md): a raw size sweep, 1 to 255 sectors (to 1024 with
 * --big), with the fit time = a + b * sectors; a read-only dump of the SD host
 * registers (PPC 0xCD070000, only when HW_AHBPROT allows); and, with --file or
 * --fat-write, timings through the FAT module (IOS_Open "fat/PATH"), which is
 * the path real loaders use. --fat-write MB writes under /sdbench/ only (a
 * scratch file it deletes); nothing writes without it.
 *
 *     --big            sweep to 512 and 1024 sectors (untried sizes: attended runs only)
 *     --file=/PATH     read this file on the SD card through the FAT module
 *     --limit=KB       read at most KB of it (default 8192)
 *     --fat-write=MB   write MB to /sdbench/w.tmp through the FAT module, read it back, delete it
 *
 * Output goes to the TV and, until the IOS reload, to the PC
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

/* FAT module (source/fat-module/module.h) */
#define FAT_MKDIR   0x01
#define FAT_MKFILE  0x02
#define FAT_DELETE  0x05
#define FAT_MOUNT   0xF0
#define FAT_UMOUNT  0xF1
#define FAT_READ    1                    /* FatFs FA_READ */
#define FAT_WRITE   2                    /* FA_WRITE */

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

/* ---- raw size sweep and its fit ---- */

static const u32 sweep_sizes[] = { 1, 2, 4, 8, 16, 32, 64, 128, 255, 512, 1024 };

/* MiB/s times 100, from bytes and microseconds */
static int mibs100(double bytes, double us)
{
	return us > 0 ? (int) (bytes / us * 1e6 / 1048576.0 * 100.0) : 0;
}

/* Microseconds per call for `calls` reads of n sectors from sector 0 onward;
   -1 when a read failed (error in *err). */
static double sweep_one(u32 n, u32 calls, void *buf, s32 *err)
{
	u64 ticks = 0;
	for (u32 i = 0; i < calls; i++) {
		s64 t = gettime();
		s32 ret = sd_read(i * n, n, buf);
		ticks += diff_ticks(t, gettime());
		if (ret != 0) {
			*err = ret;
			return -1;
		}
	}
	return (double) ticks_to_microsecs(ticks) / calls;
}

/* Least squares t = a + b n over the sizes that read; prints a in us per call,
   b in us per sector, and what the fit predicts. */
static void sweep(void *buf, u32 maxsec)
{
	double sn = 0, st = 0, snn = 0, snt = 0;
	int k = 0;
	say("sdbench: raw size sweep (calls of N sectors from sector 0):\n");
	for (u32 i = 0; i < sizeof(sweep_sizes) / sizeof(sweep_sizes[0]); i++) {
		u32 n = sweep_sizes[i];
		s32 err = 0;
		if (n > maxsec)
			break;
		u32 calls = 2048 / n;
		if (calls < 16)
			calls = 16;
		printf("sdbench: sweep %u sectors...\n", (unsigned) n);
		double t = sweep_one(n, calls, buf, &err);
		if (t < 0) {
			say("sdbench:   %4u sectors: read failed, error %d (sweep stops)\n", (unsigned) n, (int) err);
			break;
		}
		int m = mibs100((double) n * SECTOR, t);
		say("sdbench:   %4u sectors: %8.1f us per call (%u calls): %d.%02d MiB/s\n", (unsigned) n, t,
		    (unsigned) calls, m / 100, m % 100);
		sn += n; st += t; snn += (double) n * n; snt += n * t;
		k++;
	}
	if (k < 2) {
		say("sdbench: fit: fewer than two sizes read\n");
		return;
	}
	double b = (k * snt - sn * st) / (k * snn - sn * sn);
	double a = (st - b * sn) / k;
	say("sdbench: fit t(n) = a + b*n: a = %.1f us per call, b = %.2f us per sector\n", a, b);
	int p64 = mibs100(64.0 * SECTOR, a + b * 64), p255 = mibs100(255.0 * SECTOR, a + b * 255);
	int pinf = mibs100((double) SECTOR, b);
	say("sdbench: fit predicts %d.%02d MiB/s at 64 sectors, %d.%02d at 255, %d.%02d at infinity\n",
	    p64 / 100, p64 % 100, p255 / 100, p255 % 100, pinf / 100, pinf % 100);
}

/* ---- SD host controller registers, read only ----
 * 0xCD070000 is the SD slot's SDHCI. Only registers whose reads change nothing
 * (never 0x20, the data port). The words are big-endian lanes: the block size
 * (bits 11:0; 14:12 the SDMA boundary, 4 KB << n) is the high half of 0x04 and
 * the block count the low half; the host control byte (bit 1 4-bit bus, bit 2
 * high speed, bits 4:3 the DMA mode) is the top byte of 0x28; the clock control
 * (bits 15:8 the divisor code) the high half of 0x2C. The PPC reads them only
 * when HW_AHBPROT (0xCD800064) is open: otherwise this is skipped. */
static void dump_regs(const char *when)
{
	static const u32 off[] = { 0x04, 0x0C, 0x24, 0x28, 0x2C, 0x30, 0x40, 0x44 };
	u32 prot = *(volatile u32 *) 0xCD800064;
	if (prot != 0xFFFFFFFFu) {
		say("sdbench: host registers %s: not read (HW_AHBPROT = %08x, not open to the PPC)\n", when,
		    (unsigned) prot);
		return;
	}
	volatile u32 *base = (volatile u32 *) 0xCD070000;
	u32 v[8];
	for (int i = 0; i < 8; i++)
		v[i] = base[off[i] / 4];
	say("sdbench: host registers %s:", when);
	for (int i = 0; i < 8; i++)
		say(" %02x=%08x", (unsigned) off[i], (unsigned) v[i]);
	say("\n");
	u32 bs = v[0] >> 16, hc = v[3] >> 24, clk = v[4] >> 16;
	say("sdbench:   block size %u, SDMA boundary %u KB, block count %u; host control %02x: %d-bit bus, high speed %u, DMA mode %u; clock %04x: divisor code %u\n",
	    (unsigned) (bs & 0xFFF), (unsigned) (4u << ((bs >> 12) & 7)), (unsigned) (v[0] & 0xFFFF),
	    (unsigned) hc, (hc & 2) ? 4 : 1, (unsigned) ((hc >> 2) & 1), (unsigned) ((hc >> 3) & 3),
	    (unsigned) clk, (unsigned) ((clk >> 8) & 0xFF));
}

/* ---- through the FAT module ---- */

static char dev_fat[] ATTRIBUTE_ALIGN(32) = "fat";
static char fat_path[64] ATTRIBUTE_ALIGN(32);    /* IOS paths are under 64 bytes */

static s32 fat_open(const char *path, u32 mode)
{
	if (strlen(path) + 4 > sizeof(fat_path))
		return -1;
	snprintf(fat_path, sizeof(fat_path), "fat%s", path);
	return IOS_Open(fat_path, mode);
}

/* One pass over an open file in `chunk`-byte calls into buf (the read calls
   are timed, the CRC is not). Returns the bytes read. */
static u32 fat_read_pass(s32 ffd, u8 *buf, u32 chunk, u32 total, u32 *crc, u64 *us, s32 *err)
{
	u64 ticks = 0;
	u32 got = 0;
	u32 c = 0xFFFFFFFFu;
	*err = 0;
	if (IOS_Seek(ffd, 0, SEEK_SET) < 0) {
		*err = -1;
		return 0;
	}
	while (got < total) {
		u32 n = total - got < chunk ? total - got : chunk;
		s64 t = gettime();
		s32 r = IOS_Read(ffd, buf, n);
		ticks += diff_ticks(t, gettime());
		if (r < 0) {
			*err = r;
			break;
		}
		if (r == 0)
			break;
		c = crc_update(c, buf, r);
		got += r;
	}
	*crc = c ^ 0xFFFFFFFFu;
	*us = ticks_to_microsecs(ticks);
	return got;
}

static void fat_read_report(const char *name, u32 chunk, u32 got, u64 us, u32 crc, s32 err)
{
	int m = mibs100((double) got, (double) us);
	say("sdbench: FAT read %s: %u KB in %u KB calls, %llu us: %d.%02d MiB/s, CRC32 %08x%s\n", name,
	    (unsigned) (got / 1024), (unsigned) (chunk / 1024), us, m / 100, m % 100, (unsigned) crc,
	    err ? " (ERROR)" : "");
	if (err)
		say("sdbench:   read error %d\n", (int) err);
}

/* Read `path` (at most limit_kb) through the FAT module with several call
   sizes, aligned and unaligned; every pass must give the same CRC. */
static bool fat_read_phase(const char *path, u32 limit_kb, void *rbuf, u32 rbuf_size, char *why)
{
	static const u32 chunks[] = { 16384, 32768, 131072, 524288 };
	s32 ffd = fat_open(path, FAT_READ);
	say("sdbench: FAT open %s: %d\n", path, (int) ffd);
	if (ffd < 0) {
		snprintf(why, 96, "FAT open %s failed (%d)", path, (int) ffd);
		return false;
	}
	s32 size = IOS_Seek(ffd, 0, SEEK_END);
	if (size < 0) {
		IOS_Close(ffd);
		snprintf(why, 96, "FAT seek to end failed (%d)", (int) size);
		return false;
	}
	u32 total = (u32) size < limit_kb * 1024 ? (u32) size : limit_kb * 1024;
	total &= ~(u32) 511;
	say("sdbench: FAT file %s: %d bytes; reading %u bytes per pass\n", path, (int) size, (unsigned) total);
	bool ok = true;
	u32 first = 0;
	for (int i = 0; i < 5; i++) {
		u32 chunk = i < 4 ? chunks[i] : 32768;
		u8 *b = (u8 *) rbuf + (i < 4 ? 0 : 16);
		char name[32];
		u32 crc = 0, got;
		u64 us = 0;
		s32 err = 0;
		snprintf(name, sizeof(name), i < 4 ? "aligned" : "unaligned (+16)");
		if (chunk + 16 > rbuf_size)
			break;
		printf("sdbench: FAT read %s, %u KB calls...\n", name, (unsigned) (chunk / 1024));
		got = fat_read_pass(ffd, b, chunk, total, &crc, &us, &err);
		fat_read_report(name, chunk, got, us, crc, err);
		if (err || got != total)
			ok = false;
		if (i == 0)
			first = crc;
		else if (crc != first)
			ok = false;
	}
	say("sdbench: FATFILE bytes=%u crc=%08x\n", (unsigned) total, (unsigned) first);
	IOS_Close(ffd);
	if (!ok)
		snprintf(why, 96, "FAT read of %s failed or the CRCs differ", path);
	return ok;
}

/* Write mb MB to /sdbench/w.tmp in `chunk` calls, time the writes and the
   close apart, read it back and compare the CRC. */
static bool fat_write_pass(u32 mb, u32 chunk, u8 *wbuf, u8 *rbuf, u32 rbuf_size, char *why)
{
	u32 total = mb * 1024 * 1024, crc_w = 0xFFFFFFFFu, seed = 0x1234567u;
	u64 ticks = 0, close_us;
	s32 ffd = fat_open("/sdbench/w.tmp", FAT_WRITE);
	if (ffd < 0) {
		snprintf(why, 96, "FAT open for write failed (%d)", (int) ffd);
		return false;
	}
	printf("sdbench: FAT write %u MB in %u KB calls...\n", (unsigned) mb, (unsigned) (chunk / 1024));
	for (u32 off = 0; off < total; off += chunk) {
		for (u32 i = 0; i < chunk / 4; i++) {
			seed = seed * 1664525u + 1013904223u;
			((u32 *) wbuf)[i] = seed;
		}
		crc_w = crc_update(crc_w, wbuf, chunk);
		s64 t = gettime();
		s32 r = IOS_Write(ffd, wbuf, chunk);
		ticks += diff_ticks(t, gettime());
		if (r != (s32) chunk) {
			IOS_Close(ffd);
			snprintf(why, 96, "FAT write returned %d at %u", (int) r, (unsigned) off);
			return false;
		}
	}
	s64 t = gettime();
	IOS_Close(ffd);
	close_us = ticks_to_microsecs(diff_ticks(t, gettime()));
	u64 us = ticks_to_microsecs(ticks);
	int m = mibs100((double) total, (double) us);
	say("sdbench: FAT write %u KB in %u KB calls: %llu us: %d.%02d MiB/s; close %llu us\n",
	    (unsigned) (total / 1024), (unsigned) (chunk / 1024), us, m / 100, m % 100, close_us);
	crc_w ^= 0xFFFFFFFFu;

	ffd = fat_open("/sdbench/w.tmp", FAT_READ);
	if (ffd < 0) {
		snprintf(why, 96, "FAT reopen failed (%d)", (int) ffd);
		return false;
	}
	u32 crc_r = 0, got;
	u64 rus = 0;
	s32 err = 0;
	got = fat_read_pass(ffd, rbuf, rbuf_size < 131072 ? rbuf_size : 131072, total, &crc_r, &rus, &err);
	IOS_Close(ffd);
	say("sdbench: FAT readback %u KB: CRC32 %08x, written %08x: %s\n", (unsigned) (got / 1024),
	    (unsigned) crc_r, (unsigned) crc_w, (!err && got == total && crc_r == crc_w) ? "matches" : "DIFFERS");
	if (err || got != total || crc_r != crc_w) {
		snprintf(why, 96, "FAT write readback differs (%08x vs %08x)", (unsigned) crc_r, (unsigned) crc_w);
		return false;
	}
	return true;
}

/* Mount the SD card in the FAT module, run what was asked, unmount. Every
   write is under /sdbench/, and the scratch file goes at the end. */
static bool fat_phase(const char *file, u32 limit_kb, u32 write_mb, char *why)
{
	bool ok = true;
	s32 mfd = IOS_Open(dev_fat, 0);
	say("sdbench: IOS_Open(\"fat\") = %d\n", (int) mfd);
	if (mfd < 0) {
		snprintf(why, 96, "open fat failed (%d)", (int) mfd);
		return false;
	}
	s32 ret = IOS_IoctlvFormat(heap, mfd, FAT_MOUNT, ":");
	say("sdbench: FAT mount SD (0x%02x) = %d\n", FAT_MOUNT, (int) ret);
	if (ret < 0) {
		IOS_Close(mfd);
		snprintf(why, 96, "FAT mount failed (%d)", (int) ret);
		return false;
	}
	u32 rsize = 524288 + 64;
	u8 *rbuf = memalign(32, rsize);
	u8 *wbuf = write_mb ? memalign(32, 131072) : NULL;
	if (!rbuf || (write_mb && !wbuf)) {
		snprintf(why, 96, "no memory for the FAT buffers");
		ok = false;
	}
	if (ok && file)
		ok = fat_read_phase(file, limit_kb, rbuf, rsize, why);
	if (ok && write_mb) {
		static char dir[] ATTRIBUTE_ALIGN(32) = "/sdbench";
		static char tmp[] ATTRIBUTE_ALIGN(32) = "/sdbench/w.tmp";
		s32 r = IOS_IoctlvFormat(heap, mfd, FAT_MKDIR, "s:", dir);
		say("sdbench: FAT mkdir /sdbench = %d (exists is fine)\n", (int) r);
		IOS_IoctlvFormat(heap, mfd, FAT_DELETE, "s:", tmp);      /* a leftover */
		r = IOS_IoctlvFormat(heap, mfd, FAT_MKFILE, "s:", tmp);
		say("sdbench: FAT create /sdbench/w.tmp = %d\n", (int) r);
		if (r < 0) {
			snprintf(why, 96, "FAT create failed (%d)", (int) r);
			ok = false;
		}
		static const u32 wchunks[] = { 32768, 131072 };
		for (int i = 0; ok && i < 2; i++)
			ok = fat_write_pass(write_mb, wchunks[i], wbuf, rbuf, rsize, why);
		r = IOS_IoctlvFormat(heap, mfd, FAT_DELETE, "s:", tmp);
		say("sdbench: FAT delete /sdbench/w.tmp = %d\n", (int) r);
	}
	free(rbuf);
	free(wbuf);
	ret = IOS_IoctlvFormat(heap, mfd, FAT_UMOUNT, ":");
	say("sdbench: FAT unmount SD (0x%02x) = %d\n", FAT_UMOUNT, (int) ret);
	IOS_Close(mfd);
	return ok;
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
	u32 ios = 249, mb = 32, wait_ms = 0, inits = 1, probes = 0;
	u32 limit_kb = 8192, write_mb = 0;
	const char *file = NULL;
	bool libogc_first = false, big = false;
	char why[96] = "";
	bool bad = false;

	for (int i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--ios=", 6))
			ios = atoi(argv[i] + 6);
		else if (!strncmp(argv[i], "--mb=", 5))
			mb = atoi(argv[i] + 5);
		else if (!strncmp(argv[i], "--wait=", 7))       /* ms to wait before INIT */
			wait_ms = atoi(argv[i] + 7);
		else if (!strncmp(argv[i], "--probe=", 8))      /* IOS's own reset+status, N times */
			probes = atoi(argv[i] + 8);
		else if (!strcmp(argv[i], "--libogc"))          /* libogc's own SD init first */
			libogc_first = true;
		else if (!strncmp(argv[i], "--inits=", 8))      /* INIT calls while it fails */
			inits = atoi(argv[i] + 8);
		else if (!strcmp(argv[i], "--big"))
			big = true;
		else if (!strncmp(argv[i], "--file=", 7))
			file = argv[i] + 7;
		else if (!strncmp(argv[i], "--limit=", 8))
			limit_kb = atoi(argv[i] + 8);
		else if (!strncmp(argv[i], "--fat-write=", 12))
			write_mb = atoi(argv[i] + 12);
	}
	if (limit_kb < 1)
		limit_kb = 1;
	u32 maxsec = big ? 1024 : 255;
	bool fat_run = file || write_mb;
	if (mb < 1)
		mb = 1;

	console_setup();
	hbc_netlog_init();
	crc_init();
	say("sdbench: running on IOS%d v%d; slot %u holds v%u; reads %u MB%s%s\n",
	    IOS_GetVersion(), IOS_GetRevision(), ios, slot_version(ios), mb,
	    write_mb ? "; WRITES a scratch file under /sdbench/" : " (read only)",
	    big ? "; sweep to 1024 sectors" : "");

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

	for (u32 n = 0; n < probes; n++) {
		/* what IOS's own SD driver says: RESETCARD (4), GETSTATUS (0x0b) */
		u32 *st = memalign(32, 32);
		s32 sfd = IOS_Open("/dev/sdio/slot0", 1);
		s32 r1 = -1, r2 = -1;
		if (sfd >= 0 && st) {
			st[0] = 0;
			r1 = IOS_Ioctl(sfd, 4, NULL, 0, st, 4);
			u32 reset = st[0];
			st[0] = 0;
			r2 = IOS_Ioctl(sfd, 0x0b, NULL, 0, st, 4);
			say("sdbench: probe %u: fd %d reset %d (0x%08x) status %d (0x%08x)\n", n, sfd, r1,
			    reset, r2, st[0]);
		} else
			say("sdbench: probe %u: open slot0 = %d\n", n, sfd);
		if (sfd >= 0)
			IOS_Close(sfd);
		free(st);
		usleep(500000);
	}

	if (libogc_first) {
		bool ok = __io_wiisd.startup();
		say("sdbench: libogc __io_wiisd.startup = %d\n", ok);
		__io_wiisd.shutdown();
	}

	/* The IPC heap is 128 KB in all and others hold most of it: it only gets the ioctlv
	 * vectors (a 134 KB heap failed with -22). The data buffer is plain MEM2. */
	heap = iosCreateHeap(4096);
	fd = IOS_Open(dev_sdhc, 0);
	if (heap < 0 || fd < 0) {
		say("sdbench: open /dev/sdio/sdhc: heap %d, fd %d (no such device?)\n", heap, fd);
		say("RESULT fail open /dev/sdio/sdhc heap=%d fd=%d\n", heap, fd);
		return 1;
	}
	u8 *buf = (u8 *) (((u32) SYS_GetArena2Lo() + 31) & ~31u);    /* MEM2, 32-byte aligned */
	SYS_SetArena2Lo(buf + maxsec * SECTOR);
	if (!buf) {
		IOS_Close(fd);
		say("RESULT fail no memory for the buffer\n");
		return 1;
	}

	if (wait_ms)
		usleep(wait_ms * 1000);
	ret = IOS_IoctlvFormat(heap, fd, SDHC_IOCTL_INIT, ":");
	say("sdbench: INIT (0x%02x) = %d\n", SDHC_IOCTL_INIT, ret);
	while (ret != 0 && --inits > 0) {
		ret = IOS_IoctlvFormat(heap, fd, SDHC_IOCTL_INIT, ":");
		say("sdbench: INIT again = %d\n", ret);
	}
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

	dump_regs("after the 1-sector reads");
	sweep(buf, maxsec);
	dump_regs("after the sweep");

	if (fat_run) {
		/* The fallback test would leave the card in the slow mode; the FAT
		   module starts the card itself, so give it the card alone. */
		say("sdbench: the fallback test is skipped when --file or --fat-write run\n");
		IOS_Close(fd);
		fd = -1;
		char fwhy[96] = "";
		if (!fat_phase(file, limit_kb, write_mb, fwhy) && !bad) {
			bad = true;
			snprintf(why, sizeof(why), "%s", fwhy);
		}
	} else if (has_hs) {
		s32 st = IOS_IoctlvFormat(heap, fd, SDHC_IOCTL_HS_FALLBACK, ":");
		if (st == -101) {
			say("sdbench: debug ioctls not in this build (0x21 = -101); fallback check skipped\n");
		} else if (!hs_was_on) {
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

	if (fd >= 0)
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
