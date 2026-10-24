"""Run umsbench on the bench Wii and print its full report.

    python tests/wii/umsbench.py [--timeout S] [-- UMSBENCH_ARGS...]

A wii-bench queue job. `hbc.py run` shows umsbench's output only until it
reloads IOS (the reload drops the network), so umsbench also saves its report
to sd:/umsbench.txt; this script waits for the Wii to return to HBC, fetches
that file, and prints it. Example: -- --ios=249 --write-back --writes=20
--video S records the TV for S seconds through the capture card (umsbench
draws its report there, so a run never looks like a frozen Wii).
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

DOL = here.parent / "umsbench" / "umsbench.dol"
REPORT = "sd:/umsbench.txt"
WIICAP = os.environ.get("WIICAP", "C:/tools/wii-capture/wiicap.py")


def main():
    p = wt.parser(__doc__)
    p.add_argument("--timeout", type=int, default=1800)
    p.add_argument("--video", type=int, default=0, help="record the TV for this many seconds")
    p.add_argument("args", nargs="*")
    a = p.parse_args()
    if not DOL.is_file():
        sys.exit(f"{DOL}: build it first (make -C tests/wii/umsbench)")
    wii = wt.bench_wii("umsbench.py")

    wt.hbc(wii, "rm", REPORT, timeout=30)            # an old report must not count
    outdir = pathlib.Path(os.environ.get("WII_BENCH_OUT") or tempfile.mkdtemp())
    cap = None
    if a.video and pathlib.Path(WIICAP).is_file():
        cap = subprocess.Popen([sys.executable, WIICAP, "video", str(a.video), "--out",
                                str(outdir / "umsbench.mp4"), "--label", "umsbench"])
    r = wt.hbc(wii, "run", "--timeout", str(a.timeout), str(DOL), *a.args,
               timeout=a.timeout + 60)
    print(r.text, flush=True)

    end = time.monotonic() + a.timeout
    time.sleep(5)
    while not wt.in_hbc(wii):
        if time.monotonic() > end:
            sys.exit(f"umsbench.py: the Wii did not return to HBC in {a.timeout} s")
        time.sleep(5)

    out = outdir / "umsbench.txt"
    r = wt.hbc(wii, "get", REPORT, str(out), timeout=60)
    if r.rc or not out.is_file():
        sys.exit(f"umsbench.py: no report ({r.text[-200:]})")
    text = out.read_text(errors="replace")
    print("--- sd:/umsbench.txt ---\n" + text, flush=True)
    if cap:
        cap.wait(timeout=a.video + 60)
        print(f"umsbench.py: TV recording in {outdir / 'umsbench.mp4'}", flush=True)
    return 0 if "RESULT" in text and "error=" not in text else 1


if __name__ == "__main__":
    sys.exit(main())
