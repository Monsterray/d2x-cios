"""Time Wii networking through one or more IOSes with netbench, and print where the time goes.

    python tests/wii/netbench.py [--ios 58,56,249] [--mb 4] [--big] [--quick] [--port 4310]

A wii-bench queue job. For each IOS in --ios (0 = the IOS HBC starts apps with) it
runs tests/wii/netbench/netbench.dol, connects to it from this PC (the PC connects,
so the PC's firewall lets nothing new in), drives the test plan, then fetches the
Wii's own report (sd:/netbench.txt). Everything goes to the job's run folder:
netbench-iosN.txt (the Wii's report) and netbench.json (both sides' numbers).

The plan: info; the cost of cheap IOS calls; TCP echo round trips (1, 1400 and
16384 bytes); TCP Wii->PC and PC->Wii at 1, 4, 8 and 16 KB per call; the same at
16 KB with a probe thread timing an unrelated IOS call (ES) every 2 ms, which shows
whether the ARM is busy; the same after socket buffer sizes up to 64 KB (each setsockopt's answer is kept); UDP Wii->PC; UDP
PC->Wii at paced rates. --big adds 32 KB and 64 KB calls (libogc splits at 32 KB):
a size this stack has not been run with here, so only with someone at the Wii.

Writes nothing but sd:/netbench.txt; never touches NAND or settings.
"""
import json
import os
import pathlib
import re
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time

here = pathlib.Path(__file__).resolve()
sys.path.insert(0, str(here.parents[2] / "tools"))
import wii_targets as wt  # noqa: E402

DOL = here.parent / "netbench" / "netbench.dol"
REPORT = "sd:/netbench.txt"


class Wii:
    """The command connection to netbench."""

    def __init__(self, ip, port, secs):
        end = time.monotonic() + secs
        while True:
            try:
                self.s = socket.create_connection((ip, port), timeout=3)
                break
            except OSError:
                if time.monotonic() > end:
                    raise
                time.sleep(1)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.s.settimeout(120)
        self.f = self.s.makefile("rb")
        self.ip, self.port = ip, port

    def line(self):
        ln = self.f.readline()
        if not ln:
            raise ConnectionError("netbench closed the connection")
        return ln.decode(errors="replace").strip()

    def cmd(self, text, go=False):
        self.s.sendall(text.encode() + b"\n")
        ans = self.line()
        if go and ans != "go":
            raise RuntimeError(f"{text}: {ans}")
        return ans

    def recv_exact(self, n):
        out = bytearray()
        while len(out) < n:
            chunk = self.f.read1(min(65536, n - len(out)))
            if not chunk:
                raise ConnectionError("short read")
            out += chunk
        return out


def fields(ans):
    """'ok tx bytes=1 send[n=2 avg=3.0us]' -> {'bytes': '1', 'send.avg': '3.0us', ...}"""
    d = {}
    for m in re.finditer(r"(\w+)\[([^\]]*)\]", ans):
        for k, v in re.findall(r"(\w+)=(\S+)", m.group(2)):
            d[f"{m.group(1)}.{k}"] = v
    for k, v in re.findall(r"(?:^|\s)(\w+)=(\"[^\"]*\"|[^\s\[]+)(?=\s|$)", ans):
        d[k] = v.strip('"')
    return d


def tcp_tx(w, nbytes, chunk):
    """The Wii sends; the PC times first byte to last."""
    w.cmd(f"tx {nbytes} {chunk}", go=True)
    t0 = None
    got = 0
    while got < nbytes:
        b = w.f.read1(min(262144, nbytes - got))
        if not b:
            raise ConnectionError("tx: short")
        if t0 is None:
            t0 = time.perf_counter()
        got += len(b)
    pc = got / 1024 / max(time.perf_counter() - t0, 1e-9)
    return w.line(), pc


def tcp_rx(w, nbytes, chunk):
    """The PC sends as fast as TCP lets it; the Wii times its receives."""
    w.cmd(f"rx {nbytes} {chunk}", go=True)
    data = bytes(range(256)) * 256
    t0 = time.perf_counter()
    left = nbytes
    while left:
        n = min(left, len(data))
        w.s.sendall(data[:n])
        left -= n
    pc = nbytes / 1024 / max(time.perf_counter() - t0, 1e-9)
    return w.line(), pc


def tcp_ping(w, n, size):
    w.cmd(f"ping {n} {size}", go=True)
    msg = bytes(size)
    rtts = []
    for _ in range(n):
        t = time.perf_counter()
        w.s.sendall(msg)
        w.recv_exact(size)
        rtts.append((time.perf_counter() - t) * 1000)
    return w.line(), rtts


def udp_from_wii(w, u, n, size):
    """The Wii sends n datagrams; count what arrives here."""
    for _ in range(3):                         # open the firewall's state for the reply
        u.sendto(b"open", (w.ip, w.port))
    time.sleep(0.2)
    got, first, last = [0, 0], [None], [None]
    stop = threading.Event()

    def rx():
        u.settimeout(0.2)
        while not stop.is_set():
            try:
                d, _ = u.recvfrom(65536)
            except OSError:
                continue
            last[0] = time.perf_counter()
            if first[0] is None:
                first[0] = last[0]
            got[0] += 1
            got[1] += len(d)

    th = threading.Thread(target=rx, daemon=True)
    th.start()
    ans = w.cmd(f"utx {n} {size} {u.getsockname()[1]}", go=True)
    ans = w.line()
    time.sleep(1.0)
    stop.set()
    th.join()
    span = (last[0] - first[0]) if first[0] and last[0] and last[0] > first[0] else 0
    return ans, {"pc_got": got[0], "pc_bytes": got[1],
                 "pc_kbps": round(got[1] / 1024 / span) if span else 0}


def udp_to_wii(w, u, n, size, rate_kbps):
    """Send n datagrams at rate_kbps (0: as fast as possible); the Wii counts them."""
    w.cmd(f"urx {n} {size} 1500", go=True)
    gap = (size / 1024 / rate_kbps) if rate_kbps else 0
    payload = bytearray(size)
    t0 = time.perf_counter()
    for i in range(n):
        payload[0:4] = i.to_bytes(4, "big")
        if gap:
            while time.perf_counter() < t0 + i * gap:
                pass
        u.sendto(payload, (w.ip, w.port))
    sent_kbps = round(n * size / 1024 / max(time.perf_counter() - t0, 1e-9))
    return w.line(), sent_kbps


def run_plan(w, a, log):
    res = {}

    def rec(name, ans, **pc):
        f = fields(ans)
        f.update({k: v for k, v in pc.items() if not isinstance(v, list)})
        res[name] = {"answer": ans, **f}
        extra = " ".join(f"{k}={v}" for k, v in pc.items() if not isinstance(v, list))
        log(f"{name:24} {ans}" + (f"  || PC: {extra}" if extra else ""))
        return f

    nb = a.mb << 20
    rec("info", w.cmd("info"))
    rec("calls", w.cmd(f"calls {200 if a.quick else 1000}"))
    rec("nodelay", w.cmd("nodelay 1"))
    for size, n in ((1, 200), (1400, 100), (16384, 30)):
        ans, rtts = tcp_ping(w, n // (4 if a.quick else 1), size)
        rec(f"ping{size}", ans, rtt_ms_median=round(statistics.median(rtts), 2),
            rtt_ms_min=round(min(rtts), 2), rtt_ms_max=round(max(rtts), 2))
    chunks = [16384] if a.quick else [1024, 4096, 8192, 16384]
    if a.big:
        chunks += [32768, 65536]
    for c in chunks:
        ans, pc = tcp_tx(w, nb, c)
        rec(f"tcp_tx_{c}", ans, pc_kbps=round(pc))
        ans, pc = tcp_rx(w, nb, c)
        rec(f"tcp_rx_{c}", ans, pc_kbps=round(pc))
    rec("probe_on", w.cmd("probe 1"))
    ans, pc = tcp_tx(w, nb, 16384)
    rec("tcp_tx_16384_probe", ans, pc_kbps=round(pc))
    ans, pc = tcp_rx(w, nb, 16384)
    rec("tcp_rx_16384_probe", ans, pc_kbps=round(pc))

    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind(("", 0))
    u.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    try:
        n = 500 if a.quick else 3000
        ans, pc = udp_from_wii(w, u, n, 1400)
        rec("udp_tx_1400_probe", ans, **pc)
        rec("probe_off", w.cmd("probe 0"))
        ans, pc = udp_from_wii(w, u, n, 1400)
        rec("udp_tx_1400", ans, **pc)
        ans, pc = udp_from_wii(w, u, n, 256)
        rec("udp_tx_256", ans, **pc)
        for rate in ((512, 0) if a.quick else (256, 512, 1024, 2048, 0)):
            ans, sent = udp_to_wii(w, u, n, 1400, rate)
            rec(f"udp_rx_1400_at_{rate or 'max'}", ans, pc_sent_kbps=sent)
    finally:
        u.close()

    # Ascending, so the connection ends with the largest size IOS accepted.
    for v in (8192, 16384, 32768, 49152, 65536):
        rec(f"buf{v}", w.cmd(f"buf {v} {v}"))
    ans, pc = tcp_tx(w, nb, 16384)
    rec("tcp_tx_16384_bufmax", ans, pc_kbps=round(pc))
    ans, pc = tcp_rx(w, nb, 16384)
    rec("tcp_rx_16384_bufmax", ans, pc_kbps=round(pc))
    rec("regs", w.cmd("regs"))   # last: a protected read could fault the app
    rec("quit", w.cmd("quit"))
    return res


def host_view(ip):
    """ICMP round trips and the Wii's MAC from this PC (the MAC tells Wi-Fi from a USB adapter)."""
    out = {}
    try:
        r = subprocess.run(["ping", "-n", "20", ip], capture_output=True, text=True, timeout=60)
        out["icmp"] = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr
        r = subprocess.run(["arp", "-a", ip], capture_output=True, text=True, timeout=20)
        m = re.search(r"([0-9a-f]{2}(?:-[0-9a-f]{2}){5})", r.stdout, re.I)
        out["mac"] = m.group(1) if m else "?"
    except (OSError, subprocess.SubprocessError) as e:
        out["error"] = str(e)
    return out


def main():
    p = wt.parser(__doc__)
    p.add_argument("--ios", default="0", help="comma-separated IOS numbers; 0 = HBC's")
    p.add_argument("--mb", type=int, default=4, help="MB per TCP run")
    p.add_argument("--port", type=int, default=4310)
    p.add_argument("--big", action="store_true", help="also 32 and 64 KB calls (attended)")
    p.add_argument("--quick", action="store_true")
    p.add_argument("--timeout", type=int, default=900, help="per IOS")
    a = p.parse_args()
    if not DOL.is_file():
        sys.exit(f"{DOL}: build it first (make -C tests/wii/netbench)")
    wii = wt.bench_wii("netbench.py")
    outdir = pathlib.Path(os.environ.get("WII_BENCH_OUT") or tempfile.mkdtemp())
    lines = []

    def log(s):
        print(s, flush=True)
        lines.append(s)

    hv = host_view(wii)
    log(f"host: {json.dumps(hv)}")
    results = {"host": hv, "runs": {}}
    failed = 0
    for ios in [int(x) for x in a.ios.split(",") if x.strip()]:
        log(f"=== IOS {ios or 'HBC default'} ===")
        wt.hbc(wii, "rm", REPORT, timeout=30)
        app = subprocess.Popen([wt.PY, wt.HBC_TOOL, "--wii", wii, "run", "--timeout",
                                str(a.timeout), str(DOL), f"--ios={ios}", f"--port={a.port}",
                                "--wait=90"],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                               errors="replace")
        applog = []
        threading.Thread(target=lambda: applog.extend(app.stdout), daemon=True).start()
        try:
            time.sleep(8)
            w = Wii(wii, a.port, 120)
            results["runs"][str(ios)] = run_plan(w, a, log)
        except (OSError, RuntimeError, ConnectionError) as e:
            log(f"IOS {ios}: FAILED: {e!r}")
            failed += 1
        end = time.monotonic() + 120
        while time.monotonic() < end and not wt.in_hbc(wii):
            time.sleep(3)
        try:
            app.wait(timeout=30)
        except subprocess.TimeoutExpired:
            app.kill()
        rep = outdir / f"netbench-ios{ios}.txt"
        r = wt.hbc(wii, "get", REPORT, str(rep), timeout=60)
        text = rep.read_text(errors="replace") if rep.is_file() else f"(no report: {r.text[-200:]})"
        log(f"--- sd:/netbench.txt (IOS {ios}) ---\n{text.strip()}")
        if "RESULT ok" not in text:
            failed += 1
            log("--- hbc.py run output (tail) ---\n" + "".join(applog[-30:]))
    (outdir / "netbench.json").write_text(json.dumps(results, indent=1))
    (outdir / "netbench.log").write_text("\n".join(lines) + "\n")
    log(f"netbench.py: results in {outdir}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
