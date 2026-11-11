# SD card speed through d2x

Branch `claude/sd-speed` (2026-11-10). Nothing here has run on a Wii yet: the
numbers below are arithmetic from earlier measurements, marked as a model, and
the test plan at the end replaces them with data. Background: [sd-high-speed.md](sd-high-speed.md)
(the CMD6 switch to 50 MHz), [fatfs.md](fatfs.md) (the FAT module), [bench.md](bench.md).

## What was measured before

- `tests/wii/sdbench` reads through `/dev/sdio/sdhc` (the EHCI module,
  `source/cios-lib/sdhc_server.c`, then `sdio_ReadSectors`, `sdio.c`), not through the
  FAT module, in 64-sector calls: 9.97 MiB/s on devwii's card, 6.23 on lrwii's.
- bootmii-reborn reads the same card at 17915 KB/s (17.5 MiB/s) in large transfers.
  The wire limit at 4 bits and 50 MHz is 23.8 MiB/s.
- `python -c` on those: 64 sectors at 9.97 MiB/s is 3.13 ms; at 17.5 MiB/s it is 1.79 ms.
  The 1.35 ms difference per call is a cost that does not depend on the size.

## Causes, ranked

1. **FatFs read the file one cluster at a time** (`ff.c`, `f_read`). It cut every
   `disk_read` at the cluster boundary. A card formatted with 32 KB clusters therefore
   got 64-sector requests, the 3.13 ms case above, whatever the app asked for. Same
   loop in `f_write` (FAT12/16/32). Evidence: the 64-sector sdbench number equals what
   a 32 KB cluster gives; the old `ff.c` made 772 reads and 777 writes for 768 KB on the
   host test disk (1 KB clusters), the new one makes 5. **Fixed in 78f0792.**
2. **A fixed cost per call: about 1.35 ms (model).** Every `sdio_ReadSectors` or
   `WriteSectors` is three IOS calls: select the card (CMD7, `sdio.c:802`), the CMD18 or
   CMD25 ioctlv (811 / 873), deselect (815). libogc2's `wiisd.c:617-652` does one select
   and one command for up to 0xffff sectors, so the IOS accepts big counts. The fix for
   this is bigger requests, not a change in `sdio.c`.
3. **`DISK_MAX_SECTORS` is 255** (`source/fat-module/diskio.c:32`): after the change above
   the largest request is 255 sectors (127.5 KB). The model says that is 14.7 MiB/s, so a
   raise to 512 or 1024 gains another 1 to 2 MiB/s. Not done: no Wii has taken a request
   that large through d2x yet.
4. **The bounce buffer for unaligned buffers** is `BOUNCE_MAX` 16 KB (32 sectors):
   6.97 MiB/s by the model, plus the `memcpy`. The FAT module heap has 320 bytes free
   (`docs/module-size.md`), so the buffer is not enlarged. Apps that hand the FAT module a
   32-byte aligned buffer avoid it.
5. **The SDMA boundary** (host register 0x04 bits 14:12) is an open suspect: if IOS
   programs 4 KB, the controller stops every 4 KB and waits for a restart. Unknown; the new
   register dump in sdbench shows what IOS programmed.

Rejected for now: keeping the card selected between requests. The FAT module and the EHCI
module each link their own `sdio.c` with their own state, so a PPC app or the other module
deselecting would make the next CMD18 time out (about 2.8 s). It could be an opt-in flag
later; it saves about 0.3 to 0.6 ms per call (10 to 19% at 64 sectors). exFAT `f_write`
lookahead is impossible as the code is: `get_fat` on a contiguous exFAT chain depends on the
object size, which grows only after the data is written (`create_chain`, `ff.c:1546`).

## The change (78f0792)

`f_read` (all FAT types) and `f_write` (FAT12/16/32): after finding the next cluster, the
loop follows the chain while the next cluster number is the previous plus one, and passes
the whole run to one `disk_read` / `disk_write`. Fragmented files fall back to one request
per run. `tests/host/fat/test_fat.c` has four new tests (one request for a contiguous file,
fragmented reads, disk full on FAT and exFAT, few write requests on FAT); they fail on the
old `ff.c`. Host results: test_fat 70 passed, test_exfat 23 passed. Size: FAT `exe` +144
bytes, release 1136 and `+dbg` 424 bytes free, `ram` unchanged ([module-size.md](module-size.md)).

Risk: a failed `f_write` can leave an allocated cluster in the chain, the same as before.

## The model and the expected curve

`t(n) = a + b * n` for a call of n sectors, with a = 1.35 ms and b = 28 us per sector
(from the two numbers above; b is 512 bytes at 17.5 MiB/s).

| Sectors per call | 1 | 32 | 64 | 128 | 255 | 512 | 1024 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| MiB/s (model) | 0.35 | 6.97 | 9.97 | 12.7 | 14.7 | 16.0 | 16.7 |

Expected effect of 78f0792: FAT file reads on a 32 KB-cluster card go from the 64-sector
point (9.97) to the 255-sector point (14.7 MiB/s) on devwii, if the model holds; the lrwii
card is slower (6.23 at 64), so expect less there. A card with 4 KB clusters was at 8
sectors per request (2.5 MiB/s by the model) and gains the most.
sdbench's fit replaces a and b with measured values; if the 1-sector time is far from
1.38 ms, or the sweep is not a straight line in n, the model is wrong and the order of
causes 2 to 5 changes.

## sdbench, new parts

`tests/wii/sdbench` (read only unless `--fat-write`):

- the raw size sweep: 1, 2, 4, 8, 16, 32, 64, 128, 255 sectors (`--big` adds 512 and 1024),
  us per call, MiB/s, and the fit `a + b*n` with its prediction at 64, 255 and infinity;
- a dump of the SD host registers (0x04, 0x0C, 0x24, 0x28, 0x2C, 0x30, 0x40, 0x44 at PPC
  0xCD070000), decoded: block size, SDMA boundary, bus width, high speed, DMA mode, clock
  divisor. Read only, and only when HW_AHBPROT (0xCD800064) is 0xffffffff, which it was on
  every IOS tried ([bench.md](bench.md)). It never reads 0x20, the data port;
- `--file=/PATH [--limit=KB]`: read a file through the FAT module (`IOS_Open("fat")`,
  ioctlv 0xF0 mounts, `IOS_Open("fat/PATH")`, `IOS_Read`) in 16, 32, 128 and 512 KB calls
  aligned, and 32 KB at buffer + 16 (the bounce path). All CRCs must match each other;
  the report line `FATFILE bytes=N crc=...` is what `sdbench.py` compares with the PC;
- `--fat-write=MB`: creates `/sdbench/w.tmp`, writes it in 32 KB and 128 KB calls (timing the
  writes and the close apart), reads it back, compares the CRC, deletes it, and unmounts.
  It writes nothing else. Use it only on a card with room, and not on lrwii unless Monty asks.

With `--file` or `--fat-write` the forced-fallback test is skipped (it would leave the card
in the slow mode for the FAT module's own start). `tests/wii/sdbench.py`: `--fat-upload MB`
puts a pseudo-random file at `sd:/sdbench.bin`, passes `--file=/sdbench.bin`, compares the
CRC with the PC's and removes the file; `--pc-file PATH` compares a file already on the card.

## Wii test plan

Always a queued job, pinned (`--wii lrwii` only when Monty asks for that console). Q is
hbc-reborn's `tools/wii-bench/wiibench.py`. The slots on lrwii: 252 (base 56) and 253 (base 58) hold
develop builds; 247 is the rc1 reference and 249 is Monty's: never touch it. Build the
DOL first: `make -C tests/wii/sdbench` from the devkitPro MSYS2 shell.

1. **Baseline on the develop build, no install.** Raw sweep, registers, FAT file reads:

   ```
   python $Q add --wii lrwii --name "sdbench base 252" --cwd <repo> --timeout 900 -- python tests/wii/sdbench.py --fat-upload 16 -- --ios=252
   python $Q add --wii lrwii --name "sdbench base 253" --cwd <repo> --timeout 900 -- python tests/wii/sdbench.py --fat-upload 16 -- --ios=253
   ```

   With Monty at the Wii (a new transfer size could hang IOS; the plan is a power cycle, then
   continue past the BootMii menu), a second pair with `-- --ios=252 --big`.
   Optional, with his OK for writes on the card: add `--fat-write=8` to the 252 job.
   The develop FAT module will show the old behaviour: 64-sector requests with 32 KB clusters.

2. **Install this branch's build into the same slot**, with his OK, as its own job
   (`tests/wii/install_cios.py PACKAGE_DIR INSTALLER_DOL --name d2x-sdspeed`, a package
   from `tools/build.sh` of this branch, base 56 for 252). The version string is the same
   as develop's, so check the commit in the identity line the report prints
   (`cIOS 252: d2x ...`); build with `D2X_VARIANT=sdsp` if you need the build to tell itself
   apart. Keep the develop package so the slot can be restored.

3. **Rerun the same jobs** as in 1 on the same slot and compare the report lines:
   `FAT read aligned, 16 KB ... 512 KB calls` MiB/s and the fit (a, b). The FAT reads from a file
   in a 32 KB-cluster volume should go up toward the 255-sector figure; the raw sweep must not
   change (the change is in the FAT module only), and that is the control. Every CRC must
   match the PC's (`sdbench.py` prints it).

4. **Decide on `DISK_MAX_SECTORS`** from the sweep: if 512 and 1024 sectors read and the
   curve keeps rising, raise it (and the 16 KB bounce stays), then repeat step 3 attended.
   If the `--big` sweep fails or hangs, leave it at 255.

Read the result against the model: a (about 1.35 ms), b (about 28 us), the SDMA boundary
and clock in the register lines (clock divisor code 0 is the full base clock, 48 MHz on the
SD slot; host control bit 2 is high speed). A boundary of 4 KB points at cause 5.
