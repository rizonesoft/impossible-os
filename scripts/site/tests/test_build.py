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


class RawHtmlAndBaseline(unittest.TestCase):
    def _use(self, root):
        B.REPO, B.SOURCE, B._FILESET, B._DIRSET = root, "worktree", None, None
        B.set_root(root)

    def setUp(self):
        self.saved = (B.REPO, B.ROOT, B.SOURCE, B._FILESET, B._DIRSET)

    def tearDown(self):
        B.REPO, B.ROOT, B.SOURCE, B._FILESET, B._DIRSET = self.saved
        B.set_root(B.ROOT)

    def test_raw_html_links_are_rewritten_and_checked(self):
        root = fixture_repo()
        (root / "docs" / "raw.md").write_text('# Raw\n\n<a href="kernel/index.md">k</a> <img src="nope.png">\n', encoding="utf-8")
        self._use(root)
        errors: list[str] = []
        pages, _ = B.load_pages(FACTS, errors)
        self.assertIn('href="kernel/"', pages["raw.md"].body)
        self.assertTrue(any("nope.png" in e for e in errors), errors)

    def test_raw_html_attribute_forms(self):
        root = fixture_repo()
        (root / "docs" / "forms.md").write_text(
            "# Forms\n\n<a HREF = 'kernel/index.md'>a</a> <a href=\"https://x.test/?a=1&amp;b=2\">b</a> <a href=missing.md>c</a>\n",
            encoding="utf-8")
        self._use(root)
        errors: list[str] = []
        pages, _ = B.load_pages(FACTS, errors)
        body = pages["forms.md"].body
        self.assertIn('HREF="kernel/"', body)
        self.assertIn('?a=1&amp;b=2"', body)          # escaped exactly once
        self.assertNotIn("&amp;amp;", body)
        self.assertTrue(any("missing.md" in e for e in errors), errors)

    def test_raw_html_ignores_other_attributes_and_comments(self):
        root = fixture_repo()
        (root / "docs" / "tricky.md").write_text(
            '# Tricky\n\n<a title="Use href=kernel/index.md" href="kernel/index.md">x</a> <img data-src="gone.png" src="../build/x.json">\n\n<!-- href=missing.md -->\n',
            encoding="utf-8")
        self._use(root)
        errors: list[str] = []
        pages, _ = B.load_pages(FACTS, errors)
        body = pages["tricky.md"].body
        self.assertIn('title="Use href=kernel/index.md"', body)   # other attribute untouched
        self.assertIn('href="kernel/"', body)
        self.assertIn('data-src="gone.png"', body)                # not a src attribute
        self.assertFalse(any("tricky.md" in e and "missing.md" in e for e in errors), errors)  # comment ignored
        self.assertTrue(any("build/x.json" in e for e in errors), errors)  # real src still checked

    def test_duplicate_heading_slugs_get_suffixes(self):
        root = fixture_repo()
        (root / "docs" / "dup.md").write_text("# Dup\n\n## Setup\n\n## Setup\n\n## Setup\n", encoding="utf-8")
        self._use(root)
        pages, _ = B.load_pages(FACTS, [])
        self.assertTrue({"setup", "setup-1", "setup-2"} <= pages["dup.md"].anchors, pages["dup.md"].anchors)

    def test_baseline_flags_new_undocumented_and_stale_entries(self):
        root = fixture_repo()
        (root / "docs" / ".coverage-baseline.json").write_text('{"undocumented": ["todo/01-a/TODO-01-a.md"]}', encoding="utf-8")
        self._use(root)
        errors: list[str] = []
        B.check_baseline(["todo/01-a/TODO-02-new.md"], errors)
        self.assertTrue(any("TODO-02-new" in e and "no documentation page" in e for e in errors), errors)
        self.assertTrue(any("TODO-01-a" in e and "stale" in e for e in errors), errors)

    def test_baseline_may_shrink_but_never_grow(self):
        root = fixture_repo()
        env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
                   GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid")
        bl = root / "docs" / ".coverage-baseline.json"
        bl.write_text('{"undocumented": ["todo/01-a/TODO-01-a.md"]}', encoding="utf-8")
        for cmd in (["add", "-A"], ["commit", "-q", "-m", "baseline", "--no-verify"]):
            subprocess.run(["git", *cmd], cwd=root, check=True, env=env, capture_output=True)
        self._use(root)
        bl.write_text('{"undocumented": ["todo/01-a/TODO-01-a.md", "todo/01-a/TODO-02-b.md"]}', encoding="utf-8")
        errors: list[str] = []
        B.check_baseline(["todo/01-a/TODO-01-a.md", "todo/01-a/TODO-02-b.md"], errors)
        self.assertTrue(any("GREW" in e and "TODO-02-b" in e for e in errors), errors)
        bl.write_text('{"undocumented": []}', encoding="utf-8")
        errors = []
        B.check_baseline([], errors)
        self.assertEqual(errors, [])
        bl.write_text('{"undocumented": ["todo/01-a/TODO-01-a.md", "todo/01-a/TODO-02-b.md"], '
                      '"growth_reasons": {"todo/01-a/TODO-02-b.md": "a page claimed it falsely; claim removed in review"}}',
                      encoding="utf-8")
        errors = []
        B.check_baseline(["todo/01-a/TODO-01-a.md", "todo/01-a/TODO-02-b.md"], errors)
        self.assertEqual(errors, [])   # growth with a written reason is allowed

    def test_update_baseline_never_readds_from_an_orphaned_reason(self):
        data = {"undocumented": ["todo/01-a/TODO-01-a.md"],
                "growth_reasons": {"todo/01-a/TODO-09-x.md": "left behind after a manual edit",
                                   "todo/01-a/TODO-01-a.md": "kept because the entry is still listed"}}
        out = B.shrink_baseline(data, ["todo/01-a/TODO-01-a.md", "todo/01-a/TODO-09-x.md"])
        self.assertEqual(out["undocumented"], ["todo/01-a/TODO-01-a.md"])        # no re-add
        self.assertEqual(list(out["growth_reasons"]), ["todo/01-a/TODO-01-a.md"])  # orphan pruned
        self.assertEqual(B.shrink_baseline(None, ["b", "a"])["undocumented"], ["a", "b"])


class Scripts(unittest.TestCase):
    @unittest.skipUnless(__import__("shutil").which("node"), "node not installed")
    def test_script_syntax_errors_are_reported_and_valid_scripts_pass(self):
        files = {
            "index.html": b"<script>const a = 1;\n});</script><script type=\"application/ld+json\">{</script>",
            "ok.html": b"<script>document.title = 'x';</script><script src=\"x.js\"></script>",
            "x.js": b"function f( {",
        }
        errors: list[str] = []
        B.check_scripts(files, errors)
        self.assertEqual(len(errors), 2, errors)                  # JSON-LD and src= tags are not parsed as JS
        self.assertTrue(any("index.html (inline script 1)" in e for e in errors), errors)
        self.assertTrue(any(e.endswith("x.js: Unexpected token '{'") or "x.js:" in e for e in errors), errors)
        errors = []
        B.check_scripts({"ok.html": files["ok.html"]}, errors)
        self.assertEqual(errors, [])

if __name__ == "__main__":
    unittest.main(verbosity=1)
