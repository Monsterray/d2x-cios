/*
 * d2xdiag: show what a d2x debug build (+dbg, -DD2X_DEBUG) knows, and run
 * fault scenarios through it. See source/cios-lib/d2xdbg.h and
 * docs/debug-build.md.
 *
 *     hbc.py run d2xdiag.dol [--ios=249] [--wd=S] [--scenario=NAME[,NAME...]]
 *
 * Reloads to the cIOS, prints its identity, opens /dev/d2xdbg and prints the
 * previous run's "last words" (what the debug area held before this IOS
 * started: after a watchdog reset, where IOS stopped), then this run's.
 * --wd=S arms the ARM watchdog for S seconds; the app checks in through the
 * debug area while it works. Each scenario sets a fault, runs a short
 * read-only workload (USB or SD sector reads from sector 0, a DI cover
 * status), and checks the module recovered and counted it.
 *
 * Hang scenarios (hang-block, hang-spin, hang-top, usb-spin, usbs-big) hang
 * IOS on purpose: they arm the watchdog (20 s unless --wd says otherwise),
 * save sd:/d2xdiag-pre.txt, start the hang asynchronously, stop checking in
 * and show the debug area live on the TV until the watchdog resets the
 * console. Run them only with someone at the Wii (tests/wii/d2xdiag.py
 * refuses them without --attended). After the reset, run d2xdiag again to
 * read the last words.
 *
 * Output goes to the TV, to the PC through hbc_netlog until the IOS reload,
 * and to sd:/d2xdiag.txt. The last line starts with "RESULT". Writes nothing
 * but its report files: every scenario only reads.
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
#define D2XDBG_HAVE_TYPES       /* gctypes.h, from gccore.h */
#include "../../../source/cios-lib/d2xdbg.h"

/* libogc exports it without a header: reload HBC after a crash */
extern void __exception_setreload(int t);

/* /dev/usb2 (EHCI and USBS alike), source/ehci-module/module.h */
#define UMS_BASE                    (('U' << 24) | ('M' << 16) | ('S' << 8))
#define USB_IOCTL_UMS_INIT          (UMS_BASE + 0x1)
#define USB_IOCTL_UMS_GET_CAPACITY  (UMS_BASE + 0x2)
#define USB_IOCTL_UMS_READ_SECTORS  (UMS_BASE + 0x3)
#define USB_IOCTL_SET_PORT          (UMS_BASE + 0x83)

/* /dev/sdio/sdhc, source/cios-lib/sdhc_server.c */
#define SDHC_IOCTL_INIT             0x01
#define SDHC_IOCTL_READ             0x02

/* /dev/di: cover status, answered by Nintendo's DI module through the DIP */
#define DI_COVER_STATUS             0x88

#define WD_DEFAULT_S                20

static char dev_dbg[] ATTRIBUTE_ALIGN(32) = D2XDBG_DEVICE;
static char dev_usb2[] ATTRIBUTE_ALIGN(32) = "/dev/usb2";
static char dev_sdhc[] ATTRIBUTE_ALIGN(32) = "/dev/sdio/sdhc";
static char dev_di[] ATTRIBUTE_ALIGN(32) = "/dev/di";

static s32 heap = -1, dfd = -1;
static u8 snap[D2XDBG_SIZE] ATTRIBUTE_ALIGN(32);
static u32 iobuf[8] ATTRIBUTE_ALIGN(32);
static char report[32768];
static size_t report_len;
static u32 kick;

static volatile u32 *live;       /* the area, uncached: 0xD37A9000 */

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

/* Through libogc's own SD driver (Nintendo's /dev/sdio/slot0), not d2x's. */
static void save_report(const char *path)
{
	if (!fatMountSimple("sd", &__io_wiisd))
		return;
	FILE *f = fopen(path, "wb");
	if (f) {
		fwrite(report, 1, report_len, f);
		fclose(f);
	}
	fatUnmount("sd:");
}

static void console_setup(void)
{
	VIDEO_Init();
	GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
	void *xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
}

/* The cIOS identity block (as umsbench and USB Loader GX read it) */
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

/* ---- the debug area ---- */

static const char *mod_name(u32 m)
{
	static const char *const n[D2XDBG_MODS] = { "-", "EHCI", "USBS", "SD", "FAT", "DIP", "MLOAD", "?" };
	return m < D2XDBG_MODS ? n[m] : "?";
}

static s32 dbg_ioctl(u32 cmd, const u32 *in, u32 inwords, void *out, u32 outlen)
{
	memset(iobuf, 0, sizeof(iobuf));
	if (in)
		memcpy(iobuf, in, inwords * 4);
	return IOS_Ioctl(dfd, cmd, iobuf, inwords * 4, out, outlen);
}

static int snapshot(void)
{
	u32 off = 0;
	return dbg_ioctl(D2XDBG_IOCTL_READ, &off, 1, snap, sizeof(snap));
}

static void check_in(void)
{
	if (live)
		live[0x60 / 4] = ++kick;
}

static void print_head(const char *title, const u8 *area, u32 records)
{
	const d2xdbg_head *h = (const d2xdbg_head *) area;
	const u32 *ring = (const u32 *) (area + D2XDBG_RING_OFF);

	if (h->magic != D2XDBG_MAGIC || h->layout != D2XDBG_LAYOUT) {
		say("%s: none (magic %08x, layout %u)\n", title, h->magic, h->layout);
		return;
	}
	say("%s: build \"%.31s\" flags %02x, IOS start %u, %u records\n", title, h->build, h->flags,
	    h->boots, h->trace_head);
	say("  watchdog: %u ms%s, %u checks (thread prio %u), fired %s\n", h->wd_timeout_ms,
	    h->wd_timeout_ms ? " armed" : " (off)", h->wd_beats, h->wd_prio,
	    !h->wd_fired ? "no" : h->wd_noreset ? "YES, but the reset did not take" : "YES");
	if (h->map_fixes)
		say("  page entries for the area were gone and put back %u times\n", h->map_fixes);
	if (h->sd_hs)
		say("  SD high speed: %s%s\n", (h->sd_hs & 1) ? "on" : "off", (h->sd_hs & 2) ? ", fell back" : "");
	for (u32 m = 1; m < D2XDBG_MODS; m++) {
		const d2xdbg_mod *c = &h->mod[m];
		if (!c->requests && !c->errors && !c->resets && !c->faults && !c->cur_start)
			continue;
		say("  %-5s req %u err %u retry %u reset %u timeout %u fault %u; longest %u.%03u ms (cmd %x arg %u)%s\n",
		    mod_name(m), c->requests, c->errors, c->retries, c->resets, c->timeouts, c->faults,
		    c->max_ticks / 1898, (c->max_ticks % 1898) * 1000 / 1898, c->max_cmd, c->max_arg,
		    c->cur_start ? " - A REQUEST WAS STILL OPEN" : "");
		if (c->cur_start)
			say("        open: cmd %x arg %u, started at tick %u (%u ms before the last record)\n",
			    c->cur_cmd, c->cur_arg, c->cur_start, (h->now - c->cur_start) / 1898);
	}
	u32 n = h->trace_head < records ? h->trace_head : records;
	u32 first = h->trace_head - n;
	u32 t0 = n ? ring[(first & (D2XDBG_RECORDS - 1)) * 4] : 0;
	for (u32 i = first; i < h->trace_head; i++) {
		const u32 *r = ring + (i & (D2XDBG_RECORDS - 1)) * 4;
		u32 ev = (r[1] >> 16) & 0xFF;
		say("  %8u ms %-5s %c x %04x a %08x b %08x\n", (r[0] - t0) / 1898, mod_name(r[1] >> 24),
		    ev >= 0x20 && ev < 0x7F ? (char) ev : '?', r[1] & 0xFFFF, r[2], r[3]);
	}
}

/* ---- IOS calls with a time limit ---- */

/* A module that never answers must not leave this program waiting too:
   every workload call is sent asynchronously and waited for at most
   CALL_LIMIT_S, checking in meanwhile. A call still out after that is the
   finding: stuck() shows the area live, then the watchdog's answer. */
#define CALL_LIMIT_S        15
#define CALL_STUCK          ((s32) 0x80000001)

static volatile int call_done;
static volatile s32 call_ret;
static const char *call_what;

static s32 call_cb(s32 result, void *usrdata)
{
	(void) usrdata;
	call_ret = result;
	call_done = 1;
	return 0;
}

static s32 call_wait(s32 queued)
{
	u64 start = gettime();

	if (queued < 0)
		return queued;
	while (!call_done) {
		if (ticks_to_secs(diff_ticks(start, gettime())) >= CALL_LIMIT_S)
			return CALL_STUCK;
		check_in();
		usleep(1000);
	}
	return call_ret;
}

/* TIMED("what", IOS_xxxAsync(..., call_cb, NULL)) */
#define TIMED(what, call)   (call_done = 0, call_what = (what), call_wait(call))

/* ---- workloads: read only ---- */

static s32 usb_fd = -1, sd_fd = -1;
static u8 *buf;

static int usb_open(void)
{
	u32 ss = 0;
	if (usb_fd >= 0)
		return 0;
	s32 fd = TIMED("open /dev/usb2", IOS_OpenAsync(dev_usb2, 0, call_cb, NULL));
	if (fd < 0)
		return fd;
	usb_fd = fd;
	TIMED("USB set port", IOS_IoctlvFormatAsync(heap, usb_fd, USB_IOCTL_SET_PORT, call_cb, NULL, "i:", 0));
	s32 r = TIMED("USB init", IOS_IoctlvFormatAsync(heap, usb_fd, USB_IOCTL_UMS_INIT, call_cb, NULL, ":"));
	if (r == CALL_STUCK)
		return r;
	s32 n = TIMED("USB capacity", IOS_IoctlvFormatAsync(heap, usb_fd, USB_IOCTL_UMS_GET_CAPACITY, call_cb, NULL,
							     ":i", &ss));
	if (n == CALL_STUCK)
		return n;
	say("d2xdiag: USB init %d, %d sectors of %u bytes\n", r, n, ss);
	return (n > 0 && ss == 512) ? 0 : -1;
}

static s32 usb_read(u32 lba, u32 count)
{
	return TIMED("USB read", IOS_IoctlvFormatAsync(heap, usb_fd, USB_IOCTL_UMS_READ_SECTORS, call_cb, NULL, "ii:d",
						       lba, count, buf, count * 512));
}

static int sd_open(void)
{
	if (sd_fd >= 0)
		return 0;
	s32 fd = TIMED("open /dev/sdio/sdhc", IOS_OpenAsync(dev_sdhc, 0, call_cb, NULL));
	if (fd < 0)
		return fd;
	sd_fd = fd;
	s32 r = TIMED("SD init", IOS_IoctlvFormatAsync(heap, sd_fd, SDHC_IOCTL_INIT, call_cb, NULL, ":"));
	return r == CALL_STUCK ? r : r ? -1 : 0;
}

static s32 sd_read(u32 lba, u32 count)
{
	return TIMED("SD read", IOS_IoctlvFormatAsync(heap, sd_fd, SDHC_IOCTL_READ, call_cb, NULL, "ii:d", lba, count,
						      buf, count * 512));
}

static s32 di_cover(void)
{
	static u32 in[8] ATTRIBUTE_ALIGN(32), out[8] ATTRIBUTE_ALIGN(32);
	s32 fd = TIMED("open /dev/di", IOS_OpenAsync(dev_di, 0, call_cb, NULL)), r;
	if (fd < 0)
		return fd;
	memset(in, 0, sizeof(in));
	in[0] = DI_COVER_STATUS << 24;
	r = TIMED("DI cover status", IOS_IoctlAsync(fd, DI_COVER_STATUS, in, sizeof(in), out, sizeof(out), call_cb,
						    NULL));
	if (r != CALL_STUCK)
		IOS_Close(fd);
	return r;
}

/* ---- scenarios ---- */

enum work { W_USB, W_SD, W_DI, W_HANG };

struct scenario {
	const char *name;
	enum work work;
	u32 mods[2];            /* modules to set the fault for (EHCI and USBS: whichever is loaded) */
	u32 point, action, count;
	int must_fail;          /* the first request is expected to fail */
	const char *what;
};

#define A(f, arg) (((u32) (f) << 24) | (arg))
#define USB2 { D2XDBG_M_EHCI, D2XDBG_M_USBS }

static const struct scenario scenarios[] = {
	{ "usb-csw-stall", W_USB, USB2, D2XDBG_P_CSW, A(D2XDBG_F_STALL, 0), 1, 0, "the CSW read STALLs once: clear-halt, read again" },
	{ "usb-csw-lost", W_USB, USB2, D2XDBG_P_CSW, A(D2XDBG_F_LOSE, 0), 1, 0, "the CSW never comes once: reset recovery" },
	{ "usb-data-stall", W_USB, USB2, D2XDBG_P_DATA, A(D2XDBG_F_STALL, 0), 1, 0, "the data phase STALLs once" },
	{ "usb-cmd-timeout", W_USB, USB2, D2XDBG_P_CMD, A(D2XDBG_F_TIMEOUT, 0), 1, 0, "the CBW times out once" },
	{ "usb-delay", W_USB, USB2, D2XDBG_P_CMD, A(D2XDBG_F_DELAY, 1500), 1, 0, "the CBW waits 1.5 s once" },
	{ "sd-error", W_SD, { D2XDBG_M_SD, 0 }, D2XDBG_P_REQ, A(D2XDBG_F_ERROR, 0), 1, 0, "an SD transfer fails once: retry" },
	{ "sd-low", W_SD, { D2XDBG_M_SD, 0 }, D2XDBG_P_REQ, A(D2XDBG_F_SDLOW, 0), 1, 0, "SD falls back from high speed" },
	{ "sd-delay", W_SD, { D2XDBG_M_SD, 0 }, D2XDBG_P_REQ, A(D2XDBG_F_DELAY, 1500), 1, 0, "an SD transfer waits 1.5 s once" },
	{ "di-error", W_DI, { D2XDBG_M_DIP, 0 }, D2XDBG_P_DI, A(D2XDBG_F_ERROR, 0), 1, 1, "a DI command fails unrun once" },
	/* These hang IOS on purpose: attended runs only */
	{ "hang-block", W_HANG, { 0, 0 }, 1, 0, 0, 0, "MLOAD's main thread stops itself" },
	{ "hang-spin", W_HANG, { 0, 0 }, 2, 0, 0, 0, "MLOAD's main thread spins" },
	{ "hang-top", W_HANG, { 0, 0 }, 3, 0, 0, 0, "MLOAD's main thread spins at the top priority" },
	{ "usb-spin", W_USB, USB2, D2XDBG_P_DATA, A(D2XDBG_F_SPIN, 0), 1, 0, "the USB module's thread spins in a read" },
	{ "usbs-big", W_USB, { D2XDBG_M_USBS, 0 }, D2XDBG_P_DATA, A(D2XDBG_F_BIG, 0), 1, 0,
	  "base 58 only: one 32 KB transfer to /dev/usb/ven, a real IOS hang" },
};

static int is_hang(const struct scenario *s)
{
	return s->work == W_HANG || D2XDBG_FAULT_ACTION(s->action) == D2XDBG_F_SPIN ||
	       D2XDBG_FAULT_ACTION(s->action) == D2XDBG_F_BLOCK || D2XDBG_FAULT_ACTION(s->action) == D2XDBG_F_BIG;
}

static s32 workload(const struct scenario *s, u32 n)
{
	switch (s->work) {
	case W_USB: return usb_read(n * 64, 64);
	case W_SD:  return sd_read(n * 64, 64);
	case W_DI:  return di_cover();
	default:    return -1;
	}
}

static u32 faults_of(const struct scenario *s)
{
	const d2xdbg_head *h = (const d2xdbg_head *) snap;
	return h->mod[s->mods[0]].faults + (s->mods[1] ? h->mod[s->mods[1]].faults : 0);
}

/* Watch the area live, without checking in, until the watchdog resets the
   console. Returns only if it never does. */
static void watch_until_reset(u32 wd_s)
{
	u64 start = gettime();
	u32 last_beats = 0, same = 0;

	for (;;) {
		u32 s = ticks_to_secs(diff_ticks(start, gettime()));
		u32 beats = live[0x50 / 4], head = live[0x38 / 4];
		const volatile u32 *r = live + D2XDBG_RING_OFF / 4 + ((head - 1) & (D2XDBG_RECORDS - 1)) * 4;
		u32 w = r[1];

		printf("\rd2xdiag: %3u s, watchdog checks %u%s, %u records, last %s %c a %08x, map fixes %u  ", s, beats,
		       beats == last_beats ? " (STOPPED)" : "", head, mod_name(w >> 24), (char) ((w >> 16) & 0xFF), r[2],
		       live[0x5C / 4]);
		same = beats == last_beats ? same + 1 : 0;
		last_beats = beats;
		if (live[0x58 / 4]) {
			say("\nd2xdiag: the watchdog fired after %u s, but its reset did not take: it put everything back, "
			    "turned itself off, and IOS goes on\n", s);
			say("RESULT fail the watchdog's reset (HW_RESETS bit 0) did not reset the console\n");
			return;
		}
		if (s > wd_s + 30) {
			say("\nd2xdiag: no reset %u s after the watchdog's %u s; its thread %s\n", s, wd_s,
			    same > 4 ? "stopped running" : "runs, but did not fire");
			say("RESULT fail watchdog did not reset the console; power-cycle the Wii\n");
			return;
		}
		usleep(500 * 1000);
	}
}

static void copy_live(void)
{
	for (u32 i = 0; i < D2XDBG_HALF / 4; i++)
		((u32 *) snap)[i] = live[i];
}

/* An IOS call that never came back: show where, then what the watchdog does */
static int stuck(const struct scenario *s, u32 wd_s)
{
	say("d2xdiag: %s: \"%s\" did not return in %u s. The debug area as it stands:\n", s->name, call_what,
	    CALL_LIMIT_S);
	copy_live();
	print_head("  stuck", snap, 24);
	say("  in MLOAD's SWI: %08x %08x %08x %08x (0 = not inside)\n", live[D2XDBG_INSVC_OFF / 4],
	    live[D2XDBG_INSVC_OFF / 4 + 1], live[D2XDBG_INSVC_OFF / 4 + 2], live[D2XDBG_INSVC_OFF / 4 + 3]);
	if (wd_s)
		say("d2xdiag: no more check-ins: the watchdog (%u s) should reset the console\n", wd_s);
	else
		say("d2xdiag: the watchdog is off (no --wd): watching for 30 s\n");
	watch_until_reset(wd_s);
	copy_live();
	print_head("  after", snap, 8);
	say("RESULT-SCENARIO %s STUCK in %s\n", s->name, call_what);
	return 1;
}

static int run_scenario(const struct scenario *s, u32 wd_s)
{
	s32 r;

	u32 slot = 0;

	say("d2xdiag: scenario %s: %s\n", s->name, s->what);
	if (s->work == W_USB && (r = usb_open()) < 0) {
		if (r == CALL_STUCK)
			return stuck(s, wd_s);
		say("RESULT-SCENARIO %s skip (no USB drive)\n", s->name);
		return 0;
	}
	if (s->work == W_SD && (r = sd_open()) < 0) {
		if (r == CALL_STUCK)
			return stuck(s, wd_s);
		say("RESULT-SCENARIO %s skip (no SD card through /dev/sdio/sdhc)\n", s->name);
		return 0;
	}
	dbg_ioctl(D2XDBG_IOCTL_CLEAR, NULL, 0, NULL, 0);
	check_in();
	for (u32 i = 0; i < 2 && s->mods[i]; i++) {
		u32 in[5] = { slot++, (s->mods[i] << 8) | s->point, s->count, 0, s->action };
		dbg_ioctl(D2XDBG_IOCTL_FAULT, in, 5, NULL, 0);
	}

	if (is_hang(s)) {
		u32 ms = (wd_s ? wd_s : WD_DEFAULT_S) * 1000;
		static u32 in[8] ATTRIBUTE_ALIGN(32);

		dbg_ioctl(D2XDBG_IOCTL_WD_ARM, &ms, 1, NULL, 0);
		say("d2xdiag: watchdog armed (%u s); hanging IOS now. The console should reset by itself.\n", ms / 1000);
		save_report("sd:/d2xdiag-pre.txt");
		if (s->work == W_HANG) {
			in[0] = s->point;
			IOS_IoctlAsync(dfd, D2XDBG_IOCTL_HANG, in, 4, NULL, 0, NULL, NULL);
		} else {
			IOS_IoctlvFormatAsync(heap, usb_fd, USB_IOCTL_UMS_READ_SECTORS, NULL, NULL, "ii:d", 0, 64,
					      buf, 64 * 512);
		}
		watch_until_reset(ms / 1000);
		return 1;
	}

	u64 t = gettime();
	s32 r1 = workload(s, 0);
	u32 us1 = ticks_to_microsecs(diff_ticks(t, gettime()));
	if (r1 == CALL_STUCK)
		return stuck(s, wd_s);
	check_in();
	s32 r2 = workload(s, 1);
	if (r2 == CALL_STUCK)
		return stuck(s, wd_s);
	check_in();
	snapshot();
	u32 fired = faults_of(s);
	print_head("  area", snap, 24);

	int ok = fired == s->count && r2 == 0 && (s->must_fail ? r1 != 0 && r1 != 1 : r1 == 0);
	if (s->work == W_DI)    /* Nintendo's DI answers 1 for a done command; a dead drive may say more */
		ok = fired == s->count && r1 != r2;
	if (s->name[0] == 's' && !strcmp(s->name, "sd-low"))
		ok = ok && (((const d2xdbg_head *) snap)->sd_hs & 2);
	if (D2XDBG_FAULT_ACTION(s->action) == D2XDBG_F_DELAY)
		ok = ok && us1 >= D2XDBG_FAULT_ARG(s->action) * 1000;
	say("RESULT-SCENARIO %s %s (fired %u of %u; first %d in %u us, then %d)\n", s->name, ok ? "ok" : "FAIL",
	    fired, s->count, r1, us1, r2);
	dbg_ioctl(D2XDBG_IOCTL_FAULT, (u32[5]) { 0 }, 5, NULL, 0);
	dbg_ioctl(D2XDBG_IOCTL_FAULT, (u32[5]) { 1 }, 5, NULL, 0);
	return ok ? 0 : -1;
}

static int diag(int argc, char **argv)
{
	u32 ios = 249, wd_s = 0;
	const char *list = NULL;
	int failed = 0, ran = 0;

	for (int i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--ios=", 6))
			ios = atoi(argv[i] + 6);
		else if (!strncmp(argv[i], "--wd=", 5))
			wd_s = atoi(argv[i] + 5);
		else if (!strncmp(argv[i], "--scenario=", 11))
			list = argv[i] + 11;
	}

	console_setup();
	hbc_netlog_init();
	say("d2xdiag: running on IOS%d v%d; reloading to IOS%u\n", IOS_GetVersion(), IOS_GetRevision(), ios);
	hbc_netlog_close();
	net_deinit();
	s32 ret = IOS_ReloadIOS(ios);
	hbc_netlog_init();
	say("d2xdiag: IOS_ReloadIOS(%u) = %d; now IOS%d v%d\n", ios, ret, IOS_GetVersion(), IOS_GetRevision());
	if (ret < 0 || IOS_GetVersion() != (s32) ios) {
		say("RESULT fail reload ios=%u\n", ios);
		return 1;
	}
	show_cios_identity(ios);

	heap = iosCreateHeap(64 * 512 + 8192);
	buf = heap >= 0 ? iosAlloc(heap, 64 * 512) : NULL;    /* MEM2, aligned: a direct buffer */
	dfd = IOS_Open(dev_dbg, 0);
	if (dfd < 0 || !buf) {
		say("d2xdiag: %s: %d (not a +dbg build?)\n", dev_dbg, dfd);
		say("RESULT fail no %s on IOS%u\n", dev_dbg, ios);
		return 1;
	}
	static u32 info[4] ATTRIBUTE_ALIGN(32);
	ret = dbg_ioctl(D2XDBG_IOCTL_INFO, NULL, 0, info, sizeof(info));
	say("d2xdiag: %s INFO = %d: area at %08x, %u bytes, layout %u, magic %08x\n", dev_dbg, ret, info[0],
	    info[1], info[2], info[3]);
	if (ret < 0 || info[2] != D2XDBG_LAYOUT) {
		say("RESULT fail layout %u, this program reads %u\n", info[2], D2XDBG_LAYOUT);
		return 1;
	}
	live = (volatile u32 *) (0xC0000000 | info[0]);    /* uncached MEM2 */

	if (snapshot() < 0) {
		say("RESULT fail READ\n");
		return 1;
	}
	print_head("last words (the IOS run before this one)", snap + D2XDBG_HALF, 64);
	print_head("this run", snap, 16);

	if (wd_s) {
		u32 ms = wd_s * 1000;
		dbg_ioctl(D2XDBG_IOCTL_WD_ARM, &ms, 1, NULL, 0);
		say("d2xdiag: watchdog armed for %u s; checking in as it works\n", wd_s);
	}

	while (list && *list) {
		const char *end = strchr(list, ',');
		size_t len = end ? (size_t) (end - list) : strlen(list);
		const struct scenario *s = NULL;

		for (u32 i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++)
			if (strlen(scenarios[i].name) == len && !strncmp(scenarios[i].name, list, len))
				s = &scenarios[i];
		if (!s) {
			say("d2xdiag: no scenario '%.*s'\n", (int) len, list);
			failed++;
		} else {
			ran++;
			int r = run_scenario(s, wd_s);
			if (r > 0)
				return 1;       /* a hang scenario that came back: IOS is stuck */
			if (r < 0)
				failed++;
		}
		list = end ? end + 1 : NULL;
		check_in();
	}

	if (wd_s) {
		u32 off = 0;
		dbg_ioctl(D2XDBG_IOCTL_WD_ARM, &off, 1, NULL, 0);
	}
	snapshot();
	if (ran)
		print_head("after the scenarios", snap, 8);
	if (usb_fd >= 0)
		IOS_Close(usb_fd);
	if (sd_fd >= 0)
		IOS_Close(sd_fd);
	IOS_Close(dfd);

	const d2xdbg_head *prev = (const d2xdbg_head *) (snap + D2XDBG_HALF);
	say("RESULT %s ios=%u scenarios=%d failed=%d last_words=%s wd_fired_before=%s\n", failed ? "fail" : "ok",
	    ios, ran, failed, prev->magic == D2XDBG_MAGIC ? "yes" : "no",
	    prev->magic == D2XDBG_MAGIC && prev->wd_fired ? "yes" : "no");
	return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
	__exception_setreload(5);
	int rc = diag(argc, argv);
	save_report("sd:/d2xdiag.txt");
	printf("d2xdiag: done (report saved to sd:/d2xdiag.txt); back to HBC in 3 s\n");
	sleep(3);
	return rc;
}
