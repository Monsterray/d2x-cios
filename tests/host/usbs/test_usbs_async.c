/*
 * Host tests of the USBS module's asynchronous bulk transfers: usb2.c itself,
 * built for the host over a model of IOS (message queue, timers, an
 * asynchronous /dev/usb/ven) that can stop answering. What the driver must do
 * when the drive does not answer: give up after USBS_XFER_TIMEOUT_MS with
 * USBSTORAGE_ETIMEDOUT, cancel the endpoint, wait for the request to come
 * back before its memory is used again, and never take an old reply or timer
 * message for the next transfer's. Run with `make -C tests/host`.
 *
 * The model's "block forever" (a receive with nothing that can wake it) ends
 * the test as failed: that is the hang the timer exists to prevent.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define HOSTCHECK_MAIN
#include "hostcheck.h"

#undef bool	/* cios-lib's types.h typedefs it */
#include "usb2.c"	/* the code under test, with cios-lib's types */

/* ---- the module's own symbols usb2.c expects */
s32 queuehandle = 1;
areply usbCb[2];

static void *qbuf_seen;	/* the queue's buffer: USB_Init never frees it */
void *Mem_Alloc(u32 size) { return calloc(1, size + 32); }
void Mem_Free(void *ptr) { free(ptr); }

/* ---- a model of IOS */
enum { RQ_NORMAL, RQ_NEVER, RQ_CANCEL_ENDS, RQ_LATE };

static unsigned long long now_us;	/* the model's clock */
#define Q_MAX 64
static u32 q[Q_MAX];
static int q_head, q_n;
static int timer_armed;
static unsigned long long timer_at;
static u32 timer_msg;

/* the request IOS holds (one at a time: the module sends one at a time) */
static struct {
	int active;
	struct _usb_msg *msg;
	u32 reply_addr;			/* the message value */
	u8 *data;
	u32 len;
	int in;
	unsigned long long done_at;	/* 0: not by itself */
	int result;
	u8 pattern;			/* what it writes into an IN buffer */
	int timer_too;			/* the timer fires with the completion */
} rq;

static int mode;		/* what the next request does */
static unsigned long long late_us;	/* RQ_LATE: how long after the cancel */
static int submits, cancels, last_cancel_ep, cancel_hit;
static int corrupt;		/* a reply for an old request came while a new one was held */
static u8 next_pattern = 0xA5;
static int timer_with_reply;	/* the next request's reply comes with a timer message */

static void q_push(u32 m)
{
	if (q_n < Q_MAX)
		q[(q_head + q_n++) % Q_MAX] = m;
}

static void complete_request(void)
{
	if (rq.in && rq.result > 0)
		memset(rq.data, rq.pattern, rq.result);
	rq.msg->reply.result = rq.result;
	q_push(rq.reply_addr);
	if (rq.timer_too && timer_armed) {
		timer_armed = 0;
		q_push(timer_msg);
	}
	rq.active = 0;
}

s32 os_open(const char *device, s32 mode_) { return 7; }
s32 os_close(s32 fd) { return 0; }
s32 os_ioctlv(s32 fd, s32 request, s32 bytes_in, s32 bytes_out, ioctlv *vector) { return -1; }
s32 os_ioctl_async(s32 fd, s32 request, void *in, s32 bytes_in, void *out, s32 bytes_out, ...) { return 0; }

s32 os_ioctl(s32 fd, s32 request, void *in, s32 bytes_in, void *out, s32 bytes_out)
{
	if (request == USBV5_IOCTL_GETVERSION) {
		((u32 *)out)[0] = 0x50001;
		return 0;
	}
	if (request == USBV5_IOCTL_CANCELENDPOINT) {
		s32 *b = in;

		cancels++;
		last_cancel_ep = (u8)b[2];
		if (!rq.active)
			return -4;	/* nothing was cancelled (Dolphin: IPC_EINVAL) */
		cancel_hit++;
		if (mode == RQ_CANCEL_ENDS) {
			rq.result = -7022;
			rq.done_at = now_us + 100;
		} else if (mode == RQ_LATE) {
			rq.result = 99;
			rq.pattern = 0xEE;
			rq.done_at = now_us + late_us;
		}
		return 0;
	}
	return 0;
}

s32 os_ioctlv_async(s32 fd, s32 request, s32 bytes_in, s32 bytes_out, ioctlv *vector, ...)
{
	va_list ap;
	s32 queue;
	u32 msg;
	struct _usb_msg *m = vector[0].data;

	va_start(ap, vector);
	queue = va_arg(ap, s32);
	msg = va_arg(ap, u32);
	va_end(ap);

	submits++;
	if (rq.active)
		corrupt = 1;		/* the module sent while IOS still held one */
	rq.active = 1;
	rq.msg = m;
	rq.reply_addr = msg;
	rq.data = vector[1].data;
	rq.len = vector[1].len;
	rq.in = bytes_out == 1;
	rq.pattern = next_pattern;
	rq.result = rq.len;
	rq.timer_too = timer_with_reply;
	rq.done_at = mode == RQ_NORMAL ? now_us + 1000 : 0;
	return 0;
}

s32 os_message_queue_create(void *ptr, u32 id) { qbuf_seen = ptr; return 3; }
s32 os_message_queue_destroy(s32 queueid) { return 0; }
s32 os_message_queue_send(s32 queueid, u32 message, s32 flags) { q_push(message); return 0; }

s32 os_create_timer(s32 time_us, s32 repeat_time_us, s32 message_queue, s32 message)
{
	timer_msg = message;
	timer_armed = 1;
	timer_at = now_us + time_us;
	return 5;
}
s32 os_stop_timer(s32 timer_id) { timer_armed = 0; return 0; }
s32 os_restart_timer(s32 timer_id, s32 time_us, s32 repeat_time_us)
{
	timer_armed = 1;
	timer_at = now_us + (u32)time_us;
	return 0;
}

/* A receive with nothing queued lets time pass to the next thing that
 * happens: the timer, or the request's completion (the timer first on a tie). */
s32 os_message_queue_receive(s32 queueid, u32 *message, u32 flags)
{
	while (!q_n) {
		int req = rq.active && rq.done_at;

		if (!timer_armed && !req)
			hc_hang("a receive that nothing can wake: the module is blocked forever");
		if (timer_armed && (!req || timer_at <= rq.done_at)) {
			now_us = timer_at;
			timer_armed = 0;
			q_push(timer_msg);
		} else {
			now_us = rq.done_at;
			complete_request();
		}
	}
	*message = q[q_head];
	q_head = (q_head + 1) % Q_MAX;
	q_n--;
	return 0;
}

/* ---- the tests */
#define EP_IN	0x81
#define EP_OUT	0x02
#define XFER_US	USB_XFER_TIMEOUT_US
#define LEN	16384

static u8 buf[LEN] __attribute__((aligned(32)));

static void setup(void)
{
	now_us = 0;
	q_head = q_n = 0;
	timer_armed = 0;
	memset(&rq, 0, sizeof(rq));
	mode = RQ_NORMAL;
	late_us = 0;
	submits = cancels = cancel_hit = last_cancel_ep = corrupt = 0;
	next_pattern = 0xA5;
	timer_with_reply = 0;
	if (host) {
		free(host);
		host = NULL;
	}
	free(qbuf_seen);
	qbuf_seen = NULL;
	if (xorphan) {
		free(xorphan);
		xorphan = NULL;
	}
}

/* After USB_Init the way the module does it */
static int init(void)
{
	if (USB_Init() != 0 || !host)
		return -1;
	host->attached_devices[0].device_id = 0x100;
	return 0;
}

static s32 read_buf(void)
{
	return USB_ReadBlkMsg(0x100, EP_IN, LEN, buf);
}

static void test_normal_transfer_unchanged(void)
{
	CHECK(init() == 0, "init");
	memset(buf, 0, sizeof(buf));
	CHECK(read_buf() == LEN, "a normal read returns its length");
	CHECK(buf[0] == 0xA5 && buf[LEN - 1] == 0xA5, "the data arrived");
	CHECK(cancels == 0 && submits == 1, "no cancel (%d), one request (%d)", cancels, submits);
	CHECK(now_us == 1000, "it took 1 ms of model time, not a timeout (%llu us)", now_us);
	CHECK(q_n == 0 && !timer_armed, "nothing left in the queue (%d) and the timer is off", q_n);
	CHECK(USB_WriteBlkMsg(0x100, EP_OUT, 31, buf) == 31, "a write returns its length");
}

/* The reply and the timer message both reached the queue before the module
 * looked: the timer message left behind must not time out the next transfer. */
static void test_stale_timer_message_is_not_a_timeout(void)
{
	CHECK(init() == 0, "init");
	timer_with_reply = 1;
	CHECK(read_buf() == LEN, "first read");
	timer_with_reply = 0;
	CHECK(q_n == 0, "the stale timer message was dropped (%d left)", q_n);
	now_us = 0;
	CHECK(read_buf() == LEN, "the next read is not cut short");
	CHECK(cancels == 0 && now_us == 1000, "no cancel (%d), 1 ms (%llu us)", cancels, now_us);
}

static void test_never_answers_times_out_and_cancels(void)
{
	s32 r;

	CHECK(init() == 0, "init");
	mode = RQ_CANCEL_ENDS;		/* nothing completes it until it is cancelled */
	r = read_buf();
	CHECK(r == USBSTORAGE_ETIMEDOUT, "the timeout error, not %ld", (long)r);
	CHECK(now_us >= XFER_US && now_us < XFER_US + 1000, "returned at the deadline (%llu us, deadline %u)",
	      now_us, XFER_US);
	CHECK(cancels == 1 && last_cancel_ep == EP_IN && cancel_hit == 1, "one cancel of endpoint 0x81 (%d, %x)", cancels,
	      last_cancel_ep);
	CHECK(!rq.active && !xorphan, "the request came back, nothing abandoned");
	CHECK(q_n == 0 && !timer_armed, "queue empty, timer off");

	/* and the next transfer works */
	mode = RQ_NORMAL;
	CHECK(read_buf() == LEN && buf[0] == 0xA5, "the next read is fine");
}

static void test_cancel_ignored_waits_then_abandons(void)
{
	s32 r;

	CHECK(init() == 0, "init");
	mode = RQ_NEVER;
	r = read_buf();
	CHECK(r == USBSTORAGE_ETIMEDOUT, "timeout error");
	CHECK(now_us == (unsigned long long)XFER_US + USB_CANCEL_WAIT_US, "waited the cancel wait too (%llu)", now_us);
	CHECK(cancels == 1 && rq.active && xorphan, "the request is still held by IOS, so its memory is kept");
	CHECK(q_n == 0 && !timer_armed, "queue empty, timer off");
}

/* The reply of a cancelled request comes after the module gave up on it. The
 * next transfers must neither reuse its memory early nor take its reply. */
static void test_late_reply_does_not_corrupt_next_transfer(void)
{
	unsigned long long t;
	int submits_before;
	s32 r;

	CHECK(init() == 0, "init");
	mode = RQ_LATE;
	late_us = USB_CANCEL_WAIT_US + 500000;	/* 0.5 s after the module stopped waiting */
	memset(buf, 0x11, sizeof(buf));
	r = read_buf();
	CHECK(r == USBSTORAGE_ETIMEDOUT && xorphan, "first read abandoned (%ld)", (long)r);
	t = now_us;
	submits_before = submits;

	/* the old request is still held: nothing may be sent into its buffer */
	mode = RQ_NORMAL;
	next_pattern = 0x5A;
	r = read_buf();
	CHECK(!corrupt, "no request sent while IOS still held the old one");
	CHECK(r == LEN, "the second transfer ran after the late reply, with its own result %ld", (long)r);
	CHECK(submits == submits_before + 1, "one new request");
	CHECK(buf[0] == 0x5A && buf[LEN - 1] == 0x5A, "the buffer holds the second transfer's data (%02x)", buf[0]);
	CHECK(!xorphan, "the abandoned request was freed when its reply came");
	CHECK(now_us >= t + 500000 && now_us < t + USB_CANCEL_WAIT_US, "it waited for the late reply, no longer (%llu)",
	      now_us - t);
	CHECK(q_n == 0 && !timer_armed, "queue empty, timer off");
}

/* A reply that does not come within the wait: the next transfer fails too,
 * in a bounded time, without sending anything. */
static void test_held_request_blocks_new_transfer_in_time(void)
{
	unsigned long long t;
	int s;
	s32 r;

	CHECK(init() == 0, "init");
	mode = RQ_NEVER;
	r = read_buf();
	CHECK(r == USBSTORAGE_ETIMEDOUT && xorphan, "abandoned");
	t = now_us;
	s = submits;
	mode = RQ_NORMAL;
	r = read_buf();
	CHECK(r == USBSTORAGE_ETIMEDOUT, "refused with the timeout error (%ld)", (long)r);
	CHECK(submits == s, "nothing sent");
	CHECK(now_us - t == USB_CANCEL_WAIT_US, "within the cancel wait (%llu)", now_us - t);
}

static const struct hc_test tests[] = {
	{ "usbs_async_normal_transfer_unchanged", test_normal_transfer_unchanged, false },
	{ "usbs_async_stale_timer_message_is_not_a_timeout", test_stale_timer_message_is_not_a_timeout, false },
	{ "usbs_async_never_answers_times_out_and_cancels", test_never_answers_times_out_and_cancels, false },
	{ "usbs_async_cancel_ignored_waits_then_abandons", test_cancel_ignored_waits_then_abandons, false },
	{ "usbs_async_late_reply_does_not_corrupt_next_transfer", test_late_reply_does_not_corrupt_next_transfer, false },
	{ "usbs_async_held_request_blocks_new_transfer_in_time", test_held_request_blocks_new_transfer_in_time, false },
};

int main(int argc, char **argv)
{
	return hc_main(tests, HC_COUNT(tests), argc, argv, setup);
}
