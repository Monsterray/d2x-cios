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

Seen first in USB Loader GX on the bench Wii (2026-10-23): through cIOS a sector read takes 1 ms, but a single-sector write takes 20 s to several minutes. Items 1 to 6 are fixed; all but 4 pass host tests against the simulated drive. None is confirmed on hardware yet.

| Kind | Change | Check |
| --- | --- | --- |
| Fix | **6. Clearing a halt left the host's data toggle as it was.** `USB_ClearHalt()` (`tinyehci/usb.c`) sent CLEAR_FEATURE(ENDPOINT_HALT), which resets the device's toggle to DATA0 (USB 2.0 9.4.5), but tinyehci starts each bulk transfer from the toggle in `ehci_device.toggles` (`ehci_do_urb()` in `ehci.c`), which only a port or device reset zeroes. After every Bulk-Only reset recovery, the next transfer on an endpoint at DATA1 was discarded by the device as a retransmission; the command then timed out (`usb_timeout`, about 4 s) into a port reset. `USB_ClearHalt()` now sets the host toggle to DATA0 too. This is the likeliest cause of the slow writes: every recovery, including a soft one, could cost a timeout and a port reset. | `reset_recovery_keeps_toggles_in_step`: before, 4 reads around two reset recoveries cost 3 lost transfers and 4 port resets; now none. Without this fix, `write_recovers_from_one_stall` also fails (a lost transfer and 2 port resets). |
| Fix | **1. A STALL at the data phase was retried forever.** In `__cycle()` the `continue; //reset+retry` in the inner data loop restarted that loop, not the command, so it resent to a halted endpoint. Now any short transfer ends the data phase (tinyehci reports a STALL as a short count); after a short bulk-out transfer the halt is cleared; the CSW is read, and a STALLed CSW read is cleared and read once more (BOT 5.3.3, 6.7.2, 6.7.3). A phase error (status 2) or a bad CSW gets a reset recovery. | `write_recovers_from_one_stall` (twice, at both toggle parities): succeeds with 2 clear-halts, no reset, no lost transfer, in under 100 ms of simulated time. |
| Fix | **2. The CSW status was never checked.** `__USBStorage_Write()` and `__USBStorage_Read()` tested `retval > 0 && status != 0`, but `__cycle()` returns 0 on success, so a failed command counted as done. Now `retval >= 0`; a failed command gets a REQUEST SENSE (which clears the condition) and returns `USBSTORAGE_ESTATUS`. | `write_csw_failure_is_retried`: one REQUEST SENSE, then a successful retry. |
| Fix | **3. `USBStorage_Write_Sectors()` and `USBStorage_Read_Sectors()` never gave up** (`if(retry>12) retry=12; // infinite loop`), and after a non-timeout error they stopped calling the driver but kept looping. Now one loop serves both: it still waits without a limit while the device is gone (`unplug_device`, so a game survives a replug, as Hermes intended), but a device that answers and keeps failing gets `USBSTORAGE_SECTOR_TRIES` (4) attempts, then false, so FAT reports an error. | `write_to_protected_drive_gives_up`: 4 attempts, then false, in under 100 ms of simulated time. |
| Fix | **5. The CBW never carried the command's length.** `USBStorage_Open()` set `ata_protocol` with `subclass != SCSI || subclass != RBC`, always true, so every CBW said 12; and the SCSI branch of `__send_cbw()` would have said 16 (`cbLen > 6 ? 0x10 : 6`) for any 10-byte command. Now `&&`, and SCSI and RBC devices get `cbLen`, the valid length (BOT 5.1, as Linux sends it). Other subclasses keep the padded 12. The comment on READ CAPACITY already noted that some drives require its 10-byte length. | `cbw_carries_command_length`: READ(10) carried 12 before, 10 now. |
| Fix | **4. A halted qTD wrote through a NULL pointer.** In `qh_end_transfer()` (`tinyehci/ehci.c`), `if(error) qtd->urb->actual_length = error;` ran after `for(qtd = ...; qtd; ...)` had left `qtd` NULL: on every STALL or transaction error it read a pointer from low memory and wrote -1 through it. The store is gone; a halted transfer still reports the bytes it moved (a short transfer), which is what callers, and the STALL handling above, see today. | Build only: the host simulation stops above `ehci.c`. Bench check pending. |

Items 1 to 3 and 6 change only `usb.c` and `usbstorage.c`. With the build date pinned, only `EHCI.app` changes (16820 to 16836 bytes; `.text` 0x3F70 of the 0x5000 the module may use). No new compiler warnings.

Still open:

- **Report a STALL as an error code.** With item 4 fixed, a halted transfer still looks like a short one, so a STALL and a short packet are told apart only by direction. Returning `-EPIPE` from `qh_end_transfer()` for a halt with no other error bits would let `__cycle()` follow BOT exactly, but every caller of the EHCI layer (control requests in `usb.c` and `ehci.c`, and the ioctls in `loop.c`) would see a new error. Trigger: a bench run that shows a drive whose short packets and STALLs are confused.

Trigger for the bench work: build a cIOS with these fixes, install it to a spare slot on the bench Wii (with the user's OK), and time single-sector writes on the drive from the USB Loader GX finding, before and after. USB Loader GX's unmount write skip (its `usbstorage2.c`) hides the problem there; time it with that skip turned off.

### USBS USB storage (IOS's USB stack)

| Kind | Change | Check |
| --- | --- | --- |
| Fix | **The CSW status was never checked**, as in the EHCI module (item 2 there): `USBStorage_Read()` and `USBStorage_Write()` in `usb-module/usbstorage.c` tested `retval > 0 && status != 0`, but `__cycle()` returns 0 on success. Now `>= 0`, so a failed command returns `USBSTORAGE_ESTATUS` and `usbstorage_WriteSectors()` reports false. | `tests/host/usbs`: `usbs_write_csw_failure_is_reported` (the write returned 0 before). |
| Tools | `tests/host/usbs` builds the USBS driver against the same simulated drive, through IOS's USB API implemented on it. The USBS code assumes a big-endian CPU (native word loads of USB's little-endian fields with its own `Swap32()`), so the glue swaps the CBW, CSW and READ CAPACITY words at the boundary. | `usbs_roundtrip`. |

Open question: USBS defines `USB_ClearHalt(id, ep)` as `USB_CancelEndpoint(id, ep)`, an IOS request that cancels an endpoint's pending transfers. Whether IOS also sends CLEAR_FEATURE(ENDPOINT_HALT) there is not visible in this code; if it does not, a STALLed endpoint stays halted after a USBS reset recovery. Trigger: a USBS drive that stops answering after a STALL on the bench.

### FAT module (fat-module)

From a read-only review (F1-F8); the fixed ones have host tests in `tests/host/fat`, which builds `fat_wrapper.c`, FatFs and `fat_tools.c` against two RAM disks formatted by the same FatFs.

| Kind | Change | Check |
| --- | --- | --- |
| Fix | **F4. SD and USB shared one FatFs work area.** `f_mount()` got the same static `FATFS` for both volumes, so with both mounted the volume used last answered for either drive, and unmounting one cleared the other. Now `fatFs[_VOLUMES]` (4128 bytes more BSS; 4384 bytes of the RAM region stay free). | `sd_and_usb_together` (creating `1:/` failed before). |
| Fix | **F2. READDIR overflowed the module heap.** `FAT_ReadDir()` copied names into `Mem_Alloc(buflen)` with no bound, copied all `buflen` bytes out (heap leftovers included), and never freed the buffer when there was no output buffer. | `readdir_small_buffer`, `readdir_returns_names`, `readdir_count_only_frees` (guard zones in the test heap). |
| Fix | **F3. DELETEDIR and GETUSAGE overflowed a 256-byte stack buffer** with `strcpy`/`strcat` on trees whose paths pass 255 characters. Now `FS_ENAMETOOLONG`. | `deletedir_long_path`, `getusage_long_path` (ASan stack-buffer-overflow before). |
| Fix | **F7. A failed close leaked the FIL.** `FAT_Close()` returned before `Mem_Free()` when `f_close()` failed (a pulled card). | `close_failure_frees`. |

Open (verified by reading, not yet fixed; `main.c` is an upstream CRLF file):
- **F1.** `$`-prefixed opens build `path[256]` from the 192-byte NAND path plus `FAT_Escape()` output, which can grow each character to 4: about 260 bytes. Fix: a bounded escape that fails when the result does not fit.
- **F5.** SETNANDPATH writes `emuNandPath[192]` when the path is 191 characters without a trailing `/`; its alignment guards use `!31` (always 0) where `~31` was meant.
- **F6.** Read, write, seek and FILESTATS on the `/dev/fat` descriptor itself (fd 0) pass `(FIL *) 0` to FatFs.
- **F8.** Some ioctls read vectors without checking `inlen`/`iolen`. The right check depends on the IPC split each caller uses; trigger: a host test of `main.c`'s ioctl dispatch.

### DIP plugin (dip-plugin)

| Kind | Change | Check |
| --- | --- | --- |
| Fix | **D2.** `Frag_Init()` did not check the fragment count against `FRAG_MAX` or the bytes copied; lookups walk `frag[0..num-1]`. | Reading and a `-Werror` build. |
| Fix | **D4.** `DI_ReadDvd()` returned only the last chunk's result, so an early failure followed by a good chunk returned success with bad data. | Reading and build. |
| Fix | **D1.** After an IOS reload the saved WBFS/frag mode was restored even when the device did not open, and reads then used a NULL fragment list. The mode is now dropped. | Reading and build. |
| Fix | **D5.** `WBFS_Open()` indexed `devFs[]` with an unchecked device number. | Reading and build. |

Open: **D3** (read lengths not checked against the output buffer; depends on whether IOS's DI front end checks first), **D6** (an off-by-one in the fragment index cache, harmless), and the commented-out cache flush and invalidate around the plugin's IPC to the FAT module (`dip-plugin/fat.c`). Trigger for all three: a host harness for `frag.c` and the plugin's ioctl handlers.

### Bench results (dev Wii, 2026-10-23)

| Kind | Change | Check |
| --- | --- | --- |
| Measurement | The slot 249 cIOS that was installed before (an unknown d2x build, v65535): 1-sector read 653 us, 64-sector read 1.5 ms; **the first 1-sector write after reads took 192 s**, the next two 1.3 ms; 64-sector writes 3.3 ms. Data unchanged after the write-back. | `tests/wii/umsbench.py -- --ios=249 --write-back --writes=3` |
| Tools | Installed `d2x-v11-beta3-mr1` (this branch at fa5d8d8, base 56) to slot 249 with the d2x installer's `--auto` mode (local patched installer). Read back: slot 249 holds v21011, boots, mounts the drive, reads at the same speed. | `tests/wii/install_cios.py` (TV recorded), then `umsbench.py` read-only. |

### Next

- **Bench test program.** A DOL, run from HBC through the bench queue, that reports which slots hold which cIOS (read only), then opens the cIOS's USB device and times sector reads and writes to a scratch file area, printing results to the HBC log. Trigger: before the first install of a fork build.
- **ARM emulator harness.** Run the real module ELFs (big-endian ARM926) under an emulator such as Unicorn, with IOS syscalls and EHCI registers simulated in Python. This tests the compiled binary instead of a host rebuild. Trigger: a bug that the host tests cannot show (timing, endianness, the stripped ELF layout) or a second module that needs hardware-level simulation.
