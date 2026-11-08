# Contributing to d2x cIOS

This fork keeps [wiidev/d2x-cios](https://github.com/wiidev/d2x-cios) buildable, adds tests, and fixes module bugs in commits that can go back upstream.

## Layout

| Path | What is in it |
| --- | --- |
| `source/` | The modules (`mload`, `fat`, `ehci`, `usb`, `dip-plugin`, `es-plugin`, `ffs-plugin`) and `cios-lib`. Kept as upstream has it, so merges stay clean. |
| `maked2x.sh`, `maked2x.bat`, `replace.awk`, `stripios_src/`, `data/` | Upstream's build entry points and release templates. They stay at the root because the module Makefiles and `tools/build.sh` use these paths. |
| `tools/` | Build wrapper, Wii bench helpers, FAT32 repair scripts. |
| `tests/host/` | Host tests against simulated hardware. |
| `tests/wii/` | Bench programs (`sdbench`, `umsbench`, `netbench`) and their Python drivers; shared rules and header in `common/`. |
| `docs/` | Developer notes: `usb-storage.md`, `sd-high-speed.md`, `bench.md`, `module-size.md`, `roadmap.md`. Findings are in `REVIEW.md`. |

## Set up

You need Git, Docker and Python 3.10 or newer. Nothing else is installed on your machine: the toolchain runs in a container.

| OS | Docker |
| --- | --- |
| Linux | Docker Engine; add yourself to the `docker` group |
| macOS | Docker Desktop, OrbStack or Colima |
| Windows | Docker Engine inside WSL 2, or Docker Desktop. Run the commands below from Git Bash. If `docker` is not on PATH there, `tools/build.sh` uses `wsl -e docker`. |

```sh
git clone https://github.com/Monsterray/d2x-cios.git
cd d2x-cios
git remote add upstream https://github.com/wiidev/d2x-cios.git
```

## Build

```sh
tools/build.sh                 # the release in VERSION: build/d2x-v11.1.0/
tools/build.sh 11 beta4-test   # an upstream-style name for experiments: build/d2x-v11-beta4-test/
tools/build.sh clean
```

### Versions

`VERSION` holds the release number, `major.minor.patch` (optionally `-rc1` and the like), and a release is tagged `d2x-v11.1.0`. The major follows upstream d2x (11): loaders compare it, so it changes only with upstream or for a change loaders must know about. The minor counts feature and fix releases, the patch counts fix-only releases.

Inside the cIOS the version is stored the way loaders and ModMii read it: the major as a number (and the TMD revision, 21000 + major), the rest as the version string `-1-0`, since ModMii allows no dots there. USB Loader GX shows `d2x v11-1-0 base56`; upstream builds show `d2x v11beta3`. `tests/test_version.py` checks `VERSION` stays within those limits.

**One rule for every build we put on a Wii, so the loader always says which build it is:**

- The release is `VERSION` and nothing else: `11.1.0-rc1` shows `-1-0-rc1`.
- Any other build appends `+<variant>` to the name and to the version string: `D2X_VARIANT=sdhs tools/build.sh` makes `d2x-v11.1.0-rc1+sdhs`, shown as `-1-0-rc1+sdhs`. A variant is lower-case letters and digits, words joined by `-` (`sdhs`, `usbs`, `dbg`, `trace`, `sdhs-dbg`).
- A build with `EXTRA_CFLAGS` (a debug or trace flag, `-DSDIO_HIGH_SPEED`) must name a variant, or `tools/build.sh` stops. Only CI's `-Werror` is exempt, since it changes no code. So a flagged build can never look like the release.
- The version string holds 15 characters (16 bytes with the NUL). If `VERSION` plus the variant is longer, the build stops with a message; it never truncates. With `-rc1` there are 7 characters for `+variant` (`+gaperr` fits, `+sdhs-dbg` does not); a release without `-rc1` has 11.
- `tools/build.sh version` prints the name and string without building. `tests/test_version.py` checks the rule.
- The 16 bytes cannot hold the commit. Every build also writes `build/BUILD.txt` and a `Build:` line in the generated `ReadMe.txt`: name, version string, git short hash (`-dirty` when tracked files differ), variant and flags. Keep that line (or a manifest like it) with a staged package.
- `tests/wii/umsbench` prints the identity it reads from the cIOS at start, the way USB Loader GX does: `cIOS 249: d2x v11 -1-0-rc1+sdhs base 56 rev 21011`.

The first run builds the image `d2x-build:<hash>` from `docker/Dockerfile`: Debian (pinned by digest) and devkitARM r32 (pinned by SHA-256). The modules are big-endian ARM, and devkitARM releases after r32 cannot build them, so please don't build with a locally installed devkitARM.

Builds are reproducible: the date stamped into the modules comes from the last commit, so the same tree gives byte-identical `.app` files.

`build/` holds one `.app` per module (EHCI, USBS, FAT, MLOAD, DIPP, ES, FFSP), plus `ciosmaps.xml`, `ciosmaps-vWii.xml` and `ReadMe.txt` for the d2x cIOS Installer and ModMii.

### Which bases we test

The maps list ten Wii bases. We test the ones people install:

| Base | Usual slot | Used for | Tested |
|---|---|---|---|
| 56 | 249 | Most games and loaders | Map checked, boots in ironic, on the Wii |
| 57 | 250 | USB loaders | Map checked, boots in ironic |
| 38 | 251 | The few games that need it | Map checked, boots in ironic |
| 58 | | USB through Nintendo's own USB 2.0 driver (the USBS module instead of EHCI) | Map checked |
| 37, 53, 55, 80 | | Rarely used | Map checked |
| 60, 70 | | Superseded System Menu IOS; Nintendo replaced both with stubs | Not checked, kept from upstream |

"Map checked" means `tools/check_ciosmaps.py` matched every patch against that base's contents from a Wii's NAND. "Boots in ironic" means the built cIOS was installed into a copy of a NAND and started in the ironic Starlet emulator, with mload, the d2x identity and the module devices answering. ironic has no USB storage or disc drive, so USB and disc changes still need a Wii. 60 and 70 stay in the maps so installers that offer them keep working; checking them needs the old full versions (60 v6174, 70 v6687) from Nintendo's update server.

## Test

```sh
tools/build.sh test
```

| Check | Where | Needs |
| --- | --- | --- |
| Line endings and `.gitattributes` coverage | `tests/test_line_endings.py` | Python |
| No workstation paths in tracked files (shared tools go through `tools/wii_paths.py`) | `tests/test_workstation_paths.py` | Python, git |
| EHCI USB storage driver (Bulk-Only Transport, data toggles, retries, errors) against a simulated drive | `tests/host/ums/` | a C compiler (in the image) |
| USBS USB storage driver against the same drive | `tests/host/usbs/` | a C compiler (in the image) |
| The modules build, with no warnings | `EXTRA_CFLAGS=-Werror tools/build.sh` (CI does this) | Docker |
| FAT module (`fat_wrapper.c`, its IPC handlers, `diskio.c`) on RAM disks | `tests/host/fat/` | a C compiler (in the image) |
| `VERSION` is a valid release number; variant names fit the 15-character version string | `tests/test_version.py` | Python, sh |
| USB sector read and write timing through a cIOS on a Wii (`--write-back` puts back the bytes it read; `--pattern=N` writes and verifies known data, **scratch drives only**; `--log` prints the debug build's event log) | `make -C tests/wii/umsbench`, then `python tests/wii/umsbench.py -- --ios=249 [--write-back]` as a wii-bench job | devkitPPC, a Wii with HBC and a USB drive |
| SD sector read timing through `/dev/sdio/sdhc` and the SD high-speed state (`0x20`; `0x21` forces the fallback and re-reads to compare a CRC32). Read only: it never writes a sector of the card | `make -C tests/wii/sdbench`, then `python tests/wii/sdbench.py -- --ios=249 [--mb=32]` as a wii-bench job | devkitPPC, a Wii with HBC and an SD card |
| The debug build's area, counters, watchdog decision and fault points, and each module's hooks, against simulated drives (`-DD2X_DEBUG`) | `tests/host/dbg`, `tests/host/*/test_*_dbg.c` | a C compiler (in the image) |
| A `+dbg` build on a Wii: identity, counters, trace, last words, fault scenarios, and (attended) a deliberate IOS hang the ARM watchdog recovers from | `make -C tests/wii/d2xdiag`, then `python tests/wii/d2xdiag.py [--attended] -- --ios=N [--wd=S] --scenario=...` as a wii-bench job ([docs/debug-build.md](docs/debug-build.md)) | devkitPPC, a Wii with HBC and the `+dbg` cIOS in a spare slot |
| Installing a cIOS on a Wii, unattended | `python tests/wii/install_cios.py PACKAGE INSTALLER.dol` as a wii-bench job, with the d2x installer's `--auto` mode | a Wii you can recover |

Host tests build module source unchanged for your PC and run it against simulated hardware, with AddressSanitizer and UBSan. A test marked `known_bug` describes behaviour that is still broken: it is reported as `known` and doesn't fail the run. When you fix the bug, remove the mark; a `known_bug` test that passes fails the run (`FIXED?`) as a reminder.

Dolphin cannot test a cIOS: it emulates IOS at a high level and never runs the modules.

Changing how the USB driver retries or gives up? Read [docs/usb-storage.md](docs/usb-storage.md) first: the layers, the error codes and the limits, in one page.

Chasing a problem inside IOS? Build the debug variant: `D2X_VARIANT=dbg EXTRA_CFLAGS="-DD2X_DEBUG -DSDIO_HIGH_SPEED" tools/build.sh`. It keeps counters and one trace ring for EHCI, USBS, SD, FAT and DIP in a fixed MEM2 area the PowerPC can read live, keeps the previous IOS run's "last words", has an ARM watchdog that resets the console when a test app stops checking in, and takes injected faults through `/dev/d2xdbg`. The release build is byte-identical without it. See [docs/debug-build.md](docs/debug-build.md).

Chasing a hang inside IOS, where nothing on the Wii can report? Build with `EXTRA_CFLAGS=-DDIP_DEBUG_TRACE`: the DIP plugin then logs each DI command, each call into Nintendo's DI module and each emulated read into a buffer a loader registers and shows on the TV ([source/dip-plugin/trace.h](source/dip-plugin/trace.h) has the layout and the loader side; USB Loader GX's bench build shows it with `--dip-trace`).

## Conventions

- **Line endings.** Every text file is LF, in the repository and in the checkout (`.gitattributes` sets `* text=auto eol=lf`, whatever `core.autocrlf` says). The one exception: `.bat` files are checked out with CRLF, which cmd.exe needs. Upstream has some files with CRLF; merge it with `git merge -X renormalize upstream/master` so the endings do not show up as conflicts. A checkout made before this rule: run `python tools/fix_line_endings.py`.
- **Module size.** The `exe` region of each module is fixed in its `link.ld`; check headroom after a change (`arm-eabi-objdump -h` on `*.elf.orig`, or `tools/build.sh shell`; `docs/module-size.md`, "Module size headroom"). Everything is Thumb and `-Os` with `--gc-sections`: give new assembly stubs a section of their own (`.section .text.NAME`) so a module that does not call them does not carry them, and keep libgcc's divide routines out of modules that have no hot-path division (use shifts, or `wbfs_udiv`).
- **Upstream-friendly changes.** Keep commits focused. Don't reformat or rename upstream files. A fix that suits upstream goes in its own commit with its test.
- **Evidence.** Say what a change was checked with: a host test, a build, or a run on a Wii. Record findings in `REVIEW.md`, measurements in `docs/bench.md`, and ideas in `docs/roadmap.md`.
- **No keys or console data.** `keys/` is ignored; check `git status` before you commit.
- **AI agents** read `AGENTS.md` and `.agents/skills/d2x-build-and-test/SKILL.md`.

## Safety

A cIOS is installed to the Wii's NAND. Test on a console that has BootMii (or Priiloader) and a NAND backup, install to a spare slot (249 to 251 are the usual ones), and keep a known-good d2x WAD for that slot so you can put it back.
