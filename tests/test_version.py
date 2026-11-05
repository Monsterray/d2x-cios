"""VERSION: the release number, major.minor.patch (optionally -pre).

The build (tools/build.sh) turns 11.1.0 into the name d2x-v11.1.0, the cIOS's
numeric version 11 (also the TMD revision, 21000 + 11) and its version string
"-1-0". These checks keep that within what loaders and ModMii accept.

A variant build (D2X_VARIANT=sdhs) appends "+sdhs" to the name and the string;
`tools/build.sh version` computes both without building, and the tests run it."""

import os
import pathlib
import re
import shutil
import subprocess
import unittest

root = pathlib.Path(__file__).resolve().parents[1]


def parts():
    v = (root / "VERSION").read_text().strip()
    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)(-[A-Za-z0-9]+)?", v)
    return v, m


class Version(unittest.TestCase):
    def test_format(self):
        v, m = parts()
        self.assertIsNotNone(m, f"VERSION {v!r} is not major.minor.patch[-pre]")

    def test_major_fits_the_tmd_revision(self):
        # maked2x.sh limits the major to 999; the TMD revision is 21000 + major
        v, m = parts()
        self.assertLessEqual(int(m.group(1)), 999)

    def test_version_string_fits_and_modmii_accepts_it(self):
        # the version string is 16 bytes with its NUL, and ModMii accepts only
        # letters, digits and + - _ ( ) [ ] (data/d2x-beta.bat)
        v, m = parts()
        string = "-" + v.split(".", 1)[1].replace(".", "-")
        self.assertLessEqual(len(string), 15)
        self.assertRegex(string, r"^[A-Za-z0-9+\-_()\[\]]+$")


SH = shutil.which("sh")
STRING_OK = r"^[A-Za-z0-9+\-_()\[\]]+$"


def build_version(variant=None, flags=None):
    env = dict(os.environ)
    env.pop("D2X_VARIANT", None)
    env.pop("EXTRA_CFLAGS", None)
    if variant is not None:
        env["D2X_VARIANT"] = variant
    if flags is not None:
        env["EXTRA_CFLAGS"] = flags
    r = subprocess.run([SH, "tools/build.sh", "version"], cwd=root, env=env,
                       capture_output=True, text=True, timeout=60)
    out = dict(l.split("=", 1) for l in r.stdout.splitlines() if "=" in l)
    return r.returncode, out, r.stderr


@unittest.skipUnless(SH, "needs sh")
class Variant(unittest.TestCase):
    def test_release_has_no_suffix(self):
        v, m = parts()
        rc, out, _ = build_version()
        self.assertEqual(rc, 0)
        self.assertEqual(out["name"], "d2x-v" + v)
        self.assertEqual(out["string"], "-" + v.split(".", 1)[1].replace(".", "-"))

    def test_variants_fit_and_use_the_allowed_characters(self):
        v, m = parts()
        base = "-" + v.split(".", 1)[1].replace(".", "-")
        for variant in ("sdhs", "usbs", "dbg", "trace", "gaperr", "sdhs-dbg"):
            rc, out, err = build_version(variant)
            if len(base + "+" + variant) > 15:
                self.assertNotEqual(rc, 0, variant)
                continue
            self.assertEqual(rc, 0, err)
            self.assertEqual(out["string"], base + "+" + variant)
            self.assertEqual(out["name"], "d2x-v" + v + "+" + variant)
            self.assertRegex(out["string"], STRING_OK)

    def test_overflow_fails_with_a_message(self):
        rc, out, err = build_version("averyveryverylongvariant")
        self.assertNotEqual(rc, 0)
        self.assertIn("holds 15", err)
        self.assertEqual(out, {})

    def test_bad_variant_characters_fail(self):
        for bad in ("SDHS", "sd.hs", "sd hs", "+sdhs", "sd+hs", "-x", "x-"):
            rc, _, err = build_version(bad)
            self.assertNotEqual(rc, 0, bad)

    def test_flagged_build_needs_a_variant(self):
        rc, _, err = build_version(flags="-DSDIO_HIGH_SPEED")
        self.assertNotEqual(rc, 0)
        self.assertIn("D2X_VARIANT", err)
        rc, out, _ = build_version("sdhs", "-DSDIO_HIGH_SPEED")
        self.assertEqual(rc, 0)
        self.assertIn("-DSDIO_HIGH_SPEED", out["info"])
        # CI's -Werror does not change the code, so the release name stays
        rc, out, _ = build_version(flags="-Werror")
        self.assertEqual(rc, 0)
        self.assertNotIn("+", out["string"])


if __name__ == "__main__":
    unittest.main()
