# USB storage in the EHCI module: how a request fails

The EHCI module (`source/ehci-module`) serves USB mass storage to the rest
of the cIOS (the DIP plugin, the FAT module) and to loaders such as USB
Loader GX, through `/dev/usb2`. This page is for anyone changing how it
retries, recovers or gives up. Read it before touching `tinyehci/ehci.c`
or `tinyehci/usbstorage.c`.

## The layers

| Layer | Where | Job |
| --- | --- | --- |
| ioctl | `loop.c`: `USB_IOCTL_UMS_READ_SECTORS`, `_WRITE_SECTORS` | Returns 0 on success, 1 on failure, to the caller |
| Sector request | `usbstorage.c`: `__USBStorage_Sectors()` | Splits nothing; owns the attempts, the deadline and the wait for an unplugged drive |
| Command | `usbstorage.c`: `__USBStorage_Read/Write()`, `__cycle()` | One SCSI command over Bulk-Only: CBW, data, CSW; STALL handling and reset recovery |
| Transfer | `ehci.c`: `ehci_do_urb()`, `handshake()` | One bulk or control transfer on the EHCI controller |

`USBStorage_Read()` and `USBStorage_Write()` split long requests into
32 KB commands (`ehci_bulk_message()` takes a `u16` length, so a single
transfer must stay below 64 KB).

## Error codes, bottom up

| Code | From | Meaning | What happens next |
| --- | --- | --- | --- |
| `-ENODEV` (-1) | `handshake()` | Port disconnected, port owner lost, fatal controller error, or a timeout outside set-up code | `unplug_device = 1`; the request waits for the drive if PORTSC shows none (rule 1 below), else it keeps its deadline (rule 2) |
| `-ETIMEDOUT` (-2) | `handshake()` in `handshake_mode` 1 | Timeout during set-up or recovery | The command's reset recovery |
| `-EPIPE` (-5) | `ehci_do_urb()` | A control request STALLed | The caller's error path |
| short count | `ehci_do_urb()` | A bulk transfer STALLed (or the device sent a short packet) | `__cycle()` clears the halt and reads the CSW (BOT 6.7.2, 6.7.3) |
| `USBSTORAGE_ESTATUS` (-10006) | `__USBStorage_Read/Write()` | CSW status 1 or 2: the command failed | REQUEST SENSE, then another attempt |
| `USBSTORAGE_ESHORTREAD` (-10003) | `__read_csw()` | The CSW read STALLed | Clear the halt, read the CSW once more |

A STALL is never an unplug. Before 2026-10-24 every STALL set
`unplug_device` and remounted the drive, so the Bulk-Only recovery code
never ran on hardware (REVIEW.md, EHCI item 7).

## When a request gives up

`__USBStorage_Sectors()` applies these rules, in order:

0. **The drive is dead** (see below): fail at once, no USB traffic.
1. **The port shows no drive** (PORTSC CCS 0, or PORT_OWNER set: the drive is
   unplugged): wait for it without limit, polling PORTSC every 100 ms and
   resetting nothing, so a game survives a replug. Once it is back, the
   request starts afresh: new tries, new deadline. Only PORTSC decides this.
   A drive that is present but cannot be mounted (`unplug_procedure()`
   fails) is not "gone": it falls to rule 2.
2. **The drive is present but the request keeps failing:** after
   `USBSTORAGE_SECTOR_TRIES` attempts, or once `USBSTORAGE_REQUEST_MS` has
   passed, return failure. `__cycle()` checks the same deadline and starts
   no new reset recovery after it. This includes remounts that fail.
3. Otherwise, try again.

### The dead state

Before 2026-11, a present drive that would not mount hung the loader: a
failed `USBStorage_Try_Device()` runs `USBStorage_Close()`, which wipes
`__usbfd` and sets `__mounted = 0`. After the first failed remount
`unplug_procedure()` only slept 100 ms and returned 1 for ever (rule 1 waited
for a drive that was there), and GET_CAPACITY returned 0. Also
`ehci_reset_port_old()` wrote PORT_OWNER on every 100 ms poll with CCS 0.
Now `unplug_procedure()` saves and restores `__usbfd`, `__lun` and
`__mounted` when a remount fails, and:

- After `UMS_DEAD_FAILS` failed remounts with the port connected, and
  `UMS_DEAD_MIN_MS` since the start of the request in which the first one
  failed, the drive is **dead**: sector requests return failure at once
  (rule 0) with no USB traffic. Capacity stays valid (USB Loader GX ignores
  the UMS_INIT result and only tests GET_CAPACITY != 0).
- Dead ends on a replug (PORTSC CCS 0, or CSC set after it died), on
  `UMS_INIT` (with the drive still unmountable it is dead again and the call
  returns -ENODEV). Nothing retries a dead drive on a timer: a remount can
  take seconds, and on the module's single thread it would stall USB and SD
  at random moments, a hitch nobody could explain.
- The keepalive (`__EHCI_Watchdog`, `loop.c`) reads a sector only when
  `USBStorage_Keepalive_Ready()` says so: not while the drive is dead, and
  not while PORTSC shows it unplugged, because the wait of rule 1 would hold
  the module's single thread and the SD card with it.

How callers see it: USB Loader GX (`usbstorage2.c`), d2x FAT
(`fat-module/diskio.c`, `cios-lib/usbstorage.c`) and the DIP/WBFS reads
(`wbfs.c`, `loop.c`) return the error at once, with no tight retry loop.
Not verified: libfat's cache and a game's own DI retries after such errors.
Not tested in the host harness: SD requests after the drive died (no SD
card in it); the keepalive tests stand for that.

The limits:

| Limit | Value | Where |
| --- | --- | --- |
| One transfer (`usb_timeout`, `ums_io_timeout`) | 1000\*1000, about 1.05 s | `ehci.c`, `usbstorage.c` |
| Reset recoveries per command (`USBSTORAGE_CYCLE_RETRIES`) | 10 | `usbstorage.c` |
| Attempts per sector request (`USBSTORAGE_SECTOR_TRIES`) | 4 | `usbstorage.c` |
| Time per sector request (`USBSTORAGE_REQUEST_MS`) | 8000 ms | `usbstorage.c` |
| Failed remounts before dead (`UMS_DEAD_FAILS`) | 2 | `usbstorage.c` |
| Time since the first failed request before dead (`UMS_DEAD_MIN_MS`) | 15000 ms | `usbstorage.c` |
| Deadline left for a power cycle (`UMS_POWER_CYCLE_MIN_MS`) | 2500 ms | `usbstorage.c` |
| Async advance doorbell, then schedule halt (`EHCI_IAA_US`, `EHCI_ASS_US`) | 20 ms each | `ehci_recover.h` |
| Port power: off, wait for the drive, settle (`EHCI_POWER_*`) | 300 ms, 1000 ms, 100 ms | `ehci_recover.h` |

A drive that takes a write's data but never sends its status (the bench's
SanDisk "USB 3.2Gen1") now fails in about 8.5 s, with no remount. Before
the deadline it took about 48 s: 43 reset recoveries and 6 remounts for one
sector (`tests/host/ums`, `write_with_lost_status_fails_in_time`).

## The recovery ladder

A command that fails is recovered in steps, each one only between two
transfers (the module's loop is single-threaded, so nothing else runs while
a rung does), all inside the one request deadline (`USBSTORAGE_REQUEST_MS`):

| Rung | What | Where |
| --- | --- | --- |
| 1 | clear-halt on the stalled endpoint, read the CSW | `__cycle()` |
| 2 | Bulk-Only Mass Storage Reset, then clear-halt on both endpoints (`USBStorage_Reset`) | `__cycle()`, up to `USBSTORAGE_CYCLE_RETRIES` times |
| 3 | port reset, enumerate again, mount again (a transfer timeout is an unplug here) | `unplug_procedure()` |
| 4 | port power cycle, enumerate, mount; only with HCSPARAMS.PPC | `unplug_procedure()` |
| fail | the request returns failure at its deadline | `__USBStorage_Sectors()` rule 2 |

Rung 4 comes when a command fails again after rung 3 in the same request, or
when the mount after rung 3 fails. At most one power cycle per request, and only
with `UMS_POWER_CYCLE_MIN_MS` (2500 ms) of the deadline left. The power cycle
itself takes about 0.6 s (off 300 ms, wait for the drive to reconnect, settle
100 ms), then the enumeration and the mount.

So that rung 3's mount cannot use up the time rung 4 needs, rung 3's mount gets
an earlier deadline of its own when a power cycle could follow
(`ums_rung3_budget_ms()`: what is left, minus 2.5 s and a 1.2 s transfer timeout,
at least 1.5 s). A mount that runs after a deadline sends nothing more
(`ums_in_remount` in `__cycle()`), so a request without PPC that fails ends about
1 s after its deadline, not after one more timeout per mount command (12.7 s in
the model before, under 10 s now). The keepalive's dead-drive retry and `UMS_INIT`
run without a deadline: rung 3 first, rung 4 if the mount fails.

Without PPC the ladder ends at rung 3. Whether Hollywood's EHCI has PPC is not
known yet: `+dbg` builds log it at start (below).

PORTSC's CSC is write-1-to-clear. The ladder writes PORTSC only through masks
that leave CSC, PEC and OCC out. The one exception is the CSC that the power
cycle itself causes: `ehci_port_power_cycle()` clears it on purpose, so that the
dead state does not take it for a replug.

## Releasing the QH after a failed transfer

`ehci_do_urb()` links one QH into the async list, waits for STS_INT, and unlinks
it. On success it then waits for the controller's async-list address to be back
at the list head (the controller has let go). On a failure (timeout, port gone)
it used to free the qTDs without waiting, so the controller could still hold a QH
that was about to be reused. Now `ehci_async_quiesce()`
(`tinyehci/ehci_recover.h`) rings the async advance doorbell (USBCMD IAAD) and
waits for STS_IAA (write-1-to-clear, cleared after) for at most 20 ms. If the
doorbell does not come it clears CMD_ASE and waits for STS_ASS to clear, at most
20 ms more; the schedule starts again (`ehci_async_resume()`, with the list
address pointed at the list head) after the qTDs are freed. Never more than 40 ms.
A controller that is halted or has its schedule off is not waited for. The
success path is unchanged: a doorbell per transfer would cost a frame each.

## Start-up: leave the drive clean

A drive that a console or IOS reset left in the middle of a transfer can stay
stuck: port power survives a console reset, and enumeration alone did not restart
it (REVIEW.md, the 2026-11-06 finding). `EHCI_Init()` calls
`ehci_startup_recover()` after it releases the non-USB2 ports. Every port that
has a drive for EHCI is power cycled first if HCSPARAMS.PPC is set
(`ehci_port_power_cycle()`), then given a bus reset (`ehci_reset_usb_port()`).
`ehci_discover()` enumerates afterwards, at the first open of `/dev/usb2`.

Added time in the host simulation, per connected port: about 110 ms without PPC,
about 660 ms with PPC (off 300 ms, the drive reconnects after 150 ms in the model,
settle 100 ms, reset 110 ms). A real drive may take longer to reconnect; the wait
for it is limited to 1 s.

`+dbg` builds (`D2X_DEBUG`) trace `Z` events (E_RESET): `c` at start with a =
HCSPARAMS and b = PPC, `C` for a power cycle (a = 0 drive back, -1 not), `q` and
`Q` for the doorbell wait and the schedule halt (a = 0 ok, -1 timeout), `3` and
`4` for rungs 3 and 4 (a = 0 mounted).

## Waiting for a transfer

`handshake()` polls the controller's status. It checks first, polls without
sleeping for `HANDSHAKE_SPIN_TICKS` (about 53 us), and only then sleeps
between checks. `ehci_usleep()` is an IOS timer and message-queue round trip,
far longer than the time it asks for, so a small transfer that waited for it
was mostly waiting for IOS.

## Tools

- **Host tests:** `tools/build.sh test` runs `tests/host/ums`, the real
  `usb.c` and `usbstorage.c` against a simulated drive (`sim.c`) with a
  simulated clock. Faults are fields in `struct sim`: `stall_write_data`,
  `fail_write_csw`, `timeout_xfers`, `lose_write_csw`, `stall_csw` (every CSW
  STALLs: a present drive that never recovers). The port: `disconnected`
  (CCS 0), `csc` (write-1-to-clear), `replug_at_us`, `sim_unplug_for(us)`;
  counters `portsc_writes` and `port_xfers_gone` (transfers tried on an
  unplugged port). A stuck drive: `stuck` 1 answers no bulk transfer until a port
  reset, 2 until a power cycle (`stuck_io_only`: only READ(10) and WRITE(10)).
  `ppc` says whether the ports switch power (PORT_POWER writes are ignored
  without it; counters `power_offs`, `power_cycles`). The controller: `hc_cmd` and
  a STS_ASS/STS_IAA model for `ehci_recover.h` (`iaa_never`, `ass_stuck`; counters
  `doorbells`, `ase_clears`, `ase_sets`). `ehci_do_urb()` itself is not in the
  host build, so its call of `ehci_async_quiesce()` is checked by reading and by
  the hardware build, not by a test. Add a fault there
  and a test in `test_ums.c` for any new failure mode.
- **Event log:** build with `EXTRA_CFLAGS=-DEHCI_DEBUG_LOG` and run
  `tests/wii/umsbench.py -- --log`. The log shows each CBW, transfer, qTD
  token, DMA address and CSW, with microsecond times.
- **Bench quirks:** debug builds take `USB_IOCTL_UMS_SET_QUIRKS` with the
  bits in `tinyehci/ums_quirks.h` (`umsbench --quirks=N`), for trying a
  behaviour change on hardware without a rebuild.
- **Write tests:** `umsbench --pattern=N` writes known data and reads it
  back. It overwrites sectors in the middle of the drive: **scratch drives
  only.** `--write-back` writes back what is there, which cannot catch a
  write that lands stale data.

## USBS (base 58): the same rules on IOS's own USB driver

The USBS module (`usb-module/usbstorage.c`) talks to Nintendo's USB 2.0
driver through ioctls instead of driving the controller. It now follows the
same rules as the EHCI module:

- **Shared with EHCI:** a STALL in the data phase ends the data and the CSW
  says why; a CSW that STALLs gets a clear-halt and one more read; CSW status
  1 gets a REQUEST SENSE; a phase error or CSW status 2 gets reset recovery;
  `USBSTORAGE_SECTOR_TRIES` (4) attempts per sector request; one
  `USBSTORAGE_REQUEST_MS` (8 s) deadline per request.
- **Ported, differently:** the buffer and length are restored before every
  retry (the old `__cycle()` subtracted a negative return value from the
  length and overran the buffer). The deadline reads the Hollywood timer
  (`HW_TIMER`, 0x0D800010) directly, as `ehci-module` does; the STALL codes
  are IOS's (-7003, -7004; -7005 is a NAK).
- **Untested on a Wii:**
  - the STALL codes (from WiiBrew; no real drive has been seen to return them);
  - the timer register from this module (both modules are mload ELFs started
    the same way, but only EHCI has run it);
  - whether `USB_CancelEndpoint()`, which USBS uses as clear-halt, also clears
    the halt on the drive;
  - an IOS transfer that never returns: the deadline is checked between
    transfers only, so a blocked ioctl blocks the request.

### USBS transfer sizes

Measured on the dev Wii (read-only `umsbench`, 8 GB drive): `/dev/usb/ven`
costs about 1 ms per bulk transfer whatever its size. A 1-sector read is 3
transfers (CBW, data, CSW) and took 2991 us. 64 sectors in 4 KB pieces is 10
transfers and took 10002 us; in 16 KB pieces (build `+usbs16`) it took 4051 us,
with no failures and no hang. The time is the transfer count, so the module
sends fewer, bigger transfers:

| Path | When | Piece | Option |
|---|---|---|---|
| Bounce | the caller's buffer is not 32-byte aligned, or not in MEM2 | 16384 bytes, through `dev->buffer` (from the 32 KB module heap) | `USBS_MAX_TRANSFER` |
| Direct | aligned MEM2 buffer, handed to IOS as it is | 16384 bytes, the same size | `USBS_MAX_TRANSFER` |

Both paths split at `USBS_MAX_TRANSFER`. The bulk `wLength` in `usb2.c` is a u16,
so the module could send up to 65535 bytes, and an earlier version of the direct
path did (65024, `USBS_MAX_DIRECT`). **That hangs IOS on a real Wii**: umsbench's
64-sector read (32 KB, one direct transfer into an aligned MEM2 buffer) never
returned from `/dev/usb/ven` on base 58 (IOS58 v6175); the console froze and
needed a power cycle. The 8 s deadline could not help, because the IPC call
itself blocks. 16 KB transfers work. The per-transfer limit of IOS58's
`/dev/usb/ven` looks like 16 KB, which matches libogc's `MAX_TRANSFER_SIZE_V5`.
So the separate direct size is gone. **Do not try a bigger transfer again
without a timeout that IOS itself enforces**: a hang needs a power cycle. The
callers (`wbfs.c`) do the cache invalidate and flush over the whole request. A
64-sector read is 2 data transfers (4 with CBW and CSW), a 127-sector one 4, a
200-sector one 7, on either path.

The CBW, CSW, STALL, retry and deadline logic runs per piece as before; a short
piece ends the data phase and the CSW reports the residue.

The host tests are `tests/host/usbs` (the driver built against the simulated
drive of `tests/host/ums`).
