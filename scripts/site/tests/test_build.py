#!/usr/bin/env python3
"""Unit tests for scripts/site/build.py and gen_theme_header.py (stdlib unittest).

Run: python3 scripts/site/tests/test_build.py
"""

from __future__ import annotations

import json
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

if __name__ == "__main__":
    unittest.main(verbosity=1)
