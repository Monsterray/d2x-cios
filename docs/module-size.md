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

The plain release build (no SD high speed) has 1436 free in USBS and 4392 in EHCI.

| Change | Bytes (USBS) | Why it does not change speed |
| --- | --- | --- |
| `cios-lib/syscalls.s`, `tools.s`: one section per stub (`.text.NAME`), so `--gc-sections` drops the stubs a module never calls (the whole file, 55 stubs of 8 bytes, was one section). All modules link it. | -396 (MLOAD -316, FAT -360, EHCI -400, DIPP -404, ES -392, FFSP -404) | The same instructions, only unused stubs go; no call path changes. |
| `libwbfs.c`: `wbfs_open_partition` divides through `wbfs_udiv`/`wbfs_sdiv` (`wbfs_div.c`, shift and subtract) instead of libgcc's `__udivsi3`/`__divsi3` (620 bytes) that nothing else in USBS needs. One shift replaces a division by the constant 0x8000. EHCI defines both with `/`, as it links the libgcc routines for `qtd_fill` and `USBStorage_Read/Write`. | -560 (EHCI +8 to +20) | Runs once per disc open (WBFS ioctl), never in a transfer. |
| USBS `usbstorage.c`: `__cycle` shares one `__data_phase` for the read and write chunk loops, and one recovery label for the four reset-and-retry exits. | -112 | Same calls in the same order per 16 KB piece; the per-piece code is the one loop, a flag picks the endpoint and the copy direction. |

Host tests added for the refactor (`tests/host/usbs`): direct-buffer write recovering from a STALL, and short data transfers (the module's residue paths) for a read and a write, bounce and direct. They pass on the old and the new `__cycle`. The tests already covered 16 KB chunking and bounce/direct both ways.

Not done: `-Os` is already on everywhere; Thumb is already on everywhere (only the syscall stubs, `start.s` and `.init` are ARM); there are no debug strings in release (`.rodata` is 196 bytes in the data region). The WBFS ioctls are live (USB Loader GX), so wbfs.c stays. Next candidates if room is needed again: `sdio_Startup` (952 bytes, cold), `wbfs_open_partition`'s reset-only branch (about 150 bytes, needs a compile-time switch in the shared lib).
