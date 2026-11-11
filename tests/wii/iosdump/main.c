/*
 * iosdump: back up installed IOS titles (TMD, ticket, contents) to the SD card.
 *
 *     hbc.py run iosdump.dol [--from=200] [--to=255] [--out=sd:/iosdump] [--list]
 *
 * For every installed IOS in the range, copies the NAND's own files, byte for
 * byte, to OUT/<slot>/: title.tmd, title.tik (/ticket/00000001/<id>.tik) and
 * each content as <cid>.app (shared contents from /shared1, found through
 * content.map by hash), and the NAND's /sys/cert.sys to OUT. Contents on the NAND are decrypted; tools/iosdump.py
 * checks each against the TMD's SHA-1 and can pack a WAD that restores the slot
 * exactly. --list only lists the slots and their TMD versions.
 *
 * Reads only. It lifts IOS's NAND permission check the way hbc-reborn does
 * (needs AHBPROT, which HBC gives a sent app) and puts it back before exiting.
 * Report on the TV, through hbc_netlog, and in OUT/iosdump.txt; its last line
 * starts with "RESULT".
 */
#include <gccore.h>
#include <fat.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ogc/machine/processor.h>
#include "hbc_netlog.h"

#define MEM2_PROT 0x0D8B420A

static char report[32768];
static size_t report_len;

static void __attribute__((format(printf, 1, 2))) say(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	fputs(line, stdout);	/* hbc_netlog copies stdout to the PC */
	size_t len = strlen(line);
	if (report_len + len < sizeof(report)) {
		memcpy(report + report_len, line, len);
		report_len += len;
	}
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
	printf("\n\n");
}

/* IOS's FS permission check, `cmp r3, r1; beq allowed` (hbc-reborn nandperm.c) */
static const u8 perm_check[] = { 0x42, 0x8B, 0xD0, 0x01, 0x25, 0x66 };
static u16 mem2_prot;

static int nand_patch(bool lift)
{
	u8 *p = (u8 *) *(u32 *) 0x80003134, *end = (u8 *) 0x94000000;
	u8 from = lift ? 0xD0 : 0xE0, to = lift ? 0xE0 : 0xD0;
	int n = 0;

	if (read32(0x0d800064) != 0xffffffff || (u32) p < 0x93000000 || (u32) p >= 0x94000000)
		return -1;
	if (lift)
		mem2_prot = read16(MEM2_PROT);
	write16(MEM2_PROT, 2);
	for (; p < end - sizeof(perm_check); ++p) {
		if (p[0] == perm_check[0] && p[1] == perm_check[1] && p[2] == from &&
				!memcmp(p + 3, perm_check + 3, sizeof(perm_check) - 3)) {
			p[2] = to;
			DCFlushRange((void *) ((u32) p & ~31), 64);
			ICInvalidateRange((void *) ((u32) p & ~31), 64);
			++n;
		}
	}
	if (!lift || !n)
		write16(MEM2_PROT, mem2_prot);
	return n;
}

/* A whole NAND file into a new buffer; size in *len. NULL on failure (*err). */
static u8 *nand_read(const char *path, u32 *len, s32 *err)
{
	static char p[ISFS_MAXPATH] ATTRIBUTE_ALIGN(32);
	static fstats st ATTRIBUTE_ALIGN(32);
	strncpy(p, path, sizeof(p) - 1);
	s32 fd = ISFS_Open(p, ISFS_OPEN_READ);
	if (fd < 0) {
		*err = fd;
		return NULL;
	}
	s32 r = ISFS_GetFileStats(fd, &st);
	if (r < 0) {
		ISFS_Close(fd);
		*err = r;
		return NULL;
	}
	u8 *buf = memalign(32, (st.file_length + 31) & ~31);
	if (!buf) {
		ISFS_Close(fd);
		*err = -1000;
		return NULL;
	}
	u32 done = 0;
	while (done < st.file_length) {
		u32 chunk = st.file_length - done > 0x10000 ? 0x10000 : st.file_length - done;
		r = ISFS_Read(fd, buf + done, chunk);
		if (r <= 0) {
			free(buf);
			ISFS_Close(fd);
			*err = r ? r : -1001;
			return NULL;
		}
		done += r;
	}
	ISFS_Close(fd);
	*len = st.file_length;
	return buf;
}

static bool sd_write(const char *path, const void *data, u32 len)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	bool ok = fwrite(data, 1, len, f) == len;
	return fclose(f) == 0 && ok;
}

static u8 *content_map;
static u32 content_map_len;

/* /shared1/<name>.app for a shared content, by its SHA-1 */
static bool shared_path(const u8 *hash, char *out)
{
	for (u32 o = 0; o + 28 <= content_map_len; o += 28) {
		if (!memcmp(content_map + o + 8, hash, 20)) {
			sprintf(out, "/shared1/%.8s.app", (const char *) content_map + o);
			return true;
		}
	}
	return false;
}

static u16 stored_version(u32 slot)
{
	static u8 buf[MAX_SIGNED_TMD_SIZE] ATTRIBUTE_ALIGN(32);
	u64 tid = 0x0000000100000000ULL | slot;
	u32 size = 0;
	if (ES_GetStoredTMDSize(tid, &size) < 0 || size > sizeof(buf))
		return 0;
	if (ES_GetStoredTMD(tid, (signed_blob *) buf, size) < 0)
		return 0;
	return ((tmd *) SIGNATURE_PAYLOAD((signed_blob *) buf))->title_version;
}

/* Returns the number of files that failed. */
static int dump_slot(u32 slot, const char *out)
{
	char path[ISFS_MAXPATH], dst[256], dir[200];
	u32 len = 0;
	s32 err = 0;
	int failed = 0;

	snprintf(dir, sizeof(dir), "%s/%u", out, slot);
	mkdir(dir, 0777);

	sprintf(path, "/title/00000001/%08x/content/title.tmd", slot);
	u8 *t = nand_read(path, &len, &err);
	if (!t) {
		say("  IOS%u: %s: error %d\n", slot, path, err);
		return 1;
	}
	snprintf(dst, sizeof(dst), "%s/title.tmd", dir);
	failed += !sd_write(dst, t, len);
	tmd *m = (tmd *) SIGNATURE_PAYLOAD((signed_blob *) t);
	say("  IOS%u: v%u, %u contents, tmd %u bytes\n", slot, m->title_version, m->num_contents, len);

	sprintf(path, "/ticket/00000001/%08x.tik", slot);
	u32 tlen = 0;
	u8 *k = nand_read(path, &tlen, &err);
	if (k) {
		snprintf(dst, sizeof(dst), "%s/title.tik", dir);
		failed += !sd_write(dst, k, tlen);
		free(k);
	} else {
		say("    ticket: error %d\n", err);
		failed++;
	}

	for (u32 i = 0; i < m->num_contents; i++) {
		tmd_content *c = &m->contents[i];
		if (c->type & 0x8000) {
			if (!shared_path(c->hash, path)) {
				say("    content %08x (shared): not in content.map\n", c->cid);
				failed++;
				continue;
			}
		} else {
			sprintf(path, "/title/00000001/%08x/content/%08x.app", slot, c->cid);
		}
		u32 clen = 0;
		u8 *d = nand_read(path, &clen, &err);
		if (!d) {
			say("    content %08x: %s: error %d\n", c->cid, path, err);
			failed++;
			continue;
		}
		snprintf(dst, sizeof(dst), "%s/%08x.app", dir, c->cid);
		if (!sd_write(dst, d, clen)) {
			say("    content %08x: writing %s failed\n", c->cid, dst);
			failed++;
		}
		if ((u64) clen != c->size)
			say("    content %08x: %u bytes, TMD says %llu\n", c->cid, clen, c->size);
		free(d);
	}
	free(t);
	return failed;
}

int main(int argc, char **argv)
{
	u32 from = 200, to = 255;
	const char *out = "sd:/iosdump";
	bool list = false;
	for (int i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--from=", 7))
			from = atoi(argv[i] + 7);
		else if (!strncmp(argv[i], "--to=", 5))
			to = atoi(argv[i] + 5);
		else if (!strncmp(argv[i], "--out=", 6))
			out = argv[i] + 6;
		else if (!strcmp(argv[i], "--list"))
			list = true;
	}
	if (to > 255)
		to = 255;

	console_setup();
	hbc_netlog_init();
	say("iosdump: running on IOS%d v%d; IOS %u-%u\n", IOS_GetVersion(), IOS_GetRevision(), from, to);

	u32 slots[256], n = 0;
	for (u32 s = from; s <= to; s++) {
		u16 v = stored_version(s);
		if (v) {
			slots[n++] = s;
			say("  IOS%u: v%u\n", s, v);
		}
	}
	say("iosdump: %u installed\n", n);
	if (list) {
		say("RESULT listed=%u\n", n);
		return 0;
	}

	if (!fatInitDefault()) {
		say("RESULT error=sd\n");
		return 1;
	}
	mkdir(out, 0777);
	int patched = nand_patch(true);
	say("iosdump: NAND permission check lifted at %d site(s)\n", patched);
	if (patched <= 0) {
		say("RESULT error=nandperm (no AHBPROT?)\n");
		return 1;
	}
	ISFS_Initialize();
	s32 err = 0;
	content_map = nand_read("/shared1/content.map", &content_map_len, &err);
	if (!content_map)
		say("iosdump: /shared1/content.map: error %d (shared contents will fail)\n", err);

	/* the certificate chain a WAD carries (CA, CP, XS) */
	int failed = 0;
	u32 clen = 0;
	u8 *certs = nand_read("/sys/cert.sys", &clen, &err);
	char cp[256];
	snprintf(cp, sizeof(cp), "%s/cert.sys", out);
	if (!certs || !sd_write(cp, certs, clen)) {
		say("iosdump: /sys/cert.sys: error %d\n", certs ? -1 : err);
		failed++;
	}
	free(certs);
	for (u32 i = 0; i < n; i++)
		failed += dump_slot(slots[i], out);

	ISFS_Deinitialize();
	nand_patch(false);
	say("iosdump: NAND permission check put back\n");
	say("RESULT dumped=%u failed=%d\n", n, failed);

	char rp[256];
	snprintf(rp, sizeof(rp), "%s/iosdump.txt", out);
	sd_write(rp, report, report_len);
	fatUnmount("sd:");
	return failed ? 1 : 0;
}
