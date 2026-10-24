# d2x cIOS review

## Plan

Each finding records its trigger, its effect, the smallest safe fix, and the check that shows it (a host test where one can be written).

| Module | What to trace | Check |
| --- | --- | --- |
| EHCI (`ehci-module`, tinyehci) | ioctl in `loop.c` -> `usbstorage.c` Bulk-Only Transport -> `ehci.c` transfers, timeouts, port resets, unplug handling | `tests/host/ums` (simulated drive); bench write timing |
| USBS (`usb-module`) | USB storage through IOS's own `/dev/usb` stack (`usb2.c`, `usbstorage.c`) | host test with a fake `/dev/usb`; bench |
| FAT (`fat-module`) | FatFs (`ff.c`) and `diskio.c` on SD and USB; `fat_wrapper.c` paths and handles | host test with a disk image |
| DIPP (`dip-plugin`) | disc emulation from WBFS and FAT: frag lists (`frag.c`), offsets, DMA alignment | host test of frag lookups |
| MLOAD (`mload-module`) | module loading, ELF checks, patches by IOS version (`detect.c`, `patches.c`) | host test of the ELF loader |
| ES, FFSP (`es-plugin`, `ffs-plugin`) | ES and file-system ioctl hooks, NAND emulation paths | review |
| cios-lib | IPC, syscalls, `sdio.c`, `usbstorage.c` client | review |

## Findings

### Development environment (fork start, on upstream 33ad1ee)

| Kind | Change | Check |
| --- | --- | --- |
| Tools | `docker/Dockerfile` and `tools/build.sh`: one pinned image (Debian by digest, devkitARM r32 by SHA-256) for local and CI builds, on Linux, macOS, and Windows (via WSL Docker). | Modules built twice from the same tree are byte-identical. |
| Fix | `maked2x.sh` wrote empty `ciosmaps.xml`, `ciosmaps-vWii.xml`, `ReadMe.txt` and `d2x-beta.bat` when `awk` was mawk (no `rshift`), and still printed "Done!". It now uses gawk and stops if a replacement fails or comes out empty. | Under mawk the four files were 0 bytes; now they are 10793, 42837, 2973 bytes, and the `.bat`. |
| Tools | Line endings: LF, with the 50 upstream CRLF files pinned `-text`; `tests/test_line_endings.py`, `tools/fix_line_endings.py`. | `git add --renormalize .` changes no content. |
| Tools | `tests/host/ums`: `usbstorage.c` built unchanged for the host against a simulated Bulk-Only drive with fault injection and simulated time. | 5 pass, 3 `known_bug` (below). |
| Tools | The image moved from bookworm to trixie: gcc 12's ASan hung at start-up (`DEADLYSIGNAL` loop) in 2 of 20 host-test runs on a Linux 6.18 kernel; gcc 14's in 0 of 40. devkitARM r32 runs unchanged (`libelf1t64`). | Clean builds from both images, with `SOURCE_DATE_EPOCH` pinned, give byte-identical `.app` files. (The first check of this reused stale objects; see the next row.) |
| Fix | The module Makefiles tracked no dependencies: an edited header, or a `.c` file pulled in with `#include` (tinyehci builds `ehci.c`, `usb.c` and `usbstorage.c` as one unit), left the object stale, and the build reported success with the old code. Every Makefile now uses `-MMD -MP`, includes the `.d` files, and rebuilds objects when the Makefile changes. `maked2x.sh` keeps a preset `SOURCE_DATE_EPOCH`, so builds of different commits can be compared. | After editing `usbstorage.c`, an incremental build left `EHCI.app` unchanged. Clean builds before and after the Makefile change, date pinned, are byte-identical. |
| Tools | Adopted wii-homebrew-kit: `tests/host/hostcheck.h` (the runner), `tools/wii_targets.py` (bench guards), `tools/dolphin_run.py`. | Host tests unchanged: 5 pass, 3 known. |

### EHCI USB storage: write errors are not recovered (fixed in the host tests)

Seen first in USB Loader GX on the bench Wii (2026-10-23): through cIOS a sector read takes 1 ms, but a single-sector write takes 20 s to several minutes. Items 1 to 3 and 6 are fixed and pass host tests against the simulated drive; none is confirmed on hardware yet.

| Kind | Change | Check |
| --- | --- | --- |
| Fix | **6. Clearing a halt left the host's data toggle as it was.** `USB_ClearHalt()` (`tinyehci/usb.c`) sent CLEAR_FEATURE(ENDPOINT_HALT), which resets the device's toggle to DATA0 (USB 2.0 9.4.5), but tinyehci starts each bulk transfer from the toggle in `ehci_device.toggles` (`ehci_do_urb()` in `ehci.c`), which only a port or device reset zeroes. After every Bulk-Only reset recovery, the next transfer on an endpoint at DATA1 was discarded by the device as a retransmission; the command then timed out (`usb_timeout`, about 4 s) into a port reset. `USB_ClearHalt()` now sets the host toggle to DATA0 too. This is the likeliest cause of the slow writes: every recovery, including a soft one, could cost a timeout and a port reset. | `reset_recovery_keeps_toggles_in_step`: before, 4 reads around two reset recoveries cost 3 lost transfers and 4 port resets; now none. Without this fix, `write_recovers_from_one_stall` also fails (a lost transfer and 2 port resets). |
| Fix | **1. A STALL at the data phase was retried forever.** In `__cycle()` the `continue; //reset+retry` in the inner data loop restarted that loop, not the command, so it resent to a halted endpoint. Now any short transfer ends the data phase (tinyehci reports a STALL as a short count); after a short bulk-out transfer the halt is cleared; the CSW is read, and a STALLed CSW read is cleared and read once more (BOT 5.3.3, 6.7.2, 6.7.3). A phase error (status 2) or a bad CSW gets a reset recovery. | `write_recovers_from_one_stall` (twice, at both toggle parities): succeeds with 2 clear-halts, no reset, no lost transfer, in under 100 ms of simulated time. |
| Fix | **2. The CSW status was never checked.** `__USBStorage_Write()` and `__USBStorage_Read()` tested `retval > 0 && status != 0`, but `__cycle()` returns 0 on success, so a failed command counted as done. Now `retval >= 0`; a failed command gets a REQUEST SENSE (which clears the condition) and returns `USBSTORAGE_ESTATUS`. | `write_csw_failure_is_retried`: one REQUEST SENSE, then a successful retry. |
| Fix | **3. `USBStorage_Write_Sectors()` and `USBStorage_Read_Sectors()` never gave up** (`if(retry>12) retry=12; // infinite loop`), and after a non-timeout error they stopped calling the driver but kept looping. Now one loop serves both: it still waits without a limit while the device is gone (`unplug_device`, so a game survives a replug, as Hermes intended), but a device that answers and keeps failing gets `USBSTORAGE_SECTOR_TRIES` (4) attempts, then false, so FAT reports an error. | `write_to_protected_drive_gives_up`: 4 attempts, then false, in under 100 ms of simulated time. |

The change is limited to `usb.c` and `usbstorage.c`. With the build date pinned, only `EHCI.app` changes (16820 to 16836 bytes; `.text` 0x3F70 of the 0x5000 the module may use). No new compiler warnings.

Still open:

4. **A halted qTD writes through a NULL pointer.** In `qh_end_transfer()` (`tinyehci/ehci.c`), after `for(qtd = qh->qtd_head; qtd; qtd = qtd->next)` ends, `qtd` is NULL, and `if(error) qtd->urb->actual_length = error;` reads a pointer from near address 0 and writes -1 through it. The error itself is lost, so the caller sees the partial byte count of a short transfer (which is what the fixes above rely on to detect a STALL).
   - Effect: memory corruption at an address taken from low memory, on every STALL or transaction error.
   - Fix: keep the urb pointer before the loop (`qh->qtd_head->urb`). Returning the error instead of the short count would change what callers see; the STALL handling in `__cycle()` would have to treat both the same.
   - Check: needs a host test of `ehci.c` itself (the simulation stops above it), or hardware.
5. **`ata_protocol` is always 1.** `USBStorage_Open()` tests `subclass != SCSI || subclass != RBC`, which is always true, so every CBW carries a command length of 12. Most drives ignore it; it is recorded here, not changed.

Trigger for the bench work: build a cIOS with these fixes, install it to a spare slot on the bench Wii (with the user's OK), and time single-sector writes on the drive from the USB Loader GX finding, before and after. USB Loader GX's unmount write skip (its `usbstorage2.c`) hides the problem there; time it with that skip turned off.

### Next

- **Bench test program.** A DOL, run from HBC through the bench queue, that reports which slots hold which cIOS (read only), then opens the cIOS's USB device and times sector reads and writes to a scratch file area, printing results to the HBC log. Trigger: before the first install of a fork build.
- **ARM emulator harness.** Run the real module ELFs (big-endian ARM926) under an emulator such as Unicorn, with IOS syscalls and EHCI registers simulated in Python. This tests the compiled binary instead of a host rebuild. Trigger: a bug that the host tests cannot show (timing, endianness, the stripped ELF layout) or a second module that needs hardware-level simulation.
