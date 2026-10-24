"""Read-only FAT32 survey: find cluster chains no directory entry reaches.

    python -I tools/fat32_orphans.py \\\\.\\PhysicalDrive1 OUTDIR [LBA ...]

Needs an elevated (administrator) prompt on Windows to open the raw disk. It
opens the device read-only and never writes to it. It saves to OUTDIR:

  survey.json     geometry, the root directory's entries, every orphaned
                  chain (head cluster, length, whether it is a directory and
                  the names inside it), and the extra LBAs asked for
  root.bin        the root directory's clusters, as on disk
  dir_<head>.bin  each orphaned directory's clusters, as on disk
  lba_<n>.bin     each extra LBA asked for

Written for the bench drive the d2x write bug damaged (root directory sector
overwritten with a USB Bulk-Only CBW), but general for an MBR FAT32 volume.
"""
import json
import os
import struct
import sys

SECTOR = 512


def main():
    dev, outdir = sys.argv[1], sys.argv[2]
    extra = [int(x) for x in sys.argv[3:]]
    os.makedirs(outdir, exist_ok=True)
    # FAT32_PATCH=DIR: read the sectors in DIR/patch.json over the device's,
    # to check a repair before it is written
    patch = {}
    if os.environ.get("FAT32_PATCH"):
        pdir = os.environ["FAT32_PATCH"]
        for lba, name in json.load(open(os.path.join(pdir, "patch.json")))["sectors"].items():
            patch[int(lba)] = open(os.path.join(pdir, name), "rb").read()
    with open(dev, "rb", buffering=0) as f:
        def read(lba, n=1):
            f.seek(lba * SECTOR)
            raw = bytearray(f.read(SECTOR * n))
            for p, data in patch.items():
                if lba <= p < lba + n:
                    raw[(p - lba) * SECTOR:(p - lba + 1) * SECTOR] = data
            return bytes(raw)

        mbr = read(0)
        if mbr[510:512] != b"\x55\xaa":
            sys.exit("no MBR signature")
        part = None
        for i in range(4):
            e = mbr[446 + 16 * i:462 + 16 * i]
            if e[4] in (0x0B, 0x0C):
                part = struct.unpack("<II", e[8:16])
                break
        if not part:
            sys.exit("no FAT32 partition in the MBR")
        start, count = part
        bs = read(start)
        bps, spc, rsv, nfats = struct.unpack("<HBHB", bs[11:17])
        fatsz, = struct.unpack("<I", bs[36:40])
        rootclus, = struct.unpack("<I", bs[44:48])
        fsinfo, backup = struct.unpack("<HH", bs[48:52])
        if bps != SECTOR or bs[510:512] != b"\x55\xaa":
            sys.exit("unexpected boot sector")
        fat_lba = start + rsv
        data_lba = start + rsv + nfats * fatsz
        nclus = (count - rsv - nfats * fatsz) // spc
        csize = spc * SECTOR

        # The first FAT, whole (fatsz sectors, a few MB)
        fatraw = b""
        step = 2048
        for off in range(0, fatsz, step):
            fatraw += read(fat_lba + off, min(step, fatsz - off))
        fat = struct.unpack(f"<{len(fatraw) // 4}I", fatraw)
        fat = [x & 0x0FFFFFFF for x in fat]

        def chain(c):
            out, seen = [], set()
            while 2 <= c < nclus + 2 and c not in seen:
                out.append(c)
                seen.add(c)
                n = fat[c]
                if n >= 0x0FFFFFF8 or n < 2 or n == 0x0FFFFFF7:
                    break
                c = n
            return out

        def read_clusters(cl):
            return b"".join(read(data_lba + (c - 2) * spc, spc) for c in cl)

        def entries(raw):
            """Short entries (LFN joined), skipping deleted and end."""
            out, lfn = [], []
            for i in range(0, len(raw), 32):
                e = raw[i:i + 32]
                if e[0] == 0:
                    break
                if e[0] == 0xE5:
                    lfn = []
                    continue
                attr = e[11]
                if attr == 0x0F:
                    part = e[1:11] + e[14:26] + e[28:32]
                    lfn.insert(0, part.decode("utf-16-le", "replace").split("\x00")[0].rstrip("￿"))
                    continue
                short = e[0:8].decode("latin-1").rstrip() + (
                    "." + e[8:11].decode("latin-1").rstrip() if e[8:11].strip() else "")
                clus = (struct.unpack("<H", e[20:22])[0] << 16) | struct.unpack("<H", e[26:28])[0]
                size, = struct.unpack("<I", e[28:32])
                out.append({"offset": i, "short": short, "long": "".join(lfn) or None,
                            "attr": attr, "cluster": clus, "size": size,
                            "raw": e.hex()})
                lfn = []
            return out

        # Everything reachable from the root
        reached = set()
        rootchain = chain(rootclus)
        reached.update(rootchain)
        rootraw = read_clusters(rootchain)
        open(os.path.join(outdir, "root.bin"), "wb").write(rootraw)
        rootents = entries(rootraw)

        def walk(dirclusters, depth=0):
            if depth > 64:
                return
            for e in entries(read_clusters(dirclusters)):
                if e["short"] in (".", "..") or e["attr"] & 0x08 or e["cluster"] < 2:
                    continue
                cl = chain(e["cluster"])
                if any(c in reached for c in cl):
                    continue
                reached.update(cl)
                if e["attr"] & 0x10:
                    walk(cl, depth + 1)

        walk(rootchain)

        # Allocated, not reached; heads are those nothing points to
        allocated = [c for c in range(2, nclus + 2) if fat[c] != 0 and fat[c] != 0x0FFFFFF7]
        orphan = set(c for c in allocated if c not in reached)
        pointed = set(fat[c] for c in orphan)
        heads = sorted(c for c in orphan if c not in pointed)
        chains = []
        for h in heads:
            cl = chain(h)
            first = read_clusters([h])
            info = {"head": h, "clusters": len(cl), "bytes": len(cl) * csize,
                    "lba": data_lba + (h - 2) * spc, "first16": first[:16].hex()}
            ents = entries(first)
            if ents and ents[0]["short"] == "." and len(ents) > 1 and ents[1]["short"] == "..":
                info["directory"] = True
                info["parent"] = ents[1]["cluster"]
                raw = read_clusters(cl)
                open(os.path.join(outdir, f"dir_{h}.bin"), "wb").write(raw)
                info["entries"] = [{k: e[k] for k in ("short", "long", "attr", "cluster", "size")}
                                   for e in entries(raw)]
            chains.append(info)

        for lba in extra:
            open(os.path.join(outdir, f"lba_{lba}.bin"), "wb").write(read(lba))

        survey = {
            "partition_start": start, "partition_sectors": count, "sectors_per_cluster": spc,
            "reserved": rsv, "fats": nfats, "fat_sectors": fatsz, "root_cluster": rootclus,
            "fsinfo_sector": fsinfo, "backup_boot_sector": backup, "fat_lba": fat_lba,
            "data_lba": data_lba, "clusters": nclus, "root_chain": rootchain,
            "root_entries": rootents, "allocated": len(allocated), "reached": len(reached),
            "orphaned": len(orphan), "orphan_chains": chains, "extra_lbas": extra,
        }
        with open(os.path.join(outdir, "survey.json"), "w", encoding="utf-8", newline="\n") as out:
            json.dump(survey, out, indent=1)
        print(f"{len(allocated)} clusters allocated, {len(reached)} reached, "
              f"{len(orphan)} orphaned in {len(chains)} chains "
              f"({sum(1 for c in chains if c.get('directory'))} directories). Saved to {outdir}")


if __name__ == "__main__":
    main()
