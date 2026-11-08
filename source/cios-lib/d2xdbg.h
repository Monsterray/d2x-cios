/*
 * d2x debug area: counters, one trace ring, last words, an ARM watchdog and
 * fault injection, for debug builds (-DD2X_DEBUG, the "+dbg" variant).
 *
 * Release builds compile every D2XDBG_*() call to nothing and register no
 * /dev/d2xdbg; their modules are byte-identical to a build without this file.
 *
 * The area sits at a fixed physical address in MEM2, in the gap between the
 * end of EHCI's RAM (0x137a9000) and FAT's code (0x137b5000) that no d2x
 * module uses (USBS ends lower still). Modules cannot write it themselves
 * (each runs as its own IOS process), so they send events to MLOAD through
 * SWI 0xcc, and MLOAD's SWI handler writes the area with every permission.
 * MLOAD turns MEM2 protection off, so the PowerPC can read the area directly
 * (uncached: 0xD37A9000) and draw it while IOS is stuck.
 *
 * This header is shared with PowerPC programs (tests/wii/d2xdiag): plain C,
 * fixed-size fields, big-endian u32 words as both CPUs see them.
 *
 * Layout (D2XDBG_LAYOUT 1):
 *   0x0000  d2xdbg_head (header, watchdog, faults, per-module counters)
 *   0x1000  trace ring: D2XDBG_RECORDS (256) records of 4 words
 *           [0] Hollywood timer ticks (243 MHz / 128, about 1898 per ms)
 *           [1] module << 24 | event << 16 | x (a command, 16 bits)
 *           [2] a, [3] b (see each event)
 *           head.trace_head counts records written; the next goes to
 *           trace_head % D2XDBG_RECORDS.
 *   0x2000  the previous run's copy of 0x0000-0x1FFF ("last words"):
 *           MLOAD copies the area here when it starts and finds a valid one
 *           (after an IOS reload, or a reset that left MEM2 as it was).
 */
#ifndef _D2XDBG_H_
#define _D2XDBG_H_

#define D2XDBG_PHYS		0x137A9000
#define D2XDBG_SIZE		0x4000
#define D2XDBG_HALF		0x2000		/* current run; then the previous */
#define D2XDBG_MAGIC		0x44325844	/* 'D2XD' */
#define D2XDBG_LAYOUT		1
#define D2XDBG_INSVC_OFF	0x280		/* 4 words: 'SV' << 16 | cmd, arg1..3 while MLOAD's debug SWI runs; 0 after */
#define D2XDBG_RING_OFF		0x1000
#define D2XDBG_RECORDS		256		/* a power of two: no divide in the index */

#define D2XDBG_DEVICE		"/dev/d2xdbg"

/* Modules: index into head.mod[] and the top byte of a record */
#define D2XDBG_M_EHCI		1
#define D2XDBG_M_USBS		2
#define D2XDBG_M_SD		3
#define D2XDBG_M_FAT		4
#define D2XDBG_M_DIP		5
#define D2XDBG_M_MLOAD		6	/* the device, the watchdog */
#define D2XDBG_MODS		8

/* Events with a counter; any other letter is only traced */
#define D2XDBG_E_BEGIN		'B'	/* request starts: x = cmd, a = arg (sector, offset), b = length */
#define D2XDBG_E_END		'E'	/* request ends: a = result (< 0 counts an error) */
#define D2XDBG_E_RETRY		'R'	/* a = attempt */
#define D2XDBG_E_RESET		'Z'	/* recovery reset: x = kind (b BOT, p port, d device) */
#define D2XDBG_E_TIMEOUT	'T'	/* a = what timed out */
#define D2XDBG_E_ERROR		'X'	/* an error outside a request's result: a = code */
#define D2XDBG_E_STEP		's'	/* a step inside a request: x = which ('f' fault point passed, 'd' deselected), a = its result, b = attempt */
#define D2XDBG_E_SDHS		'H'	/* SD high-speed state: a = sdio_hs state word */
#define D2XDBG_E_FAULT		'F'	/* MLOAD: an injected fault fired: x = point, a = action, b = fault slot */
#define D2XDBG_E_BOOT		'S'	/* MLOAD: area started: a = boots, b = 1 if last words were kept */
#define D2XDBG_E_WDARM		'A'	/* MLOAD: watchdog armed: a = timeout ms (0 = off) */
#define D2XDBG_E_WDFIRE		'W'	/* MLOAD: watchdog resets the console: a = ms since the last check-in */
#define D2XDBG_E_NORESET	'N'	/* MLOAD: the watchdog's reset did not take; it is off again */
#define D2XDBG_E_HANG		'G'	/* MLOAD: a hang requested through the device: a = mode */
#define D2XDBG_E_USBOFF		'U'	/* MLOAD: before the watchdog's reset, USB ports reset (and power off): a = HCSPARAMS, b = port 0 status */

/* Fault points (SWI 65): only a fault that fires is traced. x = point */
#define D2XDBG_P_DATA		'd'	/* USB data phase (EHCI, USBS) */
#define D2XDBG_P_CSW		'c'	/* USB status (CSW) read */
#define D2XDBG_P_CMD		'k'	/* USB command (CBW) send */
#define D2XDBG_P_REQ		'q'	/* a module request as a whole (SD, FAT, DIP) */
#define D2XDBG_P_DI		'i'	/* DIP: a DI command it handles */

/* Fault actions: action word = F << 24 | arg */
#define D2XDBG_F_ERROR		1	/* the request fails with the module's own error */
#define D2XDBG_F_STALL		2	/* USB: the endpoint STALLs (a short transfer) */
#define D2XDBG_F_LOSE		3	/* USB: the CSW never comes (a timeout on the status) */
#define D2XDBG_F_TIMEOUT	4	/* the transfer times out */
#define D2XDBG_F_DELAY		5	/* sleep arg ms, then go on normally (cios-lib does it) */
#define D2XDBG_F_BLOCK		6	/* the thread stops itself for good (cios-lib does it) */
#define D2XDBG_F_SPIN		7	/* the thread spins for good (cios-lib does it) */
#define D2XDBG_F_SDLOW		8	/* SD: fall back from high speed, as after a failed transfer */
#define D2XDBG_F_BIG		9	/* USBS: the whole data phase in one transfer, past IOS58's
					   16 KB limit: hangs /dev/usb/ven (a real IOS hang) */
#define D2XDBG_FAULTS		8

/* /dev/d2xdbg ioctls (IOS_Ioctl, words in and out) */
#define D2XDBG_IOCTL_INFO	0x00	/* out: [0] phys, [1] size, [2] layout, [3] magic */
#define D2XDBG_IOCTL_READ	0x01	/* in: [0] offset; out: that many bytes of the area */
#define D2XDBG_IOCTL_CLEAR	0x02	/* counters, trace and faults back to zero */
#define D2XDBG_IOCTL_WD_ARM	0x10	/* in: [0] timeout ms (0 = off, at most D2XDBG_WD_MAX_MS) */
#define D2XDBG_IOCTL_WD_KICK	0x11	/* check in (the same as head.wd_kick++ in memory) */
#define D2XDBG_IOCTL_FAULT	0x20	/* in: [0] slot, [1] mod << 8 | point, [2] count, [3] skip, [4] action */
#define D2XDBG_IOCTL_HANG	0x30	/* in: [0] mode: 1 block, 2 spin, 3 spin at top priority. Never returns */

#define D2XDBG_WD_MAX_MS	600000	/* 10 min; the timer wraps after about 37 */
#define D2XDBG_WD_PERIOD_US	100000	/* the watchdog thread checks this often */

/* What reset the console: Priiloader's magic word, at physical 0x017FEFF0,
   makes it autoboot whatever its boot state says. */
#define D2XDBG_ABRA_PHYS	0x017FEFF0
#define D2XDBG_ABRA		0x41627261	/* 'Abra' */

/* head.flags: what the build was compiled with */
#define D2XDBG_FL_DEBUG		0x01
#define D2XDBG_FL_SD_HS		0x02	/* SDIO_HIGH_SPEED */
#define D2XDBG_FL_SD_HS_DEBUG	0x04	/* SDIO_HS_DEBUG */
#define D2XDBG_FL_DIP_TRACE	0x08	/* DIP_DEBUG_TRACE */
#define D2XDBG_FL_DIP_GAPERR	0x10	/* DIP_GAP_ERROR */
#define D2XDBG_FL_EHCI_LOG	0x20	/* EHCI_DEBUG_LOG */

#ifndef _LANGUAGE_ASSEMBLY

/* PowerPC programs (gctypes.h) and host tests bring their own u8/u32/s32 */
#ifndef D2XDBG_HAVE_TYPES
#include "types.h"
#endif

typedef struct {
	u32 requests;		/* 'B' events */
	u32 errors;		/* 'E' with a < 0, and 'X' */
	u32 retries;		/* 'R' */
	u32 resets;		/* 'Z' */
	u32 timeouts;		/* 'T' */
	u32 faults;		/* injected faults that fired */
	u32 max_ticks;		/* the longest request: its length in ticks */
	u32 max_cmd;		/* its command (x) */
	u32 max_arg;		/* its argument (a) */
	u32 cur_start;		/* the open request: start ticks (0 = none open) */
	u32 cur_cmd;
	u32 cur_arg;
} d2xdbg_mod;			/* 48 bytes */

typedef struct {
	u32 point;		/* mod << 8 | point; 0 = free */
	u32 count;		/* times left to fire */
	u32 skip;		/* matching passes to let through first */
	u32 action;		/* D2XDBG_F_* << 24 | arg */
} d2xdbg_fault;			/* 16 bytes */

typedef struct {
	u32 magic;		/* 0x000 D2XDBG_MAGIC */
	u32 layout;		/* 0x004 D2XDBG_LAYOUT */
	u32 size;		/* 0x008 D2XDBG_SIZE */
	u32 boots;		/* 0x00C MLOAD starts that found this area (1 = new) */
	char build[32];		/* 0x010 build identity: version string and commit */
	u32 flags;		/* 0x030 D2XDBG_FL_* */
	u32 sd_hs;		/* 0x034 SD high-speed state, last 'H' event */
	u32 trace_head;		/* 0x038 records written */
	u32 now;		/* 0x03C timer at the last event or watchdog check */
	u32 wd_timeout_ms;	/* 0x040 0 = off */
	u32 wd_seen_kick;	/* 0x044 the ARM's copy of wd_kick */
	u32 wd_seen_ticks;	/* 0x048 when the ARM saw it change */
	u32 wd_fired;		/* 0x04C timer when the watchdog reset the console; 0 = never */
	u32 wd_beats;		/* 0x050 watchdog thread checks (it is alive) */
	u32 wd_prio;		/* 0x054 the watchdog thread's priority (0 = not running) */
	u32 wd_noreset;		/* 0x058 timer when the watchdog's reset did not take; 0 = never */
	u32 map_fixes;		/* 0x05C times an SWI found the area's page entries gone and put them back */
	u32 wd_kick;		/* 0x060 the PowerPC adds one to check in. Alone in its
				   32-byte cache line: the ARM only reads it, so its
				   write-backs never undo a check-in */
	u32 resv_kick[7];	/* 0x064 */
	d2xdbg_fault fault[D2XDBG_FAULTS];	/* 0x080 */
	d2xdbg_mod mod[D2XDBG_MODS];		/* 0x100, 8 x 48 = 0x180 */
} d2xdbg_head;			/* 0x280 bytes; the rest up to the ring is spare */

/* Pure logic (mload-module/dbg_core.c), host-tested in tests/host/dbg */
void d2xdbg_start(u8 *area, const char *build, u32 flags, u32 now);
s32  d2xdbg_event(u8 *area, u32 now, u32 word, u32 a, u32 b);
u32  d2xdbg_point(u8 *area, u32 now, u32 mod, u32 point, u32 a);
void d2xdbg_clear(u8 *area);
void d2xdbg_wd_arm(u8 *area, u32 now, u32 ms);
s32  d2xdbg_wd_check(u8 *area, u32 now);
void d2xdbg_wd_noreset(u8 *area, u32 now);
s32  d2xdbg_fault_set(u8 *area, const u32 *in);
u32  d2xdbg_ms_to_ticks(u32 ms);

/* Module side (cios-lib/d2xdbg.c): events and fault points through MLOAD */
#ifdef D2X_DEBUG
#define D2XDBG_SWI_EVENT	64
#define D2XDBG_SWI_POINT	65
#define D2XDBG_SWI_WDTICK	66
#define D2XDBG_SWI_CTRL		67

s32 Swi_MLoad(u32 arg0, u32 arg1, u32 arg2, u32 arg3);	/* cios-lib tools.s */
u32 Dbg_Point(u32 mod, u32 point, u32 a);
#define D2XDBG_EV(mod, ev, x, a, b) \
	Swi_MLoad(D2XDBG_SWI_EVENT, ((u32)(mod) << 24) | ((u32)(ev) << 16) | ((x) & 0xFFFF), (u32)(a), (u32)(b))
#define D2XDBG_POINT(mod, point, a)	Dbg_Point(mod, point, a)
#else
#define D2XDBG_EV(mod, ev, x, a, b)	do {} while (0)
#define D2XDBG_POINT(mod, point, a)	0
#endif

#define D2XDBG_FAULT_ACTION(r)	((r) >> 24)
#define D2XDBG_FAULT_ARG(r)	((r) & 0xFFFFFF)

#endif /* _LANGUAGE_ASSEMBLY */

#endif
