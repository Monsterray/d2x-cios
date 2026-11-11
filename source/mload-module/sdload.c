/*
	d2x cIOS: load extra modules from the SD card when the cIOS starts.
	See sdload.h and docs/mload.md.

	The loader is a state machine that MLOAD's main loop steps on a 20 ms
	timer message: no thread, no stack, no Timer_Sleep. Each step may block
	in the FAT module for a moment (a mount, one chunk of a file); the budget
	of SDL_DEADLINE_MS is checked before every step and between the chunks.
*/

#include "types.h"

#include <string.h>

#include "elf.h"
#include "ipc.h"
#include "sdload.h"
#include "swi_mload.h"
#include "syscalls.h"
#include "tools.h"

/* The area modules may load into: MLOAD's own exe_mem. The host test sets
   these to point the area at an array. */
#ifndef SDL_AREA_BASE
#include "module.h"
#define SDL_AREA_BASE		((u32)(uintptr_t)exe_mem)
#define SDL_AREA_SIZE		(exe_mem_size)
#define SDL_PTR(a)		((void *)(uintptr_t)(a))
#endif

#define SDL_TIMER_REG		0x0D800010	/* HW_TIMER, about 1898 ticks per ms */
#define SDL_TICKS_PER_MS	1898
#define SDL_DEADLINE_TICKS	(SDL_DEADLINE_MS * SDL_TICKS_PER_MS)
#define SDL_TICK_TICKS		(SDL_TICK_US / 1000 * SDL_TICKS_PER_MS)

#define SDL_FAT_MOUNT_SD	0xF0
#define SDL_FAT_UMOUNT_SD	0xF1
#define SDL_OPEN_READ		1
#define SDL_MOUNT_TRIES		3

#define SDL_BUF			2048	/* one read: the list, or a chunk of a module */
#define SDL_HEAD		1024	/* the start of a module: its header and tables */
#define SDL_MAX_PH		12
#define SDL_MAX_PRIO		0x79	/* MLOAD's own; nothing above it */

/* One line of the list */
typedef struct {
	char name[SDL_NAME_MAX + 1];
	u32  crc;
	u8   has_crc;
	u8   name_ok;
} sdl_item;

typedef struct {
	u32 a, b;			/* [a, b) */
} sdl_range;

typedef struct {
	u32 off, filesz, memsz, vaddr;
} sdl_seg;

/* What the validator found in a module */
typedef struct {
	u32 entry, stack, stack_size, prio, arg;
	u32 lo, hi;			/* the box around the loaded segments */
	u32 nseg;
	sdl_seg seg[SDL_MAX_PH];
} sdl_elf;

static struct {
	u32 state;
	s32 error;
	s32 timer, fatfd;
	u32 start, next_at;
	u32 mount_tries, idx, nitems, extra, ntaken;
	u32 mounted;
	sdl_item  items[SDL_MAX_MODULES];
	sdl_range taken[SDL_MAX_MODULES];
	sdl_elf   elf;
	sdl_report rep;
} S;

/* Buffers the FAT module reads into or the IPC reads from: 32 byte aligned */
static u8   sdl_buf[SDL_BUF]   ATTRIBUTE_ALIGN(32);
static u8   sdl_head[SDL_HEAD] ATTRIBUTE_ALIGN(32);
static char sdl_path[64]       ATTRIBUTE_ALIGN(32);
static ioctlv sdl_vec[2]       ATTRIBUTE_ALIGN(32);
static s32  sdl_part           ATTRIBUTE_ALIGN(32);


static u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static u32 be16(const u8 *p)
{
	return ((u32)p[0] << 8) | p[1];
}


/* ---- The list ---- */

/* A plain file name: 1 to SDL_NAME_MAX of A-Z a-z 0-9 . _ -, not starting
   with a dot, no ".." inside. No separator can get in, so a name cannot
   leave the module folder. */
static int sdl_name_ok(const char *s, u32 len)
{
	u32 i;

	if (len < 1 || len > SDL_NAME_MAX || s[0] == '.')
		return 0;

	for (i = 0; i < len; i++) {
		char c = s[i];

		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
			return 0;
		if (c == '.' && i + 1 < len && s[i + 1] == '.')
			return 0;
	}

	return 1;
}

static int sdl_hex8(const char *s, u32 *v)
{
	u32 i, r = 0;

	for (i = 0; i < 8; i++) {
		char c = s[i];
		u32 d;

		if (c >= '0' && c <= '9')
			d = c - '0';
		else if (c >= 'a' && c <= 'f')
			d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			d = c - 'A' + 10;
		else
			return 0;
		r = (r << 4) | d;
	}

	*v = r;
	return 1;
}

/* Parse the list: one `name` or `name crc32=XXXXXXXX` per line, `#` starts a
   comment line, LF or CRLF, an optional UTF-8 BOM, spaces and tabs around
   the words. `full` says the buffer was filled to its limit, so its last
   line may be cut and is dropped when it has no newline. Entries past `max`
   are counted in *extra. A line that is not a valid entry is still an entry,
   with name_ok clear: the report names it. */
static u32 sdl_parse(const char *buf, u32 len, int full, sdl_item *it, u32 max, u32 *extra)
{
	u32 pos = 0, n = 0;

	*extra = 0;

	if (len >= 3 && (u8)buf[0] == 0xEF && (u8)buf[1] == 0xBB && (u8)buf[2] == 0xBF)
		pos = 3;

	while (pos < len) {
		u32 s = pos, e = pos, t, rest, tl, c;
		sdl_item *p;

		while (e < len && buf[e] != '\n')
			e++;
		if (e == len && full)
			break;
		pos = e + 1;

		if (e > s && buf[e - 1] == '\r')
			e--;
		while (s < e && (buf[s] == ' ' || buf[s] == '\t'))
			s++;
		while (e > s && (buf[e - 1] == ' ' || buf[e - 1] == '\t'))
			e--;
		if (s == e || buf[s] == '#')
			continue;

		for (t = s; t < e && buf[t] != ' ' && buf[t] != '\t'; t++)
			;
		for (rest = t; rest < e && (buf[rest] == ' ' || buf[rest] == '\t'); rest++)
			;

		if (n >= max) {
			(*extra)++;
			continue;
		}

		p  = &it[n++];
		tl = t - s;
		c  = tl > SDL_NAME_MAX ? SDL_NAME_MAX : tl;
		memcpy(p->name, buf + s, c);
		p->name[c] = 0;
		p->crc     = 0;
		p->has_crc = 0;
		p->name_ok = sdl_name_ok(buf + s, tl);

		if (rest < e) {
			if (e - rest == 14 && !memcmp(buf + rest, "crc32=", 6) &&
			    sdl_hex8(buf + rest + 6, &p->crc))
				p->has_crc = 1;
			else
				p->name_ok = 0;
		}
	}

	return n;
}

/* CRC-32 (the zlib one), a bit at a time: no table to keep. Start from
   0xFFFFFFFF and invert the result. */
static u32 sdl_crc32(u32 crc, const u8 *p, u32 n)
{
	u32 k;

	while (n--) {
		crc ^= *p++;
		for (k = 0; k < 8; k++)
			crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
	}

	return crc;
}


/* ---- The ELF ---- */

/* Check a module ELF before anything of it is copied. `h` holds the first
   `hlen` bytes of the file (the header, the program headers and the note
   segment must be in them), `fsize` is the whole file's size, `taken` are
   the boxes of the modules already loaded. Returns 0, SDL_R_ELF (not a
   module, or cut short) or SDL_R_RANGE (it would write outside the area, or
   over another module, or its thread would not be in its own memory). */
static s32 sdl_check_elf(const u8 *h, u32 hlen, u32 fsize, const sdl_range *taken, u32 ntaken, sdl_elf *o)
{
	u32 base = SDL_AREA_BASE;
	u32 end  = base + SDL_AREA_SIZE;
	u32 i, j, k, phoff, phnum, have = 0;

	memset(o, 0, sizeof(*o));

	if (hlen < 52 || be32(h) != 0x7F454C46 || be32(h + 4) != 0x01020161 ||
	    be32(h + 8) != 0x01000000)
		return SDL_R_ELF;

	o->entry = be32(h + 24);
	phoff    = be32(h + 28);
	phnum    = be16(h + 44);

	if (be16(h + 42) != 32 || phnum < 1 || phnum > SDL_MAX_PH ||
	    phoff > hlen || ((hlen - phoff) >> 5) < phnum)
		return SDL_R_ELF;

	o->lo = 0xFFFFFFFF;

	for (i = 0; i < phnum; i++) {
		const u8 *p = h + phoff + i * 32;
		u32 type = be32(p), off = be32(p + 4), va = be32(p + 8);
		u32 fs = be32(p + 16), ms = be32(p + 20);

		if (type == 4) {
			/* The note table: a zero word, then (type, value) pairs */
			if (fs < 4 || off > hlen || fs > hlen - off || be32(h + off))
				return SDL_R_ELF;

			for (j = 4; j + 8 <= fs; j += 8) {
				u32 v = be32(h + off + j + 4);

				switch (be32(h + off + j)) {
				case 0x09: o->entry      = v; break;
				case 0x0B: o->arg        = v; break;
				case 0x7D: o->prio       = v; have |= 1; break;
				case 0x7E: o->stack_size = v; have |= 2; break;
				case 0x7F: o->stack      = v; have |= 4; break;
				}
			}
		} else if (type == 1 && ms && va) {
			/* A segment at 0 is the module table, which MLOAD never copies */
			if (fs > ms)
				return SDL_R_ELF;
			if (va < base || va > end || ms > end - va)
				return SDL_R_RANGE;
			if (off > fsize || fs > fsize - off)
				return SDL_R_ELF;

			for (k = 0; k < o->nseg; k++)
				if (va < o->seg[k].vaddr + o->seg[k].memsz &&
				    o->seg[k].vaddr < va + ms)
					return SDL_R_RANGE;

			o->seg[o->nseg].off    = off;
			o->seg[o->nseg].filesz = fs;
			o->seg[o->nseg].memsz  = ms;
			o->seg[o->nseg].vaddr  = va;
			o->nseg++;
			if (va < o->lo)
				o->lo = va;
			if (va + ms > o->hi)
				o->hi = va + ms;
		}
	}

	if (!o->nseg || have != 7)
		return SDL_R_ELF;

	/* The thread must start and run inside the module's own memory */
	if ((o->entry & ~1U) < o->lo || (o->entry & ~1U) >= o->hi)
		return SDL_R_RANGE;
	if (o->stack < o->lo || o->stack > o->hi || !o->stack_size ||
	    o->stack_size > o->stack - o->lo)
		return SDL_R_RANGE;
	if (!o->prio || o->prio > SDL_MAX_PRIO)
		return SDL_R_RANGE;

	for (k = 0; k < ntaken; k++)
		if (o->lo < taken[k].b && taken[k].a < o->hi)
			return SDL_R_RANGE;

	return 0;
}


/* ---- The run ---- */

static u32 sdl_now(void)
{
	return (u32)Swi_MLoad(3, SDL_TIMER_REG, 0, 0);
}

/* Ticks to milliseconds without a divide: 69 / 2^17 is 1 / 1899.7 */
static u32 sdl_ms(u32 ticks)
{
	if (ticks > 60000000)
		ticks = 60000000;

	return (ticks * 69) >> 17;
}

/* 0 to go on, or why the run must end now */
static s32 sdl_stop(void)
{
	if (Swi_GetRunningTitle())
		return SDL_E_TITLE;
	if ((s32)(sdl_now() - S.start) >= SDL_DEADLINE_TICKS)
		return SDL_E_TIMEOUT;

	return 0;
}

static s32 sdl_icinv(void *in, void *out)
{
	ICInvalidate();
	return 0;
}

static s32 sdl_fat(u32 cmd, u32 nin)
{
	sdl_part        = -1;
	sdl_vec[0].data = &sdl_part;
	sdl_vec[0].len  = sizeof(sdl_part);
	os_sync_after_write(&sdl_part, sizeof(sdl_part));
	os_sync_after_write(sdl_vec, sizeof(sdl_vec));

	return os_ioctlv(S.fatfd, cmd, nin, 0, sdl_vec);
}

static void sdl_finish(s32 err)
{
	u32 i;

	if (err && !S.error)
		S.error = err;

	if (S.mounted) {
		sdl_fat(SDL_FAT_UMOUNT_SD, 0);
		S.mounted = 0;
	}
	if (S.fatfd >= 0) {
		os_close(S.fatfd);
		S.fatfd = -1;
	}
	if (S.timer >= 0) {
		os_stop_timer(S.timer);
		os_destroy_timer(S.timer);
		S.timer = -1;
	}

	S.rep.loaded  = 0;
	S.rep.skipped = S.extra;
	for (i = 0; i < S.nitems; i++) {
		if (S.rep.entries[i].result == 0)
			S.rep.loaded++;
		else
			S.rep.skipped++;
	}

	S.rep.elapsed_ms = sdl_ms(sdl_now() - S.start);
	S.state = SDL_ST_DONE;
}

/* Read and start one module. Returns 0 or SDL_R_*; SDL_E_TIMEOUT and
   SDL_E_TITLE end the whole run. The entry gets its size and detail. */
static s32 sdl_load_one(u32 i)
{
	sdl_item  *it = &S.items[i];
	sdl_entry *en = &S.rep.entries[i];
	sdl_elf   *e  = &S.elf;
	u32 size = 0, crc = 0xFFFFFFFF, pos, k, c, o;
	s32 fd, n, r = 0, det = 0;

	if (!it->name_ok)
		return SDL_R_NAME;

	memcpy(sdl_path, SDL_DIR_PATH, sizeof(SDL_DIR_PATH) - 1);
	for (k = 0; it->name[k]; k++)
		sdl_path[sizeof(SDL_DIR_PATH) - 1 + k] = it->name[k];
	sdl_path[sizeof(SDL_DIR_PATH) - 1 + k] = 0;

	fd = os_open(sdl_path, SDL_OPEN_READ);
	if (fd < 0) {
		en->detail = fd;
		return SDL_R_OPEN;
	}

	/* Pass 1: the size and the CRC, and the head kept for the validator.
	   Nothing is written to the area yet. */
	for (;;) {
		r = sdl_stop();
		if (r)
			goto close;

		n = os_read(fd, sdl_buf, SDL_BUF);
		if (n < 0) {
			r = SDL_R_READ;
			det = n;
			goto close;
		}
		if (n == 0)
			break;

		os_sync_before_read(sdl_buf, n);
		if (size < SDL_HEAD) {
			c = SDL_HEAD - size;
			memcpy(sdl_head + size, sdl_buf, (u32)n < c ? (u32)n : c);
		}
		if (it->has_crc)
			crc = sdl_crc32(crc, sdl_buf, n);

		size += n;
		if (size > SDL_AREA_SIZE) {
			r = SDL_R_RANGE;
			goto close;
		}
	}

	en->size = size;

	if (it->has_crc && ~crc != it->crc) {
		r = SDL_R_CRC;
		goto close;
	}

	r = sdl_check_elf(sdl_head, size < SDL_HEAD ? size : SDL_HEAD, size,
			  S.taken, S.ntaken, e);
	if (r)
		goto close;

	/* Pass 2: copy the segments in. The area is zeroed first, as Elf_Load
	   does. Past the budget, or with a title started, it is not touched. */
	r = sdl_stop();
	if (r)
		goto close;
	if (os_seek(fd, 0, 0) < 0) {
		r = SDL_R_READ;
		goto close;
	}

	for (k = 0; k < e->nseg; k++) {
		os_sync_before_read(SDL_PTR(e->seg[k].vaddr), e->seg[k].memsz);
		memset(SDL_PTR(e->seg[k].vaddr), 0, e->seg[k].memsz);
	}

	for (pos = 0; pos < size; pos += n) {
		r = sdl_stop();
		if (r)
			goto close;

		c = size - pos < SDL_BUF ? size - pos : SDL_BUF;
		n = os_read(fd, sdl_buf, c);
		if (n <= 0) {
			r = SDL_R_READ;
			det = n;
			goto close;
		}
		os_sync_before_read(sdl_buf, n);

		/* The part of this chunk that each segment holds */
		for (k = 0; k < e->nseg; k++) {
			u32 so = e->seg[k].off, se = so + e->seg[k].filesz;
			u32 a = pos > so ? pos : so;
			u32 b = pos + n < se ? pos + n : se;

			if (a < b)
				memcpy((u8 *)SDL_PTR(e->seg[k].vaddr) + (a - so),
				       sdl_buf + (a - pos), b - a);
		}
	}

	for (k = 0; k < e->nseg; k++)
		os_sync_after_write(SDL_PTR(e->seg[k].vaddr), e->seg[k].memsz);
	Swi_CallFunc(sdl_icinv, NULL, NULL);

close:
	os_close(fd);
	en->detail = det;
	if (r)
		return r;

	/* Start it. The box is taken whether or not the thread starts: a module
	   half run must not be written over. */
	S.taken[S.ntaken].a = e->lo;
	S.taken[S.ntaken].b = e->hi;
	S.ntaken++;

	/* The entry and the stack are addresses and the argument is a number:
	   all three go to the thread as they are */
	o = (u32)Elf_RunThread((void *)(uintptr_t)e->entry, (void *)(uintptr_t)e->arg,
			       (void *)(uintptr_t)e->stack, e->stack_size, e->prio);
	if ((s32)o < 0) {
		en->detail = (s32)o;
		return SDL_R_THREAD;
	}

	en->detail = (s32)o;
	return 0;
}

/* Read and parse sd:/d2x/mload.txt. Returns 0 or the overall error. */
static s32 sdl_read_list(void)
{
	s32 fd, n;
	u32 len = 0, i;
	s32 r;

	fd = os_open(SDL_LIST_PATH, SDL_OPEN_READ);
	if (fd < 0)
		return SDL_E_NOLIST;

	/* One read: the FAT module returns less than asked only at the end of
	   the file, so a short read is the whole list, and the buffer is never
	   read into at an unaligned address */
	r = sdl_stop();
	if (!r) {
		n = os_read(fd, sdl_buf, SDL_LIST_MAX);
		if (n < 0) {
			r = SDL_E_LISTIO;
		} else {
			os_sync_before_read(sdl_buf, n);
			len = n;
		}
	}
	os_close(fd);
	if (r)
		return r;

	S.nitems = sdl_parse((const char *)sdl_buf, len, len >= SDL_LIST_MAX,
			     S.items, SDL_MAX_MODULES, &S.extra);
	S.rep.count = S.nitems;
	for (i = 0; i < S.nitems; i++) {
		memcpy(S.rep.entries[i].name, S.items[i].name, SDL_NAME_MAX + 1);
		S.rep.entries[i].result = SDL_R_NOTRUN;
	}

	return 0;
}


/* ---- The interface ---- */

s32 SdLoad_Start(s32 queue)
{
	memset(&S, 0, sizeof(S));
	S.fatfd = -1;
	S.start = sdl_now();
	S.next_at = S.start;
	S.state = SDL_ST_WAIT_FAT;

	S.timer = os_create_timer(SDL_TICK_US, SDL_TICK_US, queue, SDL_COOKIE);
	if (S.timer < 0) {
		s32 t = S.timer;

		S.timer = -1;
		S.state = SDL_ST_DONE;
		S.error = SDL_E_TIMER;
		return t;
	}

	return S.timer;
}

void SdLoad_Tick(void)
{
	u32 now;
	s32 r;

	if (S.state == SDL_ST_IDLE || S.state == SDL_ST_DONE)
		return;

	r = sdl_stop();
	if (r) {
		/* Not finding the FAT module in time is not a slow load */
		sdl_finish(r == SDL_E_TIMEOUT && S.state == SDL_ST_WAIT_FAT ? SDL_E_NOFAT : r);
		return;
	}

	now = sdl_now();
	if ((s32)(now - S.next_at) < 0)
		return;
	S.next_at = now + SDL_TICK_TICKS;

	switch (S.state) {
	case SDL_ST_WAIT_FAT:
		S.fatfd = os_open("fat", 0);
		if (S.fatfd < 0)
			S.fatfd = -1;
		else
			S.state = SDL_ST_MOUNT;
		break;

	case SDL_ST_MOUNT:
		if (sdl_fat(SDL_FAT_MOUNT_SD, 1) >= 0) {
			S.mounted = 1;
			S.state = SDL_ST_LIST;
		} else if (++S.mount_tries >= SDL_MOUNT_TRIES) {
			sdl_finish(SDL_E_MOUNT);
		}
		break;

	case SDL_ST_LIST:
		r = sdl_read_list();
		if (r || !S.nitems)
			sdl_finish(r);
		else
			S.state = SDL_ST_LOAD;
		break;

	case SDL_ST_LOAD:
		r = sdl_load_one(S.idx);
		S.rep.entries[S.idx].result = r;
		if (r == SDL_E_TIMEOUT || r == SDL_E_TITLE) {
			sdl_finish(r);
		} else if (++S.idx >= S.nitems) {
			sdl_finish(0);
		}
		break;
	}
}

s32 SdLoad_GetReport(void *out, u32 len)
{
	if (len < sizeof(sdl_report))
		return IPC_EINVAL;

	S.rep.magic   = SDL_REPORT_MAGIC;
	S.rep.version = SDL_REPORT_VERSION;
	S.rep.state   = S.state;
	S.rep.error   = S.error;
	if (S.state != SDL_ST_IDLE && S.state != SDL_ST_DONE)
		S.rep.elapsed_ms = sdl_ms(sdl_now() - S.start);

	memcpy(out, &S.rep, sizeof(sdl_report));
	return 0;
}
