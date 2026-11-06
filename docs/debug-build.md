# The debug build (`+dbg`)

A d2x build for finding out what IOS was doing: counters for each module, one trace ring for all of them, the previous run's "last words", an ARM-side watchdog that resets the console when the PowerPC stops checking in, and faults you can inject to test the recovery paths on a real Wii. None of it is in a release build. The modules of a build without `-DD2X_DEBUG` are byte-identical to a build without this code: `tools/build.sh` with `SOURCE_DATE_EPOCH` pinned was compared hash for hash.

```sh
D2X_VARIANT=dbg EXTRA_CFLAGS="-DD2X_DEBUG -DSDIO_HIGH_SPEED" tools/build.sh
make -C tests/wii/d2xdiag
python tests/wii/d2xdiag.py -- --ios=250 --scenario=usb-csw-stall,sd-low
```

`+dbg` fits the version string after `-rc1` (`-1-0-rc1+dbg`). Install it only to a spare slot, never 249 of a console someone plays on. A debug build also carries the DIP trace (`DIP_DEBUG_TRACE`, [source/dip-plugin/trace.h](../source/dip-plugin/trace.h)).

## The debug area

Defined in [source/cios-lib/d2xdbg.h](../source/cios-lib/d2xdbg.h), which PowerPC programs include as well.

- **Where it is.** 16 KB at physical `0x137A9000` in MEM2, in the gap between the end of the EHCI module's RAM (`0x137A9000`) and the FAT module's code (`0x137B5000`). No d2x module uses it; USBS ends lower still. IOS's page table does not map it (MEM2 is mapped only as far as the modules reach), so MLOAD maps its 4 pages when it starts.
- **Who writes it.** MLOAD, and only MLOAD. Each module runs as its own IOS process and cannot write there. A module sends an event through MLOAD's SWI (`svc 0xcc`, commands 64 to 67), and the SWI handler does the write with every permission. It runs with IRQs off, so each event is atomic. It flushes the header and the new record at once, so a hang on the very next instruction still leaves them in memory. (Until the fix in `swi_asm.s`, an `svc 0xcc` made while the caller's N flag was set lost its command number to a stray conditional instruction at the head of the vector, so an event sent right after a negative comparison vanished. See REVIEW.md.)
- **Who reads it.** The PowerPC, directly: MLOAD turns MEM2 protection off, so `0xD37A9000` (uncached) shows the area live, even while IOS is stuck. `/dev/d2xdbg` also copies it out.

| Offset | What |
|---|---|
| `0x000` | Header: magic `D2XD`, layout 1, size, IOS starts, build identity (version string and commit), build flags, SD high-speed state, records written |
| `0x040` | Watchdog: timeout, the last check-in seen and when, when it fired, its thread's checks and priority, and (`0x058`) when its reset did not take |
| `0x060` | `wd_kick`: the PowerPC adds one to check in. It is alone in its 32-byte cache line, which the ARM only reads, so an ARM write-back never undoes a check-in |
| `0x080` | 8 fault slots |
| `0x100` | Counters for each module (EHCI 1, USBS 2, SD 3, FAT 4, DIP 5, MLOAD 6): requests, errors, retries, resets, timeouts, faults fired, the longest request (length in ticks, command, argument) and the open request |
| `0x1000` | Trace ring: 256 records of 16 bytes: Hollywood ticks (about 1898 per ms), `module << 24 \| event << 16 \| x`, `a`, `b` |
| `0x2000` | Last words: the first half as the previous IOS run left it |

**Last words.** When the debug MLOAD starts and finds a valid area, it copies it to `0x2000` before it starts afresh. A request still open there (`cur_start` set) shows where IOS stopped. That happens after every IOS reload, and after a reset if MEM2 kept its contents. (HBC's kept log at `0x91800100` survived resets on the bench Wii, so MEM2 does keep them; whether this address survives the IOSes that run in between is checked on the Wii.) A power-off clears it: after holding the power button there are no last words (devwii, 2026-11-05). Any later load of a d2x debug IOS replaces the last words, so read them first.

### Events

| Event | Module | Meaning |
|---|---|---|
| `B` / `E` | all | A request starts (`x` command, `a` sector or offset, `b` length) and ends (`a` result; negative counts an error). DIP: a DI command. FAT: an IPC message (`x` = IPC command << 8 \| ioctl). |
| `R` `Z` `T` `X` | all | Retry, reset (`x` `b` Bulk-Only reset, `p` port reset and remount), timeout, error |
| `u` | EHCI | A Bulk-Only step, the letters of the EHCI event log (`x` = letter << 8 \| endpoint) |
| `H` | SD | High-speed state (`a`: bit 0 on, bit 1 fell back) |
| `D d O o m g r I i` | DIP | The DIP trace (`m` = its `R`, `g` = its `Z`) |
| `S` `A` `W` `N` `G` `F` | MLOAD | Area started, watchdog armed, watchdog fired, its reset did not take, hang requested, fault fired |

## `/dev/d2xdbg`

`IOS_Ioctl` on the device, words in and out:

| Ioctl | In | Does |
|---|---|---|
| `0x00` INFO | | out: physical address, size, layout, magic |
| `0x01` READ | offset | copies `outlen` bytes of the area |
| `0x02` CLEAR | | counters, trace and faults back to zero |
| `0x10` WD_ARM | ms (0 = off, at most 600000) | arms the watchdog |
| `0x11` WD_KICK | | checks in (or write `wd_kick` in memory) |
| `0x20` FAULT | slot, `module << 8 \| point`, count, skip, action | sets a fault (point 0 frees the slot) |
| `0x30` HANG | mode: 1 block, 2 spin, 3 spin at the top priority | hangs MLOAD's main thread; never returns: send it asynchronously |

## The watchdog

An MLOAD thread (priority `0x7F` when IOS allows it, else MLOAD's own, `0x79` on base 56; `wd_prio` says which) wakes every 100 ms on an IOS timer. When it is armed and `wd_kick` has not changed for the timeout, it:

1. writes a `W` record;
2. writes Priiloader's magic word `Abra` (`0x41627261`) to physical `0x017FEFF0`, so Priiloader autoboots whatever its boot state says;
3. clears `HW_RESETS` bit 0, a system reset that needs nothing from IOS;
4. if the console is still running 250 ms later, puts `HW_RESETS` and the magic word back, writes an `N` record and `wd_noreset`, turns itself off and returns. IOS goes on, so the PowerPC can show what happened. (Before 4481038 it spun there for good with interrupts off, which stops every IOS thread and the RESET button.)

It is off after every IOS load and after the reset, and only a test app arms it. So it can't loop, and the magic word is never left behind without a reset. It can't help when the hang stops every thread at or above its priority, or turns interrupts off: nothing in IOS runs then. On a console with BootMii as boot2, the reset may stop at BootMii's menu. See REVIEW.md for what was shown on the bench.

## Fault injection

A fault is a slot: a module and a point, a count, a number of passes to let through first (`skip`), and an action. The module asks at each point (SWI 65, from `Dbg_Point()` in cios-lib); only a fault that fires is traced (`F`).

| Point | Modules | Where |
|---|---|---|
| `k` | EHCI, USBS | the CBW transfer |
| `d` | EHCI, USBS | each data-phase transfer |
| `c` | EHCI, USBS | the CSW transfer |
| `q` | SD, FAT | each SD transfer attempt; each FAT IPC message |
| `i` | DIP | each DI command the plugin handles |

| Action | Effect |
|---|---|
| 1 ERROR | EHCI: `-ENODEV` (the device gone: port reset, remount). USBS: -1. SD: the attempt fails. FAT: `FS_EFATAL`. DIP: `DIP_EIO`, Nintendo's DI module not called |
| 2 STALL | USB: a STALL (EHCI a short transfer, USBS -7004) |
| 3 LOSE, 4 TIMEOUT | USB: `USBSTORAGE_ETIMEDOUT` |
| 5 DELAY | sleeps `arg` ms, then goes on (any point) |
| 6 BLOCK, 7 SPIN | the module's thread stops or spins for good (any point): a controlled IOS hang |
| 8 SDLOW | SD: falls back from high speed, as a failed transfer would |
| 9 BIG | USBS: the whole data phase in one transfer when the buffer is direct, past IOS58's 16 KB limit: the real `/dev/usb/ven` hang |

The host tests (`tests/host/dbg`, `ums/test_ums_dbg`, and the USBS, SD and FAT `_dbg` tests) run each module's hooks against the real debug-area code, built for the PC.

## d2xdiag

[tests/wii/d2xdiag](../tests/wii/d2xdiag/main.c) and its runner [tests/wii/d2xdiag.py](../tests/wii/d2xdiag.py) do the following:

- reload to the cIOS and print its identity;
- print the last words and this run's area;
- arm the watchdog with `--wd=S`, checking in as they work;
- send every workload call asynchronously, with a 15 s limit. A call that does not come back is the result: d2xdiag shows the debug area as it stands, read from MEM2 without asking IOS, then stops checking in and shows what the watchdog does;
- run scenarios (`--scenario=a,b,...`). Each sets a fault, does a short read-only workload, and checks that the fault fired and the module recovered:
  - `usb-csw-stall`, `usb-csw-lost`, `usb-data-stall`, `usb-cmd-timeout`, `usb-delay`;
  - `sd-error`, `sd-low`, `sd-delay`;
  - `di-error`.

The hang scenarios are:
- `hang-block`, `hang-spin` and `hang-top`: MLOAD's main thread;
- `usb-spin`: the USB module's thread;
- `usbs-big`: base 58, the real 32 KB hang.

Each one:
1. arms the watchdog (default 20 s);
2. saves `sd:/d2xdiag-pre.txt`;
3. starts the hang;
4. stops checking in, and shows the area live on the TV until the reset.

The runner refuses them without `--attended`. After the reset, it waits for HBC, runs d2xdiag again and passes when the last words show the watchdog fired.
