"""Line endings: every text file is LF in the repository, and LF in the
checkout on every platform (.gitattributes, whatever core.autocrlf says).
The one exception is the checkout of Windows batch files, which
.gitattributes gives CRLF (eol=crlf) for cmd.exe."""

import pathlib
import shutil
import subprocess
import unittest

root = pathlib.Path(__file__).resolve().parents[1]


def eol_listing():
    # safe.directory: in a CI container the checkout belongs to
    # another user, and git refuses to read it (exit 128) without this.
    out = subprocess.run(["git", "-c", f"safe.directory={root.as_posix()}", "ls-files", "--eol"],
                         cwd=root, capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        meta, path = line.split("\t", 1)
        index, work, attr = meta.split(None, 2)
        yield index, work, attr.strip(), path


@unittest.skipUnless(shutil.which("git") and (root / ".git").exists(), "needs a git checkout")
class LineEndings(unittest.TestCase):
    def test_index_is_lf(self):
        bad = [p for i, _, _, p in eol_listing() if i in ("i/crlf", "i/mixed")]
        self.assertEqual(bad, [], "CRLF committed; convert it to LF")

    def test_checkout_is_lf(self):
        bad = [p for _, w, a, p in eol_listing()
               if w in ("w/crlf", "w/mixed") and a.startswith("attr/text") and "eol=crlf" not in a]
        self.assertEqual(bad, [], "text checked out with CRLF; run tools/fix_line_endings.py")

    def test_batch_files_are_crlf_in_checkout(self):
        bad = [p for _, w, a, p in eol_listing() if p.endswith(".bat") and
               ("eol=crlf" not in a or w in ("w/lf", "w/mixed"))]
        self.assertEqual(bad, [], "a .bat file without CRLF in the checkout")

    def test_only_binary_data_is_not_text(self):
        bad = [p for _, _, a, p in eol_listing() if a in ("attr/", "attr/-text")
               and not p.endswith((".jpg", ".png", ".bin", ".app", ".wad"))]
        self.assertEqual(bad, [], "no line-ending rule, or marked -text: "
                                  "every text file is LF now")


if __name__ == "__main__":
    unittest.main()
