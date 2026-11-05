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
| `-ENODEV` (-1) | `handshake()` | Port disconnected, port owner lost, fatal controller error, or a timeout outside set-up code | `unplug_device = 1`; the request waits for the drive (rule 1 below) |
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

1. **The drive is gone** (`unplug_procedure()` cannot set it up): wait for
   it without limit, so a game survives a replug. Once it is back, the
   request starts afresh.
2. **The drive answers but the request keeps failing:** after
   `USBSTORAGE_SECTOR_TRIES` attempts, or once `USBSTORAGE_REQUEST_MS` has
   passed, return failure. `__cycle()` checks the same deadline and starts
   no new reset recovery after it.
3. Otherwise, try again.

The limits:

| Limit | Value | Where |
| --- | --- | --- |
| One transfer (`usb_timeout`, `ums_io_timeout`) | 1000\*1000, about 1.05 s | `ehci.c`, `usbstorage.c` |
| Reset recoveries per command (`USBSTORAGE_CYCLE_RETRIES`) | 10 | `usbstorage.c` |
| Attempts per sector request (`USBSTORAGE_SECTOR_TRIES`) | 4 | `usbstorage.c` |
| Time per sector request (`USBSTORAGE_REQUEST_MS`) | 8000 ms | `usbstorage.c` |

A drive that takes a write's data but never sends its status (the bench's
SanDisk "USB 3.2Gen1") now fails in about 8.5 s, with no remount. Before
the deadline it took about 48 s: 43 reset recoveries and 6 remounts for one
sector (`tests/host/ums`, `write_with_lost_status_fails_in_time`).

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
  `fail_write_csw`, `timeout_xfers`, `lose_write_csw`. Add a fault there
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
| Direct | aligned MEM2 buffer, handed to IOS as it is | up to 16384 bytes, cut to whole sectors of the drive: 16384 (32 sectors) for 512-byte sectors, 12288 for 4096 | `USBS_MAX_DIRECT` |

The bulk `wLength` in `usb2.c` is a u16, so the module could send up to 65535
bytes, and the first version of the direct path did (65024). **That hangs IOS on
a real Wii**: umsbench's 64-sector read (32 KB, one direct transfer into an
aligned MEM2 buffer) never returned from `/dev/usb/ven` on base 58 (IOS58
v6175); the console froze and needed a power cycle. The 8 s deadline could not
help, because the IPC call itself blocked. 16 KB transfers work. The per-transfer
limit of IOS58's `/dev/usb/ven` looks like 16 KB, which matches libogc's
`MAX_TRANSFER_SIZE_V5`. So `USBS_MAX_DIRECT` is 16384; it stays a build option,
but never raise it without an attended bench run (a hang needs a power cycle).
The data vector needs no extra cache work in the module: the callers (`wbfs.c`)
do the invalidate and flush over the whole request. A 64-sector read into an
aligned MEM2 buffer is 2 data transfers (4 transfers with CBW and CSW), a
127-sector one 4 data transfers, a 200-sector one 7.

The CBW, CSW, STALL, retry and deadline logic runs per piece as before; a short
piece ends the data phase and the CSW reports the residue.

The host tests are `tests/host/usbs` (the driver built against the simulated
drive of `tests/host/ums`).
