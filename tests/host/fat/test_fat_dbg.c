/*
 * Host tests of the FAT module's debug hooks (-DD2X_DEBUG, d2xdbg.h) in the
 * IPC dispatch loop of source/fat-module/main.c: a B and an E event for each
 * message, and a D2XDBG_P_REQ fault that fails the request with FS_EFATAL
 * without running it. The module's real main() runs on the PC: IOS's message
 * queue is a script of messages, and the loop ends when the script does (the
 * queue stub jumps out). The debug area runs on the PC (dbg/swi_host.c).
 * Run with `make -C tests/host`.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HOSTCHECK_MAIN
#include "hostcheck.h"

/* cios-lib's types.h typedefs bool itself */
#undef bool
#undef true
#undef false

#include "fat_types.h"
#include "types.h"
#include "sdio.h"
#include "usbstorage.h"

bool sdio_Startup(void) { return true; }
bool usbstorage_Init(void) { return true; }
bool sdio_Shutdown(void) { return true; }
bool usbstorage_Shutdown(void) { return true; }

/* Only main() allocates (the queue's buffer, once per run, never freed): a
 * bump arena, so the leak check stays quiet about it. */
static uint8_t arena[1 << 16] __attribute__((aligned(32)));
static size_t arena_used;
void *Mem_Alloc(u32 size)
{
	void *p = arena + arena_used;

	if (arena_used + size + 32 > sizeof(arena))
		return NULL;
	arena_used += (size + 63) & ~31u;
	memset(p, 0, size);
	return p;
}
void Mem_Free(void *ptr) { (void)ptr; }
s32 Mem_Init(u32 *heapspace, u32 heapspaceSize) { (void)heapspace; (void)heapspaceSize; return 0; }

#include "fat_tools.c"
#include "fat_wrapper.c"

/* ---- IOS, scripted ---- */
#define MAX_MSGS 8
static ipcmessage *script[MAX_MSGS];
static unsigned n_script, next_msg;
static jmp_buf loop_exit;

struct ack { ipcmessage *msg; s32 result; };
static struct ack acks[MAX_MSGS];
static unsigned n_acks;
static unsigned led_on;		/* IOS_WRITE reached its handler */

void __os_sync_before_read(void *ptr, s32 size) { (void)ptr; (void)size; }
void __os_sync_after_write(void *ptr, s32 size) { (void)ptr; (void)size; }
s32 os_message_queue_create(void *ptr, u32 id) { (void)ptr; (void)id; return 5; }
s32 os_device_register(const char *name, s32 q) { (void)name; (void)q; return 0; }
void svc_write(const char *str) { (void)str; }
void InvalidateVector(ioctlv *vector, u32 inlen, u32 iolen) { (void)vector; (void)inlen; (void)iolen; }
void FlushVector(ioctlv *vector, u32 inlen, u32 iolen) { (void)vector; (void)inlen; (void)iolen; }
void Led_BlinkOn(void) { led_on++; }
void Led_BlinkOff(void) { }
void Led_CreateBlinkThread(void) { }
s32 Timer_Init(void) { return 0; }

/* main() passes the address of its ipcmessage pointer as a u32 pointer */
s32 os_message_queue_receive(s32 q, u32 *message, u32 flags)
{
	(void)q; (void)flags;
	if (next_msg >= n_script)
		longjmp(loop_exit, 1);
	*(ipcmessage **)message = script[next_msg++];
	return 0;
}

void os_message_queue_ack(void *message, s32 result)
{
	if (n_acks < MAX_MSGS) {
		acks[n_acks].msg = message;
		acks[n_acks].result = result;
	}
	n_acks++;
}

/* main.c's dispatch loop, with its main() renamed */
#define main fat_module_main
#include "main.c"
#undef main

#include "dbg_host.h"

#define FAT D2XDBG_M_FAT

/* ---- tests ---- */
static u32 sleeps_ms;
static void sleep_ms(u32 ms) { sleeps_ms += ms; }

static jmp_buf hang_jump;
static u32 hang_action;
static void on_hang(u32 action) { hang_action = action; longjmp(hang_jump, 1); }

static ipcmessage msgs[MAX_MSGS];

/* Run the module's main() on the scripted messages (msgs[0..n)) until it has
 * taken them all. */
static void run(unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++)
		script[i] = &msgs[i];
	n_script = n;
	next_msg = 0;
	n_acks = 0;
	led_on = 0;
	if (!setjmp(loop_exit))
		fat_module_main();
}

static void fresh(void)
{
	memset(msgs, 0, sizeof(msgs));
	n_script = next_msg = n_acks = 0;
	sleeps_ms = 0;
	dbg_host_sleep_ms = sleep_ms;
	dbg_host_reset();
}

/* The last record of module FAT with this event: its x, a and b */
static int last_rec(u32 ev, u32 *x, u32 *a, u32 *b)
{
	d2xdbg_head *h = DBG_HEAD;
	const u32 *ring = (const u32 *)(dbg_area + D2XDBG_RING_OFF);
	u32 i, first = h->trace_head > D2XDBG_RECORDS ? h->trace_head - D2XDBG_RECORDS : 0;

	for (i = h->trace_head; i > first; i--) {
		const u32 *r = &ring[((i - 1) & (D2XDBG_RECORDS - 1)) * 4];

		if ((r[1] >> 24) == FAT && ((r[1] >> 16) & 0xFF) == ev) {
			*x = r[1] & 0xFFFF;
			*a = r[2];
			*b = r[3];
			return 1;
		}
	}
	return 0;
}

static void open_fat(ipcmessage *m, const char *dev)
{
	m->command = IOS_OPEN;
	m->open.device = (char *)dev;
	m->open.mode = 0;
}

static void test_message_traces_begin_and_end(void)
{
	d2xdbg_mod *m = DBG_MOD(FAT);
	u32 x, a, b;

	fresh();
	open_fat(&msgs[0], "fat");
	run(1);
	CHECK(n_acks == 1 && acks[0].msg == &msgs[0] && acks[0].result == 0, "acked %u, result %d", n_acks, (int)acks[0].result);
	CHECK(m->requests == 1 && m->errors == 0, "counters %u/%u", m->requests, m->errors);
	CHECK(dbg_host_count(FAT, D2XDBG_E_BEGIN) == 1 && dbg_host_count(FAT, D2XDBG_E_END) == 1, "B and E traced");
	CHECK(last_rec(D2XDBG_E_BEGIN, &x, &a, &b) && x == (IOS_OPEN << 8), "B: x %x", x);
	CHECK(last_rec(D2XDBG_E_END, &x, &a, &b) && x == IOS_OPEN && a == 0, "E: x %x a %u", x, a);
	CHECK(m->cur_start == 0, "the request is closed");
}

static void test_read_event_carries_fd_and_length(void)
{
	d2xdbg_mod *m = DBG_MOD(FAT);
	u32 x, a, b;

	fresh();
	msgs[0].command = IOS_READ;
	msgs[0].fd = 3;			/* not a file's fd: the module refuses it */
	msgs[0].read.data = NULL;
	msgs[0].read.length = 4096;
	run(1);
	CHECK(n_acks == 1 && acks[0].result == IPC_EINVAL, "result %d", (int)acks[0].result);
	CHECK(last_rec(D2XDBG_E_BEGIN, &x, &a, &b), "B traced");
	CHECK(x == (IOS_READ << 8) && a == 3 && b == 4096, "B: x %x a %x b %u", x, a, b);
	CHECK(last_rec(D2XDBG_E_END, &x, &a, &b) && x == IOS_READ && (int)a == IPC_EINVAL, "E: x %x a %d", x, (int)a);
	CHECK(m->errors == 1, "a negative result counts as an error: %u", m->errors);
	CHECK(m->max_cmd == (IOS_READ << 8) && m->max_arg == 3, "longest: cmd %x arg %x", m->max_cmd, m->max_arg);
}

static void test_ioctl_event_carries_the_ioctl_number(void)
{
	u32 x, a, b;

	fresh();
	msgs[0].command = IOS_IOCTLV;
	msgs[0].fd = 3;
	msgs[0].ioctlv.command = 0x1234;
	msgs[0].ioctlv.num_in = 0;
	msgs[0].ioctlv.num_io = 0;
	msgs[1].command = IOS_IOCTL;
	msgs[1].fd = 3;
	msgs[1].ioctl.command = 0x1256;
	run(2);
	CHECK(n_acks == 2, "%u acks", n_acks);
	CHECK(dbg_host_count(FAT, D2XDBG_E_BEGIN) == 2 && dbg_host_count(FAT, D2XDBG_E_END) == 2, "two B and two E");
	CHECK(last_rec(D2XDBG_E_BEGIN, &x, &a, &b) && x == ((IOS_IOCTL << 8) | 0x56) && a == 3,
	      "IOCTL: x %x a %u", x, a);
	CHECK(DBG_MOD(FAT)->requests == 2, "two requests");
}

static void test_unknown_command_is_an_error(void)
{
	fresh();
	msgs[0].command = 0x55;
	run(1);
	CHECK(n_acks == 1 && acks[0].result == IPC_EINVAL, "result %d", (int)acks[0].result);
	CHECK(DBG_MOD(FAT)->errors == 1, "counted as an error");
}

static void test_req_fault_fails_unrun(void)
{
	d2xdbg_mod *m = DBG_MOD(FAT);
	u32 x, a, b;

	fresh();
	dbg_host_fault(0, FAT, D2XDBG_P_REQ, 1, 0, D2XDBG_F_ERROR << 24);
	/* IOS_WRITE on a bad fd: its handler starts the LED blinking, then refuses */
	msgs[0].command = IOS_WRITE;
	msgs[0].fd = 3;
	msgs[1] = msgs[0];
	run(2);
	CHECK(n_acks == 2, "%u acks", n_acks);
	CHECK(acks[0].result == FS_EFATAL, "faulted: result %d, expected FS_EFATAL", (int)acks[0].result);
	CHECK(acks[1].result == IPC_EINVAL, "the second runs (FS_EFATAL and IPC_EINVAL are both -101): result %d", (int)acks[1].result);
	CHECK(led_on == 1, "the handler ran once (the second request only): %u", led_on);
	CHECK(m->requests == 2 && m->errors == 2 && m->faults == 1, "counters %u/%u/%u", m->requests, m->errors, m->faults);
	CHECK(dbg_host_count(FAT, D2XDBG_E_FAULT) == 1, "the fault traced");
	CHECK(dbg_host_count(FAT, D2XDBG_E_END) == 2, "E traced for both");
	CHECK(last_rec(D2XDBG_E_END, &x, &a, &b) && x == IOS_WRITE && (int)a == IPC_EINVAL, "last E: a %d", (int)a);
}

static void test_req_fault_skip_lets_the_first_through(void)
{
	fresh();
	dbg_host_fault(0, FAT, D2XDBG_P_REQ, 1, 1, D2XDBG_F_ERROR << 24);
	open_fat(&msgs[0], "fat");
	open_fat(&msgs[1], "fat");
	run(2);
	CHECK(n_acks == 2 && acks[0].result == 0 && acks[1].result == FS_EFATAL, "results %d %d", (int)acks[0].result,
	      (int)acks[1].result);
}

static void test_delay_fault_waits_then_runs(void)
{
	fresh();
	dbg_host_fault(0, FAT, D2XDBG_P_REQ, 1, 0, (D2XDBG_F_DELAY << 24) | 40);
	open_fat(&msgs[0], "fat");
	run(1);
	CHECK(sleeps_ms == 40, "slept %u ms", sleeps_ms);
	CHECK(n_acks == 1 && acks[0].result == 0, "the request still ran: %d", (int)acks[0].result);
}

static void test_hang_fault_never_acks(void)
{
	fresh();
	dbg_host_fault(0, FAT, D2XDBG_P_REQ, 1, 0, D2XDBG_F_SPIN << 24);
	open_fat(&msgs[0], "fat");
	dbg_host_on_hang = on_hang;
	hang_action = 0;
	if (!setjmp(hang_jump))
		run(1);
	CHECK(hang_action == D2XDBG_F_SPIN, "spun");
	CHECK(n_acks == 0, "the request was never acknowledged");
	CHECK(DBG_MOD(FAT)->cur_start != 0, "the request is still open: last words show where it stopped");
}

static const struct hc_test tests[] = {
	{ "fat_dbg_message_traces_begin_and_end", test_message_traces_begin_and_end, false },
	{ "fat_dbg_read_event_carries_fd_and_length", test_read_event_carries_fd_and_length, false },
	{ "fat_dbg_ioctl_event_carries_ioctl_number", test_ioctl_event_carries_the_ioctl_number, false },
	{ "fat_dbg_unknown_command_is_an_error", test_unknown_command_is_an_error, false },
	{ "fat_dbg_req_fault_fails_unrun", test_req_fault_fails_unrun, false },
	{ "fat_dbg_req_fault_skip_lets_first_through", test_req_fault_skip_lets_the_first_through, false },
	{ "fat_dbg_delay_fault_waits_then_runs", test_delay_fault_waits_then_runs, false },
	{ "fat_dbg_hang_fault_never_acks", test_hang_fault_never_acks, false },
};

int main(int argc, char **argv)
{
	return hc_main(tests, HC_COUNT(tests), argc, argv, NULL);
}
