<p align="center"><a href="https://github.com/Monsterray/d2x-cios/" title="d2x cIOS"><img src="assets/d2x-logo.jpg" alt="d2x cIOS"></a></p>
<p align="center">
<a href="https://github.com/Monsterray/d2x-cios/releases" title="Releases"><img src="https://img.shields.io/github/v/release/Monsterray/d2x-cios?logo=github" alt="Latest release"></a>
<a href="https://github.com/Monsterray/d2x-cios/actions" title="Actions"><img src="https://img.shields.io/github/actions/workflow/status/Monsterray/d2x-cios/main.yml?branch=master&logo=github" alt="Build status"></a>
</p>

# d2x cIOS

d2x is a custom IOS (cIOS) for the Nintendo Wii and the Wii U's vWii mode. It is Nintendo's own IOS with extra modules added, so homebrew can use things the stock IOS does not offer: USB drives and SD cards as storage, loading games from them, and more. USB Loader GX, Nintendont and many other loaders need it. It is for homebrew only.

d2x is an enhanced version of Waninkoko's cIOSX rev21. This repository is a fork of [wiidev/d2x-cios](https://github.com/wiidev/d2x-cios) that adds tests, a reproducible build and fixes for USB and SD problems. The fixes are kept in separate commits so upstream can take them.

> **No warranty.** This software comes with no warranty at all, neither expressed nor implied. Installing a cIOS writes to your console's NAND. Only you are responsible for any damage to your Wii. Have BootMii or Priiloader and a NAND backup first.

## What is new in 11.1.0

The first release of this fork, built on upstream d2x v11. Mostly it is about USB drives that stop answering: they no longer freeze the Wii.

- **USB drives (EHCI):** a drive that stops answering gives loaders an error in seconds instead of hanging IOS. Write errors are recovered instead of looping.
- **USB on base 58 (USBS):** transfers time out and are cancelled, with the same recovery. Transfers are split at 16 KB, because a 32 KB one hangs IOS58.
- **SD card:** high speed (50 MHz) is on by default, with a fallback for cards that cannot switch.
- **Other modules:** fixes in MLOAD, FAT and DIP: requests are checked, errors fail cleanly.
- **Versions** are now `major.minor.patch`. USB Loader GX shows 11.1.0 as `d2x v11-1-0`.

The full list is in [data/Changelog.txt](data/Changelog.txt). Tested on two Wiis with USB Loader GX, Nintendont and the Homebrew Channel, on bases 56 and 58, with FAT32 USB drives and the SD card. All four usual slots (248 to 251, bases 38, 56, 57 and 58) were installed with the installer's NUS download and read a USB drive.

## Download

Get the zips from the [releases page](https://github.com/Monsterray/d2x-cios/releases).

| File | What it is |
| --- | --- |
| `d2x-cios-installer-with-d2x-v11.1.0.zip` | Start here. The d2x cIOS installer with d2x v11.1.0 inside. Copy it to the root of the SD card. |
| `d2x-v11.1.0.zip` | The cIOS only: for an installer you already have in `/apps/d2x-cios-installer/`, and for ModMii. |
| `d2x-v11.1.0+dbg.zip` | A debug build with counters, a trace and a watchdog, for finding hangs. Not for everyday use. Install it only to a spare slot. |

## Install

You need a Wii with the Homebrew Channel and an SD card (or USB drive).

1. Extract `d2x-cios-installer-with-d2x-v11.1.0.zip` into the root of the SD card. It creates `apps/d2x-cios-installer/`.
2. **vWii only:** in that folder, delete `ciosmaps.xml` and rename `ciosmaps-vWii.xml` to `ciosmaps.xml`.
3. Start the d2x cIOS installer from the Homebrew Channel and follow the screen: pick the base IOS and the slot.

To use `d2x-v11.1.0.zip` instead, extract it into an existing `/apps/d2x-cios-installer/`. It replaces `ciosmaps.xml` there, so rename your old one first if you want to keep it. For ModMii, see the steps in `ReadMe.txt` inside the zip.

### Slots and bases

A cIOS is installed into a slot (a number) on top of a base IOS. These are the usual choices:

| Slot | Base | Used for |
| --- | --- | --- |
| 249 | 56 | Most games and loaders. Install this one first. |
| 250 | 57 | USB loaders |
| 248 | 38 | The few games that need it |
| 251 | 58 | USB through Nintendo's own USB 2.0 driver (the USBS module) |
| Wii Mini | IOS57 v31775 or v31776, any free slot. **Untested on hardware.** Ported from d2xl by Leseratte10; see [docs/wii-mini.md](docs/wii-mini.md). The Mini has no SD slot and no network, so the base IOS cannot be downloaded. |

Bases 37, 53, 55 and 80 are also in the maps but are rarely used. The installer downloads the base IOS from Nintendo's update server when it needs one, so the Wii needs a network connection. Which bases the maintainers checked, and how, is in [CONTRIBUTING.md](CONTRIBUTING.md#which-bases-we-test).

If you are unsure, install 249 on base 56 and see whether your loader works.

## Known limits

- A USB drive left stuck by a console reset (the reset came in the middle of a transfer) has to be unplugged before it works again. d2x now fails fast instead of hanging, but the Wii cannot switch its USB ports' power from software.
- On a SanDisk "USB 3.2Gen1" flash drive, writes land stale data. This happens with every d2x version, upstream included. Reads work. Do not install games to that drive from a loader.
- Tested USB drives were all FAT32. NTFS, WBFS partitions and other drive models were not tested on a Wii.
- Dolphin cannot run a cIOS, so nothing here can be tried in an emulator.

## For developers

Builds run in a pinned container, so you need Git, Docker and Python and nothing else:

```sh
tools/build.sh         # build the release in VERSION
tools/build.sh test    # host tests against simulated hardware
```

[CONTRIBUTING.md](CONTRIBUTING.md) has the full setup for Windows, Linux and macOS, the branch model, how a release is made and what needs a real Wii. More notes are in [docs/](docs/README.md), and verified findings in [REVIEW.md](REVIEW.md).

## Credits and licence

d2x was made by davebaol and later by blackb0x, on the work of the people below. The changes in this fork are by Monty Perrotti (Monsterray). d2x is free software under the GNU General Public License v3; see [LICENSE](LICENSE). Upstream documentation and tutorials are in the [d2x wiki](https://github.com/wiidev/d2x-cios/wiki).

 * *rodries*, for the help with EHCI improvements.
 * *Crediar*, for all I learned studying [Sneek](http://code.google.com/p/sneek) source code.
 * *Oggzee*, for his brilliant fraglist.
 * *WiiPower*, for the great help with ios reload block from usb.
 * *dragbe* and *NutNut*, for their [d2x cIOS Installer](http://code.google.com/p/d2x-cios-installer).
 * *XFlak*, for his wonderful [ModMii](http://gbatemp.net/topic/207126-modmii-for-windows) which supported d2x wads since its birth. Without ModMii d2x cIOS would probably never have existed. Also, XFlak had the original idea to replace the buggy EHCI module of cIOSX rev21 with the  working one from rev19. 
 * *[HackWii](http://www.hackwii.it)* and *[GBAtemp](http://www.gbatemp.net)* communities, for their ideas and support.
 * *Totoro*, for the official d2x logo
 * *ChaN*, for his [FatFs](http://elm-chan.org/fsw/ff/00index_e.html).
 * *Waninkoko*, for his cIOSX rev21.
 * *Team Twiizers* and *devkitPRO devs* for their great work in libogc.
 * *WiiGator*, for his work in the DIP plugin.
 * *kwiirk*, for his EHCI module.
 * *Hermes*, for his EHCI improvements.
 * *neimod*, for his [Custom IOS Module Toolkit](http://wiibrew.org/wiki/Custom_IOS_Module_Toolkit).
 * *Leseratte10*, for [d2xl](https://github.com/Leseratte10/d2xl-cios), the first cIOS for the Wii Mini (GPL-3.0). The Wii Mini maps here are ported from it.
 * All the betatesters.
