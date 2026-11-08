/*
 * netbench: time Wii networking through an IOS (stock or cIOS): TCP and UDP
 * throughput both ways, and what each IOS call costs. Touches no NAND and no
 * setting; its only write is the report, sd:/netbench.txt.
 *
 *     hbc.py run netbench.dol [--ios=58] [--port=4310] [--wait=90]
 *
 * Reloads to --ios (0: stay on the IOS HBC started it with), prints the
 * IOS's identity, starts the network (timed: NCD/WD bring-up and DHCP) and
 * listens on TCP and UDP --port. The PC (tests/wii/netbench.py, a bench queue
 * job) connects and drives it with one text line per test; each answer is
 * one line starting "ok " or "err ". The PC connects to the Wii, never the
 * reverse, so the PC's firewall has nothing to let in except UDP the Wii
 * sends back to a port the PC sent from.
 *
 *   info                    IOS, identity, address, network start-up time
 *   calls N                 N of each cheap call, timed on the Wii: ES (another
 *                           module), SO's GETHOSTID, a POLLIN poll with timeout 0,
 *                           a 1-byte UDP send, and a 16 KB MEM1->MEM2 memcpy
 *                           (libogc copies every send and receive through its
 *                           own MEM2 heap)
 *   ping N SIZE             N echoes of SIZE bytes over TCP; the PC times them
 *   tx BYTES CHUNK          the Wii sends BYTES in CHUNK-byte net_send calls
 *   rx BYTES CHUNK          the Wii receives BYTES with CHUNK-byte net_recv calls
 *   buf SND RCV             setsockopt SO_SNDBUF/SO_RCVBUF on the connection (0: leave)
 *   nodelay ON              setsockopt TCP_NODELAY
 *   probe ON                a second thread times an ES call every 2 ms during
 *                           tx/rx/utx/urx: when the ARM is busy, it waits
 *   utx N SIZE PORT         N UDP datagrams of SIZE to the PC's address:PORT
 *   urx N SIZE MS           receive up to N datagrams; stops MS ms after the
 *                           last one, or MS ms after "go" if none come
 *   quit
 *
 * Per-call times are kept in time-base ticks and converted at report time.
 * Output goes to the TV and, after the IOS reload, to the PC through
 * hbc_netlog; the report is saved to sd:/netbench.txt. Its last line starts
 * with "RESULT".
 */
#include <gccore.h>
#include <malloc.h>
#include <fat.h>
#include <sdcard/wiisd_io.h>
#include <network.h>
#include <errno.h>
#include <stdarg.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "hbc_netlog.h"

/* libogc exports it without a header: reload HBC after a crash */
extern void __exception_setreload(int t);

#define IOS_POLLIN   0x0001
#define IOS_NONBLOCK 4          /* IOS's fcntl flag (hbc_netlog.h uses it too) */
#ifndef IPPROTO_TCP
#define IPPROTO_TCP 6
#endif
#define BUFSZ (64 * 1024)

static u8 buf[BUFSZ] ATTRIBUTE_ALIGN(32);
static u8 *mem2buf;
static char report[32768];
static size_t report_len;
static u32 init_ms;
static s32 init_rc;
static u32 my_ip;

/* printf, and keep a copy for sd:/netbench.txt */
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

static void save_report(void)
{
	if (!fatMountSimple("sd", &__io_wiisd))
		return;
	FILE *f = fopen("sd:/netbench.txt", "wb");
	if (f) {
		fwrite(report, 1, report_len, f);
		fclose(f);
	}
	fatUnmount("sd:");
}

/* ---- per-call statistics, in ticks ---- */

struct cstat {
	u32 n;
	u64 sum;
	u64 min, max;
};

static void st_reset(struct cstat *s)
{
	memset(s, 0, sizeof(*s));
	s->min = ~0ULL;
}

static inline void st_add(struct cstat *s, u64 t)
{
	s->n++;
	s->sum += t;
	if (t < s->min)
		s->min = t;
	if (t > s->max)
		s->max = t;
}

/* "n=.. avg=..us min=..us max=..us", tenths of a microsecond */
static const char *st_fmt(const struct cstat *s, char *out, size_t len)
{
	if (!s->n) {
		snprintf(out, len, "n=0");
		return out;
	}
	u32 avg = ticks_to_nanosecs(s->sum / s->n) / 100;
	u32 mn = ticks_to_nanosecs(s->min) / 100, mx = ticks_to_nanosecs(s->max) / 100;
	snprintf(out, len, "n=%u avg=%u.%uus min=%u.%uus max=%u.%uus", (unsigned) s->n,
	         (unsigned) avg / 10, (unsigned) avg % 10, (unsigned) mn / 10, (unsigned) mn % 10,
	         (unsigned) mx / 10, (unsigned) mx % 10);
	return out;
}

/* KB/s from bytes and ticks */
static u32 kbps(u64 bytes, u64 ticks)
{
	u64 us = ticks_to_microsecs(ticks);
	return us ? (u32) (bytes * 1000000 / 1024 / us) : 0;
}

/* ---- the probe thread: how long an unrelated IOS call waits ---- */

static lwp_t probe_thread = LWP_THREAD_NULL;
static volatile bool probe_on, probe_run, probe_wanted;
static struct cstat probe_st;

static void *probe_main(void *arg)
{
	(void) arg;
	u64 tid;
	while (probe_run) {
		if (probe_on) {
			s64 t = gettime();
			ES_GetTitleID(&tid);
			st_add(&probe_st, diff_ticks(t, gettime()));
		}
		usleep(2000);
	}
	return NULL;
}

static void probe_begin(void)
{
	st_reset(&probe_st);
	if (probe_thread != LWP_THREAD_NULL && probe_wanted)
		probe_on = true;
}

/* " probe=n=.. avg=.." appended to an answer */
static void probe_end(char *out, size_t len)
{
	char s[96];
	probe_on = false;
	if (probe_thread == LWP_THREAD_NULL || !probe_wanted) {
		out[0] = 0;
		return;
	}
	usleep(5000);       /* let a call in flight finish */
	snprintf(out, len, " probe_es[%s]", st_fmt(&probe_st, s, sizeof(s)));
}

/* ---- TCP helpers ---- */

static s32 conn = -1, usock = -1;
static struct sockaddr_in peer;

/* IOS's sockaddr is 8 bytes: a length of 16 (Dolphin takes it) is EINVAL on a
   real Wii (hbc-reborn's discover.c found it). */
#define IOS_SALEN 8

static struct sockaddr_in peer_at(u32 port)
{
	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_len = IOS_SALEN;
	to.sin_family = AF_INET;
	to.sin_port = htons(port);
	to.sin_addr.s_addr = peer.sin_addr.s_addr;
	return to;
}

static s32 send_all(const void *p, u32 len)
{
	const u8 *b = p;
	while (len) {
		s32 r = net_send(conn, b, len, 0);
		if (r <= 0)
			return r ? r : -EPIPE;
		b += r;
		len -= r;
	}
	return 0;
}

static s32 recv_all(void *p, u32 len)
{
	u8 *b = p;
	while (len) {
		s32 r = net_recv(conn, b, len, 0);
		if (r <= 0)
			return r ? r : -ECONNRESET;
		b += r;
		len -= r;
	}
	return 0;
}

static void __attribute__((format(printf, 1, 2))) answer(const char *fmt, ...)
{
	char line[600];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int) sizeof(line) - 2)
		n = sizeof(line) - 2;
	say("netbench: %s\n", line);
	line[n++] = '\n';
	send_all(line, n);
}

/* One command line, a byte at a time so no data after it is consumed. */
static s32 read_line(char *out, u32 len)
{
	u32 n = 0;
	for (;;) {
		char c;
		s32 r = net_recv(conn, &c, 1, 0);
		if (r <= 0)
			return r ? r : -ECONNRESET;
		if (c == '\n')
			break;
		if (c != '\r' && n + 1 < len)
			out[n++] = c;
	}
	out[n] = 0;
	return n;
}

/* ---- the tests ---- */

static void do_calls(u32 n)
{
	struct cstat es, hostid, poll0, udp1, cpy;
	char a[96], b[96], c[96], d[96], e[96];
	u64 tid;
	st_reset(&es);
	st_reset(&hostid);
	st_reset(&poll0);
	st_reset(&udp1);
	st_reset(&cpy);
	for (u32 i = 0; i < n; i++) {
		s64 t = gettime();
		ES_GetTitleID(&tid);
		st_add(&es, diff_ticks(t, gettime()));
	}
	for (u32 i = 0; i < n; i++) {
		s64 t = gettime();
		net_gethostip();
		st_add(&hostid, diff_ticks(t, gettime()));
	}
	for (u32 i = 0; i < n; i++) {
		struct pollsd sd = { conn, IOS_POLLIN, 0 };
		s64 t = gettime();
		net_poll(&sd, 1, 0);
		st_add(&poll0, diff_ticks(t, gettime()));
	}
	/* to the PC's discard port: nobody listens, the datagram just leaves */
	struct sockaddr_in to = peer_at(9);
	s32 udp_rc = 0;
	for (u32 i = 0; i < n; i++) {
		s64 t = gettime();
		s32 r = net_sendto(usock, buf, 1, 0, (struct sockaddr *) &to, IOS_SALEN);
		st_add(&udp1, diff_ticks(t, gettime()));
		if (r != 1 && !udp_rc)
			udp_rc = r;
	}
	for (u32 i = 0; i < 64; i++) {
		s64 t = gettime();
		memcpy(mem2buf, buf, 16384);
		DCFlushRange(mem2buf, 16384);
		st_add(&cpy, diff_ticks(t, gettime()));
	}
	answer("ok calls es[%s] so_hostid[%s] poll0[%s] udp_send1[%s] udp_send1_err=%d memcpy16k_mem2[%s]",
	       st_fmt(&es, a, sizeof(a)), st_fmt(&hostid, b, sizeof(b)), st_fmt(&poll0, c, sizeof(c)),
	       st_fmt(&udp1, d, sizeof(d)), (int) udp_rc, st_fmt(&cpy, e, sizeof(e)));
}

static void do_ping(u32 n, u32 size)
{
	struct cstat rs, ss;
	char a[96], b[96];
	if (size < 1 || size > BUFSZ) {
		answer("err ping size %u", (unsigned) size);
		return;
	}
	st_reset(&rs);
	st_reset(&ss);
	answer("go");
	for (u32 i = 0; i < n; i++) {
		s64 t = gettime();
		s32 r = recv_all(buf, size);
		s64 t2 = gettime();
		st_add(&rs, diff_ticks(t, t2));
		if (r < 0) {
			answer("err ping recv %d at %u", (int) r, (unsigned) i);
			return;
		}
		r = send_all(buf, size);
		st_add(&ss, diff_ticks(t2, gettime()));
		if (r < 0) {
			answer("err ping send %d at %u", (int) r, (unsigned) i);
			return;
		}
	}
	answer("ok ping n=%u size=%u wii_recv[%s] wii_send[%s]", (unsigned) n, (unsigned) size,
	       st_fmt(&rs, a, sizeof(a)), st_fmt(&ss, b, sizeof(b)));
}

static void do_tx(u32 bytes, u32 chunk)
{
	struct cstat s;
	char a[96], p[128];
	if (chunk < 1 || chunk > BUFSZ) {
		answer("err tx chunk %u", (unsigned) chunk);
		return;
	}
	for (u32 i = 0; i < chunk; i++)
		buf[i] = (u8) (i * 7 + 3);
	st_reset(&s);
	answer("go");
	probe_begin();
	u32 sent = 0;
	s32 err = 0;
	s64 t0 = gettime();
	while (sent < bytes) {
		u32 len = bytes - sent < chunk ? bytes - sent : chunk;
		s64 t = gettime();
		s32 r = net_send(conn, buf, len, 0);
		st_add(&s, diff_ticks(t, gettime()));
		if (r <= 0) {
			err = r ? r : -EPIPE;
			break;
		}
		sent += r;
	}
	u64 total = diff_ticks(t0, gettime());
	probe_end(p, sizeof(p));
	if (err) {
		answer("err tx %d after %u bytes", (int) err, (unsigned) sent);
		return;
	}
	answer("ok tx bytes=%u chunk=%u kbps=%u send[%s]%s", (unsigned) sent, (unsigned) chunk,
	       (unsigned) kbps(sent, total), st_fmt(&s, a, sizeof(a)), p);
}

static void do_rx(u32 bytes, u32 chunk)
{
	struct cstat s;
	char a[96], p[128];
	if (chunk < 1 || chunk > BUFSZ) {
		answer("err rx chunk %u", (unsigned) chunk);
		return;
	}
	st_reset(&s);
	answer("go");
	probe_begin();
	u32 got = 0;
	s32 err = 0;
	s64 t0 = 0;
	while (got < bytes) {
		u32 len = bytes - got < chunk ? bytes - got : chunk;
		s64 t = gettime();
		s32 r = net_recv(conn, buf, len, 0);
		if (!t0)
			t0 = t;     /* the first call also waits for the PC to start */
		else
			st_add(&s, diff_ticks(t, gettime()));
		if (r <= 0) {
			err = r ? r : -ECONNRESET;
			break;
		}
		got += r;
	}
	u64 total = diff_ticks(t0, gettime());
	probe_end(p, sizeof(p));
	if (err) {
		answer("err rx %d after %u bytes", (int) err, (unsigned) got);
		return;
	}
	answer("ok rx bytes=%u chunk=%u kbps=%u avg_read=%u recv[%s]%s", (unsigned) got,
	       (unsigned) chunk, (unsigned) kbps(got, total),
	       (unsigned) (got / (s.n + 1)), st_fmt(&s, a, sizeof(a)), p);
}

static void do_buf(s32 snd, s32 rcv)
{
	s32 r1 = 0, r2 = 0, r3 = 0, r4 = 0;
	if (snd > 0) {
		r1 = net_setsockopt(conn, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
		r3 = net_setsockopt(usock, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
	}
	if (rcv > 0) {
		r2 = net_setsockopt(conn, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
		r4 = net_setsockopt(usock, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
	}
	answer("ok buf snd=%d rcv=%d tcp_rc=%d,%d udp_rc=%d,%d", (int) snd, (int) rcv, (int) r1,
	       (int) r2, (int) r3, (int) r4);
}

static void do_nodelay(s32 on)
{
	s32 r = net_setsockopt(conn, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
	answer("ok nodelay %d rc=%d", (int) on, (int) r);
}

static void do_utx(u32 n, u32 size, u32 port)
{
	struct cstat s;
	char a[96], p[128];
	if (size < 4 || size > 1472) {
		answer("err utx size %u (4..1472)", (unsigned) size);
		return;
	}
	struct sockaddr_in to = peer_at(port);
	st_reset(&s);
	answer("go");
	probe_begin();
	u32 ok = 0, fails = 0;
	s32 first_err = 0;
	s64 t0 = gettime();
	for (u32 i = 0; i < n; i++) {
		buf[0] = i >> 24;
		buf[1] = i >> 16;
		buf[2] = i >> 8;
		buf[3] = i;
		s64 t = gettime();
		s32 r = net_sendto(usock, buf, size, 0, (struct sockaddr *) &to, IOS_SALEN);
		st_add(&s, diff_ticks(t, gettime()));
		if (r == (s32) size) {
			ok++;
		} else if (!fails++) {
			first_err = r;
		}
	}
	u64 total = diff_ticks(t0, gettime());
	probe_end(p, sizeof(p));
	answer("ok utx n=%u size=%u sent=%u failed=%u first_err=%d kbps=%u send[%s]%s", (unsigned) n,
	       (unsigned) size, (unsigned) ok, (unsigned) fails, (int) first_err,
	       (unsigned) kbps((u64) ok * size, total), st_fmt(&s, a, sizeof(a)), p);
}

static void do_urx(u32 n, u32 size, u32 ms)
{
	struct cstat s;
	char a[96], p[128];
	if (size > BUFSZ)
		size = BUFSZ;
	/* drop what is already queued (the PC's firewall-opening datagrams) */
	while (net_recvfrom(usock, buf, BUFSZ, 0, NULL, NULL) > 0)
		;
	st_reset(&s);
	answer("go");
	probe_begin();
	u32 got = 0, bytes = 0, polls = 0, maxseq = 0, misorder = 0, other = 0;
	s64 t_first = 0, t_last = 0, t_go = gettime();
	u32 last_seq = 0;
	while (got < n) {
		s64 t = gettime();
		s32 r = net_recvfrom(usock, buf, size, 0, NULL, NULL);
		if (r > 0 && (u32) r != size) {
			other++;    /* the PC's firewall-opening datagrams */
			continue;
		}
		if (r > 0) {
			st_add(&s, diff_ticks(t, gettime()));
			t_last = gettime();
			if (!got)
				t_first = t_last;
			got++;
			bytes += r;
			if (r >= 4) {
				u32 seq = buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3];
				if (got > 1 && seq < last_seq)
					misorder++;
				last_seq = seq;
				if (seq > maxseq)
					maxseq = seq;
			}
			continue;
		}
		/* nothing queued (non-blocking): wait in 10 ms slices */
		s64 since = got ? t_last : t_go;
		if (ticks_to_millisecs(diff_ticks(since, gettime())) >= ms)
			break;
		struct pollsd sd = { usock, IOS_POLLIN, 0 };
		net_poll(&sd, 1, 10);
		polls++;
	}
	probe_end(p, sizeof(p));
	u64 span = got > 1 ? diff_ticks(t_first, t_last) : 0;
	answer("ok urx got=%u of %u bytes=%u highest_seq=%u misordered=%u other=%u polls=%u kbps=%u recv[%s]%s",
	       (unsigned) got, (unsigned) n, (unsigned) bytes, (unsigned) maxseq,
	       (unsigned) misorder, (unsigned) other, (unsigned) polls, (unsigned) kbps(bytes, span),
	       st_fmt(&s, a, sizeof(a)), p);
}

/* Raw words of the two SD host controllers: 0x0D070000 is the SD slot,
 * 0x0D080000 the Wi-Fi chip (BCM4318 over SDIO). Read only, and only
 * registers whose reads change nothing: present state (0x24), host
 * control/power/gap/wakeup (0x28: bus width, high speed), clock and timeout
 * control (0x2C: divisor), capabilities (0x40: base clock). Readable from the
 * PowerPC only while HW_AHBPROT (0x0D800064) lets it; it is dumped too. */
static void do_regs(void)
{
	static const u32 off[] = { 0x24, 0x28, 0x2C, 0x40 };
	char s[2][96];
	u32 prot = *(volatile u32 *) 0xCD800064;
	for (int c = 0; c < 2; c++) {
		volatile u32 *base = (volatile u32 *) (c ? 0xCD080000 : 0xCD070000);
		int n = 0;
		for (int i = 0; i < 4; i++)
			n += snprintf(s[c] + n, sizeof(s[c]) - n, "%s%02x=%08x", i ? "," : "",
			              (unsigned) off[i], (unsigned) base[off[i] / 4]);
	}
	answer("ok regs ahbprot=%08x sd0[%s] wifi[%s]", (unsigned) prot, s[0], s[1]);
}

/* ---- set-up ---- */

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

/* The cIOS identity block (the way USB Loader GX reads it): the first content
 * starts with magic 0x1ee7c105, version, base IOS, name[16], versionstring[16]. */
static char ident[96] = "unknown";

static void read_identity(u32 ios)
{
	u64 tid = (1ULL << 32) | ios;
	u32 tmd_len = 0;
	char path[ISFS_MAXPATH] ATTRIBUTE_ALIGN(32);
	struct { u32 magic, magicver, version, base; char name[16], vstr[16]; } *info = NULL;
	signed_blob *stmd = NULL;
	fstats *st = NULL;
	s32 ifd = -1;

	if (ISFS_Initialize() < 0 || ES_GetStoredTMDSize(tid, &tmd_len) < 0 || tmd_len <= 0x1E7 ||
	    !(stmd = memalign(32, (tmd_len + 31) & ~31u)) || ES_GetStoredTMD(tid, stmd, tmd_len) < 0)
		goto done;
	u16 tv = ((tmd *) SIGNATURE_PAYLOAD(stmd))->title_version;
	snprintf(ident, sizeof(ident), "IOS%u v%u (stock)", ios, tv);
	snprintf(path, sizeof(path), "/title/00000001/%08x/content/%08x.app", ios,
	         (u32) *(u8 *) ((u32) stmd + 0x1E7));
	ifd = ISFS_Open(path, ISFS_OPEN_READ);
	if (ifd < 0 || !(st = memalign(32, sizeof(*st))) || ISFS_GetFileStats(ifd, st) < 0 ||
	    st->file_length < 0x30 || !(info = memalign(32, 64)) || ISFS_Read(ifd, info, 64) < 0x30)
		goto done;
	if (info->magic == 0x1ee7c105) {
		info->name[15] = info->vstr[15] = 0;
		snprintf(ident, sizeof(ident), "cIOS %u: %s v%u %s base %u", ios, info->name,
		         info->version, info->vstr, info->base);
	}
done:
	if (ifd >= 0)
		ISFS_Close(ifd);
	ISFS_Deinitialize();
	free(info);
	free(st);
	free(stmd);
}

static s32 start_network(void)
{
	s64 t = gettime();
	s32 r;
	do {
		r = net_init();
	} while (r == -EAGAIN && ticks_to_millisecs(diff_ticks(t, gettime())) < 20000);
	init_ms = ticks_to_millisecs(diff_ticks(t, gettime()));
	init_rc = r;
	if (r >= 0)
		my_ip = net_gethostip();
	return r;
}

static int bench(int argc, char **argv)
{
	u32 ios = 0, port = 4310, wait_s = 90;
	for (int i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--ios=", 6))
			ios = atoi(argv[i] + 6);
		else if (!strncmp(argv[i], "--port=", 7))
			port = atoi(argv[i] + 7);
		else if (!strncmp(argv[i], "--wait=", 7))
			wait_s = atoi(argv[i] + 7);
	}

	console_setup();
	hbc_netlog_init();
	say("netbench: started on IOS%d v%d; target IOS %u, port %u\n", IOS_GetVersion(),
	    IOS_GetRevision(), (unsigned) ios, (unsigned) port);
	if (ios && (s32) ios != IOS_GetVersion()) {
		hbc_netlog_close();
		net_deinit();
		printf("netbench: reloading to IOS%u...\n", (unsigned) ios);
		s32 ret = IOS_ReloadIOS(ios);
		if (ret < 0 || IOS_GetVersion() != (s32) ios) {
			hbc_netlog_init();
			say("RESULT fail reload ios=%u ret=%d now IOS%d\n", (unsigned) ios, (int) ret,
			    IOS_GetVersion());
			return 1;
		}
	} else {
		hbc_netlog_close();
		net_deinit();
	}
	ios = IOS_GetVersion();
	read_identity(ios);

	/* The network start-up is timed alone, before the log reconnects. */
	s32 r = start_network();
	hbc_netlog_init();
	say("netbench: IOS%d v%d, %s\n", IOS_GetVersion(), IOS_GetRevision(), ident);
	if (r < 0) {
		say("RESULT fail net_init %d after %u ms\n", (int) r, (unsigned) init_ms);
		return 1;
	}
	say("netbench: network up in %u ms, address %u.%u.%u.%u\n", (unsigned) init_ms,
	    my_ip >> 24, (my_ip >> 16) & 255, (my_ip >> 8) & 255, my_ip & 255);

	mem2buf = memalign(32, 16384);      /* malloc'd after the arena moves: MEM2 */
	s32 lsock = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	usock = net_socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_len = sizeof(sa);
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = INADDR_ANY;
	s32 rb1 = net_bind(lsock, (struct sockaddr *) &sa, sizeof(sa));
	s32 rl = net_listen(lsock, 1);
	s32 rb2 = net_bind(usock, (struct sockaddr *) &sa, sizeof(sa));
	if (lsock < 0 || usock < 0 || rb1 < 0 || rl < 0 || rb2 < 0) {
		say("RESULT fail sockets tcp=%d udp=%d bind=%d,%d listen=%d\n", (int) lsock,
		    (int) usock, (int) rb1, (int) rb2, (int) rl);
		return 1;
	}
	s32 fl = net_fcntl(usock, F_GETFL, 0);
	net_fcntl(usock, F_SETFL, (fl < 0 ? 0 : fl) | IOS_NONBLOCK);

	/* accept, waiting in 10 ms poll slices up to --wait */
	say("netbench: listening on TCP and UDP %u for %u s\n", (unsigned) port, (unsigned) wait_s);
	s64 t = gettime();
	for (;;) {
		struct pollsd sd = { lsock, IOS_POLLIN, 0 };
		if (net_poll(&sd, 1, 10) > 0 && (sd.revents & IOS_POLLIN))
			break;
		if (ticks_to_millisecs(diff_ticks(t, gettime())) > wait_s * 1000) {
			say("RESULT fail no PC connected in %u s\n", (unsigned) wait_s);
			return 1;
		}
	}
	socklen_t plen = sizeof(peer);
	memset(&peer, 0, sizeof(peer));
	peer.sin_len = sizeof(peer);
	conn = net_accept(lsock, (struct sockaddr *) &peer, &plen);
	net_close(lsock);
	if (conn < 0) {
		say("RESULT fail accept %d\n", (int) conn);
		return 1;
	}
	u32 pip = peer.sin_addr.s_addr;
	say("netbench: PC %u.%u.%u.%u connected\n", pip >> 24, (pip >> 16) & 255, (pip >> 8) & 255,
	    pip & 255);

	probe_run = true;
	if (LWP_CreateThread(&probe_thread, probe_main, NULL, NULL, 0, 70) < 0)
		probe_thread = LWP_THREAD_NULL;

	char line[128];
	int fails = 0, cmds = 0;
	for (;;) {
		r = read_line(line, sizeof(line));
		if (r < 0) {
			say("netbench: connection lost (%d)\n", (int) r);
			fails++;
			break;
		}
		unsigned a = 0, b = 0, c = 0;
		int k = sscanf(line, "%*s %u %u %u", &a, &b, &c);
		cmds++;
		if (!strncmp(line, "quit", 4)) {
			answer("ok bye");
			break;
		} else if (!strncmp(line, "regs", 4)) {
			do_regs();
		} else if (!strncmp(line, "info", 4)) {
			answer("ok info ios=%d rev=%d ident=\"%s\" ip=%u.%u.%u.%u net_init_ms=%u",
			       IOS_GetVersion(), IOS_GetRevision(), ident, my_ip >> 24,
			       (my_ip >> 16) & 255, (my_ip >> 8) & 255, my_ip & 255, (unsigned) init_ms);
		} else if (!strncmp(line, "calls", 5) && k >= 1) {
			do_calls(a);
		} else if (!strncmp(line, "ping", 4) && k >= 2) {
			do_ping(a, b);
		} else if (!strncmp(line, "tx", 2) && k >= 2) {
			do_tx(a, b);
		} else if (!strncmp(line, "rx", 2) && k >= 2) {
			do_rx(a, b);
		} else if (!strncmp(line, "buf", 3) && k >= 2) {
			do_buf(a, b);
		} else if (!strncmp(line, "nodelay", 7) && k >= 1) {
			do_nodelay(a);
		} else if (!strncmp(line, "probe", 5) && k >= 1) {
			probe_wanted = a != 0;
			answer("ok probe %s", probe_thread == LWP_THREAD_NULL ? "unavailable" : "ready");
		} else if (!strncmp(line, "utx", 3) && k >= 3) {
			do_utx(a, b, c);
		} else if (!strncmp(line, "urx", 3) && k >= 3) {
			do_urx(a, b, c);
		} else {
			answer("err unknown \"%s\"", line);
			fails++;
		}
	}

	probe_run = false;
	if (probe_thread != LWP_THREAD_NULL)
		LWP_JoinThread(probe_thread, NULL);
	net_close(conn);
	net_close(usock);
	if (fails) {
		say("RESULT fail %d of %d commands\n", fails, cmds);
		return 1;
	}
	say("RESULT ok %d commands\n", cmds);
	return 0;
}

int main(int argc, char **argv)
{
	__exception_setreload(5);
	int rc = bench(argc, argv);
	hbc_netlog_close();
	save_report();
	printf("netbench: done (report saved to sd:/netbench.txt); back to HBC in 3 s\n");
	sleep(3);
	return rc;
}
