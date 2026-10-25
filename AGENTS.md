# d2x cIOS agent guidance

For build, test, review, or Wii work, read [the d2x skill](.agents/skills/d2x-build-and-test/SKILL.md). Use [CONTRIBUTING.md](CONTRIBUTING.md) for setup and [REVIEW.md](REVIEW.md) for the module review plan and findings.

- Build only with `tools/build.sh` (the pinned devkitARM r32 image in `docker/Dockerfile`). The modules are big-endian ARM; a newer devkitARM cannot build them, and a build that looks fine with another toolchain proves nothing.
- Test with `tools/build.sh test`. Host tests run module code against simulated hardware (`tests/host/`). A test for a bug that is not fixed yet is marked `known_bug`; when the fix lands, remove the mark in the same commit.
- Dolphin cannot run a cIOS. It emulates IOS at a high level and never runs Starlet (ARM) code, and it answers `/dev/mload`, `/dev/usb2` and the other cIOS devices with "not found". Do not cite a Dolphin run as evidence about module behaviour.
- Installing a cIOS writes to the console's NAND. Never install one, or queue a job that does, without the user's explicit OK for that install. Never on a production Wii. A bench job must leave the Wii in the Homebrew Channel.
- Keep Wii keys and console data out of the repo. `keys/` is ignored. Check staged file names before every commit.
- Every text file is LF, in the repository and the checkout; only `.bat` files get CRLF in the checkout (`.gitattributes`, for cmd.exe). `tests/test_line_endings.py` checks it; `tools/fix_line_endings.py` converts an old checkout. Merge upstream (wiidev/d2x-cios, which has CRLF files) with `git merge -X renormalize`. Write files from Python with `write_bytes`, not `write_text`: on Windows, text mode writes CRLF.
- Keep upstream mergeable: focused commits, no reformatting of upstream files, no mass renames. Fixes that suit upstream go in commits of their own.
- Prefer a focused fix with a matching test. Record larger ideas in `REVIEW.md` with evidence and a clear trigger.
- Before changing how the USB driver retries, recovers or gives up, read [docs/usb-storage.md](docs/usb-storage.md), and add a fault to `tests/host/ums/sim.c` for any new failure mode.
- USB write tests on a Wii (`umsbench --pattern`, GX writes) only on a scratch drive whose data nobody needs: `--pattern` overwrites sectors in the middle of the drive. `--write-back` cannot catch a write that lands stale data. Never write through d2x to the SanDisk "USB 3.2Gen1" flash drive (60088319 sectors): it lands stale data (REVIEW.md).
- A hang inside IOS (for example a DI read on a Wii with a dead disc drive) needs a power cycle: nothing on the Wii recovers it. Run anything that may hang only with someone at the Wii. To see where IOS stopped, use a DIP trace build (`EXTRA_CFLAGS=-DDIP_DEBUG_TRACE`, `source/dip-plugin/trace.h`) with a loader that shows the trace on the TV.
