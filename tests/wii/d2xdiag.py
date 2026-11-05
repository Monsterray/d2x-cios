"""Run d2xdiag (a d2x +dbg build's counters, trace, last words and fault scenarios) on a bench Wii.

    python tests/wii/d2xdiag.py [--attended] [--timeout S] [--video S] [-- D2XDIAG_ARGS...]

A wii-bench queue job. d2xdiag reloads IOS, which drops `hbc.py run`'s
network log, so it saves its report to sd:/d2xdiag.txt; this script waits
for the Wii to come back to HBC, fetches the report and prints it.
Example: -- --ios=250 --wd=30 --scenario=usb-csw-stall,usb-csw-lost,sd-low

Hang scenarios (hang-block, hang-spin, hang-top, usb-spin, usbs-big) hang
IOS on purpose and rely on the cIOS's ARM watchdog to reset the console.
They need --attended: someone at the Wii, in case the watchdog or the boot
chain (BootMii as boot2 may stop at its menu after a reset) does not bring
it back. After the reset this script waits for HBC (up to --timeout), runs
d2xdiag once more to read the last words, and passes when they show the
watchdog fired.
"""
import os
import pathlib
import subprocess
import sys
import tempfile
import time

here = pathlib.Path(__file__).resolve()
sys.path.insert(0, str(here.parents[2] / "tools"))
import wii_targets as wt  # noqa: E402

DOL = here.parent / "d2xdiag" / "d2xdiag.dol"
REPORT = "sd:/d2xdiag.txt"
PRE = "sd:/d2xdiag-pre.txt"
WIICAP = os.environ.get("WIICAP", "C:/tools/wii-capture/wiicap.py")
HANGS = {"hang-block", "hang-spin", "hang-top", "usb-spin", "usbs-big"}


def scenarios(args):
    for a in args:
        if a.startswith("--scenario="):
            return set(a[len("--scenario="):].split(","))
    return set()


def back_in_hbc(wii, timeout, what):
    end = time.monotonic() + timeout
    time.sleep(5)
    while not wt.in_hbc(wii):
        if time.monotonic() > end:
            sys.exit(f"d2xdiag.py: the Wii did not return to HBC in {timeout} s ({what})")
        time.sleep(5)


def fetch(wii, path, out):
    r = wt.hbc(wii, "get", path, str(out), timeout=60)
    if r.rc or not out.is_file():
        return None
    return out.read_text(errors="replace")


def main():
    p = wt.parser(__doc__)
    p.add_argument("--timeout", type=int, default=900)
    p.add_argument("--attended", action="store_true", help="someone is at the Wii: allow hang scenarios")
    p.add_argument("--video", type=int, default=0, help="record the TV for this many seconds")
    p.add_argument("args", nargs="*")
    a = p.parse_args()
    if not DOL.is_file():
        sys.exit(f"{DOL}: build it first (make -C tests/wii/d2xdiag)")
    hangs = scenarios(a.args) & HANGS
    if hangs and not a.attended:
        sys.exit(f"d2xdiag.py: {', '.join(sorted(hangs))} hang IOS on purpose; run them only with someone "
                 "at the Wii, with --attended")
    if hangs and len(scenarios(a.args)) > 1:
        sys.exit("d2xdiag.py: a hang scenario ends the run: give it alone")
    wii = wt.bench_wii("d2xdiag.py")

    for f in (REPORT, PRE):
        wt.hbc(wii, "rm", f, timeout=30)             # an old report must not count
    outdir = pathlib.Path(os.environ.get("WII_BENCH_OUT") or tempfile.mkdtemp())
    cap = None
    if a.video and pathlib.Path(WIICAP).is_file():
        cap = subprocess.Popen([sys.executable, WIICAP, "video", str(a.video), "--out",
                                str(outdir / "d2xdiag.mp4"), "--label", "d2xdiag"])

    r = wt.hbc(wii, "run", "--timeout", str(min(a.timeout, 300)), str(DOL), *a.args,
               timeout=min(a.timeout, 300) + 60)
    print(r.text, flush=True)
    t0 = time.monotonic()
    back_in_hbc(wii, a.timeout, "after the run" + (": no watchdog reset?" if hangs else ""))
    if hangs:
        print(f"d2xdiag.py: back in HBC {time.monotonic() - t0:.0f} s after the run returned", flush=True)
        pre = fetch(wii, PRE, outdir / "d2xdiag-pre.txt")
        if pre:
            print("--- sd:/d2xdiag-pre.txt (saved before the hang) ---\n" + pre, flush=True)
        # Read the last words: the debug area as the hang left it
        ios = [x for x in a.args if x.startswith("--ios=")]
        r = wt.hbc(wii, "run", "--timeout", "120", str(DOL), *ios, timeout=180)
        print(r.text, flush=True)
        back_in_hbc(wii, 300, "after the last-words run")

    text = fetch(wii, REPORT, outdir / "d2xdiag.txt")
    if text is None:
        sys.exit("d2xdiag.py: no report")
    print("--- sd:/d2xdiag.txt ---\n" + text, flush=True)
    if cap:
        cap.wait(timeout=a.video + 60)
        print(f"d2xdiag.py: TV recording in {outdir / 'd2xdiag.mp4'}", flush=True)
    if hangs:
        ok = "wd_fired_before=yes" in text
        print(f"RESULT {'ok' if ok else 'fail'}: the watchdog {'reset' if ok else 'did NOT reset'} the console "
              f"for {', '.join(sorted(hangs))}", flush=True)
        return 0 if ok else 1
    return 0 if "RESULT ok" in text else 1


if __name__ == "__main__":
    sys.exit(main())
