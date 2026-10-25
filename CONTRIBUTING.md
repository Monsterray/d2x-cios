# Contributing to d2x cIOS

This fork keeps [wiidev/d2x-cios](https://github.com/wiidev/d2x-cios) buildable, adds tests, and fixes module bugs in commits that can go back upstream.

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

The first run builds the image `d2x-build:<hash>` from `docker/Dockerfile`: Debian (pinned by digest) and devkitARM r32 (pinned by SHA-256). The modules are big-endian ARM, and devkitARM releases after r32 cannot build them, so please don't build with a locally installed devkitARM.

Builds are reproducible: the date stamped into the modules comes from the last commit, so the same tree gives byte-identical `.app` files.

`build/` holds one `.app` per module (EHCI, USBS, FAT, MLOAD, DIPP, ES, FFSP), plus `ciosmaps.xml`, `ciosmaps-vWii.xml` and `ReadMe.txt` for the d2x cIOS Installer and ModMii.

## Test

```sh
tools/build.sh test
```

| Check | Where | Needs |
| --- | --- | --- |
| Line endings and `.gitattributes` coverage | `tests/test_line_endings.py` | Python |
| EHCI USB storage driver (Bulk-Only Transport, data toggles, retries, errors) against a simulated drive | `tests/host/ums/` | a C compiler (in the image) |
| USBS USB storage driver against the same drive | `tests/host/usbs/` | a C compiler (in the image) |
| The modules build, with no warnings | `EXTRA_CFLAGS=-Werror tools/build.sh` (CI does this) | Docker |
| FAT module (`fat_wrapper.c`, its IPC handlers, `diskio.c`) on RAM disks | `tests/host/fat/` | a C compiler (in the image) |
| `VERSION` is a valid release number | `tests/test_version.py` | Python |
| USB sector read and write timing through a cIOS on a Wii (`--write-back` puts back the bytes it read; `--pattern=N` writes and verifies known data, **scratch drives only**; `--log` prints the debug build's event log) | `make -C tests/wii/umsbench`, then `python tests/wii/umsbench.py -- --ios=249 [--write-back]` as a wii-bench job | devkitPPC, a Wii with HBC and a USB drive |
| Installing a cIOS on a Wii, unattended | `python tests/wii/install_cios.py PACKAGE INSTALLER.dol` as a wii-bench job, with the d2x installer's `--auto` mode | a Wii you can recover |

Host tests build module source unchanged for your PC and run it against simulated hardware, with AddressSanitizer and UBSan. A test marked `known_bug` describes behaviour that is still broken: it is reported as `known` and doesn't fail the run. When you fix the bug, remove the mark; a `known_bug` test that passes fails the run (`FIXED?`) as a reminder.

Dolphin cannot test a cIOS: it emulates IOS at a high level and never runs the modules.

Changing how the USB driver retries or gives up? Read [docs/usb-storage.md](docs/usb-storage.md) first: the layers, the error codes and the limits, in one page.

Chasing a hang inside IOS, where nothing on the Wii can report? Build with `EXTRA_CFLAGS=-DDIP_DEBUG_TRACE`: the DIP plugin then logs each DI command, each call into Nintendo's DI module and each emulated read into a buffer a loader registers and shows on the TV ([source/dip-plugin/trace.h](source/dip-plugin/trace.h) has the layout and the loader side; USB Loader GX's bench build shows it with `--dip-trace`).

## Conventions

- **Line endings.** Every text file is LF, in the repository and in the checkout (`.gitattributes` sets `* text=auto eol=lf`, whatever `core.autocrlf` says). The one exception: `.bat` files are checked out with CRLF, which cmd.exe needs. Upstream has some files with CRLF; merge it with `git merge -X renormalize upstream/master` so the endings do not show up as conflicts. A checkout made before this rule: run `python tools/fix_line_endings.py`.
- **Upstream-friendly changes.** Keep commits focused. Don't reformat or rename upstream files. A fix that suits upstream goes in its own commit with its test.
- **Evidence.** Say what a change was checked with: a host test, a build, or a run on a Wii. Record findings and ideas in `REVIEW.md`.
- **No keys or console data.** `keys/` is ignored; check `git status` before you commit.
- **AI agents** read `AGENTS.md` and `.agents/skills/d2x-build-and-test/SKILL.md`.

## Safety

A cIOS is installed to the Wii's NAND. Test on a console that has BootMii (or Priiloader) and a NAND backup, install to a spare slot (249 to 251 are the usual ones), and keep a known-good d2x WAD for that slot so you can put it back.
