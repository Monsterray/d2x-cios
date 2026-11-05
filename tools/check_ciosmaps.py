#!/usr/bin/env python3
"""Check data/ciosmaps*.xml against Nintendo base IOS contents.

    check_ciosmaps.py MAP.xml BASES_DIR [--modules DIR] [--version 11.1.0-rc1]

BASES_DIR holds one folder per base, IOS<n>-v<version>/, with title.tmd and the
decrypted <cid>.app files.  Per base the check reports: the version found against
the map's, the TMD content ids and count against the map, each content's SHA-1
against the TMD, every patch's originalbytes at its offset, the counts the
installer insists on (basescount, contentscount, modulescount, patchscount, patch
size), module names and tmdmoduleid, and unresolved placeholders (the template is
expanded the way replace.awk does it).  A base whose exact version is not in
BASES_DIR is listed as not checked (a failure with --strict).  Exit status 1 when
anything mismatches.  The base files are Nintendo's and are not kept in the
repository.
"""
import argparse
import hashlib
import re
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

KNOWN_MODULES = {"MLOAD", "FAT", "EHCI", "USBS", "DIPP", "ES", "FFSP"}


def expand(text, version):
    """Do what replace.awk does for the template's variables."""
    rel = "d2x-v" + version
    m = re.match(r"(\d+)\.?(.*)", version)
    major = int(m.group(1))
    minor_str = m.group(2)[:15]
    hex_major = ",".join("0x%02x" % ((major >> s) & 0xFF) for s in (24, 16, 8, 0))
    hex_minor = ",".join("0x%02x" % (ord(minor_str[i]) if i < len(minor_str) else 0) for i in range(16))
    v = {"${HEX_MAJOR_VER}": hex_major, "${HEX_MINOR_VER}": hex_minor,
         "${IOS_REV}": str(21000 + major), "${RELEASE_NAME}": rel,
         "${MAJOR_VER}": str(major), "${MINOR_VER}": minor_str}
    return re.sub(r"\$\{[^}.]+\}", lambda mo: v.get(mo.group(0), mo.group(0)), text)


def parse_tmd(b):
    ver, nc = struct.unpack(">HH", b[0x1DC:0x1E0])
    cs = []
    for k in range(nc):
        o = 0x1E4 + k * 36
        cid, idx, typ, size = struct.unpack(">IHHQ", b[o:o + 16])
        cs.append(dict(cid=cid, idx=idx, type=typ, size=size, hash=b[o + 16:o + 36]))
    return ver, cs


def hexbytes(s):
    try:
        return bytes(int(x, 16) for x in s.split(","))
    except ValueError:
        return b""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("map")
    ap.add_argument("bases")
    ap.add_argument("--modules", help="folder with EHCI.app, USBS.app, ... (presence and ELF header)")
    ap.add_argument("--version", default="11.1.0-rc1")
    ap.add_argument("--strict", action="store_true", help="a base missing from BASES_DIR is a failure")
    a = ap.parse_args()
    text = Path(a.map).read_text(encoding="utf-8")
    expanded = expand(text, a.version)
    unresolved = set(re.findall(r"\$\{[^}.]+\}", expanded))
    root = ET.fromstring(expanded)
    bad = 0
    unchecked = []

    def err(base, msg):
        nonlocal bad
        bad += 1
        print("  MISMATCH base %s: %s" % (base, msg))

    if unresolved:
        print("UNRESOLVED placeholders:", sorted(unresolved))
        bad += 1
    groups = root.findall("ciosgroup")
    if int(root.get("ciosgroupscount", -1)) != len(groups):
        print("ciosgroupscount %s != %d" % (root.get("ciosgroupscount"), len(groups)))
        bad += 1
    for g in groups:
        bases = g.findall("base")
        print("group %s rev %s: %d bases" % (g.get("name"), g.get("version"), len(bases)))
        if int(g.get("basescount")) != len(bases):
            err("-", "basescount %s != %d" % (g.get("basescount"), len(bases)))
        for b in bases:
            ios, mver = int(b.get("ios")), int(b.get("version"))
            tag = "%d (map v%d)" % (ios, mver)
            cs = b.findall("content")
            mods = [c for c in cs if c.get("module")]
            orig = [c for c in cs if not c.get("module")]
            if int(b.get("contentscount")) != len(cs):
                err(tag, "contentscount %s != %d" % (b.get("contentscount"), len(cs)))
            if int(b.get("modulescount")) != len(mods):
                err(tag, "modulescount %s != %d" % (b.get("modulescount"), len(mods)))
            names = [c.get("module") for c in mods]
            print(" base %s: %d contents (%d original + modules %s)" % (tag, len(cs), len(orig), ",".join(names)))
            for n in names:
                if n not in KNOWN_MODULES:
                    err(tag, "unknown module %s" % n)
            if len(set(names)) != len(names):
                err(tag, "module listed twice")
            used = [int(c.get("tmdmoduleid")) for c in mods if int(c.get("tmdmoduleid")) >= 0]
            if len(set(used)) != len(used):
                err(tag, "two modules replace the same TMD index %s" % used)
            for c in orig:
                if c.get("tmdmoduleid") is not None:
                    err(tag, "tmdmoduleid on a non-module content %s" % c.get("id"))
            for t in used:
                if t >= len(orig):
                    err(tag, "tmdmoduleid %d beyond the %d original contents" % (t, len(orig)))
            if a.modules:
                for n in names:
                    p = Path(a.modules) / (n + ".app")
                    if not p.is_file():
                        err(tag, "module file %s missing" % p)
                    elif p.read_bytes()[:4] != b"\x7fELF":
                        err(tag, "module %s is not an ELF" % p)
            for c in orig:
                ps = c.findall("patch")
                if c.get("patchscount") is not None and int(c.get("patchscount")) != len(ps):
                    err(tag, "content %s patchscount %s != %d" % (c.get("id"), c.get("patchscount"), len(ps)))
                for k, p in enumerate(ps):
                    w = "content %s patch %d" % (c.get("id"), k)
                    ob = hexbytes(p.get("originalbytes"))
                    nb = hexbytes(p.get("newbytes"))
                    sz = int(p.get("size"))
                    if len(ob) != sz or len(nb) != sz:
                        err(tag, "%s: size %d, originalbytes %d, newbytes %d" % (w, sz, len(ob), len(nb)))
            # Only the exact version the map was written for can be compared:
            # another version (a later stub, say) has other contents
            d = Path(a.bases) / ("IOS%d-v%d" % (ios, mver))
            if not d.is_dir():
                others = sorted(f.name for f in Path(a.bases).glob("IOS%d-v*" % ios))
                print("  NOT CHECKED: IOS%d v%d not in %s%s" % (ios, mver, a.bases,
                      " (found %s)" % ", ".join(others) if others else ""))
                unchecked.append(ios)
                continue
            ver, tcs = parse_tmd((d / "title.tmd").read_bytes())
            if ver != mver:
                err(tag, "TMD version %d != map %d" % (ver, mver))
            if len(tcs) != len(orig):
                err(tag, "TMD has %d contents, map lists %d original" % (len(tcs), len(orig)))
            npatch = 0
            for i, t in enumerate(tcs):
                f = d / ("%08x.app" % t["cid"])
                if not f.is_file():
                    err(tag, "content %08x (index %d) file missing" % (t["cid"], i))
                    continue
                data = f.read_bytes()
                if hashlib.sha1(data[:t["size"]]).digest() != t["hash"]:
                    err(tag, "content %08x SHA-1 differs from the TMD" % t["cid"])
                if i >= len(orig):
                    continue
                mc = orig[i]
                mid = int(mc.get("id"), 16)
                if mid != t["cid"]:
                    err(tag, "index %d: map id 0x%x, TMD cid 0x%x" % (i, mid, t["cid"]))
                    continue
                for k, p in enumerate(mc.findall("patch")):
                    npatch += 1
                    off = int(p.get("offset"), 16)
                    ob = hexbytes(p.get("originalbytes"))
                    if off + len(ob) > len(data):
                        err(tag, "content 0x%x patch %d at 0x%x+%d beyond size %d" % (mid, k, off, len(ob), len(data)))
                    elif data[off:off + len(ob)] != ob:
                        err(tag, "content 0x%x patch %d at offset 0x%x: file has %s, map wants %s"
                            % (mid, k, off, data[off:off + len(ob)].hex(" "), ob.hex(" ")))
            print("  %d contents hashed, %d patches compared" % (len(tcs), npatch))
    if unchecked:
        print("NOT CHECKED (base files missing): IOS %s" % ", ".join(map(str, unchecked)))
        if a.strict:
            bad += len(unchecked)
    print("RESULT:", "ALL OK" if not bad else "%d problem(s)" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
