# MLOAD and the SD module loader

MLOAD is the cIOS module that lets a loader put its own code into IOS while it runs. This page says how it works, what the SD module loader adds (`source/mload-module/sdload.c`), where a module must live, what a bad list or module can do, and how to test it on a Wii. Path:line references are to this repository unless a project is named. Checked with: the host tests (`tests/host/mloadsd`, 49 checks, including the real test module) and the builds; nothing here has run on a Wii yet.

## How mload works

- **Start.** IOS loads each module as an ELF content. `data/ciosmaps.xml:41` patches the kernel's IRQ9 vector to `0x136D0011` (per base), so MLOAD's code is the handler. MLOAD is content `0x23`. `source/mload-module/start.s:36-62`: `_start` calls `main()`; IRQ9 goes to `__MLoad_System`.
- **`main()`** (`source/mload-module/main.c`): raises software IRQ 9 (detect and patch the DIP, ES, FFS and IOP modules), runs `__MLoad_Initialize` (an `0x4000`-byte heap, the timer subsystem, an 8-message queue, the device `/dev/mload`), then loops on the queue.
- **Memory** (`source/mload-module/link.ld`): code ("exe") at `0x136D0000`, `0x3000` long; data and bss ("ram") at `0x136D3000`, `0x9000` long; and `exe_mem`, a 512 KB area at `0x13700000` (to `0x13780000`) that is the only region MLOAD's process owns. Its neighbours: EHCI `0x13780000`, FAT `0x137B5000`, DIPP `0x13800000`, FFSP `0x13843000`, ES `0x13849000`. IOS maps MEM2 only as far as the modules reach, so a new fixed address needs a mapping (`REVIEW.md:125`; `dbg.c` `__Dbg_MapArea`). The exe region grew from `0x2000` to `0x3000` for the loader; `docs/module-size.md` has the sizes.
- **Commands on `/dev/mload`** (`module.h`, `main.c`): `0x4D4C4401` GET_IOS_INFO, `02` version, `80` LOAD_ELF, `81` RUN_ELF, `82` RUN_THREAD, `84` STOP_THREAD, `85` CONTINUE_THREAD, `90` GET_LOAD_BASE, `91` MEMSET, `D0` SET_LOG_MODE, `D1` GET_LOG_BUFFER, `E0` SET_STEALTH_MODE, and now `F0` GET_SD_REPORT. Anything else answers `IPC_ENOENT` (-6). `IOS_READ`, `IOS_WRITE` and `IOS_SEEK` are raw PPC access to IOS memory at the seeked offset. The `0xcc` software interrupt (`swi.c:101-248`) adds handlers, copies memory, reads and writes any register (`3` reads any register: the loader uses it for HW_TIMER), calls a function in supervisor mode (`16`), and manages thread rights and the LED.
- **ELF format.** `Elf_Load` copies each `PT_LOAD` and reads a `PT_NOTE` table of type 4: `0x9` entry, `0x7D` priority, `0x7E` stack size, `0x7F` stack top. `stripios` (`stripios_src/main.cpp:196-300`) writes `PHDR`, `NOTE` and `LOAD` for the table, then the original segments; the segment at the strip address gets `filesz` 1. A module's ident words are `0x7F454C46`, `0x01020161`, `0x01000000`.
- **How loaders use it** (USB Loader GX, `source/mload/mload.c`): open `/dev/mload` with retries, write the ELF to the load base (`MEMSET`, seek, write), `RUN_THREAD`, `GET_LOAD_BASE`. Only the old Waninkoko mload also hooked ES ioctlv.
- **Stealth** (`cios-lib/stealth.c`, `swi.c:186-202`): on by default. The ES plugin sets `running_title` (`es-plugin/plugin.c:403`). While a title runs, `IOS_OPEN` of `/dev/mload` answers `IPC_ENOENT` (`main.c:325`); an already open descriptor still works. The FFS plugin's IOP hook (`ffs-plugin/iop.c:111-130`) blocks opens of `fat...` and `$...` while a title runs, for a thread without `TID_RIGHTS_OPEN_FAT` (`tid.c`, at most 8 threads). At cIOS start `running_title` is 0.
- **FAT module** (`fat-module/main.c`): `os_open("fat", 0)` is 0 once it is up. Ioctlv `0xF0` mounts the SD card (an optional `s32` partition in vector 0, default -1), `0xF1` unmounts and shuts the card down. `os_open("fat/d2x/x")` is a path on drive 0 (the SD card); IOS paths are shorter than 64 bytes. The FAT thread runs at priority `0x62`, MLOAD at `0x79`. A mount with no card fails fast (`sdio.c:543`, bounded 10 ms retries).
- **Clock.** HW_TIMER `0x0D800010` ticks about 1898 per ms and wraps in about 37 minutes; compare it as a signed difference (`usb-module/usbstorage.c:121-142`).
- **Limits of cios-lib.** There is no SHA-1 or SHA-256. `Timer_Sleep` has one static queue and timer: only one thread may sleep at a time, so the loader uses its own timer.

## The SD loader as built

When the cIOS starts, MLOAD reads a list from the SD card and starts each module it names in its own thread. It works one step at a time from a 20 ms timer message in MLOAD's own message loop: no extra thread and no extra stack.

**Files**

| Path on the card | What |
| --- | --- |
| `sd:/d2x/mload.txt` | the list |
| `sd:/d2x/mload/<name>` | a module: a stripios-made ELF |

**List format.** One entry per line: `name` or `name crc32=XXXXXXXX` (8 hex digits, checked against the whole file). `#` starts a comment line. LF or CRLF, a UTF-8 BOM, spaces and tabs, and a last line without a newline are all accepted. A name is 1 to 31 characters of `A-Z a-z 0-9 . _ -`, with no leading dot: no `/`, no `..`.

**Limits** (`sdload.h`)

| Limit | Value |
| --- | --- |
| list size read | 2048 bytes; a partial last line is dropped when the list fills the buffer |
| modules | 8; further lines are counted as skipped (`SDL_R_LIMIT`) |
| IOS path | `fat/d2x/mload/<name>`, 46 bytes at most |
| whole run | 5 s (`SDL_DEADLINE_MS`), measured with HW_TIMER and checked before each step and each read chunk |
| program headers | 12 |

**State machine.** `WAIT_FAT` (poll `os_open("fat")`) then `MOUNT` (`0xF0`, partition -1, 3 tries) then `LIST` (one 2048-byte read) then `LOAD` (one module per tick) then `DONE`. The end unmounts (`0xF1`), closes the FAT descriptor and destroys the timer. It stops with `SDL_E_TITLE` if a title is running, at any step. The timer message is the constant `SDL_COOKIE` (`0x53444C44`); IPC message pointers are always mapped addresses, so it cannot collide with one.

**Per module.** Nothing in `exe_mem` is touched until the whole file has passed.

1. Pass 1 reads the file in 2 KB chunks for its size and CRC32, and keeps the first 1 KB.
2. The validator checks: the ident words; `phentsize` 32; 1 to 12 program headers; the NOTE table with entries `0x9`, `0xB`, `0x7D`, `0x7E`, `0x7F` all present; every `PT_LOAD` (a segment with vaddr 0 or memsz 0 is skipped, as `Elf_Load` does) inside `[exe_mem, exe_mem + 512 KB)` without overflow; `filesz <= memsz`; the file holds every segment; no overlap with the module's own other segments or an earlier module's box; entry (bit 0 masked) and stack inside the module's own bounding box; priority `0 < prio <= 0x79`.
3. Pass 2 zeroes the segments, copies them, syncs the cache (`os_sync_after_write`), invalidates the instruction cache through `Swi_CallFunc`, and starts the thread with `Elf_RunThread(entry, arg 0xB, stack, size, prio)`.

A failed module is recorded and skipped; the next one still loads. The box of a module whose thread failed to start stays recorded, so a later module cannot overwrite it.

**Report.** `MLOAD_GET_SD_REPORT` = `0x4D4C44F0` on `/dev/mload`, ioctlv with one output vector of at least 384 bytes. All fields are 32 bits, big endian: `magic 'SDML' (0x53444D4C)`, `version 1`, `state`, `error`, `count`, `loaded`, `skipped`, `elapsed_ms`, then 8 entries of `char name[32]`, `s32 result`, `s32 detail` (the thread id on success, the raw code on failure), `u32 size`. A shorter vector answers `IPC_EINVAL`. `MLOAD_VER` and `MLOAD_SUBVER` are unchanged.

Error codes. Overall (`error`): `SDL_E_NOFAT` -1 (no FAT module in 5 s), `MOUNT` -2 (no card or no FAT volume), `NOLIST` -3, `LISTIO` -4, `TIMEOUT` -5, `TITLE` -6, `TIMER` -7. A missing list is an error code, not a fault: a console without the files just boots as before. Per module (`result`): `NAME` -10, `OPEN` -11, `READ` -12, `ELF` -13, `RANGE` -14, `CRC` -15, `THREAD` -16, `LIMIT` -17, `NOTRUN` -18 (the run ended first).

**Timing and cost.** No card takes about 3 ticks, about 60 ms (host-simulated count; it is the fast path of `sdio.c:543`). No FAT module at all costs the whole 5 s and ends with `NOFAT`. The real cost of a mount and a load on a Wii is **not measured yet**; the check app prints `elapsed_ms`. Every IOS reload repeats all of it and a loader that reloads IOS often pays it each time. The main thread is busy during a step, so `/dev/mload` requests wait (at worst 5 s in total).

**Sizes** (release, `-Werror`): code 8800 of 12288 bytes (3488 free), `.bss` ends at `0x136DB244` (3516 free); `+dbg` code 11516 (772 free), `.bss` ends at `0x136DB6D4` (2348 free). The loader itself is 3344 bytes of code and about 4 KB of buffers. The `+dbg` build has little room left.

## Where an SD module must be linked

Link it high in `exe_mem`, for example at `0x13760000` (the test module does). Other loaders also load ELFs at the load base `0x13700000` (USB Loader GX asks `MLOAD_GET_LOAD_BASE`), and a module placed at the bottom of the area gets overwritten by them. The validator refuses any segment outside `0x13700000` to `0x13780000` and any overlap with an earlier module. The area ends before the EHCI module at `0x13780000`. A module's memory is part of the ELF's program headers, so no new MEM2 mapping is needed (`REVIEW.md:125`).

## FAT rights

A module's thread gets no FAT rights. At cIOS start `running_title` is 0, so the FFS hook lets MLOAD open `fat` (`ffs-plugin/iop.c:111`). A module that needs FAT later must ask for it: `Swi_AddThreadRights(tid, TID_RIGHTS_OPEN_FAT)` (`cios-lib/swi_mload.h:42`). The tid table holds 8 threads.

## Security

- **The list is integrity only.** `crc32=` catches a damaged or swapped file. It does not stop an attacker: anyone who can write the SD card can change the module and the CRC in the list. Say so to users: a module on the card runs as IOS code with full access to the console, including the NAND.
- **A bad module can crash IOS.** The validator and CRC reduce accidents (a module cannot write outside the area or over another module), but a valid ELF can still do anything once its thread runs.
- **Never load from a path a title can name.** The loader runs only at cIOS start (`running_title` 0, abort when it is not) and reads fixed paths. Do not add a command that loads a module from a path a game or a channel supplies.
- **NAND keeps the boot-critical modules**: EHCI, USBS, FAT, DIPP, ES, FFSP and MLOAD. The SD card holds optional, loader-specific or experimental ones.
- **Phase 2 (not built): a signed list.** A SHA-256 of each module in the list, the list signed, the public key and the check in a separate small NAND module (about 10 to 20 KB; cios-lib has no hash), and a switch that makes the cIOS refuse an unsigned list. The unsigned form stays the default until then. BootMii-reborn's `menu/app/sha1.c` and MINI's hardware SHA engine are candidates for code to study (below); whether IOS offers `/dev/sha` here is unverified.

## Compared with xerpi's mload-mod

xerpi/mload-mod (af55770) does the same job with `sd:/mload/mload.txt` (`source/main.c:296-369`, `elf.c:115-228`). It reads `fat/mload/mload.txt`, loads with `Elf_LoadFromSD` in MLOAD's main thread on a 20 ms timer message, retries the mount 5 times (about 100 ms), grants `TID_RIGHTS_OPEN_FAT` to the thread and unmounts afterwards. Our loader uses the same timer idea. Differences, from reading its source:

| xerpi | here |
| --- | --- |
| a 2048-byte buffer without a length check | the list read is bounded and a partial last line dropped |
| the last line without a newline is skipped; no CRLF | both accepted |
| unbounded `strcat` for the path | names checked, path built into a 64-byte buffer |
| `PT_LOAD` addresses not range-checked: writes anywhere in IOS memory | every segment inside the module area, no overlap, before anything is written |
| no per-module result, no way to ask | `MLOAD_GET_SD_REPORT` |
| no overall deadline | 5 s |
| still has the old `IOS_READ` fall-through (`REVIEW.md:138`) | fixed here |
| its `MLOAD_GET_LOG_BUFFER_AND_EMPTY` needs the debug buffer, off by default | the report needs nothing |

The paths differ on purpose: `sd:/d2x/mload.txt` is our namespace. A card prepared for xerpi's build is not read by ours.

## BootMii-reborn: what applies

Read only, from `bootmii-reborn` (2026-11-10). MINI runs on bare Starlet with direct SDHC registers; d2x goes through IOS's `/dev/sdio`, so no driver code is shared, only ideas.

- **License.** The README says GPL-2.0-or-later, but the headers of `diskio.c`, `sha1.c` and `log.c` say GPL-2.0 only; `sdhc.c` is OpenBSD ISC. Copying into this GPL-3.0 project needs the "or later" confirmed (our own files can be relicensed).
- **SD start-up.** Identify at 400 kHz with a 2 ms settle; CMD6 check and switch for high speed; after a failed multi-block transfer send CMD12, drop to 25 MHz 1-bit and retry once (`starlet/sdmmc.c:149-182`). d2x 11.1.0 already has a high-speed fallback (`docs/sd-high-speed.md`).
- **SD speed gap, a follow-up.** BootMii-reborn reads 17.9 MB/s at 50 MHz (`release-notes.md:66-69`); d2x through IOS measured 6.27 MB/s with 64-sector calls on lrwii. The clock is not the difference: look at the transfer size and the DMA path in `cios-lib/sdio.c`. This matters for the loader only a little (modules are small) but a lot for everything else on the card.
- **ELF loading.** BootMii's `loader/main.c:325-428` reads the whole ELF into RAM, checks the header, and falls back to boot2 with an `sd_fail` code. MINI's `powerpc_elf.c` validates while streaming, with no overflow guard and no `filesz <= memsz` check. The design here validates everything before touching `exe_mem`.
- **Integrity.** `menu/app/sha1.c` is a small software SHA-1; MINI also has a hardware SHA engine. Candidates for the phase-2 signed list.
- **Logging.** `[%6u ms] text` lines in a fixed buffer with a `[log full]` marker and a per-boot header (`menu/storage/log.c:90-110,189`): a shape the SD report could grow into.
- **Deadlines everywhere.** 5 s on IPC, HW_TIMER deadlines (`loader/gecko.c:26-36`), a quick no-card path (`diskio.c:25-26`): the same approach as the 5 s budget here.

## The test module and the check app

- `tools/build.sh testmod` builds `build/mloadtest.elf` (source: `tests/wii/mloadsd/testmod`: `start.s` with the module table, `link.ld` at `0x13760000`, `main.c`). It is 332 bytes with `stripios` applied and needs `0x1180` bytes of memory. The thread registers `/dev/mloadtest`, answers ioctl 1 with `0x4D4C5453` ('MLTS') and logs through `svc_write`. It touches no hardware.
- `tests/host/mloadsd/test_sdload.c` loads that file through the validator and the loader when `build/mloadtest.elf` exists (the `MLOADTEST_ELF` variable overrides the path); otherwise it says it skipped.
- `tests/wii/mloadsd` (`make -C tests/wii/mloadsd`) is the PPC check app: it reloads to the cIOS (`--ios=N`), polls `MLOAD_GET_SD_REPORT` until the loader is done (up to 10 s), prints the report, opens `/dev/mloadtest`, calls ioctl 1 and prints everything on the TV; it saves `sd:/mloadsd.txt`. Its last line is `RESULT ok ...` or `RESULT fail ...`. `--expect=none` passes only when no module started and `/dev/mloadtest` does not exist.

## Testing on a Wii (attended)

Needs Monty at the console, a cIOS slot nobody uses, and his OK for the install. Nothing here is unattended: a module that hangs IOS cannot be recovered without a power cycle (and the BootMii menu after it).

1. Build: `tools/build.sh` (release) and `tools/build.sh testmod`; `make -C tests/wii/mloadsd`.
2. With Monty's OK, install this build in a spare slot (not `lrwii`'s 249) with `tests/wii/install_cios.py`, as a queued job pinned to the dev Wii. Read the slot back with `umsbench --slots` or the check app's identity line: the version string must name this build.
3. On the SD card make `sd:/d2x/mload/`, copy `build/mloadtest.elf` there as `mloadtest`, and write `sd:/d2x/mload.txt`:

   ```
   # d2x mload test
   mloadtest crc32=XXXXXXXX
   ```

   Take the CRC from `python -c "import zlib;print('%08X' % zlib.crc32(open('build/mloadtest.elf','rb').read()))"`. Put the files there with `hbc.py put`.
4. Run `hbc.py run tests/wii/mloadsd/mloadsd.dol --ios=N` (N is the spare slot).
5. **A pass** looks like: the identity line names the build; `report v1: state 5, error 0 (none), 1 listed, 1 started, 0 skipped, <ms> ms`; the entry `mloadtest 0 (started), thread <id>, 332 bytes`; `open /dev/mloadtest = <fd>; ioctl 1 = 0x4D4C5453`; last line `RESULT ok 1 started ...`. Note the `elapsed_ms`: it is the cost of the loader on this card, the number this page lacks.
6. **No card** (an attended step: pull the card, boot HBC from another source, or use a console without one): run with `--expect=none`. Expect `error -2 (no card, or no FAT volume on it)`, 0 listed, `elapsed_ms` of about 60, and `RESULT ok none started`.
7. **No list:** remove `mload.txt`: `error -3`, `RESULT ok` with `--expect=none`.
8. **Bad module:** change one byte of `mloadtest` on the card but keep the list's CRC: entry result `-15 (crc32 does not match)`. Truncate the file to 100 bytes (and drop the CRC from the list): `-13 (not a module ELF, or truncated)`. For both, run with `--expect=none`: it must pass, the cIOS must stay up (the check app answers, and HBC returns). Leaving only the CRC out of a good module is not an error: it starts.
9. Restore the card, and the slot as Monty wants it. Save `sd:/mloadsd.txt` from each run as the record, and add the `elapsed_ms` numbers to `docs/bench.md`.
