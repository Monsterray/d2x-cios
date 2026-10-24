---
name: d2x-build-and-test
description: Build, test, review and hardware-check the d2x cIOS modules (EHCI, USBS, FAT, MLOAD, DIPP, ES, FFSP, cios-lib). Use for any change under source/, for a failing build or test, for adding a host test or a simulated fault, for reading module behaviour from the code, and before any work that would put a cIOS on a Wii.
---

# d2x build and test

## Approach

- Trace the path from the caller (a loader's ioctl) to the hardware before changing code. The usual chain for USB storage: `ehci-module/loop.c` (ioctl) -> `tinyehci/usbstorage.c` (Bulk-Only Transport) -> `tinyehci/ehci.c` (EHCI queue heads, `handshake()`, `qh_end_transfer()`).
- Know how errors travel. In tinyehci a STALLed transfer comes back as a short byte count, not an error code. A transfer with no answer returns `-ENODEV` and sets `unplug_device` (`handshake_mode` 0), or `-ETIMEDOUT` (`handshake_mode` 1).
- Fix the smallest shared cause. Write the test first: a host test that shows the bug, marked `known_bug`. Then fix the code and remove the mark.
- Keep verified defects apart from ideas. Ideas go in `REVIEW.md` with evidence and a trigger.

## Build

```sh
tools/build.sh                 # build/d2x-v999-unknown/*.app
tools/build.sh 11 beta4        # a named version
tools/build.sh clean
tools/build.sh shell           # a shell in the build image
```

- The image (`docker/Dockerfile`) pins Debian by digest and devkitARM r32 by SHA-256. It is tagged with the Dockerfile's git hash, so editing the Dockerfile rebuilds it.
- On Windows, run from Git Bash. When `docker` is not on PATH, `build.sh` uses Docker inside WSL (`wsl -e docker`).
- The build is reproducible: `SOURCE_DATE_EPOCH` comes from the last commit. With an unchanged tree, two builds give byte-identical `.app` files. Use that to show a change is neutral (compare hashes before and after).
- `maked2x.sh` needs gawk (`replace.awk` uses `rshift`). It now stops if the replacement fails; before, mawk left empty `ciosmaps.xml`, `ReadMe.txt` and `d2x-beta.bat` files.
- Don't remove an image on your own initiative. Run `docker system df` to see the space. Old `d2x-build:*` tags are safe to remove; other images and containers are not yours.

## Host tests

```sh
tools/build.sh test            # tests/host (C, ASan+UBSan) and tests/test_*.py
make -C tests/host             # the same, with a local cc
./tests/host/ums/test_ums write    # only tests whose name contains "write"
```

- `tests/host/ums/` builds `source/ehci-module/tinyehci/usbstorage.c` unchanged, in one translation unit (`ums_unit.c`), against `sim.c`: a simulated Bulk-Only drive and EHCI layer.
- Time is simulated. A test that would hang the module stops at `sim.limit_us` and reports `hung` with its counters (CBWs, CSWs, BOT resets, clear-halts, port resets).
- To add a fault: add a countdown field to `struct sim` (`sim.h`), act on it in `sim.c` where the real device would misbehave, and write a test that sets it. Model the fault on what a real drive does (USB Mass Storage Class Bulk-Only Transport 1.0, sections 5 and 6), and say so in a comment.
- Host builds are 64-bit and little-endian. Code that compares structs containing pointers, or that depends on endianness, can behave differently. The descriptor comparison in `USBStorage_Open()` is the known case; `sim.c` serves descriptors from static storage for it.
- An unexpected `FIXED?` result means a `known_bug` test passes. Find out why before removing the mark.

## Dolphin

Dolphin does not run Starlet code, so it cannot load or test a cIOS module (see `Source/Core/Core/IOS/IOS.cpp` in Dolphin: cIOS device paths are logged as possible anti-piracy checks and answered `IPC_ENOENT`). It is useful only for the PowerPC side of a test program, for example to check that a bench test DOL starts and prints before it goes to a real Wii.

## Wii bench

Hardware runs go through the shared bench queue (`wiibench.py add ... -- CMD`, then `wait`). Rules:

- A cIOS install changes the console. Get the user's explicit OK for each install, name the slot and base IOS, and keep a known-good d2x WAD for that slot to restore. Never use a production Wii.
- Prefer read-only probes first: which slots hold which cIOS (version and base) is visible from a DOL without writing anything.
- A job must leave the Wii in the Homebrew Channel and collect its own logs.
- Test with the kind of drive the bug was seen on, and record the drive (vendor, product, size, file system) with the result.

The bench test program (a DOL that opens the cIOS's USB device, times sector reads and writes, and reports through the HBC log) is the next piece to build; see REVIEW.md.

## Review and release

- Report whether a check covered a host test, a build, or a run on a Wii with the cIOS installed. These are different levels of evidence.
- A release names its base: the upstream commit (`git merge-base HEAD upstream/master`) and the fork commits on top.
