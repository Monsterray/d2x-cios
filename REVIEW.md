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
| Tools | The image moved from bookworm to trixie: gcc 12's ASan hung at start-up (`DEADLYSIGNAL` loop) in 2 of 20 host-test runs on a Linux 6.18 kernel; gcc 14's in 0 of 40. devkitARM r32 runs unchanged (`libelf1t64`). | The seven `.app` files are byte-identical from both images. |
| Tools | Adopted wii-homebrew-kit: `tests/host/hostcheck.h` (the runner), `tools/wii_targets.py` (bench guards), `tools/dolphin_run.py`. | Host tests unchanged: 5 pass, 3 known. |

### EHCI USB storage: write errors are not recovered

Seen first in USB Loader GX on the bench Wii (2026-10-23): through cIOS a sector read takes 1 ms, but a single-sector write takes 20 s to several minutes. The causes below are from reading the code and running it in the host simulation; none is confirmed on hardware yet.

1. **A STALL at the data phase is retried forever.** `__cycle()` (`tinyehci/usbstorage.c`) sends the data in an inner `while(len > 0)` loop. On a short or failed transfer its `continue; //reset+retry` restarts that inner loop, not the command, so it resends to a halted endpoint with no reset and no CLEAR_FEATURE. A halted endpoint moves 0 bytes each time, so the loop never ends.
   - Test: `write_recovers_from_one_stall` and `write_to_protected_drive_gives_up` (both hang today).
   - Fix: leave the inner loop on any error; after a STALL, clear the halt and read the CSW (Bulk-Only Transport 6.7.2 and 6.7.3), and do a reset recovery only on a phase error or a bad CSW.
2. **The CSW status is never checked.** `__USBStorage_Write()` and `__USBStorage_Read()` test `retval > 0 && status != 0`, but `__cycle()` returns 0 on success. A drive that answers "command failed" (status 1) or "phase error" (status 2) is treated as a success: the data is not on the drive, and FAT carries on.
   - Test: `write_csw_failure_is_not_success`.
   - Fix: `retval >= 0 && status != 0`. On status 1, REQUEST SENSE, then retry or fail; on status 2, reset recovery.
3. **`USBStorage_Write_Sectors()` and `USBStorage_Read_Sectors()` never give up.** `if(retry>12) retry=12; // infinite loop`. Each failed round can cost a timeout (`usb_timeout`), a port reset, a new device setup, and `ehci_msleep(100)`.
   - Test: `write_to_protected_drive_gives_up`.
   - Fix: a bounded number of rounds, then return false so FAT reports `FR_DISK_ERR`.
4. **A halted qTD writes through a NULL pointer.** In `qh_end_transfer()` (`tinyehci/ehci.c`), after `for(qtd = qh->qtd_head; qtd; qtd = qtd->next)` ends, `qtd` is NULL, and `if(error) qtd->urb->actual_length = error;` reads a pointer from near address 0 and writes -1 through it. The error itself is lost, so the caller sees the partial byte count of a short transfer.
   - Effect: memory corruption at an address taken from low memory, on every STALL or transaction error.
   - Fix: keep the urb pointer before the loop (`qh->qtd_head->urb`) and return the error to the caller.
   - Check: needs a test of `ehci.c` itself (not written yet), or hardware.
5. **`ata_protocol` is always 1.** `USBStorage_Open()` tests `subclass != SCSI || subclass != RBC`, which is always true, so every CBW carries a command length of 12. Most drives ignore it; it is recorded here, not changed.

Trigger for the bench work: once fixes 1 to 3 pass their host tests, build a cIOS with them, install it to a spare slot on the bench Wii (with the user's OK), and time single-sector writes on the drive from the USB Loader GX finding, before and after.

### Next

- **Bench test program.** A DOL, run from HBC through the bench queue, that reports which slots hold which cIOS (read only), then opens the cIOS's USB device and times sector reads and writes to a scratch file area, printing results to the HBC log. Trigger: before the first install of a fork build.
- **ARM emulator harness.** Run the real module ELFs (big-endian ARM926) under an emulator such as Unicorn, with IOS syscalls and EHCI registers simulated in Python. This tests the compiled binary instead of a host rebuild. Trigger: a bug that the host tests cannot show (timing, endianness, the stripped ELF layout) or a second module that needs hardware-level simulation.
