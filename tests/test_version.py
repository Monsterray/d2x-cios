"""VERSION: the release number, major.minor.patch (optionally -pre).

The build (tools/build.sh) turns 11.1.0 into the name d2x-v11.1.0, the cIOS's
numeric version 11 (also the TMD revision, 21000 + 11) and its version string
"-1-0". These checks keep that within what loaders and ModMii accept."""

import pathlib
import re
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


if __name__ == "__main__":
    unittest.main()
