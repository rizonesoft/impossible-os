#!/usr/bin/env python3
"""Unit tests for scripts/site/build.py and gen_theme_header.py (stdlib unittest).

Run: python3 scripts/site/tests/test_build.py
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import build as B  # noqa: E402
import gen_theme_header as G  # noqa: E402

FACTS = {"repo_url": "https://example.invalid/o/impossible-os", "owner": "o", "repo": "impossible-os",
         "release_date_long": "August 8, 2028", "stat_todo_files": "231"}


def fixture_repo() -> Path:
    """A throwaway git repo with a tiny docs tree; returns its root."""
    root = Path(tempfile.mkdtemp(prefix="site-test-"))
    (root / "docs" / "kernel").mkdir(parents=True)
    (root / "docs" / "index.md").write_text(
        "# Home\n\n## Café\n\n[ok](kernel/index.md#caf%C3%A9-link)\n[same](#café)\n[bad](#nope)\n"
        "[art](../build/x.json)\n[gone](kernel/missing.md)\n", encoding="utf-8")
    (root / "docs" / "kernel" / "index.md").write_text("# Kernel\n\n## Café link\n", encoding="utf-8")
    (root / "build").mkdir()
    (root / "build" / "x.json").write_text("{}", encoding="utf-8")
    (root / ".gitignore").write_text("build/\n", encoding="utf-8")
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid")
    for cmd in (["init", "-q"], ["add", "-A"], ["commit", "-q", "-m", "fixture", "--no-verify"]):
        subprocess.run(["git", *cmd], cwd=root, check=True, env=env, capture_output=True)
    return root


class Pure(unittest.TestCase):
    def test_slug(self):
        self.assertEqual(B.github_slug("1. Layout at a Glance"), "1-layout-at-a-glance")

    def test_page_url(self):
        self.assertEqual(B.page_url("index.md"), "")
        self.assertEqual(B.page_url("boot/index.md"), "boot/")
        self.assertEqual(B.page_url("boot/x.md"), "boot/x.html")

    def test_sync_regions(self):
        errs: list[str] = []
        out = B.sync_regions("a <!-- project:release_date_long -->Jan 1<!-- /project --> "
                             "<!-- project:nope -->x<!-- /project -->", FACTS, "t", errs)
        self.assertIn("August 8, 2028", out)
        self.assertEqual(len(errs), 1)

    def test_region_in_inline_code_ignored(self):
        errs: list[str] = []
        text = "`<!-- project:key -->value<!-- /project -->`"
        self.assertEqual(B.sync_regions(text, FACTS, "t", errs), text)
        self.assertEqual(errs, [])

    def test_merge_stats_keeps_old_stat_values(self):
        old = "<!-- project:stat_todo_files -->9<!-- /project --> <!-- project:release_date_long -->x<!-- /project -->"
        new = B.sync_regions(old, FACTS, "t", [])
        merged = B.merge_stats(old, new)
        self.assertIn("stat_todo_files -->9<", merged)
        self.assertIn("August 8, 2028", merged)

    def test_directive_only_above_first_heading(self):
        text = "<!-- docs: covers=todo/a.md -->\n# T\n\n```\n<!-- docs: covers=todo/b.md -->\n```\n"
        self.assertEqual(B.parse_directives(text)["covers"], "todo/a.md")

    def test_colour_conversion(self):
        self.assertEqual(B.hex_to_css("#80FFFFFF"), "rgba(255, 255, 255, 0.502)")
        self.assertEqual(G.argb("#60CDFF"), "0xFF60CDFFu")

    def test_committed_theme_header_is_current(self):
        self.assertEqual(G.check(), [])


class Links(unittest.TestCase):
    def test_link_and_anchor_rules(self):
        root = fixture_repo()
        saved = (B.REPO, B.ROOT, B.SOURCE, B._FILESET, B._DIRSET)
        try:
            B.REPO, B.SOURCE, B._FILESET, B._DIRSET = root, "worktree", None, None
            B.ROOT, B.DOCS = root, root / "docs"
            errors: list[str] = []
            pages, _ = B.load_pages(FACTS, errors)
            joined = "\n".join(errors)
            # an ignored local artifact never satisfies a link, even though it exists on disk
            self.assertIn("../build/x.json", joined)
            self.assertIn("kernel/missing.md", joined)
            # same-page fragments are validated; a percent-encoded valid anchor is accepted
            self.assertIn("index.md#nope", joined)
            self.assertNotIn("caf", joined)
            self.assertEqual(len(errors), 3, joined)
        finally:
            B.REPO, B.ROOT, B.SOURCE, B._FILESET, B._DIRSET = saved
            B.DOCS = B.ROOT / "docs"


if __name__ == "__main__":
    unittest.main(verbosity=1)
