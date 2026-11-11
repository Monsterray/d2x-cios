/*
 * mloadsd: check the SD module loader of d2x's MLOAD (docs/mload.md).
 *
 *     hbc.py run mloadsd.dol --ios=249 [--expect=none]
 *
 * Needs, on the SD card, sd:/d2x/mload.txt naming the test module and the
 * module itself in sd:/d2x/mload/ (docs/mload.md, "Testing on a Wii"). Reloads
 * to the cIOS, prints its identity, asks /dev/mload for the loader's report
 * (MLOAD_GET_SD_REPORT) until the loader is done (up to 10 s), prints it, then
 * opens /dev/mloadtest, which the test module registers, and sends it ioctl 1:
 * the answer must be 0x4D4C5453. It writes nothing to the card but the report
 * sd:/mloadsd.txt, and never touches the cIOS or the NAND.
 *
 * --expect=loaded (default): at least one module started and /dev/mloadtest
 *                            answers. RESULT ok or RESULT fail.
 * --expect=none:             no module started (no card, no list, a bad
 *                            module) and /dev/mloadtest does not exist.
 *
 * Output goes to the TV and, until the IOS reload, to the PC through
 * hbc_netlog; the last line starts with "RESULT".
 */
#include <gccore.h>
#include <malloc.h>
#include <fat.h>
#include <sdcard/wiisd_io.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "hbc_netlog.h"

/* libogc exports it without a header: reload HBC after a crash */
extern void __exception_setreload(int t);

#define MLOAD_GET_SD_REPORT 0x4D4C44F0   /* source/mload-module/module.h */
#define MLT_IOCTL_PING      1            /* testmod/main.c */
#define MLT_MAGIC           0x4D4C5453

/* source/mload-module/sdload.h: all fields 32 bits big endian, which is what
   the PPC reads */
#define SDL_NAME_MAX 31
#define SDL_MAX_MODULES 8
#define SDL_REPORT_MAGIC 0x53444D4C
#define SDL_ST_DONE 5

struct sdl_entry {
	char name[SDL_NAME_MAX + 1];
	s32 result;
	s32 detail;
	u32 size;
};

struct sdl_report {
	u32 magic, version, state;
	s32 error;
	u32 count, loaded, skipped, elapsed_ms;
	struct sdl_entry entries[SDL_MAX_MODULES];
};

static const char *error_name(s32 e)
{
	switch (e) {
	case 0:  return "none";
	case -1: return "the FAT module did not answer in time";
	case -2: return "no card, or no FAT volume on it";
	case -3: return "no sd:/d2x/mload.txt";
	case -4: return "the list could not be read";
	case -5: return "the 5 s budget ran out";
	case -6: return "a title was running";
	case -7: return "no timer";
	default: return "unknown";
	}
}

static const char *result_name(s32 r)
{
	switch (r) {
	case 0:   return "started";
	case -10: return "bad name or crc32= in the list";
	case -11: return "could not open the file";
	case -12: return "read error";
	case -13: return "not a module ELF, or truncated";
	case -14: return "outside the module area, over another module, or bad priority";
	case -15: return "crc32 does not match";
	case -16: return "the thread did not start";
	case -17: return "over the 8 module limit";
	case -18: return "never tried: the run ended first";
	default:  return "unknown";
	}
}

static char dev_mload[] ATTRIBUTE_ALIGN(32) = "/dev/mload";
static char dev_test[] ATTRIBUTE_ALIGN(32) = "/dev/mloadtest";
static char report[8192];
static size_t report_len;

/* printf, and keep a copy for sd:/mloadsd.txt */
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

/* Written after the IOS devices are closed, by libogc's own SD driver. The
   only write this program makes. */
static void save_report(void)
{
	if (!fatMountSimple("sd", &__io_wiisd))
		return;
	FILE *f = fopen("sd:/mloadsd.txt", "wb");
	if (f) {
		fwrite(report, 1, report_len, f);
		fclose(f);
	}
	fatUnmount("sd:");
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
 * name[16] and versionstring[16]. */
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

static int check(int argc, char **argv)
{
	u32 ios = 249;
	bool expect_none = false;

	for (int i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--ios=", 6))
			ios = atoi(argv[i] + 6);
		else if (!strcmp(argv[i], "--expect=none"))
			expect_none = true;
	}

	console_setup();
	hbc_netlog_init();
	say("mloadsd: running on IOS%d v%d; reloading to %u; expecting %s\n", IOS_GetVersion(),
	    IOS_GetRevision(), ios, expect_none ? "no module" : "the test module");

	/* Reloading IOS takes the network down: close the log, and tell libogc
	 * its network is gone, so a new hbc_netlog_init() starts it again. */
	hbc_netlog_close();
	net_deinit();
	printf("mloadsd: reloading to IOS%u...\n", ios);
	s32 ret = IOS_ReloadIOS(ios);
	hbc_netlog_init();
	say("mloadsd: IOS_ReloadIOS(%u) = %d; now IOS%d v%d\n", ios, ret, IOS_GetVersion(),
	    IOS_GetRevision());
	if (ret < 0 || IOS_GetVersion() != (s32) ios) {
		say("RESULT fail reload ios=%u\n", ios);
		return 1;
	}
	show_cios_identity(ios);

	s32 heap = iosCreateHeap(4096);
	s32 fd = IOS_Open(dev_mload, 0);
	if (heap < 0 || fd < 0) {
		say("RESULT fail open /dev/mload: heap %d, fd %d\n", heap, fd);
		return 1;
	}

	/* The loader runs at IOS start, one step per 20 ms: poll until it is done */
	struct sdl_report *rep = iosAlloc(heap, sizeof(*rep));
	if (!rep) {
		IOS_Close(fd);
		say("RESULT fail no memory for the report\n");
		return 1;
	}
	for (int tries = 0; tries < 100; tries++) {
		memset(rep, 0, sizeof(*rep));
		ret = IOS_IoctlvFormat(heap, fd, MLOAD_GET_SD_REPORT, ":d", rep, sizeof(*rep));
		if (ret < 0 || rep->magic != SDL_REPORT_MAGIC || rep->state == SDL_ST_DONE)
			break;
		usleep(100000);
	}
	if (ret < 0) {
		say("mloadsd: MLOAD_GET_SD_REPORT = %d (a cIOS without the SD loader?)\n", ret);
		IOS_Close(fd);
		say("RESULT fail report ioctl %d\n", ret);
		return 1;
	}
	if (rep->magic != SDL_REPORT_MAGIC) {
		IOS_Close(fd);
		say("RESULT fail report magic %08x\n", rep->magic);
		return 1;
	}
	say("mloadsd: report v%u: state %u, error %d (%s), %u listed, %u started, %u skipped, %u ms\n",
	    rep->version, rep->state, (int) rep->error, error_name(rep->error), rep->count, rep->loaded,
	    rep->skipped, rep->elapsed_ms);
	for (u32 i = 0; i < rep->count && i < SDL_MAX_MODULES; i++) {
		struct sdl_entry *e = &rep->entries[i];
		e->name[SDL_NAME_MAX] = 0;
		say("mloadsd:   %-20s %d (%s), %s %d, %u bytes\n", e->name, (int) e->result,
		    result_name(e->result), e->result == 0 ? "thread" : "code", (int) e->detail, e->size);
	}
	bool done = rep->state == SDL_ST_DONE;
	u32 loaded = rep->loaded;
	IOS_Close(fd);

	/* The module's own device, if it is there */
	s32 tfd = IOS_Open(dev_test, 0);
	s32 magic = 0;
	if (tfd >= 0) {
		magic = IOS_Ioctl(tfd, MLT_IOCTL_PING, NULL, 0, NULL, 0);
		IOS_Close(tfd);
	}
	say("mloadsd: open /dev/mloadtest = %d; ioctl %d = 0x%08x (expect 0x%08x)\n", (int) tfd,
	    MLT_IOCTL_PING, (unsigned) magic, MLT_MAGIC);

	if (!done) {
		say("RESULT fail the loader was not done after 10 s (state %u)\n", rep->state);
		return 1;
	}
	if (expect_none) {
		if (loaded != 0 || tfd >= 0) {
			say("RESULT fail expected no module: %u started, /dev/mloadtest %d\n", loaded, (int) tfd);
			return 1;
		}
		say("RESULT ok none started, error %d\n", (int) rep->error);
		return 0;
	}
	if (loaded == 0 || tfd < 0 || magic != MLT_MAGIC) {
		say("RESULT fail %u started, /dev/mloadtest %d, answer 0x%08x\n", loaded, (int) tfd,
		    (unsigned) magic);
		return 1;
	}
	say("RESULT ok %u started, /dev/mloadtest answered 0x%08x in %u ms\n", loaded, (unsigned) magic,
	    rep->elapsed_ms);
	return 0;
}

int main(int argc, char **argv)
{
	__exception_setreload(5);
	int rc = check(argc, argv);
	save_report();
	/* Leave the result on the TV for a moment before HBC comes back */
	printf("mloadsd: done (report saved to sd:/mloadsd.txt); back to HBC in 3 s\n");
	sleep(3);
	return rc;
}
