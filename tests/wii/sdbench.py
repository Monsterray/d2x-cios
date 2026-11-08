"""Run sdbench (SD read timing through /dev/sdio/sdhc) on the bench Wii and print its full report.

    python tests/wii/sdbench.py [--timeout S] [-- SDBENCH_ARGS...]

A wii-bench queue job. `hbc.py run` shows sdbench's output only until it
reloads IOS (the reload drops the network), so sdbench also saves its report
to sd:/sdbench.txt; this script waits for the Wii to return to HBC, fetches
that file, and prints it. Read only. Example: -- --ios=249 --mb=32
--video S records the TV for S seconds through the capture card (sdbench
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

DOL = here.parent / "sdbench" / "sdbench.dol"
REPORT = "sd:/sdbench.txt"
try:        # the capture card's wiicap.py ($WIICAP or ~/.wii-paths.json); none: no recording
    WIICAP = str(wt.wii_paths.wiicap())
except wt.wii_paths.NotFound:
    WIICAP = ""


def main():
    p = wt.parser(__doc__)
    p.add_argument("--timeout", type=int, default=1800)
    p.add_argument("--video", type=int, default=0, help="record the TV for this many seconds")
    p.add_argument("args", nargs="*")
    a = p.parse_args()
    if not DOL.is_file():
        sys.exit(f"{DOL}: build it first (make -C tests/wii/sdbench)")
    wii = wt.bench_wii("sdbench.py")

    wt.hbc(wii, "rm", REPORT, timeout=30)            # an old report must not count
    outdir = pathlib.Path(os.environ.get("WII_BENCH_OUT") or tempfile.mkdtemp())
    cap = None
    if a.video and WIICAP:
        cap = subprocess.Popen([sys.executable, WIICAP, "video", str(a.video), "--out",
                                str(outdir / "sdbench.mp4"), "--label", "sdbench"])
    r = wt.hbc(wii, "run", "--timeout", str(a.timeout), str(DOL), *a.args,
               timeout=a.timeout + 60)
    print(r.text, flush=True)

    end = time.monotonic() + a.timeout
    time.sleep(5)
    while not wt.in_hbc(wii):
        if time.monotonic() > end:
            sys.exit(f"sdbench.py: the Wii did not return to HBC in {a.timeout} s")
        time.sleep(5)

    out = outdir / "sdbench.txt"
    r = wt.hbc(wii, "get", REPORT, str(out), timeout=60)
    if r.rc or not out.is_file():
        sys.exit(f"sdbench.py: no report ({r.text[-200:]})")
    text = out.read_text(errors="replace")
    print("--- sd:/sdbench.txt ---\n" + text, flush=True)
    if cap:
        cap.wait(timeout=a.video + 60)
        print(f"sdbench.py: TV recording in {outdir / 'sdbench.mp4'}", flush=True)
    return 0 if "RESULT ok" in text else 1


if __name__ == "__main__":
    sys.exit(main())
