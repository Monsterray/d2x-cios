"""Copy a whole raw disk to an image file, read-only on the disk.

    python -I tools/image_disk.py \\\\.\\PhysicalDrive1 C:\\wii-backup\\sandisk.img

Needs an elevated prompt on Windows. Prints progress every 1 GiB and the
image's SHA-256 at the end. The image is a full backup: a repair can be
tried on it first, and the disk restored from it if anything goes wrong.
"""
import hashlib
import os
import sys
import time

dev, out = sys.argv[1], sys.argv[2]
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
CHUNK = 4 << 20
h = hashlib.sha256()
done, t0, mark = 0, time.time(), 1 << 30
with open(dev, "rb", buffering=0) as src, open(out, "wb") as dst:
    while True:
        try:
            b = src.read(CHUNK)
        except OSError:
            # The end of a raw device can refuse a partial chunk: finish by sectors
            b = b""
            while True:
                try:
                    s = src.read(512)
                except OSError:
                    s = b""
                if not s:
                    break
                b += s
        if not b:
            break
        dst.write(b)
        h.update(b)
        done += len(b)
        if done >= mark:
            print(f"{done / 2**30:.1f} GiB, {done / 2**20 / (time.time() - t0):.0f} MiB/s", flush=True)
            mark += 1 << 30
print(f"{done} bytes in {time.time() - t0:.0f} s, sha256 {h.hexdigest()}")
