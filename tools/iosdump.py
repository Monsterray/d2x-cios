"""Check an iosdump backup, list what each IOS is, and pack WADs that restore it.

    python tools/iosdump.py check DIR
    python tools/iosdump.py wad DIR SLOT OUT.wad --keys KEYS.BIN

DIR is what tests/wii/iosdump wrote to sd:/iosdump (cert.sys, and per slot
title.tmd, title.tik, <cid>.app). `check` verifies every content against its
TMD's SHA-1 and names each IOS: a cIOS identity block (d2x and others that use
magic 0x1EE7C105), else its modules' $IOSVersion strings and other markers.

`wad` packs one slot as an installable WAD with the NAND's own TMD and ticket,
so installing it puts the slot back byte for byte. It needs the Wii common key,
read from a BootMii keys.bin (OTP at 0x100, common key at OTP+0x14); the key
is never printed. The WAD is decrypted again and checked before it is kept.
A cIOS ticket is fakesigned: install such a WAD with a WAD manager running on
an IOS that accepts it (a d2x slot, or an IOS with the signing patches).

exit codes: 0 all good; 1 a content failed its hash, or a file is missing; 2 bad usage.
"""
import argparse
import hashlib
import pathlib
import re
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

TMD_CONTENTS = 0x1E4


def parse_tmd(raw):
    tid, = struct.unpack_from(">Q", raw, 0x18C)
    ver, n = struct.unpack_from(">HH", raw, 0x1DC)
    sysver, = struct.unpack_from(">Q", raw, 0x184)
    contents = []
    for i in range(n):
        o = TMD_CONTENTS + 36 * i
        cid, idx, typ, size = struct.unpack_from(">IHHQ", raw, o)
        contents.append(dict(cid=cid, index=idx, type=typ, size=size, sha1=raw[o + 16:o + 36]))
    return dict(tid=tid, version=ver, sysver=sysver, contents=contents, issuer=raw[0x140:0x180].rstrip(b"\0").decode())


def markers(data):
    out = []
    for m in re.finditer(rb"\$IOSVersion:\s*([^\0$]{3,80})", data):
        out.append(m[1].decode("ascii", "replace").strip())
    for word in (b"d2x", b"Waninkoko", b"waninkoko", b"Hermes", b"hermes", b"mload", b"MLOAD", b"EHCI",
                 b"rodries", b"Rodries", b"FAT module", b"USB2", b"dip_plugin", b"cIOSX", b"Wiigator"):
        if word in data:
            out.append(word.decode())
    return out


def identity(data):
    if len(data) >= 0x30 and struct.unpack_from(">I", data, 0)[0] == 0x1EE7C105:
        _, _, ver, base = struct.unpack_from(">IIII", data, 0)
        name = data[16:32].split(b"\0")[0].decode("ascii", "replace")
        vstr = data[32:48].split(b"\0")[0].decode("ascii", "replace")
        return f"{name} v{ver} {vstr} base {base}"
    return None


def check(d):
    bad = 0
    slots = sorted((p for p in d.iterdir() if p.is_dir() and p.name.isdigit()), key=lambda p: int(p.name))
    if not (d / "cert.sys").is_file():
        print("cert.sys: missing (needed for a WAD)")
        bad += 1
    for s in slots:
        tmd = parse_tmd((s / "title.tmd").read_bytes())
        tik = s / "title.tik"
        tikinfo = ""
        if tik.is_file():
            t = tik.read_bytes()
            tikinfo = f"ticket {len(t)} bytes, issuer {t[0x140:0x180].rstrip(bytes(1)).decode(errors='replace')}"
        else:
            tikinfo = "NO TICKET"
            bad += 1
        print(f"IOS{s.name}: v{tmd['version']} ({tmd['version'] >> 8}.{tmd['version'] & 0xFF}), "
              f"{len(tmd['contents'])} contents, {tikinfo}")
        ident, marks, ok = None, [], 0
        for c in tmd["contents"]:
            f = s / f"{c['cid']:08x}.app"
            if not f.is_file():
                print(f"  content {c['cid']:08x}: MISSING")
                bad += 1
                continue
            data = f.read_bytes()
            if len(data) != c["size"] or hashlib.sha1(data).digest() != c["sha1"]:
                print(f"  content {c['cid']:08x}: HASH OR SIZE MISMATCH ({len(data)} vs {c['size']})")
                bad += 1
                continue
            ok += 1
            ident = ident or identity(data)
            marks += [m for m in markers(data) if m not in marks]
        print(f"  {ok}/{len(tmd['contents'])} contents match the TMD")
        if ident:
            print(f"  identity: {ident}")
        for m in marks:
            print(f"  {m}")
    print(f"{len(slots)} slots, {'all good' if not bad else f'{bad} problem(s)'}")
    return 1 if bad else 0


def certs(raw):
    """name -> cert bytes, for each certificate in cert.sys"""
    sig_len = {0x10000: 0x200, 0x10001: 0x100, 0x10002: 0x3C}
    key_len = {0: 0x200 + 4 + 0x34, 1: 0x100 + 4 + 0x34, 2: 0x3C + 0x3C}
    out, o = {}, 0
    while o + 4 <= len(raw):
        st, = struct.unpack_from(">I", raw, o)
        if st not in sig_len:
            break
        body = o + 4 + sig_len[st]
        body = (body + 0x3F) & ~0x3F
        kt, = struct.unpack_from(">I", raw, body + 0x40)
        name = raw[body + 0x44:body + 0x84].split(b"\0")[0].decode()
        end = body + 0x88 + key_len[kt]
        end = (end + 0x3F) & ~0x3F
        out[name] = raw[o:end]
        o = end
    return out


def aes_cbc(key, iv, data, enc):
    c = Cipher(algorithms.AES(key), modes.CBC(iv))
    x = c.encryptor() if enc else c.decryptor()
    return x.update(data) + x.finalize()


def pad(b, n):
    return b + bytes(-len(b) % n)


def wad(d, slot, out, keys):
    s = d / str(slot)
    tmd_raw = (s / "title.tmd").read_bytes()
    tik = (s / "title.tik").read_bytes()
    tmd = parse_tmd(tmd_raw)
    kb = pathlib.Path(keys).read_bytes()
    common = kb[0x114:0x124]
    if len(common) != 16:
        sys.exit("keys.bin: too short for the OTP's common key")
    if tik[0x1F1] != 0:
        sys.exit(f"ticket uses common key index {tik[0x1F1]}, not the Wii common key")
    tkey = aes_cbc(common, tik[0x1DC:0x1E4] + bytes(8), tik[0x1BF:0x1CF], False)
    cs = certs((d / "cert.sys").read_bytes())
    try:
        chain = cs["CA00000001"] + cs["CP00000004"] + cs["XS00000003"]
    except KeyError as e:
        sys.exit(f"cert.sys: no {e} certificate")
    data = b""
    for c in tmd["contents"]:
        raw = (s / f"{c['cid']:08x}.app").read_bytes()
        if hashlib.sha1(raw).digest() != c["sha1"]:
            sys.exit(f"content {c['cid']:08x}: does not match the TMD")
        enc = aes_cbc(tkey, struct.pack(">H", c["index"]) + bytes(14), pad(raw, 16), True)
        data += pad(enc, 0x40)
    hdr = struct.pack(">I2sHIIIIII", 0x20, b"Is", 0, len(chain), 0, len(tik), len(tmd_raw), len(data), 0)
    blob = pad(hdr, 0x40) + pad(chain, 0x40) + pad(tik, 0x40) + pad(tmd_raw, 0x40) + data
    # check: decrypt what was written
    o = 0
    for c in tmd["contents"]:
        n = (c["size"] + 15) & ~15
        start = len(blob) - len(data) + o
        dec = aes_cbc(tkey, struct.pack(">H", c["index"]) + bytes(14), blob[start:start + n], False)[:c["size"]]
        if hashlib.sha1(dec).digest() != c["sha1"]:
            sys.exit(f"WAD check: content {c['cid']:08x} does not decrypt to the TMD's hash")
        o += (n + 0x3F) & ~0x3F
    pathlib.Path(out).write_bytes(blob)
    print(f"{out}: IOS{slot} v{tmd['version']}, {len(tmd['contents'])} contents, {len(blob)} bytes, checked")
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(prog="iosdump.py", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check", help="verify a backup and name each IOS")
    c.add_argument("dir", type=pathlib.Path)
    w = sub.add_parser("wad", help="pack one slot as a WAD that restores it")
    w.add_argument("dir", type=pathlib.Path)
    w.add_argument("slot", type=int)
    w.add_argument("out")
    w.add_argument("--keys", required=True, help="a BootMii keys.bin")
    a = p.parse_args(argv)
    if a.cmd == "check":
        return check(a.dir)
    return wad(a.dir, a.slot, a.out, a.keys)


if __name__ == "__main__":
    sys.exit(main())
