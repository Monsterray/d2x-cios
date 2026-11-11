# Wii Mini (untested on hardware)

d2x has maps for the Wii Mini's base IOS57 v31775 and v31776. This is a port of the Wii Mini work in [d2xl](https://github.com/Leseratte10/d2xl-cios) (d2xl 1-beta2, 2022-08-18, GPL-3.0) by Leseratte10. **No Wii Mini has run this build.** Nothing here was checked against the real base IOS either: the maps are d2xl's, as d2xl's last commit has them, with the changes listed below.

## What d2xl changed for the Mini

d2xl is d2x v10 (r43) with these Mini changes. Everything else it has (the d2xl name, `console`/`region` map flags, base 59, reproducible build scripts) is not Mini-specific.

| d2xl change | Where | What it is |
| --- | --- | --- |
| Two `console="mini"` bases in `ciosmaps.xml`: IOS57 v31776 and IOS57 v31775 | `data/ciosmaps.xml` | The kernel header, DI and kernel patches are those of base 57 v5918, at the same offsets. The content ids differ (see below). |
| IOP module `09/27/11 18:50:17` (`0x4E821AE9`) | `mload-module/detect.c`, `mload-module/patches.c`, `ffs-plugin/patches.c` | The only module the Mini's IOS has that no other base has. It gets the same syscall base (`0xFFFF93D0`), SWI hook and FFS patch as the IOP of base 56/57/58. d2xl's comments call it "Mini IOS 56v31519". |
| Comments only | `dip-plugin`, `es-plugin`, `ffs-plugin`, `mload-module` | The Mini's DIP (`06/03/09 07:49:09`), ES (`03/03/10 10:40:14`) and FFS (`11/24/08 15:36:10`) modules are the same builds as base 56/57/58's, so the existing cases already match. d2xl only added a comment to each. |

Not changed by d2xl, as far as its tree shows: anything about the SD card, Wi-Fi or USB ports. The Mini has no SD slot and no Wi-Fi, but d2xl neither disables nor works around that.

## What this tree has

- `data/ciosmaps.xml`: the two bases, taken from d2xl with these differences:
  - **No SDHC module.** d2xl still has a separate SDHC module; this fork serves SD from the EHCI module, like every other base here. So 24 contents with 6 modules, not 25 with 7.
  - **Module ids follow the last original id.** For v31775 d2xl lists the modules from `0x12`, which is also the id of the first original content: two contents with one id. `tools/check_ciosmaps.py` now flags that. This map puts the modules at `0x13`..`0x18`. For v31776 the ids move down by one at the end because SDHC is gone. Nothing reads a module's content id except the installer when it writes the TMD.
  - **No d2xl marks.** d2xl writes `d2xl` and the base IOS version (a u16 at offset 0x30 of the first content) into the kernel header. Every other base here keeps `d2x` and zeros, so the Mini does too. This means tools that read the base revision from that field show nothing, as for the other bases.
  - **No `console`/`region` attributes.** d2xl's installer was meant to filter on them, but the installer in use here ignores them. Both bases were marked `region="pal"` in d2xl, which cannot be right for both.
- `source`: the IOP module `0x4E821AE9`, as above. The SD code needed no change: the SD driver starts only when a loader opens `/dev/sdio/sdhc`, and a failed open or a missing card returns an error ([source/cios-lib/sdio.c](../source/cios-lib/sdio.c), `sdio_Startup` and `__sd0_initio`).
- The release build for the other bases differs only in MLOAD (+60 bytes) and FFSP (+16 bytes); DIPP, EHCI, ES, FAT and USBS are byte-identical (checked with `SOURCE_DATE_EPOCH` fixed and the same `-dirty` stamp on both sides).

## What was not checked, and what a hardware test needs

The maps and the IOP case have only been compared with d2xl's text. They have not been compared with Nintendo's files. To check them:

1. **The base IOS files**: IOS57 v31775 and v31776 (title `0000000100000039`), each as `title.tmd` and the decrypted `<cid>.app` files in `IOS57-v31775/` and `IOS57-v31776/`. Then run `python tools/check_ciosmaps.py build/ciosmaps.xml BASES_DIR`. It checks the TMD version and content ids, each content's SHA-1, and every patch's `originalbytes` at its offset. It reports what it checked per base. A Mini's NAND is the other source: its own copy of the IOS, dumped with `tools/iosdump.py`. (Which of the two versions is NTSC and which is PAL is not known here; d2xl's comments disagree with its flags.)
2. **The module dates**: the modules inside that IOS (`DIP`, `ES`, `FFS`, `IOSP`) carry build dates. The IOP date must be `09/27/11 18:50:17` at `0xFFFF880B` (virtual address), and the DIP/ES/FFS dates must match the table above. A mismatch makes MLOAD refuse to start (`DETECT_ERROR`). Nobody has confirmed here that v31775 and v31776 carry the same dates as d2xl's "56v31519".
3. **A Wii Mini**, with someone next to it and a way to get HBC back, since a wrong patch can leave the IOS unable to start, and there is no recovery from IOS (see CONTRIBUTING.md). Needs:
   - a way to run the installer: the Mini has no SD slot and no network, so the installer cannot download the base IOS from NUS and can only be started from a USB drive. How d2xl's users did the install is not in its tree. Unknown: whether the installer in use can take the base from the NAND or from the USB drive.
   - a spare slot and a NAND backup (BootMii as boot2 cannot be installed on a Mini).
   - first test: `umsbench`/`d2xdiag` (see `tests/wii/`) from the Homebrew Channel on USB, to read the cIOS identity and a USB drive. SD does not apply.
