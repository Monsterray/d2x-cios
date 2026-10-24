"""Repair the bench USB drive's FAT32 root directory in place, on Windows.

    python -I tools/fat32_repair_live.py D: BACKUPDIR [--dry-run]

Run elevated, with no program holding files on the drive open. It locks and
dismounts the volume, reads the current root directory and FATs, checks that
the damage is the one analysed on 2026-10-24 (sector 1 of the root cluster
overwritten with a USB CBW by the d2x write bug), and then:

  - puts a "games" entry (the orphaned directory at cluster 6) where the CBW is;
  - removes a "wbfs" entry made since in the same sector, if its directory is
    empty, and frees its cluster, so the original WBFS entry (cluster 9, in
    the next sector) is the one that counts;
  - marks the rest of that sector's empty entries deleted (0xE5) instead of
    end-of-directory (0x00), so a directory scan reaches WBFS;
  - frees clusters 3, 4, 5 and 13 (the old System Volume Information, its two
    files, and an empty orphaned directory) in both FATs.

Every sector it changes is saved to BACKUPDIR first, written, read back and
compared. --dry-run does all the checks and saves, and writes nothing.
"""
import ctypes
import msvcrt
import os
import struct
import sys
from ctypes import wintypes

SECTOR = 512
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = wintypes.HANDLE
k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                            wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
k32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                                wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                                wintypes.LPVOID]
GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
FILE_SHARE_READ, FILE_SHARE_WRITE = 1, 2
OPEN_EXISTING = 3
FSCTL_LOCK_VOLUME, FSCTL_UNLOCK_VOLUME, FSCTL_DISMOUNT_VOLUME = 0x90018, 0x9001C, 0x90020
INVALID = wintypes.HANDLE(-1).value


def ioctl(h, code):
    n = wintypes.DWORD()
    return k32.DeviceIoControl(h, code, None, 0, None, 0, ctypes.byref(n), None)


def cluster_of(e):
    return (struct.unpack("<H", e[20:22])[0] << 16) | struct.unpack("<H", e[26:28])[0]


def main():
    letter, backup = sys.argv[1].rstrip("\\:"), sys.argv[2]
    dry = "--dry-run" in sys.argv
    os.makedirs(backup, exist_ok=True)
    path = rf"\\.\{letter}:"
    h = k32.CreateFileW(path, GENERIC_READ | (0 if dry else GENERIC_WRITE),
                        FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, 0, None)
    if h == INVALID:
        sys.exit(f"cannot open {path} (error {ctypes.get_last_error()}): run elevated")
    if not ioctl(h, FSCTL_LOCK_VOLUME):
        # something has files open: dismounting forces them closed, then lock
        if not dry:
            ioctl(h, FSCTL_DISMOUNT_VOLUME)
        if not ioctl(h, FSCTL_LOCK_VOLUME) and not dry:
            sys.exit(f"cannot lock {path} (error {ctypes.get_last_error()}): close programs using it")
    if not dry:
        ioctl(h, FSCTL_DISMOUNT_VOLUME)
    fd = msvcrt.open_osfhandle(h, os.O_RDONLY if dry else os.O_RDWR)

    # Sector numbers below are relative to the volume (the partition)
    def read(s, n=1):
        os.lseek(fd, s * SECTOR, 0)
        return os.read(fd, n * SECTOR)

    def write(s, data):
        os.lseek(fd, s * SECTOR, 0)
        assert os.write(fd, data) == len(data)

    try:
        bs = read(0)
        bps, spc, rsv, nfats = struct.unpack("<HBHB", bs[11:17])
        fatsz, = struct.unpack("<I", bs[36:40])
        rootclus, = struct.unpack("<I", bs[44:48])
        if bs[510:512] != b"\x55\xaa" or bps != SECTOR or nfats != 2 or rootclus != 2:
            sys.exit("not the FAT32 volume analysed (boot sector differs)")
        fat1, fat2, data = rsv, rsv + fatsz, rsv + 2 * fatsz
        clus = lambda c: data + (c - 2) * spc

        def fat_get(c):
            s = read(fat1 + c * 4 // SECTOR)
            return struct.unpack("<I", s[c * 4 % SECTOR:c * 4 % SECTOR + 4])[0] & 0x0FFFFFFF

        root1 = clus(2) + 1
        old = read(root1)
        if old[:4] != b"USBC":
            sys.exit(f"volume sector {root1} does not start with the CBW: already repaired?")
        nxt = read(root1 + 1)
        if nxt[:11] != b"WBFS       " or cluster_of(nxt[:32]) != 9:
            sys.exit("the original WBFS entry is not in the next sector")
        games = read(clus(6))
        if games[:11] != b".          " or games[32:43] != b"..         " or cluster_of(games[32:64]) != 0:
            sys.exit("cluster 6 is not a directory under the root")

        new = bytearray(old)
        e = bytearray(games[:32])
        e[0:11], e[11], e[12] = b"GAMES      ", 0x10, 0x08   # shown as "games"
        new[0:32] = e
        free = [3, 4, 5, 13]
        rename_original = False
        for o in range(32, SECTOR, 32):
            ent = new[o:o + 32]
            if ent[0] == 0:
                new[o] = 0xE5
            elif ent[0] != 0xE5 and ent[11] != 0x0F and ent[11] & 0x10 and ent[0:11] == b"WBFS       ":
                c = cluster_of(ent)
                d = read(clus(c), spc)
                rest = [d[i:i + 32] for i in range(64, len(d), 32) if d[i] not in (0, 0xE5)]
                if d[:11] != b".          " or rest:
                    # It holds files: keep it, and bring the original back
                    # as WBFSOLD so the two names do not clash
                    print(f"keeping the new wbfs folder (cluster {c}, not empty); "
                          "the original comes back as WBFSOLD")
                    rename_original = True
                    continue
                if fat_get(c) < 0x0FFFFFF8:
                    sys.exit(f"the new wbfs folder (cluster {c}) has more than one cluster: stop")
                new[o] = 0xE5
                # and its LFN entries just before it
                p = o - 32
                while p >= 32 and new[p + 11] == 0x0F and new[p] != 0xE5:
                    new[p] = 0xE5
                    p -= 32
                free.append(c)
                print(f"removing the new empty wbfs folder at cluster {c}")
        for c in free:
            if fat_get(c) == 0:
                sys.exit(f"cluster {c} is already free: the state differs, nothing written")

        changes = {root1: bytes(new)}
        if rename_original:
            # no WBFSOLD anywhere in the root's first cluster already
            rootraw = read(clus(2), spc)
            if any(rootraw[i:i + 11] == b"WBFSOLD    " for i in range(0, len(rootraw), 32)):
                sys.exit("a WBFSOLD entry exists already: stop, nothing written")
            n = bytearray(nxt)
            n[0:11] = b"WBFSOLD    "
            n[12] = 0                       # upper case, as written
            changes[root1 + 1] = bytes(n)
        for base in (fat1, fat2):
            for c in free:
                s = base + c * 4 // SECTOR
                sec = bytearray(changes.get(s) or read(s))
                o = c * 4 % SECTOR
                v = struct.unpack("<I", sec[o:o + 4])[0]
                sec[o:o + 4] = struct.pack("<I", v & 0xF0000000)
                changes[s] = bytes(sec)

        for s, b in changes.items():
            open(os.path.join(backup, f"before_vol{s}.bin"), "wb").write(read(s))
            open(os.path.join(backup, f"after_vol{s}.bin"), "wb").write(b)
        print(f"{len(changes)} sectors to change (volume-relative): {sorted(changes)}; saved to {backup}")
        if dry:
            print("dry run: nothing written")
            return
        for s, b in changes.items():
            write(s, b)
        # os.fsync() refuses a raw volume handle; flush it directly
        k32.FlushFileBuffers(h)
        for s, b in changes.items():
            print(f"volume sector {s}: {'written and verified' if read(s) == b else 'READ BACK DIFFERS'}")
    finally:
        try:
            ioctl(h, FSCTL_UNLOCK_VOLUME)
        finally:
            os.close(fd)


if __name__ == "__main__":
    main()
