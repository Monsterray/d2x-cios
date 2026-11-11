# SD high speed (on by default)

Nintendo's IOS sets the SD card up 4-bit, at default speed, clock `base / (2 * 1)`.
With a 48 MHz base that is 24 MHz. d2x switches the card to high speed (the SD
"50 MHz class", CMD6 function 1) after that, and falls back to default speed on
the first failed transfer (below). It is on in every build, the release included,
since 11.1.0-rc2. `-DSDIO_NO_HIGH_SPEED` builds without it (a build with that flag has
none of this code); it needs a `D2X_VARIANT`, like any `EXTRA_CFLAGS`.

The switch is in the Makefiles of `cios-lib`, `fat-module` and `mload-module`:
`ifeq ($(findstring -DSDIO_NO_HIGH_SPEED,$(EXTRA_CFLAGS)),)` adds `-DSDIO_HIGH_SPEED`,
so every translation unit that tests `SDIO_HIGH_SPEED` sees the same answer. A new
module that includes `sdio.h` needs the same block.

## What it does

Where: `source/cios-lib/sdio.c`. It uses IOS's `/dev/sdio/slot0` driver
(`WRITEHCREG` 1, `READHCREG` 2, `SETCLK` 6, `SENDCMD` 7). The FAT module and the
EHCI module (which serves `/dev/sdio/sdhc` to the DIP plugin) each link this code.

After the card is selected and in 4-bit mode (in `__sd0_initio`):

1. CMD6 check (argument `0x00FFFFF1`) with `cmd_type` 1 (BC), as libogc sends it, and one 64-byte DMA block (`isdma` 1, `blk_cnt` 1, `blk_size` 64).
   If group 1 function 1 (high speed) is not supported, stop. An SD 1.0 card,
   a failing CMD6 or an all-zero status all end here, at default speed, with no error.
2. CMD6 switch (`0x80FFFFF1`). If the status does not say function 1 was chosen, stop.
3. Host control (`0x28`) `|= 0x04`, then `SETCLK(0)` (the undivided base clock).

The status parsing and the register arithmetic are pure functions in
`source/cios-lib/sdio_hs.h`, tested on the host.

IOS's reset (`RESETCARD`, a host soft reset) clears host control `0x28`, so the
sequence is part of `__sd0_initio` and runs again after every re-init (also
for a card IOS had already initialized).

## Fallback

If a select or a transfer fails while high speed is on, the code clears
`0x28` bit 2, calls `SETCLK(1)`, marks high speed dead for the rest of the
session (a later re-init does not try again) and goes on with the normal retry
loop. A card left in high speed mode works at the lower clock. A failure at
default speed never touches this state.

The FAT module and the EHCI module keep a state each. A fallback in one
leaves the shared host controller at default speed for both; the other
module's next fallback is then harmless, and its next re-init may switch
back up. Check the state in both modules when testing.

## Build

```
tools/build.sh                                                     # high speed on
D2X_VARIANT=nohs EXTRA_CFLAGS=-DSDIO_NO_HIGH_SPEED tools/build.sh   # off
```

The version string uses only letters, digits and `+ - _ ( ) [ ]` (no dots), so
the `nohs` build is a different title name from the release.
`tools/build.sh` builds from clean when `EXTRA_CFLAGS` changes.

The debug ioctlv commands 0x21 and 0x22 (and the record behind 0x22) are
compiled only with a second flag, `SDIO_HS_DEBUG`, because the USBS/SDHC module's
code region (0x3000 bytes) has no room for them next to the USBS fixes. A debug
build:

```
D2X_VARIANT=sdhsd EXTRA_CFLAGS="-DSDIO_HS_DEBUG" tools/build.sh
```

Use the variant `sdhsd`: `sdhsdbg` or `sdhs-dbg` is too long for the 15-character
version string next to `-rc1`. Command 0x20 (the state) is in every build with high speed. `sdbench` prints "debug ioctls not in this build" for a -101
from 0x21 or 0x22.

## Seeing the state from a test app

Only in builds with high speed, the default (0x21 and 0x22 only with `SDIO_HS_DEBUG` too), the `/dev/sdio/sdhc` device (`sdhc_server.c`)
answers three extra ioctlv commands:

| Command | Vectors | Answer |
| --- | --- | --- |
| `0x20` | none | state of the EHCI module's SD code: bit 0 high speed active, bit 1 fell back this session |
| `0x21` | none | debug: fall back now (as a transfer error would), answer the state. Does nothing when high speed is not on |
| `0x22` | one output vector, at least 32 bytes | return 0; the vector holds `struct sdhs_record` (below), the outcome of the last `__sd0_hs_try()` |

A `nohs` build answers all of them with `IPC_EINVAL` (-101, `ipc.h`): they
fall into the `default:` of the ioctlv switch in `sdhc_server.c`. That is how an
app tells the builds apart. (An earlier version of this page said -4.)

### The 0x22 record

`struct sdhs_record` in `source/cios-lib/sdio_hs.h`, packed, 32 bytes, big-endian:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | `calls`: times `__sd0_hs_try()` ran since the module started |
| 4 | 4 | `step`: where the last call stopped (below) |
| 8 | 4 | `rc`: IOS return code of that step (0 when the step did not fail on a code) |
| 12 | 20 | `status`: bytes 0..19 of the last CMD6 answer. Byte 13 bit 1 = group 1 function 1 (high speed) offered; byte 16 low nibble = group 1 selected (1 = high speed, 0xF = switch failed) |

| `step` | Meaning |
| --- | --- |
| 0 | not called yet |
| 1 | skipped: a fallback marked high speed dead |
| 2 | CMD6 check: IOS returned `rc` < 0 |
| 3 | CMD6 check ok, but the card does not offer high speed |
| 4 | CMD6 switch: IOS returned `rc` < 0 |
| 5 | CMD6 switch ok, but the card did not select high speed |
| 6 | host control read failed |
| 7 | host control write failed |
| 8 | `SETCLK(0)` failed (fell back) |
| 9 | high speed is on |

The record is per module (the EHCI module answers it; the FAT module has its
own copy that this ioctl cannot reach). `__sd0_hs_try()` runs from
`__sd0_initio()` after `sdio_Startup()`, so `sdio_Startup()` returning early
(its module's `__sdio_initialized` already set) means no new attempt: `calls`
stays the same.

## Tests

`tests/host/sdio/test_sdio.c` runs the real `sdio.c` on a fake of the IOS driver
and checks the ioctl sequence for: a high speed card, a card without high
speed, a failing CMD6, a switch the card does not take, a read and a write error
at high speed (fallback, then the retry at default), a re-init (re-applied, and
not re-applied after a fallback), a card IOS has not initialized, and the
debug fallback. Run `tools/build.sh test`, or `make -C tests/host`.

## Attended Wii test plan

The plan was written before the first run. It has since run on two Wiis: see "Second Wii run" below and [bench.md](bench.md).

1. Install the cIOS in a spare slot only with the user at the Wii (never on prod or lrwii).
   Slot 249 base 56 is staged on the dev Wii's plan; keep the release cIOS in another slot.
2. On 2 or 3 cards (an SD 1.0/2.0 card, an SDHC high speed card, a card that is
   known slow), time sector reads and writes (64-sector and 1-sector, a 32 MB file)
   with the release cIOS, then with this one. Use files on the card nobody needs.
   `tests/wii/sdbench` does the read side read only (32 MB in 64-sector calls, 2 MB in 1-sector calls, CRC32, the `0x20` state, the `0x22` record in words, then `0x21` and a second read to compare; the fallback check is skipped when high speed was not on): `python tests/wii/sdbench.py -- --ios=249`.
3. Read the state ioctl (`0x20`): expect bit 0 on for a high speed card, off for an old card.
4. Forced fallback: send `0x21` during a copy and check the copy finishes with the
   same data (compare a hash) and the state reads 2.
5. Watch for read or write errors, a card that does not mount after a reboot,
   and speed that is no better; any of them is a reason to keep the default build.

## First Wii run (2026-11-05)

Dev Wii, slot 249, base 56, the `+sdhs` build, `tests/wii/sdbench`:

- INIT (ioctl 1 on `/dev/sdio/sdhc`) = 0.
- State (`0x20`) = 0: high speed not active, no fallback.
- Reads fine at 6.82 MB/s, the same as the release build: no speed gain.
- `0x21` (forced fallback) returned 0: `__sd0_hs_fallback()` does nothing when
  high speed was never on.
- On the release build `0x20` returned -101 (`IPC_EINVAL`), not -4.

So `__sd0_hs_try()` either did not run on this path or stopped early (CMD6
check failed, group 1 not offered, switch failed or not selected, host control
write failed). That is why `0x22` exists: the next run prints which step.

## What was wrong (2026-11-05)

The first run stopped at step 3 (check unsupported) with rc 0 and an all-zero
status: IOS accepted CMD6 but the data stage never filled our buffer. Cause:
`__sd0_switchfunc()` sent CMD6 with `cmd_type` 4 (ADTC). Reads use 3 (AC), and
libogc's `wiisd.c` (`__sd0_functions`, `SDIO_CMD_SWITCHFUNC`, `cmd_type` 1)
sends its CMD6 as 1 (BC) with the same ioctlv DMA form (isdma 1, blk_cnt 1,
blk_size 64, 32-byte aligned buffer, vectors request / data / response, 2 in 1
out). IOS takes `cmd_type` as given, so ADTC returned rc 0 with no data.
`cmd_type` is now `SDIOCMD_TYPE_BC`; the rest of the request already matched
libogc. The host test's fake driver now delivers the data only for a DMA
request that is not ADTC; the old code fails 13 of 20 tests, the new passes.
Run on two Wiis the same day (next section).

## Second Wii run (2026-11-05)

With CMD6 as BC the card switches: the answer arrives, high speed is on, the
forced fallback works (debug build) and the data is the same before and after.
64-sector reads: devwii 9.97 MB/s against 6.85 at default speed; lrwii 6.23
against 4.87. 0 failed requests ([bench.md](bench.md)). It has been on by
default since 11.1.0-rc2.

## Open questions

- CMD6 as BC works on the Wiis tried (above). If a card's status is all zero,
  `0x22` shows it; AC (3), as the reads use, is the next thing to try.
- The CMD6 busy bits (status bits 287:272) are not checked: the switch is
  assumed complete when the status says function 1 is selected.

## SD start-up on IOS 38 (2026-11-11)

On `lrwii` the card was not usable through a cIOS on base 38 (slots 246 and the released 248; 251 on `devwii` was fine): `INIT` failed. Findings, in the order they were found:

- IOS's own `RESETCARD` leaves the card status at 1 (inserted, not initialized), so `__sd0_initio()` takes the manual path (host reset, power, clock, CMD0, CMD8, ACMD41). libogc's `wiisd.c` takes the same path on the same Wii and succeeds; its code is the same as ours.
- The first CMD8 returned rc 0 with an all-zero response word, so the echo check failed. Repeating CMD8 every 10 ms answered after 3 to 7 tries (variable); delays of 20 to 100 ms after power and clock, and a 187 kHz identification clock, changed nothing. After CMD8 was answered, `APPCMD` failed with an error whose low bits were 2.
- The cause is the cache handling after the command. `__sdio_sendcommand()` flushed the response buffer before the call and invalidated it after. IOS 38's SD driver writes the response through the CPU cache and leaves it dirty; the invalidate threw that write away and the stale zero in memory was read. A retry worked when the line had been evicted (written back) in between. Flushing the response again before the invalidate keeps the driver's write. With that change the first CMD8 is answered at once and `INIT` returns 0 on slot 246 (base 38).
- Host test: `tests/host/sdio/test_sdio.c` models a response that lands dirty in the cache (`response_survives_cache`, and `fresh_card_init`, which takes the manual path); both fail on the old code.

`tests/wii/sdbench` has options for this kind of work: `--wait=MS`, `--inits=N`, `--probe=N` (IOS's own RESETCARD and GETSTATUS from the PC side) and `--libogc` (libogc's start-up first).
