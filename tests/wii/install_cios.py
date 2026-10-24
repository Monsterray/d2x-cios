"""Install a cIOS package on the bench Wii with the d2x installer's --auto mode.

    python tests/wii/install_cios.py PACKAGE_DIR INSTALLER_DOL [--name NAME]

A wii-bench queue job (tools/wii_targets.py refuses to run outside one). It
copies PACKAGE_DIR (CIOSMAPS.xml, the module .app files and a one-line
ciosconf.cfg: "|group| base slot revision") to sd:/apps/NAME, runs the
installer with --auto --dir=sd:/apps/NAME through `hbc.py run`, and prints
its output. The installer downloads the base IOS from NUS into sd:/nus.

This writes to the Wii's NAND. Run it only with the owner's OK for that
install, and keep a package that restores the slot.
"""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
import wii_targets as wt  # noqa: E402


def main():
    p = wt.parser(__doc__)
    p.add_argument("package", type=pathlib.Path)
    p.add_argument("installer", type=wt.existing_file)
    p.add_argument("--name", default="d2x-auto")
    a = p.parse_args()
    conf = (a.package / "ciosconf.cfg").read_text().strip()
    if not re.fullmatch(r"\|[^|]+\| \d+ \d+ \d+", conf):
        sys.exit(f"ciosconf.cfg: expected '|group| base slot revision', got {conf!r}")
    wii = wt.bench_wii("install_cios.py")
    print(f"install_cios: {conf} on {wii}", flush=True)

    dest = f"sd:/apps/{a.name}"
    r = wt.hbc(wii, "put", "-r", str(a.package), dest, timeout=300, show=True)
    if r.rc:
        sys.exit("install_cios: copying the package failed")
    r = wt.hbc(wii, "run", "--timeout", "1500", str(a.installer), "--auto", f"--dir={dest}",
               timeout=1600)
    print(r.text, flush=True)
    ok = "auto: slot" in r.text and "installed" in r.text and "FAILED" not in r.text
    print(f"install_cios: {'OK' if ok else 'FAILED'} (hbc.py run exit {r.rc})", flush=True)
    if not wt.hbc_ready(wii, 120):
        print("install_cios: HBC did not come back within 120 s", flush=True)
        return 1
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
