# USB ports: how d2x picks a drive, and the plan for both ports

Status: investigation and design (branch `claude/usb-two-ports`). Nothing here is implemented or tested on a Wii yet. Section 6 lists what is still open. Paths are relative to the repository root.

## 1. How each driver picks a port today

### EHCI module (bases 56, 57, 38 and the rarely used ones)

- The module has no port selection. A loader that sends the "set port" ioctl (`('U'<<24)|('M'<<16)|('S'<<8)) + 0x83`) gets `IPC_EINVAL` from `__EHCI_Ioctlv()` (`source/ehci-module/loop.c:274`; the UMS cases are at `loop.c:147-228`).
- Only EHCI root port 0 is ever used for storage. `__USBStorage_Init()` (`source/ehci-module/tinyehci/usbstorage.c:1505-1570`) loops over `ehci->num_port`, but calls `ehci_reset_port(0)` (`:1525`), reads `port_status[0]` (`:1545`) and returns at the first port that has no device (`:1541-1565`). A device on any other port is never reached.
- The recovery code is wired to port 0 as well: `ehci_reset_port2(0)`, `ehci_reset_port(0)`, `ehci_port_power_cycle(0)` (`usbstorage.c:1677`, `:1684`), `ums_portsc()` (`:1716-1718`) and the dead-state write (`:1740`).
- Port ownership. `EHCI_Init()` (`source/ehci-module/tinyehci.c`) sets `num_port = 4` and calls `ehci_release_ports()`, which hands EHCI ports 1 and 2 to the OHCI companion controllers whatever is plugged in, and also any port with nothing or a USB 1.1 device on it (`tinyehci/ehci.c:969-982`, condition `i==1 || i==2 || !(PORT_CONNECT&status) || PORT_USB11(status)`). `ehci_discover()` (`ehci.c:953-965`) then enumerates every port EHCI still owns, at the first open of `/dev/usb2` (`loop.c:426-432`). So the kept ports are 0 and 3 in this tree.
- All storage state is one global set: `__usbfd`, `__lun`, `__mounted`, `__vid`, `__pid`, `ums_init_done`, `first_access` (`usbstorage.c:224-234`), `unplug_device` (`ehci.c:165`) and the dead-state counters (`usbstorage.c:1712-1714`). One drive at a time.
- `ums_mode = fd` (`loop.c:159`) remembers the fd that sent UMS_INIT; the sector ioctls do not check which fd sent them (`loop.c:180-202`). Every client of `/dev/usb2` (loader, FAT module, DIP plugin) therefore shares the one drive.

### USBS module (base 58, `source/usb-module`)

- No port concept at all. IOS58's own USB stack lists devices; `usbstorage_IsInserted()` (`usbglue.c:55-183`) asks `USB_GetDeviceList()` for mass-storage devices (at most `DEVLIST_MAXSIZE` 8, `usbglue.c:13`), mounts the first that works and then remembers it by VID/PID (`:84-102`). A drive in either physical port works, as long as it is the only one. With two drives, the first in IOS's list wins. Two identical drives (same VID/PID) cannot be told apart.
- `usb_device_entry` carries only `device_id`, `vid`, `pid`, `token` (`usb2.h:152-157`). IOS58 gives no port number through this interface.
- Unknown ioctls return `IPC_EINVAL` (`usb-module/main.c:31`, `default: break`), so the port ioctl fails here too. Loaders ignore that result.
- One handle (`__usbfd`, `__lun`, `__vid`, `__pid`, `__mounted`; `usbglue.c:20-28`).

### What loaders send

USB Loader GX (`C:/projects/usbloadergx/source/usbloader/usbstorage2.c`; same on every branch we have):

- Defines `USB_IOCTL_SET_PORT = UMS_BASE+0x83` (`:56`) and sends it with one `u32` input (`:148`), before `USB_IOCTL_UMS_INIT` (`:99-102`) and before each capacity, read and write call (`:164`, `:197`, `:314`). A settings value "both ports" (`Settings.USBPort == 2`) is handled in the loader: it switches the port back and forth on one fd. The loader's own comment: "Port = 2 is handle in the loader, no need to handle it in cIOS" (`:135`).
- The GX-side code assumes the cIOS keeps **one mounted drive per port** and swaps on `SET_PORT`: it calls `UMS_INIT` once per port and later only swaps.
- `GameBooter::ShutDownDevices()` re-sends the game's port before the game starts (`GameBooter.cpp:257-269`).
- It opens `/dev/usb2`, then `/dev/usb123`, then `/dev/usb/ehc` (`usbstorage2.c:71-73`, `:94-96`).
- WiiFlow and CFG use the same ioctl numbers (from their sources as of the d2x v9 era; not re-checked here).
- DI device numbers sent to the DIP plugin: 1 = USB, 2 = SD (`dip-plugin/frag.c:30-31`, `dip-plugin/plugin.c:536-608`). Nothing in them names a port.

## 2. How the FAT module and the NAND emulation reach the drive

```
loader ----------------------------------------------> /dev/usb2 (EHCI or USBS module)
FAT module  (own thread, device "fat") --- cios-lib/usbstorage.c ---> /dev/usb2
DIP plugin  (in Nintendo's DI module)  --- cios-lib/usbstorage.c ---> /dev/usb2   (fraglist games)
                                       --- dip-plugin/wbfs.c ------> /dev/usb2   (WBFS games, devFs[0])
ffs-plugin  (in Nintendo's FS module)  --- IPC to "fat" -----------> FAT module
```

- `cios-lib/usbstorage.c` is a client library, linked into each module that needs it: it opens `"/dev/usb2"` (`:44`, `:185`), keeps one static fd and sector size (`:47-50`), sends `UMS_INIT`, `GET_CAPACITY`, `READ_SECTORS`, `WRITE_SECTORS` (`:190`, `:158`, `:91-94`), and splits requests at 64 sectors (`:41`). It never sends a port.
- The FAT module has two FatFs volumes: 0 = SD, 1 = USB (`fat-module/fat_wrapper.c:43`, `diskio.c:18-19`). `IOCTL_FAT_MOUNT_USB` (0xF2, `fat-module/main.c:98`, `:376`) calls `FAT_Mount(1, partition)` which calls `usbstorage_Init()` (`fat_wrapper.c:127-165`).
- The ffs-plugin (NAND emulation) reaches the drive only through the FAT module: `FAT_Init()` opens `"fat"` and sends `IOCTL_FAT_SETNANDPATH` (`ffs-plugin/fat.c:98-124`, `ffs-plugin/plugin.c:184-187`).
- After a fake IOS reload the DIP plugin remounts what the loader had set up. `DI_InitEmulation()` reads the saved config and mounts USB or SD (`dip-plugin/plugin.c:221-237`): `device = (mode & ISFS_MODE_SDHC) ? 0 : 1` (`:227`). The modes are NAND 0, SDHC 1, USB 2 (`cios-lib/isfs.h:39-42`). The saved config has no port field.
- Consequence: today every path (loader, FAT, DIP) ends at "the one drive of /dev/usb2". A port chosen by the loader through the global state of `SET_PORT` would also move the FAT module's and the DIP plugin's reads. That is the hazard of a global "current port".

## 3. Why d2x-alt lost NAND-emulation compatibility

What the repository shows (the history is in `upstream/d2x-alt`, which is d2x v9 (r49) plus the vWii bases, 2012-12-20/22; `data/Changelog.txt` "d2x v9", "d2x-alt v10"):

- v9's EHCI module was a new one. Its changelog says: "EHCI ioctl command to specify the current usb port", "plug&play" through an interrupt callback, reads and writes of up to 64 KB, a watchdog timeout ioctl, thread priority 0x48 to 0x78. Code: `SET_PORT` at `loop.c:225` of commit `450702a`, a global `current_port` (`tinyehci/ehci.c:525`), per-port arrays `ums_init_done[2]`, `__usbfd[2]`, `__lun[2]`, `__mounted[2]`, `unplug_device[2]` (`usbstorage.c:95-104`), `config.useUsbPort1` and `ehci_acquire_port()` (`ehci.c:1013`). It released all four ports at start and took port 0 or 1 back on demand, with `num_port = 2`.
- It also removed the SDHC and FAT modules "to prevent random freezing with the new EHCI module": FatFs moved into the ffs plugin, and the SDI and EHCI/USBS modules got a second device name without the leading slash (`dev/sdio`, `dev/usb2`) "required by nand emulation to prevent deadlock since we have to open the sd card from inside the fs-plugin". NAND emulation ran FatFs on Nintendo's FS thread and called the USB module from there.
- d2x v10 (the base of this fork) "reverted to r43, i.e. last v8 revision", with the v9 fixes for descriptors and ReadCapacity ported back (commit `4dae153`). That brought back the separate FAT and SDHC modules and the one-port EHCI module. The Changelog does not give a reason beyond "reverted". The repository does not contain a bug list for v9.
- Reading of the evidence (an inference, not a stated cause): two designs were mixed. (a) The port was one global value, so any client switching ports (a loader, with the FAT or DIP code reading in the background) moved everyone's reads. (b) The storage and FAT stack was merged into the FS thread to avoid deadlocks, which is where the NAND-emulation compatibility problems (error 002, save creation, shutdown) would show. Our plan keeps the v8/v10 process layout (separate FAT module) and avoids (a) with per-fd ports.

## 4. IOS and hardware limits

| Limit | Value | Source |
| --- | --- | --- |
| IOS58 `/dev/usb/ven`: bytes per bulk transfer | 16 KB. A 32 KB transfer never returned and needed a power cycle (2026-11-05) | `docs/usb-storage.md`, "USBS transfer sizes" |
| Cost of one `/dev/usb/ven` transfer | about 1 ms, whatever the size | same |
| EHCI one transfer | below 64 KB (u16 length); the module uses 32 KB commands | `docs/usb-storage.md`, "The layers" |
| Hollywood EHCI port power | no switching (HCSPARAMS 0x2204, PPC 0), so no power cycle of a drive | REVIEW.md, 2026-11-08 update |
| A drive left mid-transfer by a console reset | stays stuck; only unplugging frees IOS | same |
| EHCI module code | 0x5000 region, 17864 bytes used, 2616 free (release); 1448 free `+dbg` | `docs/module-size.md` |
| EHCI module RAM | 0x24000 region; module heap 16 KB (`heapspace[0x1000]` u32, `ehci-module/main.c`) | `link.ld`, `main.c` |
| USBS module code | 0x3000 region; 740 bytes free (release), 356 free `+dbg` | `docs/module-size.md` |
| USBS module heap | 32 KB (`heapspace[0x2000]` u32, `usb-module/main.c`), of which 16 KB is the bounce buffer | `docs/usb-storage.md`, `usb-module/main.c` |
| FAT module code | 20480 bytes region, 2448 free | `docs/module-size.md` |
| Which EHCI root ports are the two external sockets | **not verified in this tree.** This fork keeps 0 and 3 (`ehci_release_ports`); d2x v9 and Hermes used 0 and 1. Internal Bluetooth is on an OHCI companion port. Must be found on a Wii (section 6). | `ehci.c:969-982`, v9 `ehci.c` |
| USBS: which physical port a device is on | IOS58 does not say; only `device_id`, VID/PID | `usb2.h:152` |

Memory cost of a second drive:

- EHCI: one `usbstorage_handle` is about 170 bytes (two 16-entry u32 arrays plus fields, `tinyehci/usbstorage.h`) plus the scalar state above, so about 200 bytes of RAM per port, and one more CBW/CSW buffer from the 16 KB heap. The code cost is estimated at 300-500 bytes against 2616 free. Not built yet.
- USBS: a second handle is small. Do not allocate a second 16 KB bounce buffer: one buffer, shared (the module is single-threaded), keeps the 32 KB heap as it is. The code is the constraint: 740 bytes free in release, 356 in `+dbg`.

## 5. Design: the smallest change

Rules: the default stays as today. A loader that never sends the port ioctl keeps port 0 (EHCI) or "the first mountable drive" (USBS). No new behaviour at boot.

### 5.1 EHCI module

1. **Port per fd, not global.** `/dev/usb2` keeps a small table `fd -> port` (default 0, 8 entries is enough: loader, FAT module, DIP plugin, test tools). `USB_IOCTL_UMS_SET_PORT` (0x83, one `u32` in, 0 or 1, else -1) sets the port of the calling fd. `IOS_CLOSE` clears the entry. Every UMS and WBFS ioctl looks up the port of its fd and selects that drive. Result: the loader's fd can use port 1 while the FAT module's fd uses port 0 (or the reverse) at the same time, with no interference. This is what the v9 global `current_port` could not do.
2. **One drive context per logical port (two).** Bundle the per-drive state of section 1 into a `struct ums_ctx` and switch by copying: `ums_select(port)` saves the live set into `ctx[cur]` and loads `ctx[port]`. The module loop is single-threaded, so this needs no locks and keeps the 1900-line `usbstorage.c` almost untouched (no `[port]` on every access). Costs about 200 bytes of RAM and a `memcpy`-sized function.
3. **Port number in the hardware calls.** Replace the hard-coded `0` in `ehci_reset_port(0)`, `ehci_reset_port2(0)`, `ehci_port_power_cycle(0)`, `ums_portsc()` and the dead-state write by the current context's EHCI port. `__USBStorage_Init()` initialises the selected port only.
4. **Take port 1 on demand.** `ehci_release_ports()` keeps releasing port 1 at boot (no change for anyone). When port 1 is first selected for `UMS_INIT`, take it back from OHCI with the v9 sequence (`ehci_acquire_port()`, `450702a:source/ehci-module/tinyehci/ehci.c:1013`: flip PORT_OWNER, disable, flip back, enable), then reset and enumerate it. A full-speed device goes back to OHCI by itself, as before. Put the new function in a new header (`ehci.c` is not valid UTF-8 and cannot be edited with the Edit tool).
5. **Port map.** `logical port 0 -> EHCI port 0`, `logical port 1 -> EHCI port 1` (v9's mapping), in one table so it is one line to change if the Wii test shows the second socket is EHCI port 3.
6. **Both drives at once.** Feasible: two contexts, two enumerated devices (`dev->id = port+1` already gives distinct USB addresses), one controller. Each request runs to completion on one drive. The 10-second keepalive (`__EHCI_Watchdog`, `loop.c:313-345`) must read a sector from each mounted drive, skipping dead or unplugged ones (`USBStorage_Keepalive_Ready()`), so a head-parking disk on port 1 keeps awake as well.
7. **Failure isolation.** A drive unplugged, stuck or dead on one port sets only its context's `unplug_device` and dead state. The unlimited wait for a replug (rule 1 in `docs/usb-storage.md`) is the existing hazard: it holds the module's single thread, so a request to a gone drive on port 1 would also stall port 0. Keep the existing rule for port 0 (no change in behaviour) and give the second port a bounded wait (fail at the request deadline), or say so as a known limit. Decision for the maintainer.

### 5.2 USBS module

1. `SET_PORT` is accepted (per fd, same table idea) and means "the Nth mass-storage device in IOS's list that mounts" (N = 0 or 1), because IOS58 gives no physical port. With one drive plugged in, port 0 is that drive and port 1 is "no device": unchanged for anyone who never asks.
2. Two handles (`__usbfd[2]`, `__lun[2]`, device ids, `__mounted[2]`) with one shared bounce buffer. Mount by `device_id`, not by VID/PID, so two identical drives can be told apart.
3. Open question that only a Wii can answer: whether IOS58's list order is stable and follows the physical port. The test steps below record it.
4. Cost: code is the problem (740 bytes free). Build it and look at the numbers before going on.

### 5.3 FAT module, NAND emulation, DIP (clients)

- `cios-lib/usbstorage.c` gets `usbstorage_SetPort(u32 port)` (default 0, applied before `UMS_INIT`). A client that was never told a port is byte-for-byte as before.
- `IOCTL_FAT_MOUNT_USB` accepts an optional second input vector, the port (`inlen > 1`); missing means 0. `FAT_Mount()` shuts the USB client down and re-opens it when the port changes. The FAT module needs only one USB volume: it serves the NAND-emulation drive. The loader's own game drive is the loader's own fd. So NAND emulation on port 1 with games on port 0 (or the reverse, or both on port 1) works without a third FatFs volume.
- DIP: a new DI device number 3 = "USB, port 1" for `WBFS_OPEN` and `SET_FRAGLIST` (`dip-plugin/plugin.c:536-608`, `frag.c:30-31`, `wbfs.c:32`). Old loaders send 1 or 2 and see no change.
- Emulation across the fake IOS reload: the saved `ffsConfigState` needs the port too (`dip-plugin/plugin.c:221-237`, `:638-661`). Adding a field to a saved structure risks old config files; use a spare bit of `mode` instead (a new bit above `ISFS_MODE_USB`, `cios-lib/isfs.h:39-42`) so the struct keeps its size.
- Loaders must be extended to send the port to the FAT module and to use device 3; the cIOS side can be built and tested without them (the host tests and `umsbench`).

## 6. Open items and the Wii test (nothing run yet)

Open:

- Which EHCI root ports the two sockets are (0 and 1, or 0 and 3). One-line change in the port map once known.
- Whether USBS list order follows the physical port.
- Whether taking EHCI port 1 back from OHCI at run time works on Hollywood (v9 did it; this tree never has).
- Code size of the USBS change.

Wii test (attended, with Monty at the Wii; never unattended: a USB hang inside IOS needs a power cycle). Use `devwii` or `lrwii`, never `prod`, never lrwii slot 249. Installing a cIOS writes NAND: get Monty's OK for the slot and base first (`tests/wii/install_cios.py`, CONTRIBUTING.md "What needs a real Wii"). Two **scratch** drives whose data nobody needs; never the SanDisk "USB 3.2Gen1" (60088319 sectors), which lands stale data on writes.

1. Read only first. In a spare slot (EHCI base 56 or 57, and base 58 for USBS) run `make -C tests/wii/umsbench`, then as a queue job `python tests/wii/umsbench.py -- --ios=N` with drive A in the lower socket only. Then drive A in the upper socket only. Today's cIOS (without the new code) shows which sockets work at all; record which one answers.
2. With the new build: `umsbench --ios=N --port=P` (the `--port` option is to be added to umsbench; it sends `SET_PORT` before `UMS_INIT`). Drive A in socket X, nothing in the other: P=0 and P=1 each, record which socket is which port. This settles the port map and the USBS ordering question.
3. Both drives plugged in: read test on each port (`--port=0`, `--port=1`), then alternating requests on one fd (`--alternate`, to be added): capacity and the first sector of each drive must stay distinct (compare a volume label or a pattern written beforehand).
4. Two clients at once (two umsbench fds, or umsbench plus a loader): one on each port.
5. Write test on the two scratch drives only, `--pattern=N`, one port at a time, then both.
6. Unplug the drive in one socket while the other is being read. Expected: the unplugged port's requests wait or fail by the rule chosen in 5.1(7); the other port keeps working; a replug recovers. Have Monty's hand on the plug and the power button.
7. Last, with a loader: NAND emulation on one port and a game on the other. Needs the loader changes of 5.3.

Run the host tests first: `tools/build.sh test`, then `EXTRA_CFLAGS=-Werror tools/build.sh`.
