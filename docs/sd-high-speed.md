# SD high speed (opt-in)

By default d2x talks to the SD card the way Nintendo's IOS sets it up: 4-bit,
default speed, clock `base / (2 * 1)`. With a 48 MHz base that is 24 MHz. High
speed (the SD "50 MHz class", CMD6 function 1) is built only when asked for.
A build without the flag has none of this code (`tests/host` and a byte
comparison of the modules checked that).

## What it does

Where: `source/cios-lib/sdio.c`. It uses IOS's `/dev/sdio/slot0` driver
(`WRITEHCREG` 1, `READHCREG` 2, `SETCLK` 6, `SENDCMD` 7). The FAT module and the
EHCI module (which serves `/dev/sdio/sdhc` to the DIP plugin) each link this code.

After the card is selected and in 4-bit mode (in `__sd0_initio`):

1. CMD6 check (argument `0x00FFFFF1`) as an ADTC with one 64-byte DMA block.
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
D2XNAME=d2x-v11-rc1-sdhs EXTRA_CFLAGS=-DSDIO_HIGH_SPEED tools/build.sh 11 -rc1-sdhs
```

`D2XNAME` and the version string use only letters, digits and `+ - _ ( ) [ ]`
(no dots), so the opt-in cIOS is a different title name from the release.
`tools/build.sh` builds from clean when `EXTRA_CFLAGS` changes.

## Seeing the state from a test app

Only in builds with the flag, the `/dev/sdio/sdhc` device (`sdhc_server.c`)
answers two extra ioctlv commands (zero input and output vectors):

| Command | Answer |
| --- | --- |
| `0x20` | state of the EHCI module's SD code: bit 0 high speed active, bit 1 fell back this session |
| `0x21` | debug: fall back now (as a transfer error would), answer the state |

A default build answers both with `IPC_EINVAL` (-4): that is how an app tells the builds apart.

## Tests

`tests/host/sdio/test_sdio.c` runs the real `sdio.c` on a fake of the IOS driver
and checks the ioctl sequence for: a high speed card, a card without high
speed, a failing CMD6, a switch the card does not take, a read and a write error
at high speed (fallback, then the retry at default), a re-init (re-applied, and
not re-applied after a fallback), a card IOS has not initialized, and the
debug fallback. Run `tools/build.sh test`, or `make -C tests/host`.

## Attended Wii test plan (nothing here has run on a Wii)

1. Install the opt-in cIOS in a spare slot only with the user at the Wii (never on prod or lrwii).
   Slot 249 base 56 is staged on the dev Wii's plan; keep the release cIOS in another slot.
2. On 2 or 3 cards (an SD 1.0/2.0 card, an SDHC high speed card, a card that is
   known slow), time sector reads and writes (64-sector and 1-sector, a 32 MB file)
   with the release cIOS, then with this one. Use files on the card nobody needs.
3. Read the state ioctl (`0x20`): expect bit 0 on for a high speed card, off for an old card.
4. Forced fallback: send `0x21` during a copy and check the copy finishes with the
   same data (compare a hash) and the state reads 2.
5. Watch for read or write errors, a card that does not mount after a reboot,
   and speed that is no better; any of them is a reason to keep the default build.

## Open questions for the first Wii run

- The CMD6 data command is sent as an ADTC (as asked for); the existing read path sends
  `READ_MULTIPLE_BLOCK` as an AC. If IOS refuses the ADTC, high speed never turns
  on (it fails closed). The state ioctl shows it.
- The CMD6 busy bits (status bits 287:272) are not checked: the switch is
  assumed complete when the status says function 1 is selected.
