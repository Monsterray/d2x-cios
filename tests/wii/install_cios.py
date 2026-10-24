"""Install a cIOS package on the bench Wii with the d2x installer's --auto mode.

    python tests/wii/install_cios.py PACKAGE_DIR INSTALLER_DOL [--name NAME]

A wii-bench queue job (tools/wii_targets.py refuses to run outside one). It
copies PACKAGE_DIR to sd:/apps/NAME, runs the installer with --auto
--dir=sd:/apps/NAME through `hbc.py run`, prints its output, and records the
TV with the capture card. The installer downloads the base IOS from NUS into
sd:/nus. PACKAGE_DIR holds:

    CIOSMAPS.xml        the build's ciosmaps.xml
    ciosconf.cfg        one line: "|group| base slot revision"
    <group>/<MODULE>.app   every module the group's maps name

Try a package in Dolphin first (the installer's -DAUTO_DIR build): it runs
everything up to the ES install, which Dolphin refuses (fakesigned ticket).

This writes to the Wii's NAND. Run it only with the owner's OK for that
install, and keep a package that restores the slot.
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
import wii_targets as wt  # noqa: E402

# The shared HDMI capture card on the bench Wii (this workstation's tools).
WIICAP = os.environ.get("WIICAP", "C:/tools/wii-capture/wiicap.py")
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def main():
    p = wt.parser(__doc__)
    p.add_argument("package", type=pathlib.Path)
    p.add_argument("installer", type=wt.existing_file)
    p.add_argument("--name", default="d2x-auto")
    p.add_argument("--video", type=int, default=240,
                   help="seconds of the TV to record with the capture card (0: none)")
    p.add_argument("--dry-run", action="store_true",
                   help="run every step and check except the NAND writes")
    p.add_argument("--timeout", type=int, default=1200)
    a = p.parse_args()
    group = check_package(a.package)
    wii = wt.bench_wii("install_cios.py")
    conf = (a.package / "ciosconf.cfg").read_text().strip()
    print(f"install_cios: {conf} on {wii}{' (dry run)' if a.dry_run else ''}", flush=True)

    dest = f"sd:/apps/{a.name}"
    result = f"{dest}/auto-result.txt"
    wt.hbc(wii, "rm", result, timeout=30)          # an old result must not count
    r = wt.hbc(wii, "put", "-r", str(a.package), dest, timeout=300, show=True)
    if r.rc:
        sys.exit("install_cios: copying the package failed")

    # The installer draws its progress on the TV and writes auto-result.txt;
    # its network log does not reach the PC, so send it and wait for HBC.
    out = pathlib.Path(os.environ.get("WII_BENCH_OUT") or tempfile.mkdtemp())
    cap = None
    if a.video and pathlib.Path(WIICAP).is_file():
        cap = subprocess.Popen([sys.executable, WIICAP, "video", str(a.video), "--out",
                                str(out / "install.mp4"), "--label", f"install {group}"])
    args = ["--auto", f"--dir={dest}"] + (["--dry-run"] if a.dry_run else [])
    r = wt.hbc(wii, "send", str(a.installer), *args, timeout=120, show=True)
    if r.rc:
        sys.exit("install_cios: sending the installer failed")

    end = time.monotonic() + a.timeout
    time.sleep(10)
    while not wt.in_hbc(wii):
        if time.monotonic() > end:
            print(f"install_cios: the Wii did not return to HBC in {a.timeout} s", flush=True)
            return 1
        time.sleep(5)

    got = out / "auto-result.txt"
    r = wt.hbc(wii, "get", result, str(got), timeout=60)
    text = got.read_text(errors="replace") if r.rc == 0 and got.is_file() else ""
    print(f"install_cios: auto-result.txt:\n{text or '(none: the installer stopped early)'}",
          flush=True)
    want = "DRYRUN" if a.dry_run else "OK"
    ok = bool(text) and all(line.startswith(want) for line in text.splitlines())
    print(f"install_cios: {'OK' if ok else 'FAILED'}", flush=True)
    if cap:
        cap.wait(timeout=a.video + 60)
        print(f"install_cios: TV recording in {out / 'install.mp4'}", flush=True)
    return 0 if ok else 1


def check_package(package):
    """The layout the installer expects, checked here rather than on the Wii:
    CIOSMAPS.xml naming the group, ciosconf.cfg, and every module the group's
    maps name in a subfolder called after the group."""
    conf = (package / "ciosconf.cfg").read_text().strip()
    m = re.fullmatch(r"\|([^|]+)\| \d+ \d+ \d+", conf)
    if not m:
        sys.exit(f"ciosconf.cfg: expected '|group| base slot revision', got {conf!r}")
    group = m[1]
    maps = (package / "CIOSMAPS.xml").read_text()
    if f'name="{group}"' not in maps:
        sys.exit(f"CIOSMAPS.xml: no cIOS group named {group!r}")
    missing = [mod for mod in sorted(set(re.findall(r'module="([^"]+)"', maps)))
               if not (package / group / f"{mod}.app").is_file()]
    if missing:
        sys.exit(f"{package / group}: missing modules {', '.join(missing)} "
                 "(the installer looks for <app folder>/<group>/<module>.app)")
    return group


if __name__ == "__main__":
    sys.exit(main())
