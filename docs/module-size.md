# Module size headroom

Sizes of the cIOS modules against their fixed regions. See also [CONTRIBUTING.md](../CONTRIBUTING.md), Conventions.

## Module size headroom (opt/size, 2026-11-05)

Every module is Thumb, `-Os`, `-ffunction-sections`, `--gc-sections` already, so there was no compiler flag left to gain from; the gains came from code that was linked and never needed. The `exe` regions in each `link.ld` are fixed (the cIOS maps depend on them). Bytes = `.init` + `.text` from the region start, `+next` build (`SDIO_HIGH_SPEED`):

| Module | Region | Used before | Free before | Used after | Free after |
| --- | --- | --- | --- | --- | --- |
| MLOAD | 0x2000 (8192) | 5716 | 2476 | 5400 | 2792 |
| FAT | 0x5000 (20480) | 18400 | 2080 | 18040 | 2440 |
| EHCI | 0x5000 (20480) | 16840 | 3640 | 16440 | 4040 |
| USBS | 0x3000 (12288) | 12272 | 16 | 11204 | 1084 |
| DIPP | 0x2000 (8192) | 6852 | 1340 | 6448 | 1744 |
| ES | 0x2000 (8192) | 3264 | 4928 | 2872 | 5320 |
| FFSP | 0x3000 (12288) | 7104 | 5184 | 6700 | 5588 |

SD high speed is now in every build, so the table's "after" column is the release build. (Before 11.1.0-rc2 the plain release had none: 1436 free in USBS and 4392 in EHCI.)

The release build of `develop` (11.1.0-rc2, with the `/dev/mload` fix), the end of the loaded segment in each `exe` region read from the ELFs:

| Module | Region | Used | Free |
| --- | --- | --- | --- |
| MLOAD | 8192 | 5456 | 2736 |
| FAT | 20480 | 18032 | 2448 |
| EHCI | 20480 | 16432 | 4048 |
| USBS | 12288 | 11196 | 1092 |
| DIPP | 8192 | 6448 | 1744 |
| ES | 8192 | 2872 | 5320 |
| FFSP | 12288 | 6700 | 5588 |

FAT with FatFs R0.16 and exFAT (branch `claude/fat-exfat`, 2026-11-10, [fatfs.md](fatfs.md)): the `exe` region grew from 0x5000 to 0x6000 and `ram` gave up the 0x1000 (the module still ends at 0x137dd000). Release 23104 of 24576 used, 1472 free (was 18032 of 20480, 2448 free); +dbg 23816 used, 760 free; `ram` 138848 of 139264, 416 free (+dbg 160 free). exFAT is about 4.6 KB of that (static name buffers instead of heap ones saved 456 bytes of code); `divmod64.c` keeps libgcc's 6.2 KB `bpabi.o` out. `FAT.app` is 25152 bytes release (was 20324).
Two USB ports ([usb-ports.md](usb-ports.md)), release / `+dbg` bytes used (free): EHCI 19120 (1360) / 20424 (56); USBS 12108 (180) / 12252 (36); DIPP 6716 (1476) / 7760 (432). The review fixes (the second socket back to OHCI, the fd table that refuses a failed SET_PORT, the UMS_INIT sender count) cost EHCI +280 release / +272 `+dbg` and USBS +156 release.

All of it together (develop 2026-11-10: SD module loader, exFAT, two USB ports), measured from the program headers: FAT `exe` 23296 of 24576 (1280 free) release, 24008 (568 free) `+dbg`; FAT `ram` 138944 of 139264 (320 free) release, 139200 (64 free) `+dbg`; MLOAD `exe` 9376 of 12288 (2912 free) release, 12088 (200 free) `+dbg`. The `+dbg` FAT `ram`, `+dbg` EHCI `exe` and `+dbg` USBS `exe` (56, 64 and 36 bytes) are the tightest: any growth there needs a region change first.

FAT with cluster-run coalescing in `f_read` and `f_write` (branch `claude/sd-speed`, [sd-speed.md](sd-speed.md)): `exe` 23440 of 24576 (1136 free) release, 24152 (424 free) `+dbg`, so +144 bytes over the line above in both; `ram` unchanged, 138944 (320 free) release, 139200 (64 free) `+dbg`. Merged onto develop with the SD response flush (2026-11-11): FAT `exe` 23448 (1128 free) release, 24160 (416 free) `+dbg`.

USBS with bounded transfers (`usb2.c`, branch `feat/usbs-async`, 2026-11-08): release 11548 used, 740 free (was 11196, 1092 free); +dbg 11932 used, 356 free.

| Change | Bytes (USBS) | Why it does not change speed |
| --- | --- | --- |
| `cios-lib/syscalls.s`, `tools.s`: one section per stub (`.text.NAME`), so `--gc-sections` drops the stubs a module never calls (the whole file, 55 stubs of 8 bytes, was one section). All modules link it. | -396 (MLOAD -316, FAT -360, EHCI -400, DIPP -404, ES -392, FFSP -404) | The same instructions, only unused stubs go; no call path changes. |
| `libwbfs.c`: `wbfs_open_partition` divides through `wbfs_udiv`/`wbfs_sdiv` (`wbfs_div.c`, shift and subtract) instead of libgcc's `__udivsi3`/`__divsi3` (620 bytes) that nothing else in USBS needs. One shift replaces a division by the constant 0x8000. EHCI defines both with `/`, as it links the libgcc routines for `qtd_fill` and `USBStorage_Read/Write`. | -560 (EHCI +8 to +20) | Runs once per disc open (WBFS ioctl), never in a transfer. |
| USBS `usbstorage.c`: `__cycle` shares one `__data_phase` for the read and write chunk loops, and one recovery label for the four reset-and-retry exits. | -112 | Same calls in the same order per 16 KB piece; the per-piece code is the one loop, a flag picks the endpoint and the copy direction. |

Host tests added for the refactor (`tests/host/usbs`): direct-buffer write recovering from a STALL, and short data transfers (the module's residue paths) for a read and a write, bounce and direct. They pass on the old and the new `__cycle`. The tests already covered 16 KB chunking and bounce/direct both ways.

Not done: `-Os` is already on everywhere; Thumb is already on everywhere (only the syscall stubs, `start.s` and `.init` are ARM); there are no debug strings in release (`.rodata` is 196 bytes in the data region). The WBFS ioctls are live (USB Loader GX), so wbfs.c stays. Next candidates if room is needed again: `sdio_Startup` (952 bytes, cold), `wbfs_open_partition`'s reset-only branch (about 150 bytes, needs a compile-time switch in the shared lib).

## EHCI with the USB recovery (feat/usb-recovery, 2026-11-08)

The dead-drive state and the PORTSC-based wait (REVIEW.md, EHCI item 11), end of the loaded segment in the `exe` region (0x5000 = 20480), read from the ELFs:

| Build | Used | Free |
| --- | --- | --- |
| Release | 16968 | 3512 |
| `+dbg` | 18024 | 2456 |

The release build was 16432 (4048 free) before: +536 bytes.

## EHCI with the recovery ladder, the doorbell wait and the start-up reset (feat/usb-recovery, 2026-11-08)

Same measure (`PT_LOAD` file size at 0x13780000 in `ehci-module.elf`):

| Build | Used | Free |
| --- | --- | --- |
| Release | 17864 | 2616 |
| `+dbg` | 19032 | 1448 |

Against the entry above: +896 bytes release, +1008 `+dbg`.

## MLOAD with the SD module loader (claude/mload-sd, 2026-11-10)

`sdload.c` (list parser, CRC32, ELF validator, the state machine) and its hook in `main.c` did not fit the 0x2000 `exe` region (the link stopped with ``section `.text' will not fit in region `exe'``). As xerpi's mload-mod did, `exe` is now 0x3000 and `ram` starts at 0x136d3000 (same length 0x9000, ending at 0x136dc000, inside the unused gap before `exe_mem` at 0x13700000). `_start` and the IRQ9 vector stay at 0x136d0000 and 0x136d0010, so `ciosmaps.xml` is unchanged.

| Build | `exe` used (`.init` + `.text`) | Free of 12288 | `ram` end (`.bss`) | Free of 0x9000 |
| --- | --- | --- | --- | --- |
| Release, before (`develop`) | 5456 | 2736 of 8192 | about 0x136d9184 | about 7800 |
| Release | 9344 | 2944 | 0x136db284 | 3452 |
| `+dbg` | 12056 | 232 | 0x136db6f4 | 2316 |

The `+dbg` build fits, with 232 bytes to spare. If it must grow, the smallest region change is `exe` 0x3000 to 0x3400 and `ram` at 0x136d3400, length 0x8c00 (the IRQ9 vector stays at 0x136d0010). Each base needs a boot test before a release (`docs/mload.md`).

The loader (with the flag check, the ioctls and the one-shot timer steps) adds about 3900 bytes of code to the release build, 4 KB of buffers to `.bss` (2 KB read buffer, 1 KB module head, the report and the list entries) and 44 bytes of strings.

## The mload timer fix and the SD response flush (claude/mload-fixes, 2026-11-11)

Measured as above (program headers, `exe` in use), against `develop` 8b774f2 (the figures in the paragraphs above): the `sdio.c` fix (one more `os_sync_after_write` call per command, [sd-high-speed.md](sd-high-speed.md)) costs 8 bytes in each module that links `sdio.c`; the `sdload.c` timer fix (stop before restart) did not change MLOAD's size beyond that.

| Module | Region | Release used (free) | `+dbg` used (free) |
| --- | --- | --- | --- |
| MLOAD | 12288 | 9384 (2904) | 12096 (192) |
| FAT | 24576 | 23304 (1272) | 24016 (560) |
| EHCI | 20480 | 19128 (1352) | 20432 (48) |
| USBS | 12288 | 12116 (172) | 12260 (28) |
| DIPP | 8192 | 6716 (1476) | 7760 (432) |
| ES | 8192 | 2872 (5320) | 2872 (5320) |
| FFSP | 12288 | 6716 (5572) | 6716 (5572) |

Before: MLOAD 9376 / 12088, FAT 23296 / 24008, EHCI 19120 / 20424, USBS 12108 (180) / 12252 (36). DIPP, ES and FFSP do not link `sdio.c`. The `ram` regions are unchanged except MLOAD `+dbg` and FAT, which did not move either (FAT 138944 / 139200 of 139264). USBS `+dbg` (28 free) and EHCI `+dbg` (48 free) are now the tightest; the next byte there needs a region change.
