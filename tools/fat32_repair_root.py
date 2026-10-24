"""Build, check and apply the repair of the bench USB drive's FAT32 root
directory (sector 1 of the root cluster overwritten with a USB CBW by the d2x
write bug, 2026-10-24).

    python -I tools/fat32_repair_root.py build SOURCE SURVEY.json PATCHDIR
    python -I tools/fat32_repair_root.py apply DEVICE PATCHDIR

build (read-only, from the disk image or the disk): makes the new sectors in
PATCHDIR with patch.json; check them with
    FAT32_PATCH=PATCHDIR python -I tools/fat32_orphans.py SOURCE OUTDIR
apply (elevated, writes): saves the device's current bytes of each sector to
PATCHDIR/before_<lba>.bin, refuses unless they still match what build saw,
writes the new sectors, then reads them back and compares.
"""
import hashlib
import json
import os
import struct
import sys

SECTOR = 512


def entry_cluster(e):
    return (struct.unpack("<H", e[20:22])[0] << 16) | struct.unpack("<H", e[26:28])[0]


def build(src, survey_path, pdir):
    s = json.load(open(survey_path))
    spc, data = s["sectors_per_cluster"], s["data_lba"]
    fat1 = s["fat_lba"]
    fat2 = fat1 + s["fat_sectors"]
    root_lba = data + (s["root_cluster"] - 2) * spc
    bad_lba = root_lba + 1
    os.makedirs(pdir, exist_ok=True)
    with open(src, "rb") as f:
        def read(lba):
            f.seek(lba * SECTOR)
            return f.read(SECTOR)

        old = read(bad_lba)
        if old[:4] != b"USBC":
            sys.exit(f"LBA {bad_lba} does not start with USBC: not the damage this repairs")
        nxt = read(bad_lba + 1)
        if nxt[:11] != b"WBFS       " or entry_cluster(nxt) != 9:
            sys.exit("the WBFS entry is not where it was: stop")
        # games: the orphaned directory at cluster 6, whose ".." is the root
        games = read(data + (6 - 2) * spc)
        if games[:11] != b".          " or games[32:43] != b"..         " or entry_cluster(games[32:64]) != 0:
            sys.exit("cluster 6 is not a directory under the root")
        dot = games[:32]
        e = bytearray(dot)
        e[0:11] = b"GAMES      "
        e[11] = 0x10           # directory
        e[12] = 0x08           # NT case flag: show the base name in lower case, "games"
        new = bytearray(SECTOR)
        new[0:32] = e
        # Windows' new System Volume Information entries (LFN, LFN, SFN) stay as they are
        svi = old[32:128]
        if svi[11] != 0x0F or svi[32 + 11] != 0x0F or svi[64:75] != b"SYSTEM~1   ":
            sys.exit("Windows' System Volume Information entries are not at 544..639")
        new[32:128] = svi
        # the rest: deleted entries, not zeros, so a directory scan goes on to WBFS
        for o in range(128, SECTOR, 32):
            new[o] = 0xE5
        sectors = {bad_lba: bytes(new)}

        # free the dead clusters in both FATs: 3, 4, 5 (the old System Volume
        # Information and its two files) and 13 (an empty orphaned directory)
        dead = [3, 4, 5, 13]
        for base in (fat1, fat2):
            lba = base + (dead[0] * 4) // SECTOR
            sec = bytearray(read(lba))
            for c in dead:
                assert (c * 4) // SECTOR == 0
                v = struct.unpack("<I", sec[c * 4:c * 4 + 4])[0]
                if v & 0x0FFFFFFF == 0:
                    sys.exit(f"cluster {c} is already free")
                sec[c * 4:c * 4 + 4] = struct.pack("<I", v & 0xF0000000)
            sectors[lba] = bytes(sec)

        meta = {"sectors": {}, "before_sha256": {}}
        for lba, b in sectors.items():
            name = f"new_{lba}.bin"
            open(os.path.join(pdir, name), "wb").write(b)
            meta["sectors"][str(lba)] = name
            meta["before_sha256"][str(lba)] = hashlib.sha256(read(lba)).hexdigest()
        with open(os.path.join(pdir, "patch.json"), "w", newline="\n") as out:
            json.dump(meta, out, indent=1)
    print(f"patch of {len(sectors)} sectors in {pdir}: {sorted(sectors)}")


def apply(dev, pdir):
    meta = json.load(open(os.path.join(pdir, "patch.json")))
    with open(dev, "r+b", buffering=0) as f:
        for lba, name in meta["sectors"].items():
            lba = int(lba)
            f.seek(lba * SECTOR)
            cur = f.read(SECTOR)
            open(os.path.join(pdir, f"before_{lba}.bin"), "wb").write(cur)
            if hashlib.sha256(cur).hexdigest() != meta["before_sha256"][str(lba)]:
                sys.exit(f"LBA {lba} changed since the patch was built: nothing written")
        for lba, name in meta["sectors"].items():
            lba = int(lba)
            new = open(os.path.join(pdir, name), "rb").read()
            f.seek(lba * SECTOR)
            f.write(new)
        os.fsync(f.fileno())
        for lba, name in meta["sectors"].items():
            lba = int(lba)
            f.seek(lba * SECTOR)
            ok = f.read(SECTOR) == open(os.path.join(pdir, name), "rb").read()
            print(f"LBA {lba}: {'written and verified' if ok else 'READ BACK DIFFERS'}")


if __name__ == "__main__":
    if sys.argv[1] == "build":
        build(sys.argv[2], sys.argv[3], sys.argv[4])
    elif sys.argv[1] == "apply":
        apply(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
