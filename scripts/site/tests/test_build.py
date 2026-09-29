#!/usr/bin/env python3
"""Unit tests for scripts/site/build.py and gen_theme_header.py (stdlib unittest).

Run: python3 scripts/site/tests/test_build.py
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import time
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


class FeatureCards(unittest.TestCase):
    _use, setUp, tearDown = RawHtmlAndBaseline._use, RawHtmlAndBaseline.setUp, RawHtmlAndBaseline.tearDown

    def test_cards_render_open_sections_and_refuse_bad_owners_sources_and_dashes(self):
        B.todo_graph()                      # load the producer from the REAL repo before re-rooting
        root = Path(tempfile.mkdtemp(prefix="site-cards-"))
        (root / "todo" / "01-a").mkdir(parents=True)
        (root / "todo" / "01-a" / "TODO-01-a.md").write_text(
            "# A\n\n## Implementation Order\n\n| ⭐ | Order | Deliverable | Depends On | Status |\n"
            "| --- | :---: | --- | --- | :---: |\n| 💎 | 1 | One | -- | [x] |\n| 💎 | 2 | Two | -- | [/] |\n"
            "| 💎 | 3 | Three | -- | [ ] |\n\n## 1. One\n\n## 2. Two\n\n## 3. Three\n", encoding="utf-8")
        (root / "src").mkdir()
        (root / "src" / "a.c").write_text("int a;\n", encoding="utf-8")
        (root / "build").mkdir()
        (root / "build" / "gen.c").write_text("x\n", encoding="utf-8")   # exists on disk, ignored by git
        (root / ".gitignore").write_text("build/\n", encoding="utf-8")
        (root / "gh-pages").mkdir()
        cards = [{"title": "Good", "text": "Works <today>.", "owners": ["todo/01-a/TODO-01-a.md"],
                  "sources": ["src/a.c", "src", "build/gen.c"]},
                 {"title": "Bad", "text": "x " + chr(0x2014) + " y", "owners": ["todo/01-a/TODO-99-gone.md"],
                  "sources": ["src/gone.c"]},
                 {"title": "Ownerless", "text": "t"}]
        (root / "gh-pages" / "features.json").write_text(json.dumps({"cards": cards}), encoding="utf-8")
        env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
                   GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid")
        for cmd in (["init", "-q"], ["add", "-A"], ["commit", "-q", "-m", "f", "--no-verify"]):
            subprocess.run(["git", *cmd], cwd=root, check=True, env=env, capture_output=True)
        self._use(root)
        errors: list[str] = []
        out = B.feature_cards(FACTS, errors)
        self.assertIn("2 roadmap sections to go", out)          # [x] counts done; [/] and [ ] are still ahead
        self.assertIn("Works &lt;today&gt;.", out)               # card text is escaped
        self.assertIn("/blob/main/todo/01-a/TODO-01-a.md", out)
        joined = " | ".join(errors)
        for needle in ("owner is not a roadmap file: todo/01-a/TODO-99-gone.md",
                       "not a tracked file or directory: src/gone.c", "not a tracked file or directory: build/gen.c",
                       "text contains an em or en dash", "'Ownerless': names no owning roadmap file"):
            self.assertIn(needle, joined)
        self.assertEqual(len(errors), 5, errors)
        dup = [{"title": "Twin", "text": "a", "owners": ["todo/01-a/TODO-01-a.md"]},
               {"title": "Twin", "text": "b", "owners": ["todo/01-a/TODO-01-a.md"]}, {"title": "", "text": "c"}]
        (root / "gh-pages" / "features.json").write_text(json.dumps({"cards": dup}), encoding="utf-8")
        errors = []
        B.feature_cards(FACTS, errors)
        joined = " | ".join(errors)
        self.assertIn("duplicate card title 'Twin'", joined)
        self.assertIn("non-empty string title", joined)


class Freshness(unittest.TestCase):
    """The section-22 contract, on a throwaway repository."""

    def setUp(self):
        import freshness
        self.F, self.saved = freshness, freshness.REPO
        self.root = Path(tempfile.mkdtemp(prefix="fresh-"))
        freshness.REPO = self.root
        self.env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
                        GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid")
        self.git("init", "-q")
        self.write("src/a.c", "int a;\n")
        self.write("src/d/b.c", "int b;\n")
        self.write("docs/p.md", "# P\n")
        self.commit("page and sources")

    def tearDown(self):
        self.F.REPO = self.saved

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.root, check=True, env=self.env,
                              capture_output=True, text=True).stdout

    def write(self, rel, text):
        (self.root / rel).parent.mkdir(parents=True, exist_ok=True)
        (self.root / rel).write_text(text, encoding="utf-8")

    def commit(self, msg):
        self.git("add", "-A")
        self.git("commit", "-q", "--no-verify", "-m", msg)

    def page(self, source="worktree", path="docs/p.md", sources=("src/a.c", "src/d")):
        return self.F.check_page(path, list(sources), source, False, self.F.merging())

    def test_source_change_is_stale_and_a_revert_is_not(self):
        self.assertEqual(self.page().state, "fresh")
        self.write("src/a.c", "int a2;\n")
        self.commit("change source")
        r = self.page("HEAD")
        self.assertEqual((r.state, r.changed), ("stale", ["src/a.c"]))
        self.git("revert", "--no-edit", "HEAD")
        self.assertEqual(self.page("HEAD").state, "fresh")          # content, not history

    def test_deleting_a_file_under_a_source_directory_is_stale(self):
        self.git("rm", "-q", "src/d/b.c")
        self.commit("delete")
        self.assertEqual(self.page("HEAD").changed, ["src/d/b.c"])

    def test_pure_rename_keeps_the_old_baseline(self):
        self.write("src/a.c", "int a2;\n")
        self.commit("change source")
        self.git("mv", "docs/p.md", "docs/q.md")
        self.commit("rename page only")
        self.assertEqual(self.page("HEAD", path="docs/q.md").state, "stale")

    def test_editing_the_page_counts_as_review_except_during_a_merge(self):
        self.write("src/a.c", "int a2;\n")
        self.commit("change source")
        self.write("docs/p.md", "# P\n\nUpdated.\n")
        self.assertEqual(self.page("worktree").state, "editing")
        self.assertEqual(self.page("index").state, "stale")         # not staged yet
        self.git("add", "docs/p.md")
        self.assertEqual(self.page("index").state, "editing")
        gitdir = self.root / ".git"
        (gitdir / "MERGE_HEAD").write_text(self.git("rev-parse", "HEAD"), encoding="utf-8")
        self.assertEqual(self.page("index").state, "stale")         # merged content is not a review

    def test_unstaged_source_change_is_seen_in_worktree_mode(self):
        self.write("src/a.c", "int a3;\n")
        self.assertEqual(self.page("worktree").state, "stale")
        self.assertEqual(self.page("HEAD").state, "fresh")

    def test_shallow_is_unknown_and_uncommitted_page_is_new(self):
        self.assertEqual(self.F.check_page("docs/p.md", ["src/a.c"], "HEAD", True, False).state, "unknown")
        self.write("docs/n.md", "# N\n")
        self.assertEqual(self.page(path="docs/n.md").state, "new")

    def test_tracked_excludes_untracked_files(self):
        self.write("src/untracked.c", "x\n")
        files, dirs = self.F.tracked("worktree")
        self.assertIn("src/a.c", files)
        self.assertIn("src/d", dirs)
        self.assertNotIn("src/untracked.c", files)

    def test_each_card_keeps_its_own_baseline(self):
        cards = [{"title": "A", "text": "a", "sources": ["src/a.c"]}, {"title": "B", "text": "b", "sources": ["src/d"]}]
        self.write("gh-pages/features.json", json.dumps({"cards": cards}))
        self.commit("cards")
        self.write("src/d/b.c", "int b2;\n")
        self.commit("change B's source")
        cards[0]["text"] = "a, reworded"
        self.write("gh-pages/features.json", json.dumps({"cards": cards}))
        self.commit("edit card A only")
        states = {r.name: r.state for r in self.F.check_cards("HEAD", False, False)}
        self.assertEqual(states, {"A": "fresh", "B": "stale"})

    def test_an_uncommitted_pure_rename_keeps_the_old_baseline(self):
        body = "# P\n\n" + "".join(f"Line {i} of a page long enough to rename.\n" for i in range(20))
        self.write("docs/p.md", body)
        self.commit("longer page")
        self.write("src/a.c", "int a2;\n")
        self.commit("change source")
        self.git("mv", "docs/p.md", "docs/q.md")
        self.assertEqual(self.page("index", path="docs/q.md").state, "stale")
        self.assertEqual(self.page("worktree", path="docs/q.md").state, "stale")
        self.git("mv", "docs/q.md", "docs/p.md")
        (self.root / "docs" / "p.md").rename(self.root / "docs" / "r.md")          # plain mv: r.md untracked
        self.assertEqual(self.page("worktree", path="docs/r.md").state, "stale")
        (self.root / "docs" / "r.md").rename(self.root / "docs" / "p.md")
        self.git("mv", "docs/p.md", "docs/q.md")
        self.write("docs/q.md", body + "One new line after review.\n")      # rename WITH an edit
        self.git("add", "docs/q.md")
        self.assertEqual(self.page("index", path="docs/q.md").state, "editing")
        self.write("docs/q.md", "# Q\n\nA different page.\n")         # below git's rename threshold
        self.git("add", "docs/q.md")
        self.assertEqual(self.page("index", path="docs/q.md").state, "new")

    def test_a_merge_cannot_clear_a_card_by_dropping_its_stale_source(self):
        cards = self._two_cards_b_stale()
        cards[1]["sources"] = ["src/a.c"]                          # merge swaps the stale source away
        self.write("gh-pages/features.json", json.dumps({"cards": cards}))
        self.git("add", "gh-pages/features.json")
        states = {r.name: r.state for r in self.F.check_cards("index", False, True)}
        self.assertEqual(states["B"], "stale")

    def test_a_merge_cannot_clear_a_page_by_dropping_its_sources(self):
        import build
        self.write("docs/p.md", "<!-- docs: sources=src/a.c,src/d -->\n# P\n")
        self.commit("directive")
        self.write("src/a.c", "int a2;\n")
        self.commit("change a")
        self.write("docs/p.md", "<!-- docs: sources=src/d -->\n# P\n")    # merged-in directive drops src/a.c
        self.git("add", "docs/p.md")
        (self.root / ".git" / "MERGE_HEAD").write_text(self.git("rev-parse", "HEAD"), encoding="utf-8")
        for merged in ({"docs/p.md": ["src/d"]}, {}):                          # partial and complete removal
            srcs = self.F.with_head_sources(merged, ["docs/p.md"], build.directive_sources)
            self.assertEqual(srcs["docs/p.md"][0], "src/a.c")
            self.assertEqual(self.F.check_all(srcs, "index")[0].state, "stale")

    def test_unborn_head_reports_new_instead_of_raising(self):
        empty = Path(tempfile.mkdtemp(prefix="fresh-empty-"))
        subprocess.run(["git", "init", "-q"], cwd=empty, check=True)
        self.F.REPO = empty
        self.assertEqual([r.state for r in self.F.check_all({"docs/p.md": ["src"]}, "worktree")], ["new"])

    def test_card_sources_with_a_trailing_slash_still_detect_changes(self):
        self.write("gh-pages/features.json", json.dumps({"cards": [{"title": "D", "text": "d", "sources": ["src/a.c/"]}]}))
        self.commit("card")
        self.write("src/a.c", "int a9;\n")                              # a FILE source written with a slash
        self.commit("change")
        self.assertEqual(self.F.check_cards("HEAD", False, False)[0].state, "stale")

    def test_card_history_is_read_only_up_to_each_boundary(self):
        for i in range(12):
            self.write("gh-pages/features.json", json.dumps({"cards": [{"title": "A", "text": f"v{i}"}]}))
            self.commit(f"card v{i}")
        calls = []
        real = self.F._BlobReader.read
        self.F._BlobReader.read = lambda me, spec: (calls.append(spec), real(me, spec))[1]
        try:
            self.assertEqual(self.F.check_cards("HEAD", False, False)[0].state, "fresh")
        finally:
            self.F._BlobReader.read = real
        self.assertLessEqual(len(calls), 3, calls)                       # not all 12 versions

    def test_a_stable_card_keeps_its_baseline_across_many_versions(self):
        cards = [{"title": "Stable", "text": "s"}, {"title": "Busy", "text": "v0"}]
        self.write("gh-pages/features.json", json.dumps({"cards": cards}))
        self.commit("cards")
        first = self.git("rev-parse", "HEAD").strip()
        for i in range(1, 15):
            cards[1]["text"] = f"v{i}"
            self.write("gh-pages/features.json", json.dumps({"cards": cards}))
            self.commit(f"busy v{i}")
        spawned = []
        real = self.F._BlobReader.__init__
        self.F._BlobReader.__init__ = lambda me: (spawned.append(1), real(me))[1]
        try:
            recs = {r.name: r for r in self.F.check_cards("HEAD", False, False)}
        finally:
            self.F._BlobReader.__init__ = real
        self.assertEqual(recs["Stable"].baseline, first)
        self.assertEqual(len(spawned), 1)                                # one batch process for all versions

    def test_a_dead_card_reader_is_unknown_not_new(self):
        self.write("gh-pages/features.json", json.dumps({"cards": [{"title": "A", "text": "a", "sources": ["src/a.c"]}]}))
        self.commit("card")
        real = self.F._BlobReader.read

        def dying(me, spec):
            me.proc.kill()
            me.proc.wait()
            return real(me, spec)
        self.F._BlobReader.read = dying
        try:
            recs = self.F.check_cards("HEAD", False, False)
        finally:
            self.F._BlobReader.read = real
        self.assertEqual([r.state for r in recs], ["unknown"])
        self.assertIn("could not be checked", self.F.warning(recs[0]))

    def test_unknown_reporting_names_the_cause(self):
        import build
        failed = self.F.Record("page", "docs/p.md", ["src"], state="unknown", error="git log failed")
        shallow = self.F.Record("page", "docs/p.md", ["src"], state="unknown")
        self.assertEqual(build.fresh_label(failed), "unknown: git log failed")
        self.assertEqual(build.fresh_label(shallow), "unknown (shallow clone)")
        self.assertIn("unknown: git log failed", build.freshness_table([failed]))

    def test_a_git_failure_is_unknown_with_a_warning_not_new(self):
        self.git("config", "diff.orderFile", str(self.root / "missing-order-file"))
        r = self.page("HEAD")
        self.assertEqual(r.state, "unknown")
        self.assertIn("could not be checked", self.F.warning(r))

    def test_a_merge_rename_still_restores_head_sources(self):
        import build
        body = "".join(f"Line {i} of a long page.\n" for i in range(20))
        self.write("docs/p.md", "<!-- docs: sources=src/a.c,src/d -->\n# P\n" + body)
        self.commit("directive")
        self.write("src/a.c", "int a2;\n")
        self.commit("change a")
        self.git("mv", "docs/p.md", "docs/q.md")
        self.write("docs/q.md", "<!-- docs: sources=src/d -->\n# P\n" + body)   # renamed AND drops src/a.c
        self.git("add", "docs/q.md")
        (self.root / ".git" / "MERGE_HEAD").write_text(self.git("rev-parse", "HEAD"), encoding="utf-8")
        for merged in ({"docs/q.md": ["src/d"]}, {}):
            srcs = self.F.with_head_sources(merged, ["docs/q.md"], build.directive_sources, "index")
            self.assertIn("src/a.c", srcs["docs/q.md"])
            self.assertEqual(self.F.check_all(srcs, "index")[0].state, "stale")

    def test_long_history_baseline_is_the_newest_content_change(self):
        for i in range(30):
            self.write("docs/p.md", f"# P\n\nrevision {i}\n")
            self.commit(f"rev {i}")
        head = self.git("rev-parse", "HEAD").strip()
        calls = []
        real = self.F._git
        self.F._git = lambda *a, **k: (calls.append(a[0]), real(*a, **k))[1]
        try:
            commit, path, date = self.F.content_baseline("docs/p.md", "HEAD")
        finally:
            self.F._git = real
        self.assertEqual((commit, path), (head, "docs/p.md"))
        self.assertEqual(calls, ["log"])                                  # one bounded query, not a full walk
        self.assertRegex(date, r"^\d{4}-\d{2}-\d{2}$")

    def test_a_merge_that_changed_the_page_is_its_baseline(self):
        self.write("docs/p.md", "# P\n\none\n\ntwo\n\nthree\n")
        self.commit("three lines")
        base = self.git("rev-parse", "--abbrev-ref", "HEAD").strip()
        self.git("checkout", "-q", "-b", "side")
        self.write("docs/p.md", "# P\n\nONE\n\ntwo\n\nthree\n")
        self.commit("side edits line one")
        self.git("checkout", "-q", base)
        self.write("docs/p.md", "# P\n\none\n\ntwo\n\nTHREE\n")
        self.commit("main edits line three")
        self.git("merge", "-q", "--no-edit", "side")                # result differs from both parents
        self.assertEqual(self.page("HEAD").state, "fresh")
        self.write("src/a.c", "int a2;\n")
        self.commit("source only")
        self.assertEqual(self.page("HEAD").state, "stale")

    def test_unusual_file_names_are_read_raw(self):
        for name in ("docs/with\ttab.md", "docs/caf\u00e9.md"):
            self.write(name, "# X\n")
            self.commit(f"add {name!r}")
            self.write("src/a.c", self.git("rev-parse", "HEAD"))
            self.commit("touch source")
            self.assertEqual(self.page("HEAD", path=name, sources=["src/a.c"]).state, "stale", name)

    def test_sources_are_literal_paths_not_pathspec_magic(self):
        self.write(":b.c", "x\n")
        self.write("b.c", "y\n")
        self.commit("colon and plain")
        self.write("docs/p.md", "# P\n\nnow covers the colon file\n")
        self.commit("page")
        self.write(":b.c", "x2\n")
        self.commit("change the colon file only")
        r = self.page("HEAD", sources=[":b.c"])
        self.assertEqual((r.state, r.changed), ("stale", [":b.c"]))

    def _two_cards_b_stale(self):
        cards = [{"title": "A", "text": "a", "sources": ["src/a.c"]},
                 {"title": "B", "text": "b", "sources": ["src/d"], "reviewed": "2026-01-01"}]
        self.write("gh-pages/features.json", json.dumps({"cards": cards}))
        self.commit("cards")
        self.write("src/d/b.c", "int b2;\n")
        self.commit("change B's source")
        return cards

    def test_bumping_a_card_reviewed_date_clears_only_that_card(self):
        cards = self._two_cards_b_stale()
        self.write("src/a.c", "int a2;\n")
        self.commit("change A's source too")
        cards[1]["reviewed"] = "2026-09-28"
        self.write("gh-pages/features.json", json.dumps({"cards": cards}))
        self.commit("review B")
        states = {r.name: r.state for r in self.F.check_cards("HEAD", False, False)}
        self.assertEqual(states, {"A": "stale", "B": "fresh"})

    def test_a_card_changed_by_a_merge_in_progress_stays_stale(self):
        cards = self._two_cards_b_stale()
        cards[1]["text"] = "b, as merged"
        self.write("gh-pages/features.json", json.dumps({"cards": cards}))
        self.git("add", "gh-pages/features.json")
        (self.root / ".git" / "MERGE_HEAD").write_text(self.git("rev-parse", "HEAD"), encoding="utf-8")
        states = {r.name: r.state for r in self.F.check_cards("index", False, True)}
        self.assertEqual(states["B"], "stale")
        self.assertEqual(states["A"], "fresh")


PP = "paypal." + "com/donate/?hosted_button_id="   # split so the repo-wide link scan skips this file


class DonateAndBadge(unittest.TestCase):
    _use, setUp, tearDown = RawHtmlAndBaseline._use, RawHtmlAndBaseline.setUp, RawHtmlAndBaseline.tearDown

    def test_a_stale_donate_link_and_a_wrong_line_count_are_errors(self):
        root = Path(tempfile.mkdtemp(prefix="site-donate-"))
        (root / "README.md").write_text(
            '<img src="https://img.shields.io/badge/lines-1%2C325%2C570-blueviolet" />\n'
            f"[d](https://www.{PP}OLDBUTTON)\n[e](https://www.{PP}NEWBUTTON)\n", encoding="utf-8")
        (root / "COUNT.md").write_text("| **All lines in tree** | **9** | **1325572** |\n", encoding="utf-8")
        env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
                   GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid")
        for cmd in (["init", "-q"], ["add", "-A"], ["commit", "-q", "-m", "f", "--no-verify"]):
            subprocess.run(["git", *cmd], cwd=root, check=True, env=env, capture_output=True)
        self._use(root)
        errors: list[str] = []
        B.check_donate_links({"donate_url": f"https://www.{PP}NEWBUTTON"}, errors)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("README.md:2: PayPal donate link uses button OLDBUTTON", errors[0])
        errors = []
        B.check_count_badge(errors)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("says 1,325,570, COUNT.md says 1,325,572", errors[0])
        (root / "README.md").write_text('<img src="https://img.shields.io/badge/lines-1%2C325%2C572-blueviolet" />\n',
                                        encoding="utf-8")
        errors = []
        B.check_count_badge(errors)
        self.assertEqual(errors, [])

class RepoMeta(unittest.TestCase):
    def setUp(self):
        import repo_meta
        self.R = repo_meta
        self.good = {"tagline": "A kernel: fast.", "site_url": "https://example.org", "topics": ["kernel", "x86-64"]}

    def test_valid_values_pass_and_bad_values_are_named(self):
        self.assertEqual(self.R.validate(self.good), [])
        bad = dict(self.good, tagline="A kernel " + chr(0x2014) + " fast", site_url="http://example.org",
                   topics=["Kernel", "kernel", "x" * 60])
        errs = " | ".join(self.R.validate(bad))
        for needle in ("dash", "https", "not a valid GitHub topic", "duplicate"):
            self.assertIn(needle, errs)
        self.assertIn("missing or empty", " | ".join(self.R.validate(dict(self.good, topics=[]))))

    def test_wrong_shapes_are_refused_before_anything_is_derived(self):
        self.assertIn("list of strings", " | ".join(self.R.validate(dict(self.good, topics="linux"))))
        self.assertIn("list of strings", " | ".join(self.R.validate(dict(self.good, topics=["kernel", 3]))))
        self.assertIn("tagline must be", " | ".join(self.R.validate({"site_url": "https://x.org", "topics": ["a"]})))
        self.assertIn("site_url must be", " | ".join(self.R.validate(dict(self.good, site_url=None))))

    def test_diff_reports_each_field_and_ignores_topic_order_and_trailing_slash(self):
        want = self.R.expected(self.good)
        same = self.R.normalise({"description": "A kernel: fast.", "homepage": "https://example.org",
                                 "topics": ["x86-64", "kernel"]})
        self.assertEqual(self.R.diff(want, same), [])
        other = self.R.normalise({"description": "old", "homepage": None, "topics": ["kernel", "hobby-os"]})
        d = " | ".join(self.R.diff(want, other))
        for needle in ("description:", "homepage:", "missing on GitHub: x86-64", "not in project.json: hobby-os"):
            self.assertIn(needle, d)

class ReviewedFormat(unittest.TestCase):
    """A date, or a minute-precision time for a second review the same day;
    anything else is refused."""

    def test_accepted(self):
        for v in ("2026-09-28", "2026-09-28T08:30"):
            self.assertTrue(B.valid_reviewed(v), v)

    def test_refused(self):
        for v in ("2026-9-28", "2026-02-30", "2026-09-28T25:00", "2026-09-28T08:30:00",
                  "2026-09-28 08:30", "yesterday", "", None, 20260928):
            self.assertFalse(B.valid_reviewed(v), repr(v))


class VerifyLive(unittest.TestCase):
    """verify_live.py against a local HTTP server: identical passes; a changed,
    a missing and an unreachable file each FAIL (the refusal directions)."""

    def setUp(self):
        import functools
        import http.server
        import threading
        import verify_live as V
        self.V = V
        self.built = Path(tempfile.mkdtemp(prefix="vl-built-"))
        self.live = Path(tempfile.mkdtemp(prefix="vl-live-"))
        for d in (self.built, self.live):
            (d / "docs").mkdir()
            (d / "index.html").write_text("<a href=donate>new</a>")
            (d / "docs" / "index.html").write_text("docs")
            (d / "logo.svg").write_text("<svg/>")
        handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(self.live))
        handler.log_message = lambda *a, **k: None
        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.base = f"http://127.0.0.1:{self.srv.server_address[1]}/"

    def tearDown(self):
        self.srv.shutdown()

    def test_identical_passes(self):
        self.assertEqual(self.V.compare(self.built, self.base, "t"), [])

    def test_changed_file_fails(self):
        (self.live / "index.html").write_text("<a href=donate>old</a>")
        self.assertEqual(self.V.compare(self.built, self.base, "t"), ["DIFFERS     index.html"])

    def test_missing_file_fails(self):
        (self.live / "logo.svg").unlink()
        self.assertEqual(self.V.compare(self.built, self.base, "t"), ["MISSING     logo.svg"])

    def test_unreachable_site_fails(self):
        self.srv.shutdown()
        self.srv.server_close()
        out = self.V.compare(self.built, self.base, "t")
        self.assertEqual(len(out), 3)
        self.assertTrue(all(p.startswith("UNREACHABLE") for p in out))

    def test_empty_build_fails(self):
        empty = Path(tempfile.mkdtemp(prefix="vl-empty-"))
        self.assertEqual(self.V.compare(empty, self.base, "t"), ["EMPTY BUILD: nothing to compare"])

    def test_index_paths(self):
        self.assertEqual(self.V.live_path("index.html"), "")
        self.assertEqual(self.V.live_path("docs/index.html"), "docs/")
        self.assertEqual(self.V.live_path("logo.svg"), "logo.svg")


def git_env(date: str | None = None) -> dict:
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid")
    if date:
        env.update(GIT_AUTHOR_DATE=date, GIT_COMMITTER_DATE=date)
    return env


class SitePolish(unittest.TestCase):
    """Section 23: GitHub heading ids, anchors into non-docs Markdown, page dates,
    head tags, sitemap."""

    def setUp(self):
        self.saved = (B.REPO, B.ROOT, B.SOURCE, B._FILESET, B._DIRSET, B.ANCHOR_CACHE)
        self.root = Path(tempfile.mkdtemp(prefix="polish-"))
        B.ANCHOR_CACHE = self.root / "anchor-cache.json"

    def tearDown(self):
        B.REPO, B.ROOT, B.SOURCE, B._FILESET, B._DIRSET, B.ANCHOR_CACHE = self.saved
        B.set_root(B.ROOT)

    def git(self, *args, date=None, cwd=None):
        return subprocess.run(["git", *args], cwd=cwd or self.root, check=True, env=git_env(date),
                              capture_output=True, text=True).stdout

    def write(self, rel, text):
        (self.root / rel).parent.mkdir(parents=True, exist_ok=True)
        (self.root / rel).write_text(text, encoding="utf-8")

    def commit(self, msg, date):
        self.git("add", "-A")
        self.git("commit", "-q", "--no-verify", "-m", msg, date=date)

    def use(self, root, source="worktree"):
        B.REPO, B.SOURCE, B._FILESET, B._DIRSET = root, source, None, None
        B.ROOT, B.DOCS = root, root / "docs"

    def test_slugger_reserves_every_generated_id(self):
        s = B.Slugger()
        self.assertEqual([s.slug(t) for t in ("Foo", "Foo", "Foo-1", "Foo")], ["foo", "foo-1", "foo-1-1", "foo-2"])
        self.assertEqual(B.github_slug("5. Kernel `mark_boot_successful()` \U0001F680"), "5-kernel-mark_boot_successful-")

    def test_anchors_into_non_docs_markdown_use_github_ids(self):
        self.git("init", "-q")
        self.write("todo/t.md", "# T\n\n## 5. Kernel `mark_boot_successful()`\n\n## Dup\n\n## Dup\n\n"
                               "```html\n<a id=\"in-code\"></a>\n```\n\n<a id=\"real\"></a>\n")
        self.write("docs/p.md", "# P\n\n[a](../todo/t.md#5-kernel-mark_boot_successful)\n[b](../todo/t.md#dup-1)\n"
                               "[c](../todo/t.md#real)\n[d](../todo/t.md#in-code)\n[e](../todo/t.md#dup-2)\n"
                               "[f](../todo/t.md)\n")
        self.commit("fixture", "2026-01-01T00:00:00Z")
        self.use(self.root)
        errors: list[str] = []
        B.load_pages(FACTS, errors)
        # An anchor quoted inside a code block is not an anchor; a third "Dup" does not exist.
        self.assertEqual(sorted(e.rsplit("#", 1)[1] for e in errors), ["dup-2", "in-code"], errors)
        self.assertTrue(all("no such GitHub heading id" in e for e in errors))

    def test_dates_follow_the_first_parent_line_including_merges(self):
        self.git("init", "-q", "-b", "main")
        self.write("docs/a.md", "# A\n")
        self.write("docs/b.md", "# B\n")
        self.commit("start", "2026-01-01T12:00:00Z")
        self.git("checkout", "-q", "-b", "side")
        self.write("docs/a.md", "# A changed on a side branch\n")
        self.commit("side edit", "2026-02-01T12:00:00Z")
        self.git("checkout", "-q", "main")
        self.write("docs/b.md", "# B2\n")
        self.commit("main edit", "2026-03-01T12:00:00Z")
        self.git("merge", "-q", "--no-ff", "--no-edit", "side", date="2026-04-01T12:00:00Z")
        self.write("docs/new.md", "# New, never committed\n")
        self.use(self.root)
        dates = B.last_updated()
        # a.md reached main through the merge: it is dated by the merge, not its first commit.
        self.assertEqual(dates["docs/a.md"], "2026-04-01")
        self.assertEqual(dates["docs/b.md"], "2026-03-01")
        self.assertNotIn("docs/new.md", dates)
        # A pinned ref dates by that commit's history.
        B.SOURCE = self.git("rev-parse", "HEAD~1").strip()
        self.assertEqual(B.last_updated()["docs/a.md"], "2026-01-01")

    def test_shallow_clone_has_no_dates_and_unborn_head_has_none(self):
        self.git("init", "-q")
        self.use(self.root)
        self.assertEqual(B.last_updated(), {})
        for n in range(2):
            self.write("docs/a.md", f"# A{n}\n")
            self.commit(f"c{n}", f"2026-0{n + 1}-01T00:00:00Z")
        shallow = Path(tempfile.mkdtemp(prefix="polish-shallow-")) / "c"
        subprocess.run(["git", "clone", "-q", "--depth", "1", f"file://{self.root}", str(shallow)], check=True,
                       capture_output=True)
        self.use(shallow)
        self.assertIsNone(B.last_updated())

    def test_slug_normalisation_follows_github(self):
        self.assertEqual(B.github_slug("a "), "a-")                     # `# a&#x20;`: nothing is trimmed
        self.assertEqual(B.github_slug("Cafe\u0301 x"), "cafe\u0301-x")  # combining marks are word characters
        self.assertEqual(B.github_slug("x\u00b2 \u2167"), "x-")          # No/Nl numbers are not
        self.assertEqual(B.github_slug("A_b-C d.e"), "a_b-c-de")

    def test_anchor_gate_uses_a_real_parse(self):
        # Shapes a line scanner got wrong (round-2 review): a heading after a line
        # of inline HTML, a fence inside a list, a reference link in a heading, and
        # an anchor that is only an indented code example.
        B.ANCHOR_CACHE = self.root / "cache.json"
        self.write("todo/t.md", "<em>intro</em>\n## Real heading\n\n- item\n\n    ```\n    ## Example\n    ```\n\n"
                               "## After list\n\n# [Label][ref]\n\n[ref]: https://example.invalid/\n\n"
                               "    <a id=\"fake\"></a>\n")
        self.use(self.root)
        self.write("todo/h.md", "# H\n\n<!-- <a id=\"deleted\"></a> -->\n\n<a class=\"bookmark\" id=\"live\"></a>\n\n"
                               "Text <a name='single'></a> here.\n")
        self.use(self.root)
        got = B.markdown_anchors("todo/t.md")
        self.assertTrue({"real-heading", "after-list", "label"} <= got, got)
        self.assertFalse({"example", "fake", "labelref"} & got, got)
        got = B.markdown_anchors("todo/h.md")
        self.assertTrue({"live", "single"} <= got, got)       # any attribute order, either quote
        self.assertNotIn("deleted", got)                        # a commented-out anchor is gone

    def test_anchor_cache_is_keyed_by_content_and_version(self):
        path = self.root / "cache.json"
        c = B.AnchorCache(path)
        self.assertEqual(c.anchors("# A\n"), {"a"})
        c.save()
        data = json.loads(path.read_text())
        self.assertEqual(data["version"], B._anchor_cache_version())
        key = next(iter(data["entries"]))
        data["entries"][key] = ["sentinel"]            # a hit is served from the cache, by content
        path.write_text(json.dumps(data))
        self.assertEqual(B.AnchorCache(path).anchors("# A\n"), {"sentinel"})
        self.assertEqual(B.AnchorCache(path).anchors("# B\n"), {"b"})   # other content: parsed
        data["version"] = "other"
        path.write_text(json.dumps(data))
        self.assertEqual(B.AnchorCache(path).anchors("# A\n"), {"a"})   # another version: ignored
        path.write_text("{not json")
        self.assertEqual(B.AnchorCache(path).anchors("# A\n"), {"a"})   # corrupt: rebuilt
        v = B._anchor_cache_version()
        for bad in ({"version": v, "entries": None}, {"version": v, "entries": []}, [1, 2],
                    {"version": v, "entries": {key: {"x": 1}}}, {"version": v, "entries": {key: [1]}}):
            path.write_text(json.dumps(bad))
            self.assertEqual(B.AnchorCache(path).anchors("# A\n"), {"a"}, bad)   # malformed: a miss
        c = B.AnchorCache(path)
        c.anchors("# C\n")
        c.save()
        self.assertEqual(json.loads(path.read_text())["version"], B._anchor_cache_version())
        # A cache that cannot be written, nor its temporary file removed, only costs time.
        from unittest import mock
        c = B.AnchorCache(path)
        c.anchors("# E\n")
        with mock.patch.object(Path, "write_text", side_effect=PermissionError("ro")), \
                mock.patch.object(Path, "unlink", side_effect=PermissionError("ro")):
            c.save()
        # Saving keeps only what this run used: an edited file replaces its entry.
        c = B.AnchorCache(path)
        c.anchors("# C\n")
        c.anchors("# D\n")
        c.save()
        c = B.AnchorCache(path)
        c.anchors("# D\n")
        c.save()
        self.assertEqual(list(json.loads(path.read_text())["entries"].values()), [["d"]])

    def test_dates_ignore_git_config_and_odd_file_names(self):
        self.git("init", "-q", "-b", "main")
        self.write("docs/a.md", "# A\n")
        self.commit("start", "2026-01-01T12:00:00Z")
        self.git("checkout", "-q", "-b", "side")
        self.write("docs/a.md", "# A2\n")
        self.commit("side", "2026-02-01T12:00:00Z")
        self.git("checkout", "-q", "main")
        self.write("docs/b.md", "# B\n")
        self.commit("main", "2026-03-01T12:00:00Z")
        self.git("merge", "-q", "--no-ff", "--no-edit", "side", date="2026-04-01T12:00:00Z")
        self.write("docs/x\x01oops.md", "# X\n")
        self.write("docs/index.md", "# I\n")
        self.commit("odd name", "2026-05-01T12:00:00Z")
        self.use(self.root)
        plain = B.last_updated()
        self.git("config", "log.diffMerges", "combined")
        self.git("config", "log.showSignature", "true")
        self.assertEqual(B.last_updated(), plain)
        self.assertEqual(plain["docs/a.md"], "2026-04-01")
        self.assertEqual(plain["docs/index.md"], "2026-05-01")
        self.assertEqual(plain["docs/x\x01oops.md"], "2026-05-01")

    def test_run_refuses_to_write_a_site_from_a_shallow_clone(self):
        from unittest import mock
        from contextlib import redirect_stderr
        import argparse
        import io
        out = self.root / "site"

        def fake_build(facts, errors):
            fake_build.undocumented, fake_build.freshness = [], []
            return {"index.html": b"x"}
        quiet = {name: mock.patch.object(B, name, lambda *a, **k: None) for name in (
            "check_regions", "check_owner_urls", "check_donate_links", "check_count_badge", "check_icon_renders",
            "check_scripts", "check_baseline")}

        def run(shallow, check):
            fake_build.shallow = shallow
            args = argparse.Namespace(sync_head=None, emit_head=None, sync=None, freshness=False,
                                      update_baseline=False, check=check, skip_stats=False, out=out, quiet=True)
            err = io.StringIO()
            with mock.patch.object(B, "build", fake_build), redirect_stderr(err):
                for p in quiet.values():
                    p.start()
                try:
                    fake_build.__dict__["shallow"] = shallow
                    rc = B.run(args)
                finally:
                    for p in quiet.values():
                        p.stop()
            return rc, err.getvalue()

        rc, err = run(shallow=True, check=False)
        self.assertEqual(rc, 1)
        self.assertIn("refusing to write the site", err)
        self.assertFalse(out.exists())
        rc, err = run(shallow=True, check=True)
        self.assertEqual(rc, 0)
        self.assertIn("WARN: shallow clone", err)
        self.assertFalse(out.exists())
        rc, _ = run(shallow=False, check=False)
        self.assertEqual(rc, 0)
        self.assertEqual((out / "index.html").read_bytes(), b"x")

    def test_page_template_is_substituted_in_one_pass(self):
        facts = dict(FACTS, site_url="https://site.invalid", name="OS")
        page = B.Page(src=Path("docs/k/x.md"), rel="k/x.md", url="k/x.html", title="X %ROOT%",
                      body="<p>see %EDIT% and %UPDATED%</p>", summary="word " * 40)
        tpl = ("<title>%TITLE%</title><meta content=\"%DESCRIPTION%\"><link href=\"%CANONICAL%\">"
               "%BODY%<footer>%UPDATED%<a href=\"%EDIT%\"></a></footer>%NOT_A_KEY%")
        out = B.render_page(tpl, facts, {}, page, "2026-09-28")
        self.assertIn("<title>X %ROOT%</title>", out)
        self.assertIn("<p>see %EDIT% and %UPDATED%</p>", out)          # body text is never rescanned
        self.assertIn('href="https://site.invalid/docs/k/x.html"', out)
        self.assertIn('<time datetime="2026-09-28">2026-09-28</time>', out)
        self.assertIn("%NOT_A_KEY%", out)
        desc = re.search(r'<meta content="([^"]*)"', out).group(1)
        self.assertLessEqual(len(desc), 160)
        self.assertTrue(desc.endswith("word..."))
        self.assertEqual(B.site_url(facts, "docs/index.html"), "https://site.invalid/docs/")
        self.assertNotIn("<time", B.render_page(tpl, facts, {}, page, ""))

    def test_summary_is_the_first_top_level_paragraph(self):
        self.git("init", "-q")
        self.write("docs/p.md", "# P\n\n> [!NOTE]\n> not this\n\n- nor this\n\nThis `one` *here*,\nwrapped\\\nand broken.\n\n"
                               "Not the second.\n")
        self.use(self.root)
        errors: list[str] = []
        pages, _ = B.load_pages(FACTS, errors)
        self.assertEqual(pages["p.md"].summary, "This one here, wrapped and broken.")

    def test_sitemap_lists_indexable_pages_with_their_dates(self):
        facts = dict(FACTS, site_url="https://site.invalid/")
        files = {"index.html": b"<html>", "404.html": b'<meta name="robots" content="noindex, follow">',
                 "docs/index.html": b"x", "docs/coverage.html": b"x", "design/controls.html": b"x",
                 "logo.svg": b"<svg/>", "docs/search.json": b"[]"}
        sources = {"index.html": "gh-pages/index.html", "docs/index.html": "docs/index.md",
                   "docs/coverage.html": "", "design/controls.html": "gh-pages/design/controls.html"}
        dates = {"gh-pages/index.html": "2026-01-02", "docs/index.md": "2026-03-04"}
        xml = B.sitemap(facts, files, sources, dates).decode()
        locs = re.findall(r"<loc>([^<]*)</loc>", xml)
        self.assertEqual(locs, ["https://site.invalid/design/controls.html", "https://site.invalid/docs/coverage.html",
                                "https://site.invalid/docs/", "https://site.invalid/"])
        self.assertIn("<loc>https://site.invalid/</loc><lastmod>2026-01-02</lastmod>", xml)
        self.assertIn("<loc>https://site.invalid/docs/coverage.html</loc></url>", xml)
        from xml.dom import minidom
        minidom.parseString(xml)   # well-formed


class LinkCheck(unittest.TestCase):
    """scripts/site/linkcheck.py: collection skips code and preconnect hints; a
    404/410 or a missing host is DEAD; bot walls, outages and temporary DNS
    failures are UNVERIFIED, never dead and never ok."""

    @classmethod
    def setUpClass(cls):
        import http.server
        import threading
        import linkcheck as L
        cls.L = L

        hits = cls.hits = {}
        conc = cls.conc = [0, 0]   # active, peak
        lock = threading.Lock()

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def answer(self, body):
                path = self.path
                route = path.split("?")[0]
                hits[path] = hits.get(path, 0) + 1
                if path.startswith("/conc"):
                    with lock:
                        conc[0] += 1
                        conc[1] = max(conc[1], conc[0])
                    time.sleep(0.2)
                    with lock:
                        conc[0] -= 1
                    self.send_response(200)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                redirects = {"/redir-proto": (302, "__proto__"), "/redir-space": (302, "/live "), "/redir-utf8": (302, "/caf\u00c3\u00a9"),
                             "/redir-rel": (302, "ok"), "/redir-gone": (301, f"{cls.base}/gone"),
                             "/redir-down": (307, "/down"), "/loop": (302, "/loop"), "/redir-none": (302, None),
                             "/redir-empty": (302, ""), "/redir-file": (302, "file:///dev/null"),
                             "/redir-bad": (302, "https://[broken/path"), "/redir-hostless": (302, "http://exa mple/")}
                if path.startswith("/redir-conc"):
                    redirects[path] = (302, f"{cls.base}/conc{path[len('/redir-conc'):]}")
                if route == "/redir-stall":
                    redirects[path] = (302, f"{cls.base}/stall{path[len('/redir-stall'):]}")
                if path in redirects or route in redirects:
                    code, loc = redirects.get(path) or redirects[route]
                    self.send_response(code)
                    if loc is not None:
                        self.send_header("Location", loc)
                    self.send_header("Content-Length", "2")
                    self.end_headers()
                    return
                if path == "/big-redirect":
                    # A redirect announcing a huge body it never sends: any read() blocks.
                    self.send_response(302)
                    self.send_header("Location", "/ok")
                    self.send_header("Content-Length", "1000000000")
                    self.end_headers()
                    self.wfile.flush()
                    time.sleep(3)
                    return
                if path == "/slow" or route == "/stall":
                    time.sleep(3)
                # `/a^b` and `/a%5Eb` are one resource to a real server, and which
                # one Node sends depends on its URL-parser build: the official
                # builds encode `^` in a path, Ubuntu's apt nodejs and the CI
                # runner's do not (main went red on exactly this, 2026-09-29).
                if route in ("/caf%C3%A9", "/live", "/a/live", "/a%5Eb", "/a^b", "/__proto__"):
                    self.send_response(200)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                if route == "/bad-header":
                    self.send_response(200)
                    self.send_header("Content-Type", "multipart/mixed; boundary*=undefined''foo")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                code = {"/ok": 200, "/gone": 404, "/removed": 410, "/down": 503, "/wall": 403, "/slow": 200,
                        "/counted": 200, "/stall": 200}.get(route)
                if path == "/nohead":
                    code = 405 if self.command == "HEAD" else 200
                if path == "/head-gone":
                    code = 404 if self.command == "HEAD" else 200
                if path == "/flaky":
                    code = 503 if hits[path] == 1 else 200
                self.send_response(code or 404)
                self.send_header("Content-Length", "2")
                self.end_headers()
                if body:
                    self.wfile.write(b"ok")

            def do_HEAD(self):
                self.answer(False)

            def do_GET(self):
                self.answer(True)

        cls.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        cls.base = f"http://127.0.0.1:{cls.srv.server_address[1]}"
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()

    def test_http_verdicts(self):
        got = self.L.run_checks([f"{self.base}/{p}" for p in ("ok", "gone", "removed", "down", "wall", "nohead")],
                                retries=2, wait=0.01, timeout=5)
        verdicts = {u.rsplit("/", 1)[1]: v for u, (v, _) in got.items()}
        self.assertEqual(verdicts, {"ok": "OK", "nohead": "OK", "gone": "DEAD", "removed": "DEAD",
                                    "down": "UNVERIFIED", "wall": "UNVERIFIED"})

    def test_redirects_retries_and_head_refusals(self):
        b = self.base
        got = self.L.run_checks([f"{b}/redir-rel", f"{b}/redir-gone", f"{b}/redir-down", f"{b}/loop",
                                 f"{b}/head-gone", f"{b}/flaky"], retries=2, wait=0.01, timeout=5)
        verdicts = {u.rsplit("/", 1)[1]: v for u, v in got.items()}
        self.assertEqual(verdicts["redir-rel"], ("OK", ""))
        self.assertEqual(verdicts["redir-gone"], ("DEAD", "HTTP 404"))        # the destination decides
        self.assertEqual(verdicts["redir-down"], ("UNVERIFIED", "HTTP 503"))
        self.assertEqual(verdicts["loop"][0], "UNVERIFIED")
        self.assertIn("redirects", verdicts["loop"][1])
        self.assertEqual(verdicts["head-gone"], ("OK", ""))                   # HEAD 404, GET 200: healthy
        self.assertEqual(verdicts["flaky"], ("OK", ""))                       # recovered on the retry
        self.assertEqual(self.hits["/flaky"], 2)
        before = self.hits.get("/gone", 0)
        self.assertEqual(self.L.check(f"{b}/gone", 3, 0.01, 5)[0], "DEAD")
        self.assertEqual(self.hits["/gone"] - before, 6)                      # 3 attempts x (HEAD + GET)

    def test_unfollowable_redirects_are_unverified_and_cost_one_link(self):
        b = self.base
        urls = [f"{b}/redir-none", f"{b}/redir-empty"] + [f"{b}/redir-file?{i}" for i in range(5)] + [f"{b}/ok"]
        start = time.monotonic()
        got = self.L.run_checks(urls, retries=1, wait=0, timeout=5, budget=30)
        self.assertLess(time.monotonic() - start, 10)     # dead workers would stall the host until the budget
        self.assertEqual(got[f"{b}/ok"], ("OK", ""))
        self.assertEqual(got[f"{b}/redir-none"], ("UNVERIFIED", "HTTP 302 without a usable Location"))
        self.assertEqual(got[f"{b}/redir-empty"][0], "UNVERIFIED")
        for i in range(5):
            v, why = got[f"{b}/redir-file?{i}"]
            self.assertEqual(v, "UNVERIFIED")
            self.assertIn("non-HTTP", why)

    def test_a_stalled_redirect_target_does_not_starve_other_hosts(self):
        # 16 links on one host redirect to a second host that stalls; a dead link
        # on the first host must still be checked long before the stall ends.
        port = self.srv.server_address[1]
        urls = [f"http://localhost:{port}/redir-stall?{i}" for i in range(16)] + [f"http://localhost:{port}/gone"]
        start = time.monotonic()
        got = self.L.run_checks(urls, retries=1, wait=0, timeout=10, budget=1.5)
        self.assertLess(time.monotonic() - start, 2.5)
        self.assertEqual(got[f"http://localhost:{port}/gone"], ("DEAD", "HTTP 404"))
        self.assertIn("budget", got[f"http://localhost:{port}/redir-stall?0"][1])

    def test_a_malformed_link_is_dead_and_does_not_stop_the_rest(self):
        found: dict[str, list[str]] = {}
        self.L.markdown_links('# T\n\n<a href="https://[broken/path#part">x</a> [ok](' + self.base + '/ok)\n'
                              '[p](http://example.invalid:abc/p) <a href="http://exa mple.invalid/">h</a>\n', "docs/t.md", found)
        self.assertIn("https://[broken/path", found)       # a fragment is cut without parsing the URL
        got = self.L.run_checks(sorted(found) + [f"{self.base}/redir-bad"], retries=1, wait=0, timeout=5, budget=30)
        self.assertEqual(got[f"{self.base}/ok"], ("OK", ""))
        for bad in ("https://[broken/path", "http://example.invalid:abc/p", "http://exa mple.invalid/"):
            self.assertEqual(got[bad][0], "DEAD", bad)
            self.assertIn("malformed URL", got[bad][1])
        # Host names the HTTP client refuses: decided without any network request.
        self.assertEqual(got[f"{self.base}/redir-bad"], ("UNVERIFIED", "HTTP 302 to a malformed Location"))
        odd = ["https://exa%20mple.invalid/", "https://exa mple.invalid/", "https://ex%00a.invalid/"]
        start = time.monotonic()
        got = self.L.run_checks(odd, retries=3, wait=5, timeout=5, budget=30)
        self.assertLess(time.monotonic() - start, 2)      # final at once: no retry backoff
        for bad in odd:
            self.assertEqual(got[bad][0], "DEAD", bad)
            self.assertIn("malformed URL", got[bad][1])
        # The same exception types raised while PARSING a response are the server's
        # doing: retried, and never DEAD.
        got = self.L.run_checks([f"{self.base}/bad-header"], retries=3, wait=0, timeout=5, budget=30)
        self.assertEqual(got[f"{self.base}/bad-header"][0], "UNVERIFIED")
        self.assertEqual(self.hits["/bad-header"], 3)
        # A Location a browser cannot parse is the far server's fault: unverified, never grafted here.
        got = self.L.run_checks([f"{self.base}/redir-hostless"], retries=1, wait=0, timeout=5, budget=30)
        self.assertEqual(got[f"{self.base}/redir-hostless"], ("UNVERIFIED", "HTTP 302 to a malformed Location"))
        self.assertFalse([p for p in self.hits if "mple" in p])

    def test_through_a_proxy_only_an_http_answer_is_dead(self):
        # A dead link behind a broken proxy: the proxy fails before any answer, so
        # nothing is known about the link. Bypassed by no_proxy, the server answers 404.
        link = f"{self.base}/gone"
        saved, env = (self.L.PROXIES, self.L.OPENER), {k: os.environ.get(k) for k in ("no_proxy", "NO_PROXY")}
        try:
            for k in env:
                os.environ.pop(k, None)
            self.L.PROXIES = {"http": "http://proxy.invalid:bad"}
            self.L.OPENER = self.L.make_opener(self.L.PROXIES)
            got = self.L.run_checks([link], retries=1, wait=0, timeout=5, budget=30)[link]
            self.assertEqual(got[0], "UNVERIFIED")
            self.assertIn("through a proxy", got[1])
            # no_proxy decides the route exactly as urllib does: bypassed, the link itself is judged.
            os.environ["no_proxy"] = "127.0.0.1"
            self.assertEqual(self.L.run_checks([link], retries=1, wait=0, timeout=5, budget=30)[link], ("DEAD", "HTTP 404"))
        finally:
            self.L.PROXIES, self.L.OPENER = saved
            for k, v in env.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v

    def test_unicode_links_get_one_verdict_in_markdown_and_html(self):
        found: dict[str, list[str]] = {}
        self.L.markdown_links(f"# T\n\n[m]({self.base}/caf\u00e9)\n\n<a href=\"{self.base}/caf\u00e9?q=\u00e9 x\">h</a>\n",
                              "docs/t.md", found)
        self.assertEqual(len(found), 2, found)                  # encoded by Markdown, raw in HTML
        got = self.L.run_checks(sorted(found), retries=1, wait=0, timeout=5, budget=30)
        self.assertEqual(set(got.values()), {("OK", "")}, got)
        sent = self.L.browser_urls(["http://h/caf%C3%A9?a=%20b", "http://h/document[1]", "http://h/document?",
                                    "http://h/a b?x y'z#frag"])
        self.assertEqual(sent, {"http://h/caf%C3%A9?a=%20b": "http://h/caf%C3%A9?a=%20b",   # escapes kept
                                "http://h/document[1]": "http://h/document[1]",             # as a browser sends it
                                "http://h/document?": "http://h/document?",                 # empty query kept
                                "http://h/a b?x y'z#frag": "http://h/a%20b?x%20y%27z"})     # fragment never sent

    def test_links_are_requested_as_a_browser_would(self):
        b = self.base
        written = [f"{b}/live ", f"\t{b}/li\nve", f"{b}/a/../live", f"{b}/a/%2e%2e/live", f"{b}/a\\live", f"{b}/a^b"]
        got = self.L.run_checks(written, retries=1, wait=0, timeout=5, budget=30)
        self.assertEqual(got, {u: ("OK", "") for u in written})
        self.assertEqual(self.L.browser_urls(["http://h/a b#frag", "http://[x"]),
                         {"http://h/a b#frag": "http://h/a%20b", "http://[x": None})

    def test_the_url_parser_failing_is_the_checkers_error_not_a_verdict(self):
        b = self.base
        self.assertEqual(self.L.check(f"{b}/redir-space", 1, 0, 5), ("OK", ""))   # Location resolved like a browser
        self.assertEqual(self.L.check(f"{b}/redir-utf8", 1, 0, 5), ("OK", ""))    # raw UTF-8 Location bytes
        self.assertEqual(self.L.check(f"{b}/redir-proto", 1, 0, 5), ("OK", ""))   # a Location named __proto__
        saved = self.L.NODE_URL
        try:
            self.L.NODE_URL = "process.exit(3)"
            with self.assertRaises(self.L.NormalizeError):
                self.L.run_checks([f"{b}/ok"], retries=1, wait=0, timeout=5, budget=30)
            from contextlib import redirect_stderr, redirect_stdout
            import io
            err = io.StringIO()
            saved_collect = self.L.collect
            self.L.collect = lambda: {f"{b}/ok": ["docs/a.md:1"]}
            try:
                with redirect_stdout(io.StringIO()), redirect_stderr(err):
                    self.assertEqual(self.L.main(["--retries", "1"]), 2)
            finally:
                self.L.collect = saved_collect
            self.assertIn("linkcheck cannot run", err.getvalue())
            # A parser that works up front but fails while resolving a redirect
            # also stops the run: a broken checker never exits green.
            self.L.NODE_URL = ('const [l, b] = JSON.parse(require("fs").readFileSync(0, "utf8"));'
                               'if (b !== null) process.exit(3); const o = {};'
                               'for (const u of l) o[u] = new URL(u).href; process.stdout.write(JSON.stringify(o));')
            with self.assertRaises(self.L.NormalizeError):
                self.L.run_checks([f"{b}/redir-rel", f"{b}/ok"], retries=1, wait=0, timeout=5, budget=30)
            # ... even when the worker is slow to handle the failure and the budget
            # runs out in between: the failure is published before the parser call
            # stops counting as in flight.
            real_step = self.L.step

            def slow_step(*args, **kw):
                try:
                    return real_step(*args, **kw)
                except self.L.NormalizeError:
                    time.sleep(0.6)
                    raise
            self.L.step = slow_step
            try:
                with self.assertRaises(self.L.NormalizeError):
                    self.L.run_checks([f"{b}/redir-rel"], retries=1, wait=0, timeout=5, budget=0.2)
            finally:
                self.L.step = real_step
            # A parser call that outlives even the grace after the budget fails closed.
            real_resolve = self.L.resolve_location
            self.L.resolve_location = lambda loc, base, t: time.sleep(4)
            try:
                start = time.monotonic()
                with self.assertRaises(self.L.NormalizeError) as cm:
                    self.L.run_checks([f"{b}/redir-rel"], retries=1, wait=0, timeout=0.3, budget=0.2)
                self.assertIn("still running", str(cm.exception))
                self.assertLess(time.monotonic() - start, 3.5)
            finally:
                self.L.resolve_location = real_resolve
            # Undecodable parser output is the same parser failure, never a verdict.
            self.L.NODE_URL = ('const [l, b] = JSON.parse(require("fs").readFileSync(0, "utf8"));'
                               'if (b !== null) { process.stderr.write(Buffer.from([0xff])); process.exit(3); }'
                               'const o = {}; for (const u of l) o[u] = new URL(u).href; process.stdout.write(JSON.stringify(o));')
            with self.assertRaises(self.L.NormalizeError):
                self.L.run_checks([f"{b}/redir-rel"], retries=1, wait=0, timeout=5, budget=30)
            # ... and so does one that STALLS on a redirect when the budget runs out first.
            self.L.NODE_URL = self.L.NODE_URL.replace("process.exit(3)", "setTimeout(() => {}, 20000)")
            start = time.monotonic()
            with self.assertRaises(self.L.NormalizeError):
                self.L.run_checks([f"{b}/redir-rel"], retries=1, wait=0, timeout=1, budget=0.3)
            self.assertLess(time.monotonic() - start, 4)
            self.L.NODE_URL = "setTimeout(() => {}, 20000)"       # a stalled parser respects the budget
            start = time.monotonic()
            with self.assertRaises(self.L.NormalizeError):
                self.L.run_checks([f"{b}/ok"], retries=1, wait=0, timeout=5, budget=0.5)
            self.assertLess(time.monotonic() - start, 3)
        finally:
            self.L.NODE_URL = saved

    def test_nothing_starts_after_the_deadline(self):
        # The up-front parse alone outlasts a 1 ms budget: no request may start.
        n = self.hits.get("/counted", 0)
        got = self.L.run_checks([f"{self.base}/counted"], retries=1, wait=0, timeout=5, budget=0.001)
        self.assertIn("budget", got[f"{self.base}/counted"][1])
        time.sleep(0.2)
        self.assertEqual(self.hits.get("/counted", 0), n)

    def test_per_host_limit_covers_redirect_hops(self):
        port = self.srv.server_address[1]
        self.conc[1] = 0
        urls = [f"http://localhost:{port}/redir-conc?{i}" for i in range(8)] + \
               [f"http://127.0.0.1:{port}/conc?d{i}" for i in range(8)]
        got = self.L.run_checks(urls, retries=1, wait=0, timeout=5, budget=60)
        self.assertTrue(all(v == ("OK", "") for v in got.values()), got)
        self.assertLessEqual(self.conc[1], self.L.PER_HOST)

    def test_no_body_is_read_and_the_budget_bounds_the_run(self):
        start = time.monotonic()
        self.assertEqual(self.L.check(f"{self.base}/big-redirect", 1, 0, 10), ("OK", ""))
        self.assertLess(time.monotonic() - start, 2.5)    # a read() of the redirect body would block 3 s
        start = time.monotonic()
        got = self.L.run_checks([f"{self.base}/slow", f"{self.base}/ok"], retries=1, wait=0, timeout=10, budget=0.8)
        self.assertLess(time.monotonic() - start, 2.5)
        self.assertEqual(got[f"{self.base}/ok"], ("OK", ""))
        self.assertEqual(got[f"{self.base}/slow"][0], "UNVERIFIED")
        self.assertIn("budget", got[f"{self.base}/slow"][1])

    def test_main_exit_codes_allowlist_and_list(self):
        from contextlib import redirect_stdout, redirect_stderr
        import io
        b = self.base
        allow = Path(tempfile.mkdtemp(prefix="allow-")) / "allow.txt"
        saved = (self.L.collect, self.L.ALLOWLIST)

        def run(links, allow_text, *argv):
            allow.write_text(allow_text, encoding="utf-8")
            self.L.collect, self.L.ALLOWLIST = (lambda: links), allow
            out, err = io.StringIO(), io.StringIO()
            with redirect_stdout(out), redirect_stderr(err):
                rc = self.L.main(["--retries", "1", "--wait", "0", *argv])
            return rc, out.getvalue(), err.getvalue()
        try:
            rc, out, _ = run({f"{b}/ok": ["docs/a.md:3"], f"{b}/down": ["docs/a.md:4"]}, "")
            self.assertEqual(rc, 0)                                  # unverified never fails
            self.assertIn(f"UNVERIFIED  {b}/down  (HTTP 503)  docs/a.md:4", out)
            rc, out, _ = run({f"{b}/gone": ["docs/a.md:9"]}, "")
            self.assertEqual(rc, 1)
            self.assertIn(f"DEAD        {b}/gone  (HTTP 404)  docs/a.md:9", out)
            rc, out, _ = run({"http://exa mple.invalid/": ["docs/a.md:2"], f"{b}/ok": ["docs/a.md:3"]}, "")
            self.assertEqual(rc, 1)                                  # a malformed link fails the run
            self.assertIn("DEAD        http://exa mple.invalid/  (malformed URL", out)
            n = self.hits.get("/counted", 0)
            rc, out, err = run({f"{b}/counted": ["docs/a.md:1"]},
                               f"{b}/counted  # fixture reason\nhttps://unused.invalid/  # nothing links here\n")
            self.assertEqual((rc, self.hits.get("/counted", 0)), (0, n))  # allowlisted: never fetched
            self.assertIn("STALE ALLOW https://unused.invalid/", out)
            self.assertIn("1 allowlisted", err)
            rc, _, err = run({f"{b}/ok": ["docs/a.md:1"]}, "https://bad.invalid/\n")
            self.assertEqual(rc, 1)                                  # an entry without a reason is an error
            self.assertIn("allow.txt:1", err)
            rc, out, _ = run({f"{b}/counted": ["docs/a.md:1"]}, "", "--list")
            self.assertEqual((rc, self.hits.get("/counted", 0)), (0, n))  # --list checks nothing
            self.assertIn(f"{b}/counted  docs/a.md:1", out)
            rc, _, _ = run({}, "")
            self.assertEqual(rc, 0)
            with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as cm:
                self.L.main(["--retries", "x"])
            self.assertEqual(cm.exception.code, 2)
        finally:
            self.L.collect, self.L.ALLOWLIST = saved

    def test_dns_failures_are_dead_only_when_definite(self):
        import socket
        import urllib.error
        nx = urllib.error.URLError(socket.gaierror(socket.EAI_NONAME, "Name or service not known"))
        again = urllib.error.URLError(socket.gaierror(socket.EAI_AGAIN, "Temporary failure in name resolution"))
        self.assertEqual(self.L.classify_error(nx)[0], "DEAD")
        self.assertEqual(self.L.classify_error(again)[0], "UNVERIFIED")
        self.assertEqual(self.L.classify_error(urllib.error.URLError(ConnectionRefusedError()))[0], "UNVERIFIED")
        self.assertEqual(self.L.classify_error(urllib.error.URLError(TimeoutError()))[0], "UNVERIFIED")

    def test_collection_skips_code_and_host_hints(self):
        out: dict[str, list[str]] = {}
        self.L.markdown_links("# T\n\n[a](https://a.invalid/x#frag) <https://b.invalid/>\n`https://code.invalid/`\n\n"
                              "```\nhttps://fence.invalid/\n```\n\n<p><a href=\"https://raw.invalid/\">r</a></p>\n\n"
                              "![i](https://img.invalid/i.png) [rel](../x.md)\n", "docs/t.md", out)
        self.assertEqual(sorted(out), ["https://a.invalid/x", "https://b.invalid/", "https://img.invalid/i.png",
                                       "https://raw.invalid/"])
        self.assertEqual(out["https://raw.invalid/"], ["docs/t.md:10"])
        page: dict[str, list[str]] = {}
        self.L.html_links('<link rel="preconnect" href="https://fonts.invalid">\n<link href="https://css.invalid/s.css" '
                          'rel="stylesheet">\n<script src=\'https://js.invalid/a.js\'></script>', "gh-pages/i.html", 1, page)
        self.assertEqual(sorted(page), ["https://css.invalid/s.css", "https://js.invalid/a.js"])
        tricky: dict[str, list[str]] = {}
        self.L.html_links('<!-- <a href="https://commented.invalid/">old</a> -->\n'
                          '<a data-href="https://data.invalid/">x</a>\n'
                          '<a title="use href=https://title.invalid/ here" href="https://real.invalid/">y</a>\n'
                          '<a title="a > b" href="https://after-gt.invalid/">z</a>', "gh-pages/t.html", 1, tricky)
        self.assertEqual(tricky, {"https://real.invalid/": ["gh-pages/t.html:3"],
                                  "https://after-gt.invalid/": ["gh-pages/t.html:4"]})

    def test_allowlist_needs_a_reason(self):
        p = Path(tempfile.mkdtemp(prefix="allow-")) / "allow.txt"
        p.write_text("# comment\nhttps://ok.invalid/  # admin page, 404 signed out\nhttps://bare.invalid/\nnot-a-url  # why\n")
        prefixes, errors = self.L.load_allowlist(p)
        self.assertEqual(prefixes, ["https://ok.invalid/"])
        self.assertEqual(len(errors), 2)

    def test_weekly_workflow_runs_the_checker(self):
        import yaml
        wf = yaml.safe_load((B.REPO / ".github" / "workflows" / "linkcheck.yml").read_text(encoding="utf-8"))
        self.assertTrue(wf[True]["schedule"])      # PyYAML reads the bare key `on` as True
        steps = [s.get("run", "") for s in wf["jobs"]["linkcheck"]["steps"]]
        self.assertIn("python3 scripts/site/linkcheck.py", steps)


class PageContract(unittest.TestCase):
    """docs/contributing/: the folder map must track the roadmap's domains, and the
    template must render without claiming to document anything."""
    CONTRACT = B.REPO / "docs" / "contributing" / "docs-page-contract.md"
    TEMPLATE = B.REPO / "docs" / "contributing" / "_template.md"

    def test_folder_map_lists_every_domain(self):
        text = self.CONTRACT.read_text(encoding="utf-8")
        domains = sorted(d.name for d in (B.REPO / "todo").iterdir()
                         if d.is_dir() and d.name[:2].isdigit() and d.name[2] == "-")
        self.assertTrue(domains)
        missing = [d for d in domains if not re.search(rf"^\| `{re.escape(d)}` +\| `docs/[a-z-]+/` +\|$", text, re.M)]
        self.assertEqual(missing, [], "domains with no row in the contract's folder map")

    def test_roadmap_sections_follow_the_folder_map(self):
        # TODO-10's per-domain sections name the docs/ folder each page goes in;
        # they must agree with the contract, or two authors place one page twice.
        mapping = dict(re.findall(r"^\| `(\d\d-[a-z-]+)` +\| `(docs/[a-z-]+/)` +\|$",
                                  self.CONTRACT.read_text(encoding="utf-8"), re.M))
        roadmap = (B.REPO / "todo" / "00-infrastructure" / "TODO-10-documentation-site.md").read_text(encoding="utf-8")
        folder, checked, wrong = None, 0, []
        for line in roadmap.splitlines():
            m = re.match(r"- \[.\] Pages? in `(docs/[a-z-]+/)`", line)
            if m:
                folder = m.group(1)
                continue
            f = re.match(r"  - `todo/(\d\d-[a-z-]+)/", line)
            if f and folder:
                checked += 1
                if mapping.get(f.group(1)) != folder:
                    wrong.append((f.group(1), folder))
            elif not line.startswith("  "):
                folder = None
        self.assertGreater(checked, 150)
        self.assertEqual(wrong, [])

    def test_template_claims_nothing(self):
        d = B.parse_directives(self.TEMPLATE.read_text(encoding="utf-8"))
        self.assertEqual((d.get("covers"), d.get("sources"), d.get("reviewed")), ("", "", ""))
        self.assertEqual(B.directive_sources(self.TEMPLATE.read_text(encoding="utf-8")), [])

    def test_template_links_survive_a_copy(self):
        # The template is copied into other docs/<folder>/ directories, so a
        # folder-relative link would go dead there; ../contributing/ works from any.
        links = re.findall(r"\]\(([^)#]+)", self.TEMPLATE.read_text(encoding="utf-8"))
        self.assertTrue(links)
        self.assertEqual([l for l in links if not l.startswith(("../contributing/", "https://"))], [])


if __name__ == "__main__":
    unittest.main(verbosity=1)
