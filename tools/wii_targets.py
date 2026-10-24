"""wii_targets.py -- which Wiis a test tool may touch, and the helpers bench jobs share.

From wii-homebrew-kit (docs/bench.md). Two kinds of console:

  bench       the shared test Wii. Reached only from inside a wii-bench queue job, which
              sets $WII_BENCH_IP and $WII_BENCH_JOB_START. Unattended runs, logs and
              cleanups happen here.
  protected   everyday consoles with real saves and settings. Test tools refuse them,
              in every spelling of the address (scheme, port, decimal, hex, trailing dot).

Addresses are local to each workstation, never in the repo:
  $WII_BENCH_IP                 the bench Wii (the queue sets it inside a job)
  $WII_PROTECTED                comma-separated protected Wiis
  .dev/wiis.json (ignored)      {"bench": "...", "protected": ["...", "..."]}
  $HBC_TOOL                     path to hbc-reborn's tools/hbc.py
  $WII_BENCH_QUEUE              path to wiibench.py

Helpers:
  bench_wii(what)       the bench Wii's address inside a queue job; exits outside one
  refuse_production(ip, what), is_production(ip)
  queue_cmd(args)       the command line that queues a script, for --help and errors
  parser(doc)           an ArgumentParser whose --help is the script's docstring
  hbc(wii, *args)       one hbc.py call with a timeout; never hangs the queue lease
  hbc_ready(wii, s)     TCP 4299 answers (probing the way HBC tolerates)
  in_hbc(wii)           HBC itself answers, not an app's agent
  card_ls(wii, dir)     {name: size} of the files in an SD card folder
  wiiload(wii, dol, args), local_ip_towards(wii)

Self-check (touches no Wii): python tools/wii_targets.py
"""
import argparse
import json
import os
import pathlib
import socket
import struct
import subprocess
import sys
import time
import zlib

REPO = pathlib.Path(__file__).resolve().parents[1]


def _config():
    path = REPO / ".dev" / "wiis.json"
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError):
        data = {}
    bench = os.environ.get("WII_BENCH_IP") or data.get("bench") or ""
    env = os.environ.get("WII_PROTECTED", "")
    protected = [p.strip() for p in env.split(",") if p.strip()] or list(data.get("protected", []))
    return bench, tuple(protected)


BENCH_WII, PROTECTED_WIIS = _config()
HBC_TOOL = os.environ.get("HBC_TOOL") or str(REPO.parent / "hbc-reborn" / "tools" / "hbc.py")
QUEUE = os.environ.get("WII_BENCH_QUEUE") or "wiibench.py"
PY = sys.executable


def _canon(ip):
    """Normalize an address for comparison, so a scheme, port, user, trailing dot or case
    cannot slip past an exact-string match. Hostnames are not resolved: the gate must stay
    fast and must not depend on the network."""
    s = (ip or "").strip().lower()
    for scheme in ("http://", "https://"):
        if s.startswith(scheme):
            s = s[len(scheme):]
    if s.startswith("::ffff:"):
        s = s[len("::ffff:"):]
    s = s.split("@")[-1]
    return s.split("/")[0].split(":")[0].rstrip(".")


def _same(a, b):
    a, b = _canon(a), _canon(b)
    if a == b:
        return True
    try:
        nb = socket.inet_aton(b)
    except OSError:
        return False
    if a.isdigit():
        return int(a) == int.from_bytes(nb, "big")
    try:
        return socket.inet_aton(a) == nb      # 0xC0A80001, 192.168.1, ...
    except OSError:
        return False


def is_production(ip, protected=None):
    """True for a protected Wii, in any spelling of its address."""
    return any(_same(ip, p) for p in (PROTECTED_WIIS if protected is None else protected))


def refuse_production(ip, what):
    """Stop a test tool aimed at a protected Wii."""
    if is_production(ip):
        sys.exit(f"{what}: refused -- {ip} is a protected Wii (real saves and data). "
                 "Test tools run on the bench Wii through the queue; see docs/bench.md "
                 "in wii-homebrew-kit.")


def queue_cmd(args, timeout=None):
    """The line that queues `tools/ARGS` as a bench job."""
    t = f" --timeout {timeout}" if timeout else ""
    return f"{PY} {QUEUE} add --cwd {REPO.as_posix()}{t} -- {PY} tools/{args}"


def bench_wii(what, ip=None):
    """The bench Wii's address for a job the queue started. Outside a job it exits without
    touching any Wii: the queue keeps two workstations, or two agents, off one console."""
    if not os.environ.get("WII_BENCH_JOB_START"):
        sys.exit(f"{what}: run it as a wii-bench queue job, not directly (nothing was sent):\n"
                 f"  {queue_cmd(what + ' ...')}\n"
                 f"then: {PY} {QUEUE} wait ID")
    ip = ip or os.environ.get("WII_BENCH_IP") or BENCH_WII
    if not ip:
        sys.exit(f"{what}: no bench Wii address ($WII_BENCH_IP)")
    refuse_production(ip, what)
    return ip


def parser(doc):
    """ArgumentParser whose --help prints the docstring as written. No abbreviated flags:
    a typo must be an error, never a different option."""
    return argparse.ArgumentParser(description=doc, allow_abbrev=False,
                                   formatter_class=argparse.RawDescriptionHelpFormatter)


def existing_file(s):
    """argparse type: a file that exists, as a Path."""
    p = pathlib.Path(s)
    if not p.is_file():
        raise argparse.ArgumentTypeError(f"{s}: no such file")
    return p


class Result:
    """What hbc() returns: rc (124 after a timeout), out (stdout), text (stdout + stderr)."""
    def __init__(self, rc, out, err=""):
        self.rc, self.out, self.text = rc, out, (out + err).strip()


def hbc(wii, *args, timeout=120, show=False, env=None):
    """One hbc.py call. A wedged Wii costs at most `timeout` seconds and comes back as
    rc 124, never a hang of the queue lease."""
    try:
        r = subprocess.run([PY, HBC_TOOL, "--wii", wii, *map(str, args)],
                           capture_output=True, text=True, errors="replace",
                           timeout=timeout, env=env)
        res = Result(r.returncode, r.stdout, r.stderr)
    except subprocess.TimeoutExpired:
        res = Result(124, "", f"timed out after {timeout} s")
    if show:
        print(f"$ hbc.py {' '.join(map(str, args))} -> {res.rc}: {res.text[-300:]}", flush=True)
    return res


def hbc_ready(wii, secs):
    """True once TCP 4299 accepts a connection. The probe sends a header HBC rejects at
    once: a bare connect and close holds HBC's loader for 10 s, and repeated bare probes
    filled its listen backlog until it stopped answering."""
    end = time.monotonic() + secs
    while time.monotonic() < end:
        try:
            with socket.create_connection((wii, 4299), timeout=2) as s:
                s.sendall(b"PING" + bytes(12))
            return True
        except OSError:
            time.sleep(1)
    return False


def in_hbc(wii):
    """HBC itself answers: its version does not end in ' agent' (an app's agent does)."""
    r = hbc(wii, "version", timeout=20)
    return r.rc == 0 and not r.out.strip().endswith("agent")


def card_ls(wii, folder, timeout=60):
    """{name: size} of the files in a card folder; None when ls fails. Uses --json: the
    text listing's column layout has changed between HBC versions."""
    r = hbc(wii, "ls", "--json", folder, timeout=timeout)
    if r.rc:
        return None
    try:
        return {e["name"]: e["size"] for e in json.loads(r.out) if e["type"] == "f"}
    except (ValueError, KeyError, TypeError):
        return None


def wiiload(wii, dol, args):
    """The Wiiload protocol, version 0.5: HAXX, version, argument length, compressed and
    uncompressed sizes (big-endian), the zlib data, then NUL-separated arguments."""
    data = pathlib.Path(dol).read_bytes()
    z = zlib.compress(data, 6)
    argb = b"".join(a.encode() + b"\0" for a in [pathlib.Path(dol).name] + list(args)) + b"\0"
    for attempt in range(6):          # HBC takes one connection at a time
        try:
            with socket.create_connection((wii, 4299), timeout=10) as s:
                s.sendall(b"HAXX" + bytes([0, 5]) + struct.pack(">HII", len(argb), len(z), len(data)))
                s.sendall(z)
                s.sendall(argb)
            return
        except OSError as e:
            print(f"  wiiload attempt {attempt + 1}: {e}")
            time.sleep(5)
    sys.exit("HBC did not take the DOL")


def local_ip_towards(wii):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as u:
        u.connect((wii, 4299))
        return u.getsockname()[0]


def self_check():
    protected = ("192.168.8.200",)
    for ip in ("192.168.8.200", "http://192.168.8.200:4299", "3232237768", "0xC0A808C8",
               "192.168.8.200.", "user@192.168.8.200", "::ffff:192.168.8.200"):
        assert is_production(ip, protected), ip
    assert not is_production("192.168.8.213", protected)
    saved = dict(os.environ)
    try:
        os.environ.pop("WII_BENCH_JOB_START", None)
        try:
            bench_wii("self-check", "192.168.8.213")
            raise AssertionError("bench_wii ran outside a queue job")
        except SystemExit as e:
            assert "queue job" in str(e.code)
    finally:
        os.environ.clear()
        os.environ.update(saved)
    print("wii_targets self-check ok")


if __name__ == "__main__":
    self_check()
