#!/usr/bin/env python3
"""Fixtures for scripts/inventory/check-no-ms-assets.py, run through the real
script against a throwaway git repository and a fixture hash list.

    python3 scripts/inventory/tests/test_check_no_ms_assets.py
"""
import hashlib
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = REPO / "scripts" / "inventory" / "check-no-ms-assets.py"
TYPES = REPO / "scripts" / "inventory" / "asset-types.txt"
MS = b"\x89PNG a Microsoft icon"
OURS = b"\x89PNG our own icon"


def line(data):
    return f"{hashlib.sha256(data).hexdigest()} {len(data)}"


class Checker(unittest.TestCase):
    def setUp(self):
        self.repo = pathlib.Path(tempfile.mkdtemp())
        self.hashes = self.repo / "hashes.txt"
        self.hashes.write_text(f"# build 26300.9711\n{line(MS)}\n{line(b'a font')}\n")
        self.git("init", "-q")

    def tearDown(self):
        shutil.rmtree(self.repo)

    def git(self, *args):
        subprocess.run(["git", "-C", str(self.repo), *args], check=True, capture_output=True, timeout=60)

    def put(self, name, data):
        p = self.repo / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)

    def run_check(self, *args, hashes=None):
        return subprocess.run([sys.executable, str(SCRIPT), "--repo", str(self.repo),
                               "--hashes", str(hashes or self.hashes), "--types", str(TYPES), *args],
                              capture_output=True, text=True, timeout=60)

    def test_clean_tree_passes(self):
        self.put("resources/icons/start.png", OURS); self.git("add", "-A")
        for mode in ("--all", "--index"):
            with self.subTest(mode=mode):
                r = self.run_check(mode)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assertIn("1 file(s) checked, 0 copied", r.stdout)

    def test_copied_file_is_refused_under_any_name_and_case(self):
        for name in ("resources/icons/start.png", "x/Start.PNG", "deep/a b/c.ico"):
            with self.subTest(name=name):
                self.put(name, MS); self.git("add", "-A")
                r = self.run_check("--all")
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertIn(f"{name}: a file of the Windows 11 reference VM", r.stdout)
                self.git("rm", "-q", "-f", name)

    def test_only_asset_types_are_hashed(self):
        self.put("notes.txt", MS); self.put("tool.py", MS); self.git("add", "-A")
        r = self.run_check("--all")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("0 file(s) checked", r.stdout)

    def test_index_judges_staged_bytes(self):
        self.put("a.png", MS); self.git("add", "-A")
        self.put("a.png", OURS)               # an unstaged fix does not clear the commit
        self.assertEqual(self.run_check("--all").returncode, 0)
        self.assertEqual(self.run_check("--index").returncode, 1)
        self.git("add", "-A")
        self.put("a.png", MS)                  # an unstaged copy does not fail the commit
        self.assertEqual(self.run_check("--index").returncode, 0)

    def test_staged_deletion_and_untracked_file_are_not_checked(self):
        self.put("a.png", MS); self.git("add", "-A"); self.git("rm", "-q", "--cached", "a.png")
        self.assertEqual(self.run_check("--index").returncode, 0)
        self.assertEqual(self.run_check("--all").returncode, 0)

    def test_named_paths(self):
        self.put("a.ttf", b"a font"); self.put("b.ttf", b"another font")
        r = self.run_check(str(self.repo / "a.ttf"), str(self.repo / "b.ttf"))
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("2 file(s) checked, 1 copied", r.stdout)

    def test_size_must_match_too(self):
        self.hashes.write_text(f"{hashlib.sha256(MS).hexdigest()} {len(MS) + 1}\n")
        self.put("a.png", MS); self.git("add", "-A")
        self.assertEqual(self.run_check("--all").returncode, 0)

    def test_unreadable_hash_list_fails_closed(self):
        self.put("a.png", OURS); self.git("add", "-A")
        bad = self.repo / "bad.txt"
        for text in ("", "# only a header\n", "nothex 12\n", f"{'a' * 64}\n", f"{'A' * 64} 12\n"):
            with self.subTest(text=text):
                bad.write_text(text)
                self.assertEqual(self.run_check("--all", hashes=bad).returncode, 2)
        self.assertEqual(self.run_check("--all", hashes=self.repo / "missing.txt").returncode, 2)

    def test_outside_a_repository_fails_closed(self):
        shutil.rmtree(self.repo / ".git")
        self.assertEqual(self.run_check("--index").returncode, 2)

    def test_real_hash_list_and_tree(self):
        r = subprocess.run([sys.executable, str(SCRIPT), "--all"], capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=1)
