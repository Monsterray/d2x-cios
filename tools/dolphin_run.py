#!/usr/bin/env python3
"""Run a DOL, ELF or WAD in Dolphin unattended and give a verdict from its log.

    python tools/dolphin_run.py IMAGE [--seconds 60] [--out DIR]
                                [--expect REGEX] [--fail REGEX]
                                [--set System.Section.Key=Value ...]
                                [--sd DIR] [--mmu] [--cache] [--keep]

From wii-homebrew-kit (docs/dolphin.md). Each run gets its own directory (--out,
default .runs/<image>-<time>) holding a copy of the image, a throwaway Dolphin
profile, the log, and run.json with the verdict. Every Dolphin setting is passed
as a -C flag, so no Dolphin.ini is written, and only the Dolphin this script
started is ever stopped.

Verdicts (exit code):
  PASS (0)     --expect matched in the guest log (or, with no --expect, the run
               lasted --seconds, or Dolphin exited, with no fault)
  FAIL (1)     --fail matched, or a fault line appeared (Invalid read/write,
               Unknown instruction, PANIC, Exception)
  TIMEOUT (2)  --expect was given and never matched in --seconds

--sd DIR      copy DIR onto the emulated SD card (Load/WiiSDSync in the profile)
--mmu         Dolphin.Core.MMU=True: a bad pointer raises a DSI in the guest, as on a
              Wii, and the guest's own handler takes it (a crash screen). Dolphin logs
              nothing, so the log verdict cannot see it: use a guest-side signal
              (--fail on what the handler prints) or a GDB stub. Without --mmu, Dolphin
              logs "Invalid write to 0x..." and the run FAILs.
--cache       Dolphin.Core.AccurateCPUCache=True: finds missing cache flushes
Set DOLPHIN to the Dolphin executable if it is not in a standard place.
"""
import argparse
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parents[1]

SETTINGS = [
    "Dolphin.Interface.ConfirmStop=False",
    "Dolphin.Interface.UsePanicHandlers=False",
    "Dolphin.Analytics.PermissionAsked=True",
    "Dolphin.Analytics.Enabled=False",
    "Dolphin.Core.CPUThread=True",
    "Logger.Options.WriteToFile=True",
    "Logger.Options.Verbosity=4",
    "Logger.Logs.MASTER=True",
    "Logger.Logs.BOOT=True",
    "Logger.Logs.OSREPORT=True",
    "Logger.Logs.IOS=True",
    "Logger.Logs.POWERPC=True",     # exceptions (DSI, program) and MMU faults
    "Logger.Logs.MEMMAP=True",      # "Invalid read/write" with --mmu
]
SD_SETTINGS = [
    "Dolphin.Core.WiiSDCard=True",
    "Dolphin.Core.WiiSDCardAllowWrites=True",
    "Dolphin.Core.WiiSDCardEnableFolderSync=True",
]
FAULTS = re.compile(r"Unknown instruction|Invalid (read|write)|PANIC|Exception")
SEGMENT = re.compile(r"Starting core = (Wii|GameCube) mode")
LOG_MAX = int(os.environ.get("DOLPHIN_LOG_MAX_MB", "512")) * 1024 * 1024


def find_dolphin():
    if os.environ.get("DOLPHIN"):
        return os.environ["DOLPHIN"]
    candidates = {
        "Windows": [r"C:\tools\Dolphin-x64\Dolphin.exe", r"C:\Program Files\Dolphin\Dolphin.exe"],
        "Darwin": ["/Applications/Dolphin.app"],
    }.get(platform.system(), [])
    candidates.append(shutil.which("dolphin-emu") or "")
    for path in candidates:
        if path and os.path.exists(path):
            return path
    sys.exit("Dolphin not found; set DOLPHIN to its executable")


def last_segment(text):
    """The log of the last boot in this profile only."""
    starts = [m.start() for m in SEGMENT.finditer(text)]
    return text[starts[-1]:] if starts else text


class Run:
    def __init__(self, image, out, settings=(), sd=None, mmu=False, cache=False):
        self.dolphin = find_dolphin()
        self.out = pathlib.Path(out)
        self.out.mkdir(parents=True, exist_ok=True)
        # A copy, so a rebuild during the run cannot change what ran.
        self.image = self.out / pathlib.Path(image).name
        shutil.copy2(image, self.image)
        self.profile = self.out / "profile"
        self.log = self.profile / "Logs" / "dolphin.log"
        cfg = list(SETTINGS)
        if sd:
            cfg += SD_SETTINGS
            shutil.copytree(sd, self.profile / "Load" / "WiiSDSync", dirs_exist_ok=True)
        if mmu:
            cfg.append("Dolphin.Core.MMU=True")
        if cache:
            cfg.append("Dolphin.Core.AccurateCPUCache=True")
        if self.image.suffix.lower() == ".wad":
            # With cheats on (no codes), Dolphin keeps its HBReload hook out of an
            # installed Homebrew Channel's reload stub.
            cfg.append("Dolphin.Core.EnableCheats=True")
        cfg += list(settings)
        self.args = ["-b", "-e", str(self.image), "-u", str(self.profile)]
        for c in cfg:
            self.args += ["-C", c]

    def start(self):
        if self.dolphin.endswith(".app"):
            # A direct executable launch aborts in Qt/Cocoa start-up on macOS.
            cmd = ["open", "-n", "-W", "-a", self.dolphin, "--args"] + self.args
        else:
            cmd = [self.dolphin] + self.args
        (self.out / "command.txt").write_bytes(" ".join(cmd).encode() + b"\n")
        self.proc = subprocess.Popen(cmd)

    def running(self):
        return self.proc.poll() is None

    def stop(self):
        if not self.running():
            return
        if self.dolphin.endswith(".app"):
            subprocess.run(["pkill", "-f", str(self.profile)])
        self.proc.kill()                # this run's process only
        self.proc.wait()

    def text(self):
        try:
            return last_segment(self.log.read_bytes().decode(errors="replace"))
        except OSError:
            return ""


def run(image, seconds=60, out=None, expect=None, fail=None, settings=(), sd=None,
        mmu=False, cache=False, keep=False):
    """Run IMAGE and return the verdict as a dict (also written to OUT/run.json)."""
    image = pathlib.Path(image)
    out = pathlib.Path(out or REPO / ".runs" / f"{image.stem}-{time.strftime('%Y%m%d-%H%M%S')}")
    r = Run(image, out, settings, sd, mmu, cache)
    expect_re = re.compile(expect) if expect else None
    fail_re = re.compile(fail) if fail else None
    verdict, reason = None, ""
    t0 = time.monotonic()
    r.start()
    try:
        while verdict is None:
            time.sleep(0.5)
            text = r.text()
            fault = FAULTS.search(text) or (fail_re.search(text) if fail_re else None)
            if fault:
                line = text[text.rfind("\n", 0, fault.start()) + 1:].split("\n", 1)[0]
                verdict, reason = "FAIL", line.strip()
            elif expect_re and expect_re.search(text):
                verdict, reason = "PASS", f"matched {expect!r}"
            elif r.log.exists() and r.log.stat().st_size > LOG_MAX:
                verdict, reason = "FAIL", f"log passed {LOG_MAX >> 20} MB"
            elif not r.running():
                verdict, reason = ("TIMEOUT", "Dolphin exited before a match") if expect_re \
                    else ("PASS", "Dolphin exited with no fault")
            elif time.monotonic() - t0 > seconds:
                verdict, reason = ("TIMEOUT", f"no match in {seconds} s") if expect_re \
                    else ("PASS", f"ran {seconds} s with no fault")
    finally:
        r.stop()
    if r.log.exists():
        shutil.copy2(r.log, out / "dolphin.log")
    result = {"verdict": verdict, "reason": reason, "image": str(image),
              "seconds": round(time.monotonic() - t0, 1), "dolphin": r.dolphin,
              "out": str(out)}
    (out / "run.json").write_bytes(json.dumps(result, indent=2).encode() + b"\n")
    if not keep and verdict == "PASS":
        shutil.rmtree(r.profile, ignore_errors=True)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__, allow_abbrev=False,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("image")
    p.add_argument("--seconds", type=float, default=60)
    p.add_argument("--out")
    p.add_argument("--expect")
    p.add_argument("--fail")
    p.add_argument("--set", action="append", default=[], metavar="S.S.K=V")
    p.add_argument("--sd")
    p.add_argument("--mmu", action="store_true")
    p.add_argument("--cache", action="store_true")
    p.add_argument("--keep", action="store_true", help="keep the profile after a PASS")
    a = p.parse_args()
    res = run(a.image, a.seconds, a.out, a.expect, a.fail, a.set, a.sd, a.mmu, a.cache, a.keep)
    print(f"{res['verdict']}: {res['reason']} ({res['seconds']} s, {res['out']})")
    return {"PASS": 0, "FAIL": 1, "TIMEOUT": 2}[res["verdict"]]


if __name__ == "__main__":
    sys.exit(main())
