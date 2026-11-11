/*
 * Host test of mload-module/sdload.c: the list parser, the name check, the
 * CRC, the ELF validator and the whole state machine, against a fake of the
 * IOS calls it makes (the FAT module, the timer, the clock, the thread
 * start). The module's area (exe_mem) is an array here.
 */
#define HOSTCHECK_MAIN
#include "hostcheck.h"

#include <stdint.h>
#include <stdlib.h>

/* The module area is 0x13700000 + 512 KB on the Wii; here it is an array */
#define SDL_AREA_BASE	0x13700000u
#define SDL_AREA_SIZE	0x80000u
static uint8_t area[SDL_AREA_SIZE];
#define SDL_PTR(a)	((void *)(area + ((uint32_t)(a) - SDL_AREA_BASE)))

#include "sdload.c"

#define BASE	SDL_AREA_BASE
#define TICK	37960u	/* 20 ms of HW_TIMER ticks */
#define MS	1898u


/* ---- The fake IOS ---- */

#define NFILES	16
#define NFDS	8

static struct {
	char     path[80];
	const uint8_t *data;
	uint32_t len;
} files[NFILES];
static int nfiles;

static uint8_t  arena[1 << 21];
static uint32_t arena_used;

static uint32_t clk;
static int running_title;

static struct {
	int fat_up_after;	/* "fat" is refused this many times first */
	int fat_tries;
	int card;		/* the mount works */
	int mounts, umounts;
	int open_fds;		/* opened and not closed */
	int reads, read_fail_at, title_at_read;
	uint32_t cost_open, cost_read, cost_ioctl;
	int timer_fail, timer_created, timer_stopped, timer_destroyed;
	int armed, restarts, overqueue, delay_us;	/* the one-shot timer: armed = a message is due or queued */
	int fired, dead_restarts;	/* fired: it sent its message and was not stopped since; a restart then arms nothing (IOS) */
	int flag, flag_opens;		/* the flag file on the NAND; -1: the file system errs */
	int sleeps;
	int fat_calls;			/* any open of "fat" or a file under it, mount, unmount */
	int thread_fail;
	int fd_file[NFDS];
	uint32_t fd_pos[NFDS];
	int opens;
	char opened[8][80];
	int nopened;
	struct { uintptr_t start, arg, stack; uint32_t ssize, prio; } th[16];
	int nth;
	int cache_syncs, icinv, callfuncs;
} sim;

static void reset(void)
{
	memset(&sim, 0, sizeof(sim));
	sim.card = 1;
	nfiles = 0;
	arena_used = 0;
	clk = 0x1000;
	running_title = 0;
	memset(area, 0xAA, sizeof(area));
	memset(&S, 0, sizeof(S));
	S.fatfd = S.timer = S.m.fd = -1;
	memset(sdl_buf, 0, sizeof(sdl_buf));
	memset(sdl_head, 0, sizeof(sdl_head));
}

static uint8_t *arena_alloc(uint32_t n)
{
	uint8_t *p = arena + arena_used;

	arena_used += (n + 31) & ~31u;
	if (arena_used > sizeof(arena))
		hc_hang("test arena full");
	return p;
}

static void add_file(const char *path, const void *data, uint32_t len)
{
	uint8_t *p = arena_alloc(len);

	memcpy(p, data, len);
	snprintf(files[nfiles].path, sizeof(files[nfiles].path), "%s", path);
	files[nfiles].data = p;
	files[nfiles].len = len;
	nfiles++;
}

static void add_list(const char *text)
{
	add_file("fat/d2x/mload.txt", text, (uint32_t)strlen(text));
}

static void add_mod(const char *name, const void *data, uint32_t len)
{
	char path[80];

	snprintf(path, sizeof(path), "fat/d2x/mload/%s", name);
	add_file(path, data, len);
}

s32 os_open(const char *device, s32 mode)
{
	int i, slot;

	sim.opens++;
	clk += sim.cost_open;

	if (!strcmp(device, SDL_FLAG_PATH)) {
		sim.flag_opens++;
		if (sim.flag < 0)
			return -102;
		if (!sim.flag)
			return -106;
		sim.open_fds++;
		return 99;
	}

	sim.fat_calls++;
	if (!strcmp(device, "fat")) {
		if (++sim.fat_tries <= sim.fat_up_after)
			return -6;
		sim.open_fds++;
		return 1;
	}

	if (sim.nopened < 8)
		snprintf(sim.opened[sim.nopened++], 80, "%s", device);

	for (i = 0; i < nfiles; i++) {
		if (strcmp(files[i].path, device))
			continue;
		for (slot = 0; slot < NFDS; slot++)
			if (!sim.fd_file[slot])
				break;
		if (slot == NFDS)
			hc_hang("too many open files");
		sim.fd_file[slot] = i + 1;
		sim.fd_pos[slot] = 0;
		sim.open_fds++;
		return 100 + slot;
	}
	return -106;
}

s32 os_close(s32 fd)
{
	sim.open_fds--;
	if (fd == 99)
		return 0;
	if (fd >= 100)
		sim.fd_file[fd - 100] = 0;
	return 0;
}

s32 os_read(s32 fd, void *d, s32 len)
{
	int slot = fd - 100;
	uint32_t left, n;

	if (fd < 100 || !sim.fd_file[slot])
		hc_hang("read on a bad fd %d", (int)fd);
	if ((uintptr_t)d & 3)
		hc_hang("read into an unaligned buffer");

	sim.reads++;
	clk += sim.cost_read;
	if (sim.title_at_read && sim.reads == sim.title_at_read)
		running_title = 1;
	if (sim.read_fail_at && sim.reads == sim.read_fail_at)
		return -1;

	left = files[sim.fd_file[slot] - 1].len - sim.fd_pos[slot];
	n = (uint32_t)len < left ? (uint32_t)len : left;
	memcpy(d, files[sim.fd_file[slot] - 1].data + sim.fd_pos[slot], n);
	sim.fd_pos[slot] += n;
	return (s32)n;
}

s32 os_seek(s32 fd, s32 offset, s32 mode)
{
	if (fd < 100 || mode != 0)
		hc_hang("bad seek");
	sim.fd_pos[fd - 100] = (uint32_t)offset;
	return offset;
}

s32 os_ioctlv(s32 fd, s32 request, s32 bytes_in, s32 bytes_out, ioctlv *vector)
{
	if (fd != 1)
		hc_hang("ioctlv on fd %d, not the FAT module", (int)fd);

	sim.fat_calls++;
	if (request == 0xF0) {
		if (bytes_in != 1 || bytes_out != 0 || *(s32 *)vector[0].data != -1)
			hc_hang("bad mount request");
		sim.mounts++;
		clk += sim.cost_ioctl;
		return sim.card ? 0 : -101;
	}
	if (request == 0xF1) {
		sim.umounts++;
		return 0;
	}
	hc_hang("unknown FAT ioctl %x", (int)request);
	return -1;
}

s32 os_create_timer(s32 time_us, s32 repeat_time_us, s32 message_queue, s32 message)
{
	if (time_us != 20000 || repeat_time_us != 0 || message_queue != 5 || message != SDL_COOKIE)
		hc_hang("bad timer arguments");
	sim.delay_us = time_us;
	if (sim.timer_fail)
		return -5;
	sim.timer_created++;
	sim.armed = 1;
	return 7;
}

s32 os_restart_timer(s32 id, s32 time_us, s32 repeat_time_us)
{
	if (id != 7 || (time_us != 20000 && time_us != 1000) || repeat_time_us != 0)
		hc_hang("bad restart of timer %d", (int)id);
	sim.delay_us = time_us;
	if (sim.fired) {
		/* IOS: restarting a one-shot timer that already fired returns 0 and
		   never sends again; only a stop makes it restartable */
		sim.dead_restarts++;
		return 0;
	}
	if (sim.armed)
		sim.overqueue++;	/* a second message while one is still due */
	sim.armed = 1;
	sim.restarts++;
	return 0;
}

void Timer_Sleep(u32 us)
{
	sim.sleeps++;
	clk += us / 1000 * MS;
}

s32 os_stop_timer(s32 id)
{
	if (id != 7)
		hc_hang("stop of timer %d", (int)id);
	sim.timer_stopped++;
	sim.fired = 0;
	return 0;
}

s32 os_destroy_timer(s32 id)
{
	if (id != 7)
		hc_hang("destroy of timer %d", (int)id);
	sim.timer_destroyed++;
	return 0;
}

void __os_sync_before_read(void *p, s32 n)
{
	sim.cache_syncs++;
}

void __os_sync_after_write(void *p, s32 n)
{
	sim.cache_syncs++;
}

s32 Swi_MLoad(u32 a0, u32 a1, u32 a2, u32 a3)
{
	if (a0 == 3 && a1 == 0x0D800010)
		return (s32)clk;
	if (a0 == 33)
		return running_title;
	hc_hang("Swi_MLoad(%u, %x)", a0, a1);
	return 0;
}

s32 Swi_CallFunc(s32 (*func)(void *in, void *out), void *in, void *out)
{
	sim.callfuncs++;
	return func(in, out);
}

void ICInvalidate(void)
{
	sim.icinv++;
}

s32 Elf_RunThread(void *start, void *arg, void *stack, u32 stacksize, u32 priority)
{
	if (sim.thread_fail)
		return -4;
	sim.th[sim.nth].start = (uintptr_t)start;
	sim.th[sim.nth].arg = (uintptr_t)arg;
	sim.th[sim.nth].stack = (uintptr_t)stack;
	sim.th[sim.nth].ssize = stacksize;
	sim.th[sim.nth].prio = priority;
	return 0x40 + sim.nth++;
}


/* ---- Building modules ---- */

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void put16(uint8_t *p, uint32_t v)
{
	p[0] = v >> 8; p[1] = v;
}

struct elfp {
	uint32_t vaddr, filesz, memsz;
	uint32_t entry, stack, stack_size, prio, arg;
	uint32_t note_first;	/* the note table's first word: 0 */
	int      with_note;
};

/* A module the way stripios leaves one: the header, a NOTE and a table LOAD
   (at address 0) up front, then the code segment at 256. */
static struct elfp defp(uint32_t vaddr)
{
	struct elfp p = {0};

	p.vaddr = vaddr;
	p.filesz = 5000;
	p.memsz = (5000 + 0x1000 + 31) & ~31u;	/* whole cache lines */
	p.entry = vaddr + 0x11;
	p.stack = vaddr + p.memsz;
	p.stack_size = 0x800;
	p.prio = 0x70;
	p.arg = 0xB4;
	p.with_note = 1;
	return p;
}

static uint32_t mk_elf(uint8_t *out, const struct elfp *p)
{
	uint32_t i, len = 256 + p->filesz;
	uint8_t *ph = out + 52, *note = out + 148;

	memset(out, 0, len);
	put32(out, 0x7F454C46);
	put32(out + 4, 0x01020161);
	put32(out + 8, 0x01000000);
	put16(out + 16, 2);
	put16(out + 18, 0x28);
	put32(out + 20, 1);
	put32(out + 24, p->entry);
	put32(out + 28, 52);
	put16(out + 40, 52);
	put16(out + 42, 32);
	put16(out + 44, 3);

	/* NOTE */
	put32(ph, 4); put32(ph + 4, 148); put32(ph + 16, 44); put32(ph + 20, 44);
	/* the table, at address 0 */
	put32(ph + 32, 1); put32(ph + 36, 192); put32(ph + 48, 0x28); put32(ph + 52, 0x28);
	/* the code */
	put32(ph + 64, 1); put32(ph + 68, 256); put32(ph + 72, p->vaddr);
	put32(ph + 80, p->filesz); put32(ph + 84, p->memsz);

	put32(note, p->note_first);
	put32(note + 4, 0x0B);  put32(note + 8, p->arg);
	put32(note + 12, 0x09); put32(note + 16, p->entry);
	put32(note + 20, 0x7D); put32(note + 24, p->prio);
	put32(note + 28, 0x7E); put32(note + 32, p->stack_size);
	put32(note + 36, 0x7F); put32(note + 40, p->stack);
	if (!p->with_note)
		put32(ph, 7);	/* no NOTE segment */

	for (i = 0; i < p->filesz; i++)
		out[256 + i] = (uint8_t)(i * 7 + 3);
	return len;
}

static uint32_t elf_in_arena(const struct elfp *p, uint8_t **out)
{
	uint8_t tmp[256 + 0x10000];
	uint32_t len = mk_elf(tmp, p);

	*out = arena_alloc(len);
	memcpy(*out, tmp, len);
	return len;
}

static void add_elf(const char *name, const struct elfp *p)
{
	uint8_t *e;
	uint32_t len = elf_in_arena(p, &e);

	add_mod(name, e, len);
}

static uint32_t crc_of(const uint8_t *d, uint32_t n)
{
	return ~sdl_crc32(0xFFFFFFFF, d, n);
}

/* The timer fires: its delay passes and MLOAD's loop calls SdLoad_Tick */
static void fire(void)
{
	sim.armed = 0;
	sim.fired = 1;
	clk += sim.delay_us / 1000 * MS;
	SdLoad_Tick();
}

/* The boot-time run: the flag file is there, MLOAD's loop delivers the timer's
   message each time it is armed */
static void run_loader(int max_ticks)
{
	int i;

	sim.flag = 1;
	SdLoad_Boot(5);
	for (i = 0; i < max_ticks && S.state != SDL_ST_DONE && sim.armed; i++) {
		fire();
	}
}

static s32 get_rep_vec(sdl_report *r, u32 inlen, u32 iolen)
{
	ioctlv v[2];

	memset(v, 0, sizeof(v));
	v[0].data = r;
	v[0].len = sizeof(*r);
	return SdLoad_IoctlReport(v, inlen, iolen);
}

static int get_rep(sdl_report *r)
{
	return get_rep_vec(r, 0, 1);
}

/* MLOAD_SD_LOAD from the PPC */
static s32 sd_load(sdl_report *r)
{
	ioctlv v[2];

	memset(v, 0, sizeof(v));
	v[0].data = r;
	v[0].len = sizeof(*r);
	return SdLoad_IoctlLoad(v, 0, 1);
}


/* ---- The parser ---- */

static u32 parse(const char *s, int full, sdl_item *it, u32 *extra)
{
	return sdl_parse(s, (u32)strlen(s), full, it, SDL_MAX_MODULES, extra);
}

static void test_parse_basic(void)
{
	sdl_item it[8];
	u32 extra, n;

	n = parse("alpha\nbeta crc32=DEADbeef\n", 0, it, &extra);
	CHECK(n == 2 && extra == 0, "n=%u extra=%u", n, extra);
	CHECK(!strcmp(it[0].name, "alpha") && it[0].name_ok && !it[0].has_crc, "first");
	CHECK(!strcmp(it[1].name, "beta") && it[1].name_ok && it[1].has_crc && it[1].crc == 0xDEADBEEF,
	      "second: ok=%d has=%d crc=%x", it[1].name_ok, it[1].has_crc, it[1].crc);
}

static void test_parse_comments_and_space(void)
{
	sdl_item it[8];
	u32 extra, n;

	n = parse("# a comment\n\n   \n\t  x.elf \t\n  # indented comment\n  y-1_2.app\tcrc32=00000001  \n", 0, it, &extra);
	CHECK(n == 2, "n=%u", n);
	CHECK(!strcmp(it[0].name, "x.elf") && it[0].name_ok && !it[0].has_crc, "x");
	CHECK(!strcmp(it[1].name, "y-1_2.app") && it[1].has_crc && it[1].crc == 1, "y");
}

static void test_parse_crlf(void)
{
	sdl_item it[8];
	u32 extra, n;

	n = parse("one\r\ntwo crc32=0000ABCD\r\n\r\n# c\r\nthree\r\n", 0, it, &extra);
	CHECK(n == 3, "n=%u", n);
	CHECK(!strcmp(it[0].name, "one") && it[0].name_ok, "one");
	CHECK(!strcmp(it[1].name, "two") && it[1].has_crc && it[1].crc == 0xABCD, "two");
	CHECK(!strcmp(it[2].name, "three") && it[2].name_ok, "three");
}

static void test_parse_bom(void)
{
	sdl_item it[8];
	u32 extra, n;

	n = parse("\xEF\xBB\xBF" "first\nsecond\n", 0, it, &extra);
	CHECK(n == 2 && !strcmp(it[0].name, "first") && it[0].name_ok, "bom: n=%u name=%s", n, it[0].name);

	/* a BOM only counts at the start of the file */
	n = parse("a\n\xEF\xBB\xBF" "b\n", 0, it, &extra);
	CHECK(n == 2 && !it[1].name_ok, "a BOM inside is not a name");
}

static void test_parse_no_final_newline(void)
{
	sdl_item it[8];
	u32 extra, n;

	n = parse("a\nlast", 0, it, &extra);
	CHECK(n == 2 && !strcmp(it[1].name, "last") && it[1].name_ok, "n=%u", n);
	n = parse("a\nlast crc32=12345678", 0, it, &extra);
	CHECK(n == 2 && it[1].has_crc && it[1].crc == 0x12345678, "crc on the last line");
}

static void test_parse_bad_names(void)
{
	static const char *bad[] = {
		"../x", "a/b", "/abs", ".hidden", "a..b", "..", "a b c", "name;rm",
		"0123456789012345678901234567890123456789",	/* 40 characters */
		"x crc32=ZZZZZZZZ", "x crc32=1234567", "x crc32=123456789", "x crc=00000000",
		"x crc32=0000000g", "x   extra", "back\\slash", "colon:name",
	};
	size_t i;

	for (i = 0; i < HC_COUNT(bad); i++) {
		sdl_item it[8];
		u32 extra, n;
		char line[100];

		snprintf(line, sizeof(line), "%s\n", bad[i]);
		n = parse(line, 0, it, &extra);
		CHECK(n == 1, "'%s' is an entry (n=%u)", bad[i], n);
		CHECK(!it[0].name_ok, "'%s' must be a bad entry", bad[i]);
	}
}

static void test_name_ok_limits(void)
{
	char name[40];

	memset(name, 'a', sizeof(name));
	CHECK(sdl_name_ok(name, 31), "31 characters pass");
	CHECK(!sdl_name_ok(name, 32), "32 do not");
	CHECK(!sdl_name_ok(name, 0), "empty does not");
	CHECK(sdl_name_ok("A.b_c-9", 7), "mixed");
	CHECK(sdl_name_ok("x.", 2), "a trailing dot is a name");
	CHECK(!sdl_name_ok("a\0b", 3), "a NUL inside");
	CHECK(!sdl_name_ok("a\n", 2), "a newline inside");
	CHECK(!sdl_name_ok("\xC3\xA9", 2), "non-ASCII");
}

static void test_parse_long_lines(void)
{
	static char text[1000];
	sdl_item it[8];
	u32 extra, n;

	/* a long comment is fine */
	text[0] = '#';
	memset(text + 1, 'x', 500);
	strcpy(text + 501, "\nok\n");
	n = parse(text, 0, it, &extra);
	CHECK(n == 1 && !strcmp(it[0].name, "ok"), "comment then ok: n=%u", n);

	/* a long name is a bad entry whose name is cut, not an overflow */
	memset(text, 'n', 600);
	strcpy(text + 600, "\nafter\n");
	n = parse(text, 0, it, &extra);
	CHECK(n == 2, "n=%u", n);
	CHECK(!it[0].name_ok && strlen(it[0].name) == 31, "cut name %zu", strlen(it[0].name));
	CHECK(!strcmp(it[1].name, "after") && it[1].name_ok, "the next line is read");
}

static void test_parse_over_limit(void)
{
	sdl_item it[8];
	u32 extra, n;
	char text[200] = "";
	int i;

	for (i = 0; i < 11; i++)
		sprintf(text + strlen(text), "m%d\n", i);
	n = parse(text, 0, it, &extra);
	CHECK(n == 8 && extra == 3, "n=%u extra=%u", n, extra);
	CHECK(!strcmp(it[7].name, "m7"), "the first eight are kept");
}

static void test_parse_truncated_tail(void)
{
	sdl_item it[8];
	u32 extra, n;

	/* the buffer was filled to its limit: a last line without a newline may be cut */
	n = parse("aa\nbb\ncc", 1, it, &extra);
	CHECK(n == 2 && !strcmp(it[1].name, "bb"), "partial line dropped: n=%u", n);
	/* a last line that does end in a newline is whole */
	n = parse("aa\nbb\ncc\n", 1, it, &extra);
	CHECK(n == 3, "whole last line kept: n=%u", n);
	/* the file was short: no newline is fine */
	n = parse("aa\nbb\ncc", 0, it, &extra);
	CHECK(n == 3, "short file: n=%u", n);
}

static void test_parse_empty(void)
{
	sdl_item it[8];
	u32 extra;

	CHECK(parse("", 0, it, &extra) == 0, "empty");
	CHECK(parse("\n\n\r\n# x\n", 0, it, &extra) == 0, "only blanks and comments");
	CHECK(sdl_parse("", 0, 0, it, 8, &extra) == 0, "zero length");
}


/* ---- CRC ---- */

static void test_crc32(void)
{
	uint8_t d[1000];
	u32 c, i;

	CHECK(crc_of((const uint8_t *)"123456789", 9) == 0xCBF43926, "the check value: %08x",
	      crc_of((const uint8_t *)"123456789", 9));
	CHECK(crc_of((const uint8_t *)"", 0) == 0, "empty is 0");

	for (i = 0; i < sizeof(d); i++)
		d[i] = (uint8_t)(i * 31 + 7);
	c = sdl_crc32(0xFFFFFFFF, d, 333);
	c = sdl_crc32(c, d + 333, sizeof(d) - 333);
	CHECK(~c == crc_of(d, sizeof(d)), "in pieces equals in one go");
}


/* ---- The ELF validator ---- */

static s32 check(const uint8_t *e, uint32_t len, const sdl_range *taken, u32 nt)
{
	static sdl_elf o;

	return sdl_check_elf(e, len < SDL_HEAD ? len : SDL_HEAD, len, taken, nt, &o);
}

static void test_elf_good(void)
{
	struct elfp p = defp(BASE + 0x60000);
	uint8_t *e;
	uint32_t len = elf_in_arena(&p, &e);
	static sdl_elf o;
	s32 r = sdl_check_elf(e, SDL_HEAD, len, NULL, 0, &o);

	CHECK(r == 0, "good module: %d", (int)r);
	CHECK(o.nseg == 1 && o.seg[0].vaddr == p.vaddr && o.seg[0].filesz == 5000 && o.seg[0].off == 256,
	      "segment");
	CHECK(o.lo == p.vaddr && o.hi == p.vaddr + p.memsz, "box");
	CHECK(o.entry == p.entry && o.stack == p.stack && o.stack_size == 0x800 && o.prio == 0x70 && o.arg == 0xB4,
	      "notes");
}

static void test_elf_bad_header(void)
{
	struct elfp p = defp(BASE + 0x60000);
	uint8_t *e;
	uint32_t len = elf_in_arena(&p, &e);

	CHECK(check(e, 20, NULL, 0) == SDL_R_ELF, "short header");
	e[0] = 0;
	CHECK(check(e, len, NULL, 0) == SDL_R_ELF, "bad magic");
	e[0] = 0x7F; e[5] = 1;
	CHECK(check(e, len, NULL, 0) == SDL_R_ELF, "little endian");
	e[5] = 2; e[8] = 2;
	CHECK(check(e, len, NULL, 0) == SDL_R_ELF, "ident word 2");
	e[8] = 1;
	put16(e + 42, 40);
	CHECK(check(e, len, NULL, 0) == SDL_R_ELF, "phentsize");
	put16(e + 42, 32);
	put16(e + 44, 0);
	CHECK(check(e, len, NULL, 0) == SDL_R_ELF, "no program headers");
	put16(e + 44, 13);
	CHECK(check(e, len, NULL, 0) == SDL_R_ELF, "too many program headers");
	put16(e + 44, 3);
	put32(e + 28, 0xFFFFFFF0);
	CHECK(check(e, len, NULL, 0) == SDL_R_ELF, "phoff past the head");
	put32(e + 28, 52);
	CHECK(check(e, len, NULL, 0) == 0, "restored header passes again");
}

static void test_elf_ranges(void)
{
	struct elfp p;
	uint8_t *e;
	uint32_t len;

	/* below the area */
	p = defp(BASE - 0x2000);
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_RANGE, "below the area");

	/* at the very start is fine */
	p = defp(BASE);
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == 0, "at the start of the area");

	/* the last byte of the area is the end of the box */
	p = defp(BASE + SDL_AREA_SIZE - p.memsz);
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == 0, "ending exactly at the end");

	p = defp(BASE + SDL_AREA_SIZE - p.memsz + 32);
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_RANGE, "one line past the end");

	/* above it, as the EHCI module's neighbour */
	p = defp(0x13780000);
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_RANGE, "over the next module");

	/* a huge memsz that wraps */
	p = defp(BASE + 0x1000);
	p.memsz = 0xFFFFF000;
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_RANGE, "memsz that wraps");

	/* a vaddr near 2^32 */
	p = defp(0xFFFFFF00);
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_RANGE, "vaddr near the top of the address space");
}

static void test_elf_truncated(void)
{
	struct elfp p = defp(BASE + 0x60000);
	uint8_t *e;
	uint32_t len = elf_in_arena(&p, &e);

	CHECK(check(e, len - 1, NULL, 0) == SDL_R_ELF, "one byte short");
	CHECK(check(e, 256, NULL, 0) == SDL_R_ELF, "header only");

	/* filesz bigger than memsz */
	p = defp(BASE + 0x60000);
	p.memsz = p.filesz - 1;
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_ELF, "filesz over memsz");

	/* an offset past the end of the file */
	p = defp(BASE + 0x60000);
	len = elf_in_arena(&p, &e);
	put32(e + 52 + 64 + 4, 0xFFFFFF00);
	CHECK(check(e, len, NULL, 0) == SDL_R_ELF, "offset beyond the file");
}

static void test_elf_entry_and_stack(void)
{
	struct elfp p;
	uint8_t *e;
	uint32_t len;

	p = defp(BASE + 0x60000);
	p.entry = BASE + 0x10;
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_RANGE, "entry outside the module");

	p = defp(BASE + 0x60000);
	p.entry = p.vaddr + p.memsz;
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_RANGE, "entry at the end");

	p = defp(BASE + 0x60000);
	p.entry = p.vaddr;
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == 0, "entry at the start, even");

	p = defp(BASE + 0x60000);
	p.stack = p.vaddr + p.memsz + 4;
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_RANGE, "stack above the module");

	p = defp(BASE + 0x60000);
	p.stack = p.vaddr - 4;
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_RANGE, "stack below the module");

	p = defp(BASE + 0x60000);
	p.stack_size = p.memsz + 4;
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_RANGE, "stack larger than the module");

	p = defp(BASE + 0x60000);
	p.stack_size = 0;
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_RANGE, "no stack");

	p = defp(BASE + 0x60000);
	p.stack_size = p.memsz;
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == 0, "the stack may be the whole module");
}

static void test_elf_priority(void)
{
	struct elfp p;
	uint8_t *e;
	uint32_t len;

	p = defp(BASE + 0x60000);
	p.prio = 0x79;
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == 0, "priority 0x79 is MLOAD's own");

	p.prio = 0x7A;
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_RANGE, "priority 0x7A");

	p.prio = 0x7F;
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_RANGE, "priority 0x7F");

	p.prio = 0;
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_RANGE, "priority 0");
}

static void test_elf_notes(void)
{
	struct elfp p;
	uint8_t *e;
	uint32_t len;

	p = defp(BASE + 0x60000);
	p.with_note = 0;
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_ELF, "no notes");

	p = defp(BASE + 0x60000);
	p.note_first = 1;
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_ELF, "note table that does not start with 0");

	/* a note segment that claims more than the head holds */
	p = defp(BASE + 0x60000);
	len = elf_in_arena(&p, &e);
	put32(e + 52 + 16, 0x10000);
	CHECK(check(e, len, NULL, 0) == SDL_R_ELF, "note larger than the head");

	/* a missing stack note: overwrite 0x7F's type */
	p = defp(BASE + 0x60000);
	len = elf_in_arena(&p, &e);
	put32(e + 148 + 36, 0x55);
	CHECK(check(e, len, NULL, 0) == SDL_R_ELF, "no stack note");
}

static void test_elf_overlap(void)
{
	struct elfp p = defp(BASE + 0x60000);
	uint8_t *e;
	uint32_t len = elf_in_arena(&p, &e);
	sdl_range t[2];

	t[0].a = BASE + 0x10000; t[0].b = BASE + 0x20000;
	CHECK(check(e, len, t, 1) == 0, "far from an earlier module");

	t[1].a = BASE + 0x60000 + p.memsz - 1; t[1].b = BASE + 0x70000;
	CHECK(check(e, len, t, 2) == SDL_R_RANGE, "one byte over an earlier module");

	t[1].a = BASE + 0x50000; t[1].b = BASE + 0x60000;
	CHECK(check(e, len, t, 2) == 0, "touching is not overlapping");

	t[1].a = BASE + 0x50000; t[1].b = BASE + 0x60001;
	CHECK(check(e, len, t, 2) == SDL_R_RANGE, "one byte over, from below");
}

static void test_elf_own_segments_overlap(void)
{
	/* two code segments: header, 4 program headers (52 + 128 = 180), note at 180 */
	uint8_t f[600];
	uint8_t *ph = f + 52, *note = f + 200;
	static sdl_elf o;
	uint32_t v = BASE + 0x40000;

	memset(f, 0, sizeof(f));
	put32(f, 0x7F454C46); put32(f + 4, 0x01020161); put32(f + 8, 0x01000000);
	put32(f + 24, v + 1); put32(f + 28, 52); put16(f + 42, 32); put16(f + 44, 4);
	put32(ph, 4); put32(ph + 4, 200); put32(ph + 16, 44); put32(ph + 20, 44);
	put32(ph + 32, 1); put32(ph + 36, 300); put32(ph + 40, v);        put32(ph + 48, 100); put32(ph + 52, 0x400);
	put32(ph + 64, 1); put32(ph + 68, 400); put32(ph + 72, v + 0x400); put32(ph + 80, 100); put32(ph + 84, 0x400);
	put32(ph + 96, 1); put32(ph + 100, 500); put32(ph + 104, v + 0x7E0); put32(ph + 112, 50); put32(ph + 116, 0x400);
	put32(note + 4, 0x7D); put32(note + 8, 0x70);
	put32(note + 12, 0x7E); put32(note + 16, 0x200);
	put32(note + 20, 0x7F); put32(note + 24, v + 0x400);
	put32(note + 28, 0x0B); put32(note + 32, 0);

	/* the third segment starts one line before the second ends */
	CHECK(sdl_check_elf(f, 600, 550, NULL, 0, &o) == SDL_R_RANGE, "segments over each other");
	put32(ph + 104, v + 0x800);
	CHECK(sdl_check_elf(f, 600, 550, NULL, 0, &o) == 0, "segments side by side");
	CHECK(o.nseg == 3 && o.lo == v && o.hi == v + 0x800 + 0x400, "box of three: %u segs", o.nseg);
}


/* ---- The whole run ---- */

static void test_flow_ok(void)
{
	struct elfp p1 = defp(BASE + 0x60000), p2 = defp(BASE + 0x40000);
	sdl_report r;
	uint8_t *e;
	uint32_t len;
	char list[100];
	int i;

	p2.filesz = 300;
	p2.memsz = 0x2000;
	p2.entry = p2.vaddr + 0x21;
	p2.stack = p2.vaddr + p2.memsz;
	p2.prio = 0x50;
	len = elf_in_arena(&p1, &e);
	add_mod("one.elf", e, len);
	snprintf(list, sizeof(list), "# test\none.elf crc32=%08X\ntwo.elf\n", crc_of(e, len));
	add_elf("two.elf", &p2);
	add_list(list);

	run_loader(50);

	CHECK(S.state == SDL_ST_DONE, "state %u", S.state);
	CHECK(get_rep(&r) == 0, "report");
	CHECK(r.magic == 0x53444D4C && r.version == 1 && r.state == SDL_ST_DONE && r.error == 0, "header");
	CHECK(r.count == 2 && r.loaded == 2 && r.skipped == 0, "count=%u loaded=%u skipped=%u", r.count, r.loaded, r.skipped);
	CHECK(!strcmp(r.entries[0].name, "one.elf") && r.entries[0].result == 0 && r.entries[0].detail == 0x40 &&
	      r.entries[0].size == len, "entry 0: res=%d detail=%d size=%u", (int)r.entries[0].result,
	      (int)r.entries[0].detail, r.entries[0].size);
	CHECK(!strcmp(r.entries[1].name, "two.elf") && r.entries[1].result == 0 && r.entries[1].detail == 0x41,
	      "entry 1");

	/* the segment bytes are in the area, the rest of the segment zeroed, the rest untouched */
	for (i = 0; i < 5000; i++)
		CHECK(area[0x60000 + i] == (uint8_t)(i * 7 + 3), "byte %d of module one", i);
	for (i = 5000; i < (int)p1.memsz; i++)
		CHECK(area[0x60000 + i] == 0, "bss byte %d", i);
	CHECK(area[0x60000 - 1] == 0xAA && area[0x60000 + p1.memsz] == 0xAA, "neighbours untouched");
	CHECK(area[0x40000 + 299] == (uint8_t)(299 * 7 + 3) && area[0x40000 + 300] == 0, "module two");
	CHECK(area[0] == 0xAA, "the table at address 0 was not copied anywhere");

	CHECK(sim.nth == 2, "threads %d", sim.nth);
	CHECK(sim.th[0].start == p1.entry && sim.th[0].arg == 0xB4 && sim.th[0].stack == p1.stack &&
	      sim.th[0].ssize == 0x800 && sim.th[0].prio == 0x70, "thread 0");
	CHECK(sim.th[1].start == p2.entry && sim.th[1].prio == 0x50, "thread 1");

	CHECK(sim.mounts == 1 && sim.umounts == 1, "mount %d unmount %d", sim.mounts, sim.umounts);
	CHECK(sim.open_fds == 0, "%d files left open", sim.open_fds);
	CHECK(sim.timer_created == 1 && sim.timer_destroyed == 1, "timer");
	CHECK(sim.timer_stopped == sim.restarts + 1 && sim.dead_restarts == 0,
	      "every re-arm stops the fired timer first, one more stop at the end: %d stops, %d restarts, %d dead",
	      sim.timer_stopped, sim.restarts, sim.dead_restarts);
	CHECK(sim.icinv == 2 && sim.callfuncs == 2, "icache invalidated once per module: %d", sim.icinv);
	CHECK(!strcmp(sim.opened[0], "fat/d2x/mload.txt") && !strcmp(sim.opened[1], "fat/d2x/mload/one.elf"),
	      "paths: %s, %s", sim.opened[0], sim.opened[1]);
	CHECK(r.elapsed_ms < 500, "elapsed %u ms", r.elapsed_ms);
}

static void test_flow_no_fat(void)
{
	sdl_report r;

	sim.fat_up_after = 100000;
	run_loader(400);

	CHECK(S.state == SDL_ST_DONE, "ended");
	CHECK(get_rep(&r) == 0 && r.error == SDL_E_NOFAT, "error %d", (int)r.error);
	CHECK(sim.mounts == 0 && sim.open_fds == 0, "nothing mounted or open");
	CHECK(r.elapsed_ms >= 4900 && r.elapsed_ms <= 5300, "ended at the deadline: %u ms", r.elapsed_ms);
	CHECK(sim.fat_tries >= 200 && sim.fat_tries <= 260, "polled about every 20 ms: %d", sim.fat_tries);
	CHECK(sim.timer_destroyed == 1, "timer destroyed");
	CHECK(sim.umounts == 0, "no unmount without a mount");
}

static void test_flow_fat_late(void)
{
	struct elfp p = defp(BASE + 0x60000);
	sdl_report r;

	add_elf("m", &p);
	add_list("m\n");
	sim.fat_up_after = 12;
	run_loader(100);

	CHECK(get_rep(&r) == 0 && r.error == 0 && r.loaded == 1, "loaded after a slow FAT: error %d loaded %u", (int)r.error, r.loaded);
	CHECK(sim.fat_tries == 13, "tries %d", sim.fat_tries);
}

static void test_flow_no_card(void)
{
	sdl_report r;

	sim.card = 0;
	add_list("m\n");
	run_loader(100);

	CHECK(S.state == SDL_ST_DONE && get_rep(&r) == 0 && r.error == SDL_E_MOUNT, "error %d", (int)r.error);
	CHECK(sim.mounts == 3, "three tries: %d", sim.mounts);
	CHECK(r.elapsed_ms < 200, "ended quickly: %u ms", r.elapsed_ms);
	CHECK(sim.open_fds == 0 && sim.timer_destroyed == 1, "fat closed, timer gone");
	CHECK(sim.umounts == 0, "nothing mounted, nothing unmounted");
	CHECK(r.count == 0 && r.loaded == 0, "no entries");
}

static void test_flow_no_list(void)
{
	sdl_report r;

	run_loader(100);

	CHECK(get_rep(&r) == 0 && r.error == SDL_E_NOLIST, "error %d", (int)r.error);
	CHECK(sim.mounts == 1 && sim.umounts == 1 && sim.open_fds == 0, "unmounted and closed");
}

static void test_flow_empty_list(void)
{
	sdl_report r;

	add_list("# nothing here\n\n");
	run_loader(100);

	CHECK(get_rep(&r) == 0 && r.error == 0 && r.count == 0 && r.loaded == 0 && r.skipped == 0, "quiet");
	CHECK(sim.umounts == 1 && sim.open_fds == 0, "cleaned up");
}

static void test_flow_list_read_error(void)
{
	sdl_report r;

	add_list("m\n");
	sim.read_fail_at = 1;
	run_loader(100);

	CHECK(get_rep(&r) == 0 && r.error == SDL_E_LISTIO, "error %d", (int)r.error);
	CHECK(sim.umounts == 1 && sim.open_fds == 0, "cleaned up");
}

static void test_flow_deadline(void)
{
	struct elfp p = defp(BASE + 0x60000);
	sdl_report r;

	add_elf("a", &p);
	p.vaddr = BASE + 0x40000; p.entry = p.vaddr + 1; p.stack = p.vaddr + p.memsz;
	add_elf("b", &p);
	add_list("a\nb\n");
	/* each read takes 1.2 s: the list, then the first module's first reads */
	sim.cost_read = 1200 * MS;
	run_loader(400);

	CHECK(S.state == SDL_ST_DONE && get_rep(&r) == 0, "ended");
	CHECK(r.error == SDL_E_TIMEOUT, "error %d", (int)r.error);
	CHECK(r.count == 2 && r.loaded == 0 && r.skipped == 2, "loaded %u skipped %u", r.loaded, r.skipped);
	CHECK(r.entries[0].result == SDL_E_TIMEOUT, "the module it stopped in: %d", (int)r.entries[0].result);
	CHECK(r.entries[1].result == SDL_R_NOTRUN, "the next one never ran: %d", (int)r.entries[1].result);
	CHECK(sim.nth == 0, "no thread");
	CHECK(sim.umounts == 1 && sim.open_fds == 0, "cleaned up");
	CHECK(area[0x60000] == 0xAA, "pass 1 wrote nothing to the area");
	CHECK(r.elapsed_ms >= 5000 && r.elapsed_ms < 9000, "elapsed %u", r.elapsed_ms);
}

static void test_flow_deadline_in_pass2(void)
{
	struct elfp p = defp(BASE + 0x60000);
	sdl_report r;

	/* a big file: pass 1 fits the budget, pass 2 does not */
	p.filesz = 40000; p.memsz = 40000 + 0x1000; p.stack = p.vaddr + p.memsz;
	add_elf("big", &p);
	add_list("big\n");
	sim.cost_read = 150 * MS;
	run_loader(400);

	CHECK(get_rep(&r) == 0 && r.error == SDL_E_TIMEOUT, "error %d", (int)r.error);
	CHECK(r.entries[0].result == SDL_E_TIMEOUT, "entry %d", (int)r.entries[0].result);
	CHECK(sim.nth == 0, "a half-copied module is not started");
}

static void test_flow_title_running(void)
{
	struct elfp p = defp(BASE + 0x60000);
	sdl_report r;

	add_elf("m", &p);
	add_list("m\n");
	running_title = 1;
	run_loader(100);

	CHECK(S.state == SDL_ST_DONE && get_rep(&r) == 0 && r.error == SDL_E_TITLE, "error %d", (int)r.error);
	CHECK(sim.fat_calls == 0 && sim.mounts == 0, "touched no SD or FAT: %d calls", sim.fat_calls);
	CHECK(sim.timer_destroyed == 1 && sim.nth == 0, "timer gone, no thread");
}

static void test_flow_title_starts_midway(void)
{
	struct elfp p = defp(BASE + 0x60000);
	sdl_report r;

	add_elf("m", &p);
	add_list("m\n");
	sim.title_at_read = 4;	/* read 1 is the list, 2 and 3 the module's first chunks */
	run_loader(100);

	CHECK(get_rep(&r) == 0 && r.error == SDL_E_TITLE, "error %d", (int)r.error);
	CHECK(sim.nth == 0 && sim.umounts == 1 && sim.open_fds == 0, "stopped and cleaned up");
	CHECK(area[0x60000] == 0xAA, "area untouched: nothing validated yet");
}

static void test_flow_bad_module_skipped(void)
{
	struct elfp good = defp(BASE + 0x60000);
	uint8_t junk[3000];
	sdl_report r;
	int i;

	for (i = 0; i < 3000; i++)
		junk[i] = (uint8_t)i;
	add_mod("junk", junk, sizeof(junk));
	add_elf("good", &good);
	add_list("junk\nmissing\n../etc\ngood\n");
	run_loader(100);

	CHECK(get_rep(&r) == 0 && r.error == 0, "error %d", (int)r.error);
	CHECK(r.count == 4 && r.loaded == 1 && r.skipped == 3, "loaded %u skipped %u", r.loaded, r.skipped);
	CHECK(r.entries[0].result == SDL_R_ELF, "junk: %d", (int)r.entries[0].result);
	CHECK(r.entries[1].result == SDL_R_OPEN && r.entries[1].detail == -106, "missing: %d/%d", (int)r.entries[1].result, (int)r.entries[1].detail);
	CHECK(r.entries[2].result == SDL_R_NAME, "bad name: %d", (int)r.entries[2].result);
	CHECK(r.entries[3].result == 0 && r.entries[3].detail == 0x40, "good: %d", (int)r.entries[3].result);
	CHECK(sim.nth == 1 && sim.open_fds == 0, "one thread, nothing left open");
	CHECK(r.entries[0].size == 3000, "size recorded for the junk file");
	/* the bad name never reached the FAT module */
	for (i = 0; i < sim.nopened; i++)
		CHECK(!strstr(sim.opened[i], "etc"), "opened %s", sim.opened[i]);
}

static void test_flow_crc(void)
{
	struct elfp p = defp(BASE + 0x60000);
	uint8_t *e;
	uint32_t len = elf_in_arena(&p, &e);
	char list[100];
	sdl_report r;

	add_mod("m", e, len);
	snprintf(list, sizeof(list), "m crc32=%08X\n", crc_of(e, len) ^ 1);
	add_list(list);
	run_loader(100);

	CHECK(get_rep(&r) == 0 && r.entries[0].result == SDL_R_CRC, "result %d", (int)r.entries[0].result);
	CHECK(sim.nth == 0 && area[0x60000] == 0xAA, "nothing started or written");
}

static void test_flow_read_error_in_module(void)
{
	struct elfp p = defp(BASE + 0x60000);
	sdl_report r;

	add_elf("m", &p);
	add_list("m\n");
	sim.read_fail_at = 2;	/* 1: the list; 2: the first chunk of the module */
	run_loader(100);

	CHECK(get_rep(&r) == 0 && r.entries[0].result == SDL_R_READ && r.entries[0].detail == -1,
	      "result %d detail %d", (int)r.entries[0].result, (int)r.entries[0].detail);
	CHECK(r.error == 0 && sim.open_fds == 0, "the run goes on, no leak");
}

static void test_flow_read_error_in_pass2(void)
{
	struct elfp p = defp(BASE + 0x60000);
	sdl_report r;

	add_elf("m", &p);
	add_list("m\n");
	/* the list: read 1. pass 1: reads 2 to 5 (5256 bytes in 3, then EOF). pass 2: from read 6 */
	sim.read_fail_at = 8;
	run_loader(100);

	CHECK(get_rep(&r) == 0 && r.entries[0].result == SDL_R_READ, "result %d", (int)r.entries[0].result);
	CHECK(sim.nth == 0, "a half-copied module is not started");
}

static void test_flow_thread_fail(void)
{
	struct elfp p = defp(BASE + 0x60000);
	sdl_report r;

	add_elf("m", &p);
	add_list("m\n");
	sim.thread_fail = 1;
	run_loader(100);

	CHECK(get_rep(&r) == 0 && r.entries[0].result == SDL_R_THREAD && r.entries[0].detail == -4,
	      "result %d detail %d", (int)r.entries[0].result, (int)r.entries[0].detail);
	CHECK(r.loaded == 0 && r.skipped == 1, "counted as skipped");
}

static void test_flow_overlap_between_modules(void)
{
	struct elfp p = defp(BASE + 0x60000);
	sdl_report r;

	add_elf("a", &p);
	p.vaddr += 0x800; p.entry += 0x800; p.stack += 0x800;
	add_elf("b", &p);
	add_list("a\nb\n");
	run_loader(100);

	CHECK(get_rep(&r) == 0, "report");
	CHECK(r.entries[0].result == 0, "the first loads");
	CHECK(r.entries[1].result == SDL_R_RANGE, "the second would overwrite it: %d", (int)r.entries[1].result);
	CHECK(sim.nth == 1, "one thread");
	/* the first module's bytes are intact */
	CHECK(area[0x60000 + 100] == (uint8_t)(100 * 7 + 3), "first module intact");
}

static void test_flow_range_writes_nothing(void)
{
	struct elfp p = defp(BASE + 0x60000);
	uint8_t *e;
	uint32_t len = elf_in_arena(&p, &e);
	sdl_report r;
	uint32_t i;
	int changed = 0;

	/* a second code segment aimed below the area: header 3 -> make the code segment bad */
	put32(e + 52 + 64 + 8, 0x10000);
	add_mod("evil", e, len);
	add_list("evil\n");
	run_loader(100);

	CHECK(get_rep(&r) == 0 && r.entries[0].result == SDL_R_RANGE, "result %d", (int)r.entries[0].result);
	for (i = 0; i < sizeof(area); i++)
		changed += area[i] != 0xAA;
	CHECK(changed == 0, "%d bytes of the area changed", changed);
}

static void test_flow_file_bigger_than_area(void)
{
	static uint8_t big[SDL_AREA_SIZE + 5000];
	sdl_report r;

	memset(big, 0x41, sizeof(big));
	add_mod("big", big, sizeof(big));
	add_list("big\n");
	run_loader(300);

	CHECK(get_rep(&r) == 0 && r.entries[0].result == SDL_R_RANGE, "result %d", (int)r.entries[0].result);
	CHECK(sim.reads < 300, "stopped reading: %d reads", sim.reads);
}

static void test_flow_empty_file(void)
{
	sdl_report r;

	add_mod("empty", "", 0);
	add_list("empty\n");
	run_loader(100);

	CHECK(get_rep(&r) == 0 && r.entries[0].result == SDL_R_ELF && r.entries[0].size == 0, "result %d", (int)r.entries[0].result);
}

static void test_flow_limit(void)
{
	struct elfp p = defp(BASE + 0x60000);
	char list[200] = "";
	sdl_report r;
	int i;

	add_elf("m", &p);
	for (i = 0; i < 10; i++)
		strcat(list, "m\n");
	add_list(list);
	run_loader(100);

	CHECK(get_rep(&r) == 0, "report");
	CHECK(r.count == 8, "eight listed: %u", r.count);
	CHECK(r.loaded == 1 && r.skipped == 9, "loaded %u skipped %u (7 overlap, 2 over the limit)", r.loaded, r.skipped);
}

static void test_flow_list_cut_at_limit(void)
{
	static char text[SDL_LIST_MAX + 200];
	sdl_report r;
	int i, n = 0;

	/* 2048 bytes and more: lines of 8 bytes, the file ends inside a line */
	for (i = 0; i < SDL_LIST_MAX + 100; i += 8)
		n += sprintf(text + n, "name%03d\n", (i / 8) % 1000);
	text[SDL_LIST_MAX + 100 - 4] = 0;
	add_list(text);
	run_loader(100);

	CHECK(get_rep(&r) == 0 && r.error == 0, "error %d", (int)r.error);
	CHECK(r.count == 8, "count %u", r.count);
	CHECK(r.skipped >= 8, "all fail to open: %u", r.skipped);
}

static void test_report_api(void)
{
	sdl_report r;
	char small[sizeof(sdl_report) - 1];
	ioctlv v[2];

	memset(&r, 0x55, sizeof(r));
	CHECK(get_rep(&r) == 0, "idle report");
	CHECK(r.magic == 0x53444D4C && r.version == 1 && r.state == SDL_ST_IDLE && r.error == 0 &&
	      r.count == 0 && r.loaded == 0 && r.skipped == 0 && r.elapsed_ms == 0 && r.mode == SDL_M_NONE,
	      "idle fields");
	CHECK(sizeof(sdl_report) == 40 + 8 * 44, "size %zu", sizeof(sdl_report));
	CHECK(sizeof(sdl_entry) == 44, "entry size %zu", sizeof(sdl_entry));

	/* bad vectors: nothing is written, nothing is started */
	memset(small, 0x77, sizeof(small));
	memset(v, 0, sizeof(v));
	v[0].data = small;
	v[0].len = sizeof(small);
	CHECK(SdLoad_IoctlReport(v, 0, 1) == IPC_EINVAL, "a vector one byte short");
	CHECK(SdLoad_IoctlLoad(v, 0, 1) == IPC_EINVAL, "load: a vector one byte short");
	v[0].len = sizeof(r);
	CHECK(SdLoad_IoctlReport(v, 0, 0) == IPC_EINVAL, "no vectors");
	CHECK(SdLoad_IoctlLoad(v, 0, 0) == IPC_EINVAL, "load: no vectors");
	CHECK(SdLoad_IoctlReport(v, 1, 0) == IPC_EINVAL, "an input vector is not an output");
	CHECK(SdLoad_IoctlLoad(v, 1, 0) == IPC_EINVAL, "load: an input vector");
	CHECK(SdLoad_IoctlReport(NULL, 0, 1) == IPC_EINVAL, "a NULL vector table");
	CHECK(SdLoad_IoctlLoad(NULL, 0, 1) == IPC_EINVAL, "load: a NULL vector table");
	v[0].data = NULL;
	CHECK(SdLoad_IoctlReport(v, 0, 1) == IPC_EINVAL, "a NULL buffer");
	CHECK(SdLoad_IoctlLoad(v, 0, 1) == IPC_EINVAL, "load: a NULL buffer");
	CHECK((unsigned char)small[0] == 0x77, "no bad request wrote anything");

	CHECK(S.state == SDL_ST_IDLE && sim.fat_calls == 0 && sim.opens == 0,
	      "the bad requests did nothing: state %u, %d opens", S.state, sim.opens);
}

static void test_flow_timer_fail(void)
{
	struct elfp p = defp(BASE + 0x60000);
	sdl_report r;

	add_elf("m", &p);
	add_list("m\n");
	sim.flag = 1;
	sim.timer_fail = 1;
	SdLoad_Boot(5);
	CHECK(get_rep(&r) == 0 && r.state == SDL_ST_IDLE && r.error == SDL_E_TIMER && r.mode == SDL_M_NONE,
	      "state %u error %d", r.state, (int)r.error);
	SdLoad_Tick();
	CHECK(sim.fat_calls == 0, "a dead boot-time run does nothing");

	/* the app can still ask */
	CHECK(sd_load(&r) == 0 && r.loaded == 1 && r.error == 0 && r.mode == SDL_M_IOCTL, "MLOAD_SD_LOAD still works");
}

static void test_boot_no_flag(void)
{
	sdl_report r;
	struct elfp p = defp(BASE + 0x60000);

	/* a card with a list and a module, and no flag on the NAND */
	add_elf("m", &p);
	add_list("m\n");
	SdLoad_Boot(5);

	CHECK(sim.flag_opens == 1 && sim.opens == 1, "the flag file is the only thing opened: %d", sim.opens);
	CHECK(sim.fat_calls == 0 && sim.mounts == 0 && sim.umounts == 0, "no SD, no FAT");
	CHECK(sim.timer_created == 0 && sim.armed == 0, "no timer");
	CHECK(sim.nth == 0 && sim.open_fds == 0, "no thread, nothing left open");
	CHECK(S.state == SDL_ST_IDLE, "idle");
	CHECK(get_rep(&r) == 0 && r.state == SDL_ST_IDLE && r.flag == -106 && r.mode == SDL_M_NONE && r.error == 0,
	      "report: flag %d", (int)r.flag);

	/* a timer message that should not exist does nothing */
	SdLoad_Tick();
	CHECK(sim.fat_calls == 0, "a stray tick does nothing");
	/* and so does a PPC write */
	SdLoad_PpcRequest();
	CHECK(S.state == SDL_ST_IDLE && sim.fat_calls == 0, "a PPC write with no run going");

	/* a file system that errs (no permission) is no flag either */
	reset();
	add_list("m\n");
	sim.flag = -1;
	SdLoad_Boot(5);
	CHECK(sim.fat_calls == 0 && sim.timer_created == 0 && S.state == SDL_ST_IDLE, "file system error: nothing");
	CHECK(get_rep(&r) == 0 && r.flag == -102, "the raw error is in the report: %d", (int)r.flag);
}

static void test_boot_flag_starts(void)
{
	sdl_report r;
	struct elfp p = defp(BASE + 0x60000);
	int i;

	add_elf("m", &p);
	add_list("m\n");
	sim.flag = 1;
	SdLoad_Boot(5);

	CHECK(sim.timer_created == 1 && S.state == SDL_ST_WAIT_FAT, "the flag starts the run: state %u", S.state);
	CHECK(sim.fat_calls == 0, "and Boot itself makes no FAT call");
	CHECK(sim.open_fds == 0, "the flag file is closed");

	/* the run completes on ticks */
	for (i = 0; i < 100 && S.state != SDL_ST_DONE; i++) {
		fire();
	}
	CHECK(get_rep(&r) == 0 && r.state == SDL_ST_DONE && r.mode == SDL_M_AUTO && r.flag == 0 &&
	      r.loaded == 1 && r.error == 0, "auto run: mode %u loaded %u", r.mode, r.loaded);
	CHECK(sim.nth == 1, "the module runs");

	/* an app that then asks gets the old report, nothing is loaded again */
	{
		int opens = sim.opens, mounts = sim.mounts;
		sdl_report r2;

		CHECK(sd_load(&r2) == SDL_ALREADY, "second request: already");
		CHECK(r2.loaded == 1 && r2.mode == SDL_M_AUTO, "the report of the boot-time run");
		CHECK(sim.opens == opens && sim.mounts == mounts && sim.nth == 1, "nothing reloaded");
	}
}

static void test_ioctl_load(void)
{
	struct elfp p1 = defp(BASE + 0x60000), p2 = defp(BASE + 0x40000);
	sdl_report r, r2;
	int mounts;

	p2.entry = p2.vaddr + 0x21;
	add_elf("one", &p1);
	add_elf("two", &p2);
	add_list("one\ntwo\n");

	SdLoad_Boot(5);		/* no flag */
	CHECK(sim.fat_calls == 0, "nothing before the request");

	CHECK(sd_load(&r) == 0, "the first request runs the load");
	CHECK(r.state == SDL_ST_DONE && r.error == 0 && r.mode == SDL_M_IOCTL && r.loaded == 2 && r.count == 2,
	      "report: state %u error %d mode %u loaded %u", r.state, (int)r.error, r.mode, r.loaded);
	CHECK(r.entries[0].result == 0 && r.entries[1].result == 0 && sim.nth == 2, "both started");
	CHECK(sim.timer_created == 0 && sim.armed == 0, "no timer: the ioctl runs it in place");
	CHECK(sim.mounts == 1 && sim.umounts == 1 && sim.open_fds == 0, "mounted, unmounted, closed");
	CHECK(area[0x60000 + 100] == (uint8_t)(100 * 7 + 3), "bytes in place");

	mounts = sim.mounts;
	memset(&r2, 0x66, sizeof(r2));
	CHECK(sd_load(&r2) == SDL_ALREADY, "the second request does not reload");
	CHECK(sim.mounts == mounts && sim.nth == 2 && sim.open_fds == 0, "no mount, no thread");
	CHECK(r2.loaded == 2 && r2.mode == SDL_M_IOCTL && r2.state == SDL_ST_DONE, "it returns the report");
	CHECK(!memcmp(&r, &r2, sizeof(r)), "the same report");

	CHECK(get_rep(&r2) == 0 && !memcmp(&r, &r2, sizeof(r)), "GET_SD_REPORT agrees");
}

static void test_ioctl_load_failures(void)
{
	sdl_report r;
	struct elfp p;

	/* no FAT module at all: the 5 s budget, polled by sleeping */
	sim.fat_up_after = 100000;
	SdLoad_Boot(5);
	CHECK(sd_load(&r) == 0 && r.error == SDL_E_NOFAT, "no FAT: error %d", (int)r.error);
	CHECK(r.elapsed_ms >= 4900 && r.elapsed_ms <= 5400, "after the budget: %u ms", r.elapsed_ms);
	CHECK(sim.sleeps >= 200 && sim.sleeps <= 260, "polled by sleeping: %d", sim.sleeps);

	/* FAT up after a few polls */
	reset();
	p = defp(BASE + 0x60000);
	add_elf("m", &p);
	add_list("m\n");
	sim.fat_up_after = 4;
	SdLoad_Boot(5);
	CHECK(sd_load(&r) == 0 && r.error == 0 && r.loaded == 1 && sim.sleeps == 4,
	      "late FAT: error %d sleeps %d", (int)r.error, sim.sleeps);

	/* no card */
	reset();
	sim.card = 0;
	add_list("m\n");
	SdLoad_Boot(5);
	CHECK(sd_load(&r) == 0 && r.error == SDL_E_MOUNT && sim.mounts == 3 && sim.umounts == 0, "no card: %d", (int)r.error);

	/* a title is running: never */
	reset();
	running_title = 1;
	add_list("m\n");
	SdLoad_Boot(5);
	CHECK(sd_load(&r) == 0 && r.error == SDL_E_TITLE && sim.fat_calls == 0, "title: %d", (int)r.error);
	CHECK(sd_load(&r) == SDL_ALREADY, "it ran: no second try");
}

static void test_get_report_blocks(void)
{
	struct elfp p = defp(BASE + 0x60000);
	sdl_report r;
	int i;

	add_elf("m", &p);
	add_list("m\n");
	sim.flag = 1;
	SdLoad_Boot(5);

	/* a few ticks in, the run is mid-way */
	for (i = 0; i < 4; i++) {
		fire();
	}
	CHECK(S.state != SDL_ST_DONE && S.state != SDL_ST_IDLE, "mid-run: state %u", S.state);

	/* the report waits for the end: loaded, unmounted, the timer gone */
	CHECK(get_rep(&r) == 0, "report");
	CHECK(r.state == SDL_ST_DONE && r.loaded == 1 && r.error == 0 && r.mode == SDL_M_AUTO,
	      "it waited: state %u loaded %u", r.state, r.loaded);
	CHECK(sim.umounts == 1 && sim.timer_destroyed == 1 && sim.open_fds == 0, "ended and cleaned up");
	CHECK(sim.nth == 1, "started once");

	/* the timer message that was already queued does nothing */
	i = sim.opens;
	sim.armed = 0;
	SdLoad_Tick();
	CHECK(sim.opens == i, "the stale message is ignored");

	/* waiting does not outlast the budget: no FAT module */
	reset();
	sim.fat_up_after = 100000;
	sim.flag = 1;
	SdLoad_Boot(5);
	CHECK(get_rep(&r) == 0 && r.state == SDL_ST_DONE && r.error == SDL_E_NOFAT, "bounded: error %d", (int)r.error);
	CHECK(r.elapsed_ms >= 4900 && r.elapsed_ms <= 5400, "after the budget: %u ms", r.elapsed_ms);
}

static void test_one_message_at_most(void)
{
	struct elfp p1 = defp(BASE + 0x60000), p2 = defp(BASE + 0x40000);
	int ticks = 0, worst_reads = 0, worst_mounts = 0;
	sdl_report r;

	p2.filesz = 30000;
	p2.memsz = 0x8000;
	p2.entry = p2.vaddr + 0x21;
	p2.stack = p2.vaddr + p2.memsz;
	add_elf("one", &p1);
	add_elf("two", &p2);
	add_list("one\ntwo\n");
	sim.flag = 1;
	SdLoad_Boot(5);

	while (S.state != SDL_ST_DONE && ticks < 300) {
		int reads = sim.reads, calls = sim.mounts + sim.umounts;

		CHECK(sim.armed == 1, "a message is due before every tick");
		fire();
		ticks++;
		if (sim.reads - reads > worst_reads)
			worst_reads = sim.reads - reads;
		if (sim.mounts + sim.umounts - calls > worst_mounts)
			worst_mounts = sim.mounts + sim.umounts - calls;
	}

	CHECK(sim.overqueue == 0, "the timer was never armed while a message was due: %d", sim.overqueue);
	CHECK(sim.restarts == ticks - 1, "one re-arm per tick but the last: %d for %d ticks", sim.restarts, ticks);
	CHECK(sim.dead_restarts == 0, "no restart of a fired timer without a stop: %d", sim.dead_restarts);
	CHECK(sim.armed == 0, "nothing is armed after the end");
	CHECK(worst_reads <= 1, "at most one FAT read per tick: %d", worst_reads);
	CHECK(worst_mounts <= 1, "at most one mount or unmount per tick");
	CHECK(ticks > 20, "a long module takes many ticks: %d", ticks);
	CHECK(get_rep(&r) == 0 && r.loaded == 2, "and both load");
	CHECK(area[0x40000 + 29999] == (uint8_t)(29999 * 7 + 3) && area[0x40000 + 30000] == 0, "second module intact");
}

static void test_busy_ends_the_run(void)
{
	struct elfp p1 = defp(BASE + 0x60000), p2 = defp(BASE + 0x40000);
	static uint8_t snap[SDL_AREA_SIZE];
	sdl_report r;
	int i, changed = 0;

	p1.filesz = 30000;
	p1.memsz = 0x8000;
	p1.stack = p1.vaddr + p1.memsz;
	add_elf("one", &p1);
	add_elf("two", &p2);
	add_list("one\ntwo\n");
	sim.flag = 1;
	SdLoad_Boot(5);

	/* until module one is part way through its copy */
	for (i = 0; i < 300 && !(S.m.phase == SDL_M_PASS2 && S.m.pos >= 8192); i++) {
		fire();
	}
	CHECK(S.m.phase == SDL_M_PASS2 && S.m.pos >= 8192 && S.state == SDL_ST_LOAD, "part way: pos %u", S.m.pos);

	/* the PPC's first write ends the run, before it is handled */
	SdLoad_PpcRequest();
	CHECK(S.state == SDL_ST_DONE, "ended");
	CHECK(S.error == SDL_E_BUSY, "error %d", (int)S.error);
	CHECK(sim.nth == 0, "the half-copied module was not started");
	CHECK(sim.umounts == 1 && sim.open_fds == 0 && sim.timer_destroyed == 1, "cleaned up");

	/* the PPC writes; a stale timer message, a report, a load request: none touches the area */
	memset(area + 0x60000, 0x5A, 0x4000);
	memcpy(snap, area, sizeof(area));
	sim.armed = 0;
	SdLoad_Tick();
	CHECK(get_rep(&r) == 0, "report");
	CHECK(r.error == SDL_E_BUSY && r.state == SDL_ST_DONE, "report says busy: %d", (int)r.error);
	CHECK(r.entries[0].result == SDL_E_BUSY && r.entries[1].result == SDL_R_NOTRUN,
	      "entries: %d, %d", (int)r.entries[0].result, (int)r.entries[1].result);
	CHECK(r.loaded == 0 && r.skipped == 2, "loaded %u skipped %u", r.loaded, r.skipped);
	CHECK(sd_load(&r) == SDL_ALREADY, "no second try");
	for (i = 0; i < (int)sizeof(area); i++)
		changed += area[i] != snap[i];
	CHECK(changed == 0, "%d bytes of the area changed after the PPC's write", changed);
	CHECK(sim.nth == 0, "still no thread");

	/* between two modules: the second never starts */
	reset();
	p1 = defp(BASE + 0x60000);
	add_elf("one", &p1);
	add_elf("two", &p2);
	add_list("one\ntwo\n");
	sim.flag = 1;
	SdLoad_Boot(5);
	for (i = 0; i < 300 && sim.nth == 0; i++) {
		fire();
	}
	CHECK(sim.nth == 1 && S.state == SDL_ST_LOAD, "module one started");
	SdLoad_PpcRequest();
	CHECK(get_rep(&r) == 0 && r.error == SDL_E_BUSY && r.loaded == 1 && r.entries[0].result == 0 &&
	      r.entries[1].result == SDL_R_NOTRUN, "one loaded, two not run");
	CHECK(sim.nth == 1, "no second thread");

	/* after a finished run a PPC write is nobody's business */
	reset();
	add_list("# none\n");
	run_loader(100);
	CHECK(S.state == SDL_ST_DONE && S.error == 0, "finished quietly");
	SdLoad_PpcRequest();
	CHECK(S.error == 0, "a write after the end changes nothing");
}

static void test_elf_alignment(void)
{
	struct elfp p;
	uint8_t *e;
	uint32_t len;
	sdl_report r;

	p = defp(BASE + 0x60000);
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == 0, "whole lines pass");

	p = defp(BASE + 0x60004);
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_ALIGN, "va not a multiple of 32");

	p = defp(BASE + 0x60000);
	p.memsz = 5000;
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_ALIGN, "memsz not a multiple of 32");

	p = defp(BASE + 0x60010);
	p.memsz = 0x2010;
	len = elf_in_arena(&p, &e);
	CHECK(check(e, len, NULL, 0) == SDL_R_ALIGN, "va and end both on a half line");

	/* the whole run: nothing written, the next module still loads */
	reset();
	p = defp(BASE + 0x60004);
	add_elf("bad", &p);
	p = defp(BASE + 0x40000);
	add_elf("good", &p);
	add_list("bad\ngood\n");
	run_loader(100);
	CHECK(get_rep(&r) == 0 && r.entries[0].result == SDL_R_ALIGN && r.entries[1].result == 0,
	      "results %d, %d", (int)r.entries[0].result, (int)r.entries[1].result);
	CHECK(area[0x60004] == 0xAA && area[0x60000] == 0xAA, "the unaligned module wrote nothing");
}

static void test_elf_arg_note_required(void)
{
	struct elfp p = defp(BASE + 0x60000);
	uint8_t *e;
	uint32_t len = elf_in_arena(&p, &e);

	CHECK(check(e, len, NULL, 0) == 0, "with the 0xB note");
	put32(e + 148 + 4, 0x55);	/* the first pair was 0xB */
	CHECK(check(e, len, NULL, 0) == SDL_R_ELF, "without it");
}

/* Two code segments with a gap: A is [v, v+0x400), B is [v+0x800, v+0xC00) */
static void mk_two(uint8_t *f, uint32_t v, uint32_t entry, uint32_t stack, uint32_t ssize)
{
	uint8_t *ph = f + 52, *note = f + 200;

	memset(f, 0, 700);
	put32(f, 0x7F454C46); put32(f + 4, 0x01020161); put32(f + 8, 0x01000000);
	put32(f + 24, entry); put32(f + 28, 52); put16(f + 42, 32); put16(f + 44, 3);
	put32(ph, 4); put32(ph + 4, 200); put32(ph + 16, 44); put32(ph + 20, 44);
	put32(ph + 32, 1); put32(ph + 36, 300); put32(ph + 40, v);         put32(ph + 48, 100); put32(ph + 52, 0x400);
	put32(ph + 64, 1); put32(ph + 68, 400); put32(ph + 72, v + 0x800); put32(ph + 80, 100); put32(ph + 84, 0x400);
	put32(note + 4, 0x0B); put32(note + 8, 0);
	put32(note + 12, 0x7D); put32(note + 16, 0x70);
	put32(note + 20, 0x7E); put32(note + 24, ssize);
	put32(note + 28, 0x7F); put32(note + 32, stack);
}

static void test_elf_gaps(void)
{
	uint8_t f[700];
	static sdl_elf o;
	uint32_t v = BASE + 0x50000;

	mk_two(f, v, v + 1, v + 0x400, 0x200);
	CHECK(sdl_check_elf(f, 700, 500, NULL, 0, &o) == 0, "entry and stack in the first segment");
	CHECK(o.nseg == 2 && o.lo == v && o.hi == v + 0xC00, "box covers the gap");

	mk_two(f, v, v + 0x800, v + 0xC00, 0x200);
	CHECK(sdl_check_elf(f, 700, 500, NULL, 0, &o) == 0, "entry and stack in the second segment");

	mk_two(f, v, v + 0x400, v + 0x400, 0x200);
	CHECK(sdl_check_elf(f, 700, 500, NULL, 0, &o) == SDL_R_RANGE, "entry at the end of a segment");

	mk_two(f, v, v + 0x600, v + 0x400, 0x200);
	CHECK(sdl_check_elf(f, 700, 500, NULL, 0, &o) == SDL_R_RANGE, "entry in the gap");

	mk_two(f, v, v + 1, v + 0x700, 0x200);
	CHECK(sdl_check_elf(f, 700, 500, NULL, 0, &o) == SDL_R_RANGE, "stack in the gap");

	mk_two(f, v, v + 1, v + 0xA00, 0x800);
	CHECK(sdl_check_elf(f, 700, 500, NULL, 0, &o) == SDL_R_RANGE, "stack that starts in the gap");

	mk_two(f, v, v + 1, v + 0x900, 0x600);
	CHECK(sdl_check_elf(f, 700, 500, NULL, 0, &o) == SDL_R_RANGE, "stack from one segment into the other");

	mk_two(f, v, v + 1, v + 0x400, 0x401);
	CHECK(sdl_check_elf(f, 700, 500, NULL, 0, &o) == SDL_R_RANGE, "stack bigger than its segment");

	mk_two(f, v, v + 1, 0x100, 0x200);
	CHECK(sdl_check_elf(f, 700, 500, NULL, 0, &o) == SDL_R_RANGE, "stack size over the stack address");
}

static void test_flow_tick_after_done(void)
{
	sdl_report r;

	add_list("# x\n");
	run_loader(100);
	CHECK(S.state == SDL_ST_DONE, "done");
	{
		int opens = sim.opens, i;

		for (i = 0; i < 10; i++) {
			clk += TICK;
			SdLoad_Tick();	/* messages that were queued before the timer died */
		}
		CHECK(sim.opens == opens && sim.timer_destroyed == 1, "stays done");
	}
	CHECK(get_rep(&r) == 0 && r.state == SDL_ST_DONE, "report");
}

static void test_flow_clock_wrap(void)
{
	struct elfp p = defp(BASE + 0x60000);
	sdl_report r;

	/* the HW timer wraps every ~37 minutes: start just before it */
	add_elf("m", &p);
	add_list("m\n");
	clk = 0xFFFFFFFFu - 3 * TICK;
	run_loader(100);

	CHECK(get_rep(&r) == 0 && r.error == 0 && r.loaded == 1, "loaded across the wrap: error %d", (int)r.error);
}

static void test_flow_second_run(void)
{
	struct elfp p = defp(BASE + 0x60000);
	sdl_report r;

	add_elf("m", &p);
	add_list("m\n");
	run_loader(100);
	CHECK(get_rep(&r) == 0 && r.loaded == 1, "first run");
	/* Start resets the state; the same module now overlaps its own earlier box? No:
	   the boxes are forgotten, so it loads again */
	sim.nth = 0;
	run_loader(100);
	CHECK(get_rep(&r) == 0 && r.loaded == 1 && r.error == 0, "second run: loaded %u", r.loaded);
	CHECK(S.state == SDL_ST_DONE, "done again");
}

/* The real test module (tools/build.sh testmod), as stripios wrote it: it must
   pass the validator and start from a list. Skipped when it was not built. */
static void test_real_testmod(void)
{
	const char *path = getenv("MLOADTEST_ELF");
	static uint8_t file[4096];
	sdl_report r;
	size_t n;
	FILE *f;
	char list[64];

	if (!path)
		path = "../../build/mloadtest.elf";
	f = fopen(path, "rb");
	if (!f) {
		printf("  (skipped: %s not built; run tools/build.sh testmod)\n", path);
		return;
	}
	n = fread(file, 1, sizeof(file), f);
	fclose(f);
	CHECK(n > 100 && n < sizeof(file), "read %u bytes", (unsigned)n);


	add_mod("mloadtest", file, (uint32_t)n);
	snprintf(list, sizeof(list), "mloadtest crc32=%08X\n", crc_of(file, (uint32_t)n));
	add_list(list);
	run_loader(100);
	CHECK(get_rep(&r) == 0 && r.error == 0 && r.loaded == 1 && r.entries[0].result == 0,
	      "the built module loads: loaded %u, result %d", r.loaded, (int)r.entries[0].result);
	CHECK(sim.nth == 1 && sim.th[0].start >= BASE + 0x60000 && sim.th[0].start < BASE + 0x64000 &&
	      sim.th[0].prio == 0x79, "thread: start %08x prio %x", (unsigned)sim.th[0].start, (unsigned)sim.th[0].prio);
}

static const struct hc_test tests[] = {
	{ "parse basic", test_parse_basic, false },
	{ "parse comments and space", test_parse_comments_and_space, false },
	{ "parse CRLF", test_parse_crlf, false },
	{ "parse BOM", test_parse_bom, false },
	{ "parse no final newline", test_parse_no_final_newline, false },
	{ "parse bad names", test_parse_bad_names, false },
	{ "name limits", test_name_ok_limits, false },
	{ "parse long lines", test_parse_long_lines, false },
	{ "parse over the limit", test_parse_over_limit, false },
	{ "parse truncated tail", test_parse_truncated_tail, false },
	{ "parse empty", test_parse_empty, false },
	{ "crc32", test_crc32, false },
	{ "elf good", test_elf_good, false },
	{ "elf bad header", test_elf_bad_header, false },
	{ "elf ranges", test_elf_ranges, false },
	{ "elf truncated", test_elf_truncated, false },
	{ "elf entry and stack", test_elf_entry_and_stack, false },
	{ "elf priority", test_elf_priority, false },
	{ "elf notes", test_elf_notes, false },
	{ "elf overlap with loaded modules", test_elf_overlap, false },
	{ "elf own segments overlap", test_elf_own_segments_overlap, false },
	{ "flow ok", test_flow_ok, false },
	{ "flow no FAT module", test_flow_no_fat, false },
	{ "flow FAT late", test_flow_fat_late, false },
	{ "flow no card", test_flow_no_card, false },
	{ "flow no list", test_flow_no_list, false },
	{ "flow empty list", test_flow_empty_list, false },
	{ "flow list read error", test_flow_list_read_error, false },
	{ "flow deadline", test_flow_deadline, false },
	{ "flow deadline in pass 2", test_flow_deadline_in_pass2, false },
	{ "flow title running", test_flow_title_running, false },
	{ "flow title starts midway", test_flow_title_starts_midway, false },
	{ "flow bad module skipped, good one started", test_flow_bad_module_skipped, false },
	{ "flow crc mismatch", test_flow_crc, false },
	{ "flow read error in a module", test_flow_read_error_in_module, false },
	{ "flow read error in pass 2", test_flow_read_error_in_pass2, false },
	{ "flow thread fails", test_flow_thread_fail, false },
	{ "flow overlap between modules", test_flow_overlap_between_modules, false },
	{ "flow out-of-range module writes nothing", test_flow_range_writes_nothing, false },
	{ "flow file bigger than the area", test_flow_file_bigger_than_area, false },
	{ "flow empty file", test_flow_empty_file, false },
	{ "flow module limit", test_flow_limit, false },
	{ "flow list cut at its limit", test_flow_list_cut_at_limit, false },
	{ "report api and bad vectors", test_report_api, false },
	{ "flow timer fails", test_flow_timer_fail, false },
	{ "boot without the flag: no SD, no FAT, no timer", test_boot_no_flag, false },
	{ "boot with the flag starts the run", test_boot_flag_starts, false },
	{ "MLOAD_SD_LOAD loads, a second call does not", test_ioctl_load, false },
	{ "MLOAD_SD_LOAD failures", test_ioctl_load_failures, false },
	{ "GET_SD_REPORT waits for a boot-time run", test_get_report_blocks, false },
	{ "one timer message at most, one chunk per tick", test_one_message_at_most, false },
	{ "a PPC write ends a boot-time run", test_busy_ends_the_run, false },
	{ "elf segment alignment", test_elf_alignment, false },
	{ "elf: the 0xB note is required", test_elf_arg_note_required, false },
	{ "elf: entry and stack inside one segment", test_elf_gaps, false },
	{ "flow tick after done", test_flow_tick_after_done, false },
	{ "flow clock wrap", test_flow_clock_wrap, false },
	{ "flow second run", test_flow_second_run, false },
	{ "the built test module loads", test_real_testmod, false },
};

int main(int argc, char **argv)
{
	return hc_main(tests, HC_COUNT(tests), argc, argv, reset);
}
