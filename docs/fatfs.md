# FatFs R0.16 and exFAT in the FAT module

The FAT module (`source/fat-module`, the `/dev/fat` device that `FAT.app` provides for SD cards, USB drives and emulated NAND) used FatFs R0.08b (2010). It now uses **FatFs R0.16** with exFAT on, so an exFAT drive mounts, lists, reads and writes like a FAT32 one. FAT12/16/32 are read and written as before.

## Why R0.16

exFAT came with FatFs R0.12 (2016). R0.08b has no exFAT code and no long-name or 64-bit file size support to build on, so the port is the whole library, not a patch. The same change brings the long-name table of R0.16 (`ffunicode.c`, code page 437), 64-bit offsets, and the newer partition and volume-detection rules.

## The d2x changes, re-applied

`ff.c`, `ff.h`, `ffunicode.c` and `diskio.h` are R0.16 as released, plus three changes, each marked `d2x` in `ff.c` and listed at its top:

- `check_fs()`: a sector-0 signature of `0xAB55` (UStealth drives) is taken as `0xAA55`.
- `find_volume()`: a FAT VBR in sector 0 is the volume whatever partition is forced (a superfloppy drive with a forced partition index still mounts), as in R0.08b.
- `ff.h`: `FATFS.win` is the first member, so it is 32-byte aligned for the disk driver's DMA (a compile-time check in `fat_wrapper.c` tests the offset).

Other changes are in the module's own files: `fat_wrapper.c` and `diskio.c` use the R0.16 API (`LBA_t`, `UINT` counts, `FILINFO` with long names, one shared `FILINFO` for the directory walks, disk reads in chunks of 255 sectors); `get_fattime()` returns 1980-01-01 00:00 (`0x00210000`) instead of 0 (0 is not a valid date in exFAT); `ffconf.h` is a new file (code page 437, long names with the working buffer on the heap (`FF_USE_LFN 3`, `Mem_Alloc`), `FF_FS_TINY`, `FF_FS_EXFAT 1`, two volumes, multi-partition, no `f_mkfs`, no `f_expand`).

## divmod64.c

exFAT sizes and offsets are 64 bits, and `ff.c` divides them by the sector and cluster size. GCC 4.5 on ARM does that through libgcc's `__aeabi_uldivmod`, which calls `__gnu_uldivmod_helper`, which lives in `bpabi.o`. That object needs `__divdi3`, and with it the exception unwinder: 6.2 KB of code in a module with 20 KB. `source/fat-module/divmod64.c` defines `__gnu_uldivmod_helper` (shift and subtract; a fast path when both numbers fit 32 bits), so `bpabi.o` is never linked. Division by zero gives all ones as the quotient and the dividend as the remainder, as the ARM divide instruction would; FatFs never divides by zero. Nothing in the module divides signed 64-bit numbers; if something starts to, the link fails on `__gnu_ldivmod_helper` and that one needs adding. `tests/host/fat/test_divmod64.c` compares it with the PC's own `/` and `%` on edge, power-of-two and 200000 random values.

## Sizes

Release build, measured from the ELF (`.init` + `.text` from the start of `exe`) and the RAM region:

| | R0.08b (develop dbf8c15) | R0.16, exFAT on |
| --- | --- | --- |
| exe region | 0x5000 (20480) | 0x6000 (24576) |
| exe used, release | 18032 | 23560 (1016 free) |
| exe used, `+dbg` | n/a | 24192 (384 free) |
| ram region | 0x23000 | 0x22000 (origin 0x137bb000): 138880 of 139264 used (384 free) |
| `FAT.app` | 20324 | 25608 release, 26240 `+dbg` |

exFAT itself adds 5028 bytes of code (18532 to 23560 between the two exFAT commits). `link.ld` moves 0x1000 from `ram` to `exe`: `exe` and `ram` still fill `0x137b5000` to `0x137dd000`, the module ends where it ended, and the heap is not shrunk. The space from `0x137dd000` to `0x13800000` is not used: whether IOS maps memory beyond the last module's end is unverified (REVIEW.md, the MEM2 mapping finding, line 125: IOS maps MEM2 only as far as its modules reach), so growing past `0x137dd000` stays a decision for a Wii run. The next FAT change that adds code has to find the bytes elsewhere.

## Limits and behaviour differences

- **Drives over 2 TB** are not supported: sector numbers are 32 bits in the disk layer (`FF_MIN_GPT` leaves GPT off).
- **`FAT_Seek`** refuses a position of 2 GB or more (`0x7FFFFFFF` is the most it can return as a positive `s32`); the file position stays. Sequential reads and writes carry on past 2 GB.
- **Sizes and positions saturate at `0xFFFFFFFF`** in the IPC structures (`stats`, `fstats`) for exFAT files of 4 GB or more. `GETUSAGE` adds up the real sizes in 64 bits.
- **Names outside code page 437.** On exFAT a name with characters outside CP437 lists as `?` for the whole name (exFAT has no short name to fall back on); on FAT it lists as the short name (`NAME~1.TXT`), as before.
- **A FAT boot sector with a wrong `0x55AA` signature still mounts** (the early-DOS rule of FatFs R0.16; exFAT and MBR need the signature). `0xAB55` is also accepted.
- **`DELETEDIR`** empties a directory and keeps it. The R0.08b module did the same (`__FAT_DeleteDir` deletes the entries, never the directory itself); the host tests check it.
- **FAT12/16 are chosen by volume size**, as FatFs does, not by the cluster count a formatter wrote; the module never formats.
- **Time stamps** of files created or changed are 1980-01-01 00:00 (the module has no clock).

## A second big file after deleting a first (investigated 2026-11-10)

On the 6 GB sparse RAM disk of `tests/host/fat/test_exfat.c`, after deleting a 5 GB exFAT file, `f_expand` of a second 5 GB file returned `FR_DENIED`, and 1 GB worked. Result:

- **The free space is right.** `f_getfree` gives the same number (6441500672 bytes) with the cached count and with the count dropped and rescanned from the bitmap, before the first file and after the delete, and again after a second file is written and deleted.
- **It is FatFs's `f_expand` search, not the harness and not the free count.** `find_bitmap()` starts at the end of the last allocation (`fs->last_clst`) and scans around the volume once. A free run that contains its starting cluster is cut in two: the part before the start is seen last, and the part after the start is seen first, and they never join. Deleting a file that was allocated by `f_expand` leaves `last_clst` at the file's last cluster, so the free block that used to be the file is exactly one cluster short for a file of the same size. Fresh volumes: 3 GB, 5 GB and 6 GB minus 300 MB all expand; a second 3 GB after deleting a first 3 GB is denied, and so is a second 5 GB; 1 GB and 2 GB after the 5 GB delete work because they fit in one of the two pieces.
- **The module does not call `f_expand`** (`FF_USE_EXPAND 0`; only the host tests turn it on to make a big file without writing it). FAT_Write allocates cluster by cluster. The test writes a second 5 GB file that way after the delete (`f_lseek` past the end of a file opened for writing allocates the chain the same way): it fits, the free count drops by that much and comes back after the second delete.

So free space is reported and reused correctly after deleting a big exFAT file. An upstream limit of the contiguous search is the only thing seen, and nothing in the module reaches it.

## Wii test plan

None of this has run on a Wii. The host tests (`tools/build.sh test`) run the module's code on RAM disks; they do not run the disk driver on a real drive or IOS's mapping of the new `exe`/`ram` split. On the dev Wii, with Monty at the console (a hang inside IOS cannot be recovered from a queue):

1. **IOS accepts the new split.** Install the cIOS with the new `FAT.app` in a spare slot and boot it (the `exe` region is 0x6000, `ram` starts at 0x137bb000, the module still ends at 0x137dd000). It must reach the loader as before; the first `/dev/fat` call proves FAT is up.
2. **An exFAT USB drive**, formatted by Windows (a drive of 64 GB or more, so Windows offers exFAT), with an emuNAND folder and a Nintendont game on it:
   - mount partition indexes 0, 1, 2 and 3 with the drive laid out so that the exFAT partition is each of them in turn, and a drive with no partition table (superfloppy);
   - emuNAND: start a title that creates a save, read it back, delete it; list the NAND folder (long names);
   - Nintendont: boot a game from the exFAT drive;
   - list a folder with long, accented and non-CP437 names; the non-CP437 names show as `?`.
3. **FAT32 drives are unchanged**: the same checks on a FAT32 USB drive and on the SD card, emuNAND included.
4. A file over 4 GB on the exFAT drive: it lists, reads at the start and at the end, and the loader reports a size saturated at 4 GB (not a crash).
5. Nothing here is run unattended; a stuck drive or a hung IOS needs a power cycle.

## Risks

- 1016 bytes free in `exe`, 384 in `ram` (and 384 in `exe` for `+dbg`): any later FAT change has to be size-neutral.
- The whole library is new in a cIOS module that has only run on the host: exFAT code paths that the host RAM disks do not reach (timeouts and errors from a real drive, a drive that is unplugged mid-write) are as untested as R0.08b's were on those paths, but the code is different.
- The `0x137dd000..0x13800000` gap stays unused until IOS's mapping is verified.
