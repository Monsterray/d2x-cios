/*
	d2x cIOS: load extra modules from the SD card.
	See sdload.h and docs/mload.md.

	The loader is a state machine, one small step at a time (a mount, one
	2 KB chunk of a file). It never touches the SD card on its own:
	- MLOAD_SD_LOAD runs the steps back to back, inside the ioctlv.
	- A flag file on the NAND makes MLOAD run them at cIOS start instead, on a
	  one-shot timer that MLOAD's main loop re-arms after each step: at most
	  one timer message is ever queued, and the PPC's requests get their turn
	  between steps. MLOAD_GET_SD_REPORT waits for such a run to end.
	The budget of SDL_DEADLINE_MS is checked before every step. One FAT call
	can still block past it (docs/mload.md, "Known limits").
*/

#include "types.h"

#include <string.h>

#include "elf.h"
#include "ipc.h"
#include "sdload.h"
#include "swi_mload.h"
#include "syscalls.h"
#include "timer.h"
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
#define SDL_PENDING		1	/* a module's step is done, more steps follow */
#define SDL_FLAG_NOENT		(-106)	/* the file system's "no such file" */

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

/* The module being loaded, between steps */
typedef struct {
	s32 fd;				/* -1 when none is open */
	u32 phase, size, crc, pos;
} sdl_mod;

#define SDL_M_OPEN		0	/* sdl_mod.phase */
#define SDL_M_PASS1		1	/* reading for the size, the CRC and the head */
#define SDL_M_PASS2		2	/* copying the segments in */

static struct {
	u32 state, mode;
	s32 error, flag;
	s32 timer, fatfd, queue;
	u32 start;
	u32 mount_tries, idx, nitems, extra, ntaken;
	u32 mounted;
	sdl_mod   m;
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
   module, or cut short), SDL_R_ALIGN (a segment that is not whole 32 byte
   cache lines: the cache calls work on lines) or SDL_R_RANGE (it would write
   outside the area, or over another module, or its entry or stack is not
   inside one of its own segments). */
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
				case 0x0B: o->arg        = v; have |= 8; break;
				case 0x7D: o->prio       = v; have |= 1; break;
				case 0x7E: o->stack_size = v; have |= 2; break;
				case 0x7F: o->stack      = v; have |= 4; break;
				}
			}
		} else if (type == 1 && ms && va) {
			/* A segment at 0 is the module table, which MLOAD never copies */
			if (fs > ms)
				return SDL_R_ELF;
			if ((va | ms) & 31)
				return SDL_R_ALIGN;
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

	if (!o->nseg || have != 15)
		return SDL_R_ELF;

	/* The thread must start and run inside one segment of the module: the
	   box around them can hold gaps that nothing wrote or zeroed */
	if (!o->stack_size || o->stack_size > o->stack)
		return SDL_R_RANGE;
	for (k = 0, i = 0, j = 0; k < o->nseg; k++) {
		u32 a = o->seg[k].vaddr, b = a + o->seg[k].memsz;

		if ((o->entry & ~1U) >= a && (o->entry & ~1U) < b)
			i = 1;
		if (o->stack - o->stack_size >= a && o->stack <= b)
			j = 1;
	}
	if (!i || !j)
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

	/* A module in the middle of its load: closed, and it says why */
	if (S.m.fd >= 0) {
		os_close(S.m.fd);
		S.m.fd = -1;
		S.rep.entries[S.idx].result = err;
	}
	S.m.phase = SDL_M_OPEN;

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

/* One step of loading module i: one open, one 2 KB chunk, or the end of a
   pass. Returns SDL_PENDING while more steps follow, else 0 or SDL_R_*;
   SDL_E_TIMEOUT and SDL_E_TITLE end the whole run. The entry gets its size
   and detail. Pass 1 (size, CRC, the head for the validator) writes nothing to
   the area; pass 2 zeroes the segments and copies them in. */
static s32 sdl_load_step(u32 i)
{
	sdl_item  *it = &S.items[i];
	sdl_entry *en = &S.rep.entries[i];
	sdl_elf   *e  = &S.elf;
	sdl_mod   *m  = &S.m;
	u32 k, c, o;
	s32 n, r;

	if (m->phase == SDL_M_OPEN) {
		if (!it->name_ok)
			return SDL_R_NAME;

		memcpy(sdl_path, SDL_DIR_PATH, sizeof(SDL_DIR_PATH) - 1);
		for (k = 0; it->name[k]; k++)
			sdl_path[sizeof(SDL_DIR_PATH) - 1 + k] = it->name[k];
		sdl_path[sizeof(SDL_DIR_PATH) - 1 + k] = 0;

		m->fd = os_open(sdl_path, SDL_OPEN_READ);
		if (m->fd < 0) {
			en->detail = m->fd;
			m->fd = -1;
			return SDL_R_OPEN;
		}
		m->phase = SDL_M_PASS1;
		m->size  = 0;
		m->crc   = 0xFFFFFFFF;
		m->pos   = 0;
	}

	if (m->phase == SDL_M_PASS1) {
		n = os_read(m->fd, sdl_buf, SDL_BUF);
		if (n < 0) {
			en->detail = n;
			r = SDL_R_READ;
			goto fail;
		}

		if (n > 0) {
			os_sync_before_read(sdl_buf, n);
			if (m->size < SDL_HEAD) {
				c = SDL_HEAD - m->size;
				memcpy(sdl_head + m->size, sdl_buf, (u32)n < c ? (u32)n : c);
			}
			if (it->has_crc)
				m->crc = sdl_crc32(m->crc, sdl_buf, n);

			m->size += n;
			if (m->size > SDL_AREA_SIZE) {
				r = SDL_R_RANGE;
				goto fail;
			}
			return SDL_PENDING;
		}

		/* The whole file has been read: check it before the area is touched */
		en->size = m->size;

		if (it->has_crc && ~m->crc != it->crc) {
			r = SDL_R_CRC;
			goto fail;
		}

		r = sdl_check_elf(sdl_head, m->size < SDL_HEAD ? m->size : SDL_HEAD,
				  m->size, S.taken, S.ntaken, e);
		if (r)
			goto fail;

		/* Past the budget, or with a title started, the area is not touched */
		r = sdl_stop();
		if (r)
			goto fail;
		if (os_seek(m->fd, 0, 0) < 0) {
			r = SDL_R_READ;
			goto fail;
		}

		/* The segments are zeroed first, as Elf_Load does */
		for (k = 0; k < e->nseg; k++) {
			os_sync_before_read(SDL_PTR(e->seg[k].vaddr), e->seg[k].memsz);
			memset(SDL_PTR(e->seg[k].vaddr), 0, e->seg[k].memsz);
		}

		m->phase = SDL_M_PASS2;
		return SDL_PENDING;
	}

	/* Pass 2: one chunk of the file into the segments that hold it */
	c = m->size - m->pos < SDL_BUF ? m->size - m->pos : SDL_BUF;
	n = os_read(m->fd, sdl_buf, c);
	if (n <= 0) {
		en->detail = n;
		r = SDL_R_READ;
		goto fail;
	}
	os_sync_before_read(sdl_buf, n);

	for (k = 0; k < e->nseg; k++) {
		u32 so = e->seg[k].off, se = so + e->seg[k].filesz;
		u32 a = m->pos > so ? m->pos : so;
		u32 b = m->pos + n < se ? m->pos + n : se;

		if (a < b)
			memcpy((u8 *)SDL_PTR(e->seg[k].vaddr) + (a - so),
			       sdl_buf + (a - m->pos), b - a);
	}

	m->pos += n;
	if (m->pos < m->size)
		return SDL_PENDING;

	for (k = 0; k < e->nseg; k++)
		os_sync_after_write(SDL_PTR(e->seg[k].vaddr), e->seg[k].memsz);
	Swi_CallFunc(sdl_icinv, NULL, NULL);

	os_close(m->fd);
	m->fd = -1;

	/* Start it. The box is taken whether or not the thread starts: a module
	   half run must not be written over. */
	S.taken[S.ntaken].a = e->lo;
	S.taken[S.ntaken].b = e->hi;
	S.ntaken++;

	/* The entry and the stack are addresses and the argument is a number:
	   all three go to the thread as they are */
	o = (u32)Elf_RunThread((void *)(uintptr_t)e->entry, (void *)(uintptr_t)e->arg,
			       (void *)(uintptr_t)e->stack, e->stack_size, e->prio);
	en->detail = (s32)o;
	return (s32)o < 0 ? SDL_R_THREAD : 0;

fail:
	os_close(m->fd);
	m->fd = -1;
	return r;
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

static void sdl_begin(u32 mode)
{
	S.mode  = mode;
	S.error = 0;
	S.start = sdl_now();
	S.state = SDL_ST_WAIT_FAT;
}

/* One step of the state machine, with no pacing of its own */
static void sdl_step(void)
{
	s32 r = sdl_stop();

	if (r) {
		/* Not finding the FAT module in time is not a slow load */
		sdl_finish(r == SDL_E_TIMEOUT && S.state == SDL_ST_WAIT_FAT ? SDL_E_NOFAT : r);
		return;
	}

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
		r = sdl_load_step(S.idx);
		if (r == SDL_PENDING)
			break;

		S.m.phase = SDL_M_OPEN;
		S.rep.entries[S.idx].result = r;
		if (r == SDL_E_TIMEOUT || r == SDL_E_TITLE)
			sdl_finish(r);
		else if (++S.idx >= S.nitems)
			sdl_finish(0);
		break;
	}
}

/* Run the steps back to back until the run ends; the FAT module's start is
   polled every SDL_TICK_US. Bounded by the budget, which sdl_step checks. */
static void sdl_drive(void)
{
	while (S.state != SDL_ST_DONE) {
		sdl_step();
		if (S.state == SDL_ST_WAIT_FAT)
			Timer_Sleep(SDL_TICK_US);
	}
}

static s32 sdl_vec_ok(const ioctlv *v, u32 inlen, u32 iolen)
{
	if (inlen != 0 || iolen < 1 || !v || !v[0].data || v[0].len < sizeof(sdl_report))
		return IPC_EINVAL;

	return 0;
}

static void sdl_fill(void *out)
{
	S.rep.magic   = SDL_REPORT_MAGIC;
	S.rep.version = SDL_REPORT_VERSION;
	S.rep.state   = S.state;
	S.rep.error   = S.error;
	S.rep.mode    = S.mode;
	S.rep.flag    = S.flag;
	if (S.state != SDL_ST_IDLE && S.state != SDL_ST_DONE)
		S.rep.elapsed_ms = sdl_ms(sdl_now() - S.start);

	memcpy(out, &S.rep, sizeof(sdl_report));
}

void SdLoad_Boot(s32 queue)
{
	s32 fd;

	memset(&S, 0, sizeof(S));
	S.fatfd = S.timer = S.m.fd = -1;

	/* The only thing done without being asked: one open of a file on the
	   NAND. No flag, no timer, no SD, no FAT. */
	fd = os_open(SDL_FLAG_PATH, SDL_OPEN_READ);
	S.flag = fd < 0 ? fd : 0;
	if (fd < 0)
		return;
	os_close(fd);

	S.timer = os_create_timer(SDL_TICK_US, 0, queue, SDL_COOKIE);
	if (S.timer < 0) {
		S.timer = -1;
		S.error = SDL_E_TIMER;
		return;
	}

	sdl_begin(SDL_M_AUTO);
}

void SdLoad_Tick(void)
{
	if (S.state == SDL_ST_IDLE || S.state == SDL_ST_DONE || S.timer < 0)
		return;

	sdl_step();

	/* One shot: the next message is queued only now, so a long step cannot
	   leave a pile of them in MLOAD's 8-entry queue. Polling for the FAT
	   module waits SDL_TICK_US; the other steps SDL_STEP_US, just long enough
	   for the PPC's requests to get in between. */
	if (S.state != SDL_ST_DONE)
		os_restart_timer(S.timer, S.state == SDL_ST_WAIT_FAT ? SDL_TICK_US : SDL_STEP_US, 0);
}

void SdLoad_PpcRequest(void)
{
	/* A boot-time run is between two steps and the PPC is about to write
	   the area it loads into: stop before the PPC's request is handled, and
	   never write the area again */
	if (S.state != SDL_ST_IDLE && S.state != SDL_ST_DONE)
		sdl_finish(SDL_E_BUSY);
}

s32 SdLoad_IoctlLoad(ioctlv *vector, u32 inlen, u32 iolen)
{
	s32 r = sdl_vec_ok(vector, inlen, iolen);

	if (r)
		return r;

	if (S.state == SDL_ST_IDLE) {
		sdl_begin(SDL_M_IOCTL);
		sdl_drive();
	} else {
		/* It ran, or a boot-time run is still going: wait for it, load nothing */
		sdl_drive();
		r = SDL_ALREADY;
	}

	sdl_fill(vector[0].data);
	return r;
}

s32 SdLoad_IoctlReport(ioctlv *vector, u32 inlen, u32 iolen)
{
	s32 r = sdl_vec_ok(vector, inlen, iolen);

	if (r)
		return r;

	/* An app that knows the boot-time run exists waits here, then opens the SD card */
	if (S.state != SDL_ST_IDLE)
		sdl_drive();

	sdl_fill(vector[0].data);
	return 0;
}
