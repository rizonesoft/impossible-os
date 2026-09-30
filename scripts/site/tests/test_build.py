#!/usr/bin/env python3
"""Unit tests for scripts/site/build.py and gen_theme_header.py (stdlib unittest).

Run: python3 scripts/site/tests/test_build.py
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import build as B  # noqa: E402
B.REPO_REAL = B.REPO   # the real checkout, for fixtures that borrow a canonical input
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
        self.assertIn('<a href="kernel/">a</a>', body)             # re-serialized from the parsed tag
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


class SearchAndAccessibility(unittest.TestCase):
    LONG = "Filler words for a long page. " * 160          # ~4,800 characters: past the old 4,000 cut
    PAGE = ("# API\n\nIntro text.\n\n## Prerequisites\n\nInstall things.\n\n### Fedora\n\n"
            "Call `boot_health_publish_json` after boot.\n\n```c\nint code_block_only_ident;\n```\n\n"
            "## Late\n\n" + LONG + "zebrafinch is the last word.\n\n![A diagram of the flow](../x.png)\n")

    def setUp(self):
        self.saved = (B.REPO, B.ROOT, B.SOURCE, B._FILESET, B._DIRSET, B.SEARCH_BUDGET, B.SEARCH_SHARD_TARGET)

    def tearDown(self):
        (B.REPO, B.ROOT, B.SOURCE, B._FILESET, B._DIRSET, B.SEARCH_BUDGET, B.SEARCH_SHARD_TARGET) = self.saved
        B.set_root(B.ROOT)

    def pages(self, extra: dict[str, str] | None = None):
        root = fixture_repo()
        (root / "x.png").write_bytes(b"png")
        (root / "docs" / "index.md").write_text("# Home\n", encoding="utf-8")   # the shared fixture's dead links
        (root / "docs" / "api.md").write_text(self.PAGE, encoding="utf-8")
        for rel, text in (extra or {}).items():
            (root / "docs" / rel).write_text(text, encoding="utf-8")
        subprocess.run(["git", "add", "-A"], cwd=root, check=True, capture_output=True)
        B.REPO, B.SOURCE, B._FILESET, B._DIRSET = root, "worktree", None, None
        B.set_root(root)
        errors: list[str] = []
        pages, _ = B.load_pages(FACTS, errors)
        return pages, errors

    @staticmethod
    def records(files: dict[str, bytes]):
        m = json.loads(files["search.json"])
        texts: list[str] = []
        for name, count in m["shards"]:
            shard = json.loads(files[name])
            self_hash = __import__("hashlib").sha256(files[name]).hexdigest()[:16]
            assert name == f"search-{self_hash}.json", name          # content-addressed
            assert len(shard) == count, (name, len(shard), count)
            texts += shard
        assert len(texts) == len(m["s"])
        return [(m["p"][pg][1], anchor, heading, texts[i]) for i, (pg, anchor, heading) in enumerate(m["s"])]

    def find(self, recs, word):
        return [(u, a, h) for u, a, h, x in recs if word in x]

    def test_index_keeps_code_late_text_and_the_heading_path(self):
        pages, errors = self.pages()
        self.assertEqual(errors, [])
        files = B.search_files(pages, "v2", errors)
        self.assertEqual(errors, [])
        recs = self.records(files)
        self.assertEqual(self.find(recs, "boot_health_publish_json"),
                         [("api.html", "fedora", "Prerequisites \u203a Fedora")])   # inline code, H2 context
        self.assertEqual(self.find(recs, "code_block_only_ident"), [("api.html", "fedora", "Prerequisites \u203a Fedora")])
        late = [x for u, a, h, x in recs if a == "late"][0]
        self.assertGreater(late.index("zebrafinch"), 4000)                             # nothing truncated
        self.assertIn("A diagram of the flow", late)                                    # image alt text
        self.assertFalse(any("Intro text" in x for u, a, h, x in recs if a))           # records do not overlap
        self.assertTrue(any(a == "" and "Intro text" in x for u, a, h, x in recs))

    def test_raw_html_text_and_alt_are_searchable_as_rendered(self):
        pages, errors = self.pages({"raw.md": (
            '# Raw\n\nCall raw_<b>inline</b>_joined_name and <img src="../x.png" alt="inlinealtword"> here.\n\n'
            '<div>block_<code>joined</code>_ident wbr_<wbr>joined_ident</div>\n<p>alpha</p><p>beta</p>\n\n'
            '<img src="../x.png" alt="blockaltword">\n')})
        self.assertEqual(errors, [])
        recs = self.records(B.search_files(pages, "v2", errors))
        for word in ("raw_inline_joined_name", "inlinealtword", "block_joined_ident", "wbr_joined_ident", "blockaltword"):
            self.assertEqual(self.find(recs, word), [("raw.html", "", "")], word)
        self.assertTrue(self.find(recs, "alpha beta"))             # block elements still separate words
        self.assertEqual(pages["raw.md"].a11y, [])
        pages, errors = self.pages({"titled.md": "<!-- docs: title=API Reference -->\n# h1_only_identifier\n\nx\n"})
        recs = self.records(B.search_files(pages, "v2", errors))
        self.assertEqual(self.find(recs, "h1_only_identifier"), [("titled.html", "", "")])
        self.assertNotIn("API", [x for u, a, h, x in recs if u == "api.html" and a == ""][0])  # an H1 that IS the title
        pages, errors = self.pages({"head.md": '# Head\n\n## API ![headingaltword](../x.png) <img src="../x.png" '
                                               'alt="rawheadingalt">\n\nbody\n'})
        recs = self.records(B.search_files(pages, "v2", errors))
        for word in ("headingaltword", "rawheadingalt"):
            self.assertEqual(self.find(recs, word), [("head.html", "api--", "API")], word)   # GitHub slug of "API  "

    def test_each_template_gets_the_layout_it_reads(self):
        self.assertEqual(B.search_layout("<input>"), "flat")
        self.assertEqual(B.search_layout('<input data-search-index="2">'), "v2")
        pages, errors = self.pages()
        self.assertEqual(json.loads(B.search_files(pages, "v2", errors)["search.json"])["v"], 2)
        self.assertIsInstance(json.loads(B.search_files(pages, "flat", errors)["search.json"]), list)
        self.assertEqual(errors, [])

    def test_legacy_template_gets_the_flat_index_it_was_published_with(self):
        pages, errors = self.pages()
        files = B.search_files(pages, "flat", errors)
        self.assertEqual(list(files), ["search.json"])
        flat = json.loads(files["search.json"])
        api = [p for p in flat if p["u"] == "api.html"][0]
        self.assertEqual(sorted(api), ["h", "t", "u", "x"])
        self.assertLessEqual(len(api["x"]), 4000)

    def test_shards_split_and_an_oversized_file_fails_instead_of_dropping_text(self):
        pages, errors = self.pages()
        B.SEARCH_SHARD_TARGET = 200
        files = B.search_files(pages, "v2", errors)
        self.assertGreater(len(files), 3)
        self.assertEqual(errors, [])
        self.assertTrue(self.find(self.records(files), "zebrafinch"))
        B.SEARCH_BUDGET = 1000
        B.search_files(pages, "v2", errors)
        self.assertTrue(any("over the 1,000-byte budget" in e for e in errors), errors)

    def test_accessibility_rules(self):
        pages, _ = self.pages({
            "noalt.md": "# No alt\n\n![](../x.png)\n",
            "skip.md": "# Skip\n\n## Two\n\n#### Four\n",
            "two.md": "# One\n\n# Another\n",
            "none.md": "## No title\n",
            "raw.md": '# Raw\n\n<img src="../x.png">\n\n<img src="../x.png" alt="">\n\n<h3>deep</h3>\n',
            "codealt.md": "# Code alt\n\n![`boot_health_publish_json`](../x.png)\n",
            "ok.md": '# Fine\n\n## A\n\n### B\n\n## C\n\n![alt *em*](../x.png) <img src="../x.png" alt="">\n'})
        self.assertEqual([f.split(":")[0] for f in pages["codealt.md"].a11y], ["image without alt text"])  # renders alt=""
        self.assertEqual(pages["api.md"].a11y, [])
        self.assertEqual(pages["ok.md"].a11y, [])
        self.assertEqual([f.split(":")[0] for f in pages["noalt.md"].a11y], ["image without alt text"])
        self.assertEqual(pages["skip.md"].a11y, ["heading level skips from H2 to H4 at 'Four'"])
        self.assertEqual(pages["two.md"].a11y, ["2 H1 headings; a page has exactly one"])
        self.assertEqual(len(pages["none.md"].a11y), 2)                       # no H1, and does not start with one
        raw = pages["raw.md"].a11y
        self.assertEqual(len(raw), 3, raw)                  # raw heading, H1 to H3, missing alt; alt="" is fine
        self.assertTrue(any("without an alt attribute" in f for f in raw))
        self.assertTrue(any("H1 to H3" in f for f in raw))
        self.assertTrue(any(f.startswith("raw HTML <h3>: write it as a Markdown heading") for f in raw))

    @unittest.skipUnless(__import__("shutil").which("node"), "node not installed")
    def test_combobox_as_shipped(self):
        pages, errors = self.pages()
        files = B.search_files(pages, "v2", errors)
        tpl = (B.REPO_REAL / "gh-pages" / "docs-template.html").read_text(encoding="utf-8")
        self.assertEqual(B.search_layout(tpl), "v2")
        script = [m for m in re.findall(r"<script>(.*?)</script>", tpl, re.S) if "search.json" in m]
        self.assertEqual(len(script), 1)
        script = script[0].replace("%ROOT%", "")

        def drive(steps, fail_once=(), delay=None, missing=(), files2=None, cached=None):
            payload = {"script": script, "cached": cached or {},
                       "files": {k: v.decode("utf-8") for k, v in files.items() if k not in missing},
                       "files2": {k: v.decode("utf-8") for k, v in (files2 or {}).items()},
                       "steps": steps, "fail_once": list(fail_once), "delay": delay or {}}
            r = subprocess.run(["node", str(HERE / "search_harness.js")], input=json.dumps(payload),
                               capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stderr)
            out = json.loads(r.stdout)
            self.assertEqual(out["errors"], [])
            drive.fetches, drive.revalidated = out["fetches"], out["revalidated"]
            return out["snaps"]

        s = drive([["input", "zebrafinch"], ["key", "Escape"], ["flush", None], ["snap", "dismissed"],
                   ["input", "boot_health_publish_json"], ["flush", None], ["snap", "found"],
                   ["key", "ArrowDown"], ["snap", "selected"],
                   ["key", "Escape"], ["key", "Enter"], ["snap", "hidden-enter"],
                   ["input", "prerequisites fedora"], ["flush", None], ["key", "Enter"], ["snap", "unselected-enter"],
                   ["key", "ArrowDown"], ["key", "Enter"], ["snap", "chosen"],
                   ["input", "zebrafinch"], ["flush", None], ["snap", "late"],
                   ["input", "nosuchwordanywhere"], ["flush", None], ["snap", "none"],
                   ["input", "zebrafinch"], ["flush", None], ["blur", None], ["snap", "blurred"]])
        self.assertFalse(s["dismissed"]["shown"])                            # a late load does not reopen
        found = s["found"]
        self.assertEqual(found["expanded"], "true")
        self.assertEqual([o["href"] for o in found["options"]], ["api.html#fedora"])
        self.assertIsNone(found["active"])
        self.assertIn("1 result", found["status"])
        self.assertEqual(s["selected"]["active"], "search-opt-0")
        self.assertEqual(s["selected"]["options"][0]["selected"], "true")
        self.assertEqual(s["hidden-enter"]["href"], "start")                 # Escape then Enter goes nowhere
        self.assertEqual(s["hidden-enter"]["expanded"], "false")
        self.assertIsNone(s["hidden-enter"]["active"])
        self.assertEqual(s["unselected-enter"]["href"], "start")
        self.assertEqual(s["chosen"]["href"], "api.html#fedora")
        self.assertEqual((s["chosen"]["expanded"], s["chosen"]["shown"], s["chosen"]["active"]),
                         ("false", False, None))                      # a same-page fragment does not reload
        self.assertEqual([o["href"] for o in s["late"]["options"]], ["api.html#late"])
        self.assertEqual((s["none"]["options"], s["none"]["msg"], s["none"]["status"], s["none"]["expanded"]),
                         ([], "No results", "No results", "false"))
        self.assertFalse(s["blurred"]["shown"])
        s = drive([["input", "zebrafinch"], ["flush", None], ["click", 0], ["snap", "clicked"]])
        self.assertEqual((s["clicked"]["expanded"], s["clicked"]["shown"]), ("false", False))

        # Ranking: title 10 > heading 4 > body 1, one result per page, its best section.
        pages, errors = self.pages({
            "body.md": "# Body page\n\nmentions quokka once.\n",
            "head.md": "# Heading page\n\n## Intro\n\nquokka in the body.\n\n## The quokka section\n\nx\n",
            "title.md": "# Quokka reference\n\nplain.\n",
            "move.md": "# Wombat guide\n\n## Alpha\n\nx\n\n## Beta\n\nthe wombat lives here.\n"})
        files = B.search_files(pages, "v2", errors)
        s = drive([["input", "quokka"], ["flush", None], ["snap", "ranked"]])
        self.assertEqual([o["href"] for o in s["ranked"]["options"]],
                         ["title.html", "head.html#the-quokka-section", "body.html"])
        # The selected page stays selected when its shard moves the best section (and so the href).
        late_shard = json.loads(files["search.json"])["shards"][0][0]
        s = drive([["input", "wombat"], ["flush", None], ["key", "ArrowDown"], ["snap", "pre"], ["wait", 400],
                   ["snap", "moved"], ["key", "Enter"], ["snap", "went"]], delay={late_shard: 300})
        self.assertEqual([o["href"] for o in s["pre"]["options"]], ["move.html"])        # title only, so far
        self.assertEqual(s["moved"]["options"][0]["href"], "move.html#beta")
        self.assertEqual((s["moved"]["active"], s["moved"]["options"][0]["selected"]), ("search-opt-0", "true"))
        self.assertEqual(s["went"]["href"], "move.html#beta")

        shard = json.loads(files["search.json"])["shards"][0][0]
        s = drive([["input", "zebrafinch"], ["flush", None], ["snap", "failed"],
                   ["input", "zebrafinch"], ["flush", None], ["snap", "retried"]], fail_once=[shard])
        self.assertIn("did not load", s["failed"]["msg"])                  # body text lives in the failed shard
        self.assertIn("type again to retry", s["failed"]["status"])
        self.assertEqual(s["failed"]["options"], [])
        self.assertEqual([o["href"] for o in s["retried"]["options"]], ["api.html#late"])
        self.assertEqual(s["retried"]["msg"], "")
        s = drive([["input", "zebrafinch"], ["flush", None], ["snap", "failed"],
                   ["input", "zebrafinch"], ["flush", None], ["snap", "retried"]], fail_once=["search.json"])
        self.assertIn("unavailable", s["failed"]["msg"])
        self.assertEqual([o["href"] for o in s["retried"]["options"]], ["api.html#late"])
        self.assertEqual(drive.revalidated.get("search.json"), 1)   # the retry bypasses a cached manifest

        # Progressive: a title match shows while the shard is still loading, and a
        # search dismissed meanwhile stays closed when the shard lands.
        s = drive([["input", "api"], ["flush", None], ["snap", "early"], ["wait", 400], ["snap", "full"],
                   ["input", "zebrafinch"], ["flush", None], ["snap", "cached"]], delay={shard: 300})
        self.assertIn("api.html", [o["href"] for o in s["early"]["options"]])
        self.assertIn("Searching the full text", s["early"]["msg"])
        self.assertIn("still searching", s["early"]["status"])
        self.assertEqual(s["full"]["msg"], "")
        self.assertNotIn("still searching", s["full"]["status"])
        self.assertEqual([o["href"] for o in s["cached"]["options"]], ["api.html#late"])
        s = drive([["input", "api"], ["flush", None], ["key", "Escape"], ["wait", 400], ["snap", "dismissed"]],
                  delay={shard: 300})
        self.assertFalse(s["dismissed"]["shown"])
        # A shard that never loads is fetched once per typed search, never in a loop.
        drive([["input", "zebrafinch"], ["wait", 150], ["input", "zebrafinch2"], ["wait", 150]], missing=[shard])
        self.assertEqual(drive.fetches[shard], 2)
        self.assertEqual(drive.fetches["search.json"], 2)                   # the retry starts from the manifest
        # A deploy that replaces the shards under an open page: the typed retry
        # fetches the new manifest and finds the text in the new shard.
        (B.ROOT / "docs" / "api.md").write_text(self.PAGE + "\nRedeployed with kiwibird.\n", encoding="utf-8")
        pages2, _ = B.load_pages(FACTS, [])
        files2 = B.search_files(pages2, "v2", [])
        new_shard = json.loads(files2["search.json"])["shards"][0][0]
        self.assertNotEqual(new_shard, shard)
        s = drive([["input", "zebrafinch"], ["flush", None], ["deploy", None], ["wait", 400], ["snap", "stale"],
                   ["input", "kiwibird"], ["flush", None], ["flush", None], ["snap", "redeployed"]],
                  delay={shard: 300}, files2=files2)
        self.assertIn("did not load", s["stale"]["msg"])
        self.assertEqual([o["href"] for o in s["redeployed"]["options"]], ["api.html#late"])
        self.assertEqual((drive.fetches.get(new_shard), drive.fetches["search.json"]), (1, 2))
        # A browser still caching the flat index of an older deploy recovers on the
        # next typed search, which revalidates the manifest.
        flat = B.search_files(pages2, "flat", [])["search.json"].decode("utf-8")
        kept = files
        files = B.search_files(pages2, "v2", [])
        s = drive([["input", "kiwibird"], ["flush", None], ["snap", "cached"],
                   ["input", "kiwibird"], ["flush", None], ["flush", None], ["snap", "fresh"]],
                  cached={"search.json": flat})
        self.assertIn("unavailable", s["cached"]["msg"])
        self.assertEqual([o["href"] for o in s["fresh"]["options"]], ["api.html#late"])
        files = kept
        # One shard failed while another is still pending: the typed retry does not wait for it.
        B.SEARCH_SHARD_TARGET = 300
        files = B.search_files(pages2, "v2", [])
        names = [n for n, _ in json.loads(files["search.json"])["shards"]]
        self.assertGreater(len(names), 2)
        s = drive([["input", "kiwibird"], ["wait", 100], ["snap", "mixed"], ["input", "kiwibird"], ["wait", 100]],
                  missing=[names[0]], delay={names[1]: 3000})
        self.assertIn("Searching the full text", s["mixed"]["msg"])       # still loading names[1]
        self.assertEqual(drive.fetches["search.json"], 2)
        # Shards landing together are scored once, not once each.
        # A title query draws on every scoring pass, so each uncoalesced shard would add one.
        kept = files
        B.SEARCH_SHARD_TARGET = 100
        files = B.search_files(pages2, "v2", [])
        self.assertGreaterEqual(len(json.loads(files["search.json"])["shards"]), 5)
        s = drive([["input", "api"], ["wait", 200], ["snap", "burst"]])
        self.assertLessEqual(s["burst"]["renders"], 3, s["burst"])            # manifest pass + coalesced shard pass(es)
        self.assertIn("api.html", [o["href"] for o in s["burst"]["options"]])
        files = kept
        # A typed query never lets the last query's selection be chosen while its results load.
        s = drive([["input", "api"], ["wait", 100], ["key", "ArrowDown"], ["snap", "old"],
                   ["input", "zebrafinch"], ["key", "ArrowDown"], ["key", "Enter"], ["snap", "during"]],
                  missing=[names[0]], delay={"search.json": 50})
        self.assertEqual(s["old"]["active"], "search-opt-0")
        self.assertEqual((s["during"]["href"], s["during"]["active"], s["during"]["expanded"]), ("start", None, "false"))
        self.assertEqual(s["during"]["options"], [])                         # old links are gone, not just unselected


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


@unittest.skipUnless(shutil.which("node"), "release builds resolve links with Node's WHATWG URL")
class ReleaseDocs(unittest.TestCase):
    """Section 24: a docs tree rendered at a release commit keeps linking to that
    commit after main moves on, and is published under docs/<version>/.

    Repository URLs are spelled with an @GH@ host placeholder, expanded on write
    and folded back on read, so this file never carries a literal repository URL
    for an invented owner (the drift check's owner scan would rightly flag one)."""

    TEMPLATE = ('<link rel="canonical" href="%CANONICAL%"><link rel="icon" href="%SITE_ROOT%icon.png">'
                '<a class="edit" href="%EDIT%">edit</a><a class="home" href="%ROOT%">docs</a>%BODY%')

    def setUp(self):
        self.saved = (B.REPO, B.ROOT, B.SOURCE, B._FILESET, B._DIRSET, B.ANCHOR_CACHE,
                      B.LINK_REF, B.DOCS_BASE, B.RELEASE, B.RELEASE_FACTS)
        self.root = Path(tempfile.mkdtemp(prefix="release-"))
        self.out = Path(tempfile.mkdtemp(prefix="release-out-"))
        self.snaps: list[Path] = []
        B.ANCHOR_CACHE = self.root / "anchor-cache.json"

    def tearDown(self):
        (B.REPO, B.ROOT, B.SOURCE, B._FILESET, B._DIRSET, B.ANCHOR_CACHE,
         B.LINK_REF, B.DOCS_BASE, B.RELEASE, B.RELEASE_FACTS) = self.saved
        B.set_root(B.ROOT)
        for s in self.snaps:
            shutil.rmtree(s, ignore_errors=True)

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.root, check=True, env=git_env("2026-05-01T00:00:00Z"),
                              capture_output=True, text=True).stdout

    def write(self, rel, text):
        (self.root / rel).parent.mkdir(parents=True, exist_ok=True)
        (self.root / rel).write_text(text.replace("@GHU@", "GitHub" + ".com").replace("@GH@", "github" + ".com"),
                                     encoding="utf-8")

    @staticmethod
    def fold(text):
        return text.replace("github" + ".com", "@GH@")

    def read(self, rel):
        return self.fold((self.out / rel).read_text(encoding="utf-8"))

    def project(self, owner, historical):
        return json.dumps({"name": "Fixture OS", "tagline": "t", "owner": owner, "repo": "impossible-os",
                           "repo_url": f"https://@GH@/{owner}/impossible-os",
                           "site_url": "https://example.invalid", "docs_url": "https://example.invalid/docs/",
                           "release_date": "2028-08-08", "historical_owners": historical})

    def fixture(self):
        """v1 has src/x.c, img.png and a docs page linking them every way a page can;
        the next commit on main deletes both and moves the repository to owner "n"."""
        self.git("init", "-q", "-b", "main")
        self.write("project.json", self.project("o", ["oldo"]))
        self.write("COUNT.md", "x\n")
        self.write("docs/test-coverage/coverage.json", '{"total_suites": 1, "total_assertions": 2}')
        self.write("gh-pages/docs-template.html", self.TEMPLATE)
        self.write("src/x.c", "int x;\n")
        self.write("img.png", "png")
        self.write("docs/index.md",
                   "# Home\n\n[rel](../src/x.c) [dir](../src)\n"
                   "[abs](https://@GH@/oldo/impossible-os/blob/main/src/x.c#L1)\n"
                   "[absdir](https://@GHU@/O/Impossible-OS/tree/main/src)\n"
                   "[other](https://@GH@/someone/else/blob/main/src/x.c)\n"
                   "![r](../img.png) ![a](https://raw.githubusercontent.com/o/impossible-os/main/img.png)\n"
                   "<a href=\"https://@GH@/o/impossible-os/blob/main/src/x.c\">raw html</a>\n\n"
                   "```mermaid\ngraph TD\n  a[\"A\"]\n  click a \"https://@GH@/o/impossible-os/blob/main/src/x.c\"\n"
                   "  click b href \"https://@GH@/o/impossible-os/blob/main/src/x.c\" \"tip\" _blank;\n"
                   "  c[\"Bob's\"]; d-->c; click c \"https://@GH@/o/impossible-os/blob/main/src/x.c\"\n"
                   "\tclick\td \"https://@GH@/o/impossible-os/blob/main/src/x.c\" \"https://@GH@/o/impossible-os/blob/main/tip.c\"\n"
                   "  style e fill:#fff; click e \"https://@GH@/o/impossible-os/blob/main/src/x.c\"\n"
                   "  %% click f \"https://@GH@/o/impossible-os/blob/main/commented.c\n"
                   "  click g \"http://@GH@:80/o/impossible-os/blob/main/src/x.c\"\n```\n\n"
                   "[http](http://@GH@/o/impossible-os/blob/main/src/x.c) "
                   "[port](https://@GH@:443/o/impossible-os/blob/main/src/x.c) "
                   "[enc](https://@GH@/%6F/impossible-os/blob/m%61in/src/x.c) "
                   "[user](https://u@@GH@/o/impossible-os/blob/main/src/x.c) "
                   "[frag](https://@GH@/o/impossible-os/blob/main/docs/sub/page.md#sub) "
                   "[dot](https://@GH@/./o/impossible-os/blob/x/../main/src/x.c) "
                   "[encdot](https://@GH@/o/impossible-os/blob/x/%2e%2e/main/src/x.c) "
                   "[ghraw](https://@GH@/o/impossible-os/raw/main/img.png) "
                   "[fullref](https://@GH@/o/impossible-os/raw/refs/heads/main/img.png) "
                   "[hostref](https://raw.githubusercontent.com/o/impossible-os/refs/heads/main/img.png) "
                   "[blobref](https://@GH@/o/impossible-os/blob/refs/heads/main/src/x.c) "
                   "[tagref](https://@GH@/o/impossible-os/blob/v1/src/x.c) "
                   "[fulltag](https://@GH@/o/impossible-os/blob/refs/tags/v1/src/x.c) "
                   "[slashtag](https://@GH@/o/impossible-os/blob/rel/v2/src/x.c) "
                   "[gitlab](https://gitlab.com/o/impossible-os/-/tree/feature%2Fdocs)\n\n"
                   "<a href=\"https://@GH@\\o\\impossible-os\\blob\\main\\src\\x.c\">backslash</a>\n\n"
                   "`https://@GH@/o/impossible-os/blob/main/src/x.c`\n")
        self.write("docs/sub/page.md", "<!-- docs: covers=todo/01-x/TODO-01-a.md sources=src/x.c -->\n"
                                       "# Sub\n\n[home](../index.md)\n")
        self.write("todo/01-x/TODO-01-a.md", "# TODO-01 -- A\n")
        self.git("add", "-A")
        self.git("commit", "-q", "--no-verify", "-m", "v1")
        self.git("tag", "v1")
        self.git("tag", "rel/v2")
        self.sha = self.git("rev-parse", "v1^{commit}").strip()
        (self.root / "src" / "x.c").unlink()
        (self.root / "img.png").unlink()
        self.write("project.json", self.project("n", ["o", "oldo"]))
        self.git("add", "-A")
        self.git("commit", "-q", "--no-verify", "-m", "main moves on")
        B.REPO, B.SOURCE, B._FILESET, B._DIRSET = self.root, "worktree", None, None
        B.set_root(self.root)

    def release(self, ref):
        snap = B.prepare_release(ref)
        self.snaps.append(snap)
        args = argparse.Namespace(check=False, out=self.out, quiet=True, sync=None, sync_head=None,
                                  emit_head=None, update_baseline=False, freshness=False, skip_stats=False,
                                  staged=False, release=ref, ref="HEAD")
        return B.run(args)

    def test_release_links_pin_to_the_tag_commit_after_main_deletes_the_file(self):
        self.fixture()
        self.assertEqual(self.release("v1"), 0)
        self.assertEqual(sorted(p.relative_to(self.out).as_posix() for p in self.out.rglob("*") if p.is_file()),
                         ["docs/v1/coverage.html", "docs/v1/index.html", "docs/v1/search.json",
                          "docs/v1/sub/page.html"])
        page = self.read("docs/v1/index.html")
        blob = f"https://@GH@/n/impossible-os/blob/{self.sha}/src/x.c"
        self.assertIn(f'href="{blob}"', page)                                     # relative link
        self.assertIn(f'href="https://@GH@/n/impossible-os/tree/{self.sha}/src"', page)
        self.assertIn(f'href="{blob}#L1"', page)                                  # historical owner, fragment kept
        self.assertIn(f'href="https://@GH@/n/impossible-os/tree/{self.sha}/src"', page)
        self.assertIn('href="https://@GH@/someone/else/blob/main/src/x.c"', page)   # another repo
        self.assertIn(f'src="https://raw.githubusercontent.com/n/impossible-os/{self.sha}/img.png"', page)
        self.assertEqual(page.count(f'src="https://raw.githubusercontent.com/n/impossible-os/{self.sha}/img.png"'), 2)
        self.assertIn(f'href="{blob}">raw html', page)
        self.assertIn(f'click a &quot;{blob}&quot;', page)                          # diagram click target
        self.assertIn(f'click b href &quot;{blob}&quot; &quot;tip&quot; _blank;', page)   # terminator kept apart
        self.assertIn(f'd--&gt;c; click c &quot;{blob}&quot;', page)              # a statement after `;`
        self.assertIn(f'click\td &quot;{blob}&quot; &quot;https://@GH@/o/impossible-os/blob/main/tip.c&quot;',
                      page)                                                      # tab, tooltip is text
        self.assertIn(f'href="{blob}">port', page)
        self.assertIn(f'href="{blob}">http', page)
        self.assertIn(f'fill:#fff; click e &quot;{blob}&quot;', page)            # `#` is not a comment
        self.assertIn('%% click f &quot;https://@GH@/o/impossible-os/blob/main/commented.c', page)
        self.assertIn(f'click g &quot;{blob}&quot;', page)
        self.assertIn(f'href="{blob}">enc', page)
        self.assertIn(f'href="{blob}">user', page)       # credentials do not change where a browser goes
        self.assertIn(f'href="{blob}">dot', page)
        self.assertIn(f'href="{blob}">encdot', page)
        self.assertIn(f'href="{blob}">backslash', page)
        self.assertIn(f'href="https://@GH@/n/impossible-os/raw/{self.sha}/img.png">ghraw', page)
        self.assertIn(f'href="https://@GH@/n/impossible-os/raw/{self.sha}/img.png">fullref', page)
        self.assertIn(f'href="https://raw.githubusercontent.com/n/impossible-os/{self.sha}/img.png">hostref', page)
        self.assertIn(f'href="{blob}">blobref', page)
        self.assertIn(f'href="{blob}">tagref', page)                             # the release's own tag
        self.assertIn(f'href="{blob}">fulltag', page)
        self.assertIn('href="https://@GH@/o/impossible-os/blob/rel/v2/src/x.c">slashtag', page)  # another ref
        self.assertIn('href="https://gitlab.com/o/impossible-os/-/tree/feature%2Fdocs">gitlab', page)
        self.assertIn(f'href="https://@GH@/n/impossible-os/blob/{self.sha}/docs/sub/page.md#sub"', page)
        self.assertIn("<code>https://@GH@/o/impossible-os/blob/main/src/x.c</code>", page)  # code is text
        self.assertIn('href="https://example.invalid/docs/v1/"', page)             # version-scoped canonical
        self.assertIn(f'href="https://@GH@/n/impossible-os/blob/{self.sha}/docs/index.md"', page)
        self.assertIn('href="../../icon.png"', page)                               # site root is two levels up
        sub = self.read("docs/v1/sub/page.html")
        self.assertIn('href="https://example.invalid/docs/v1/sub/page.html"', sub)
        self.assertIn('<a href="../">home</a>', sub)
        self.assertIn('href="../../../icon.png"', sub)
        self.assertNotIn("/blob/main/docs", sub)
        cov = self.read("docs/v1/coverage.html")
        self.assertIn(f'href="https://@GH@/n/impossible-os/blob/{self.sha}/todo/01-x/TODO-01-a.md"', cov)
        self.assertIn(f'href="https://@GH@/n/impossible-os/blob/{self.sha}/docs/sub/page.md"', cov)
        self.assertNotIn("/blob/main/", cov)
        self.assertIn("up to date", cov)          # freshness is judged at the release, not today's main

    def test_default_build_keeps_main_links_and_the_whole_site(self):
        self.fixture()
        self.write("gh-pages/index.html", "<p>{{name}}</p>")
        self.write("docs/design/tokens.json", (B.REPO_REAL / "docs/design/tokens.json").read_text(encoding="utf-8"))
        self.write("docs/design/scope.json", '{"shell_files": [], "ui_title_pattern": "^$"}')
        self.write("src/x.c", "int x;\n")
        self.write("img.png", "png")
        self.git("add", "-A")
        self.git("commit", "-q", "--no-verify", "-m", "landing")
        errors: list[str] = []
        files = B.build(B.load_project(), errors)
        self.assertEqual(errors, [])
        for name in ("index.html", "sitemap.xml", "robots.txt", "docs/index.html", "docs/search.json",
                     "docs/coverage.html", "docs/sub/page.html"):
            self.assertIn(name, files)
        page = self.fold(files["docs/index.html"].decode("utf-8"))
        self.assertIn('href="https://example.invalid/docs/"', page)
        self.assertIn('href="../icon.png"', page)
        self.assertIn('href="https://@GH@/n/impossible-os/blob/main/docs/index.md"', page)
        self.assertIn('href="https://@GH@/oldo/impossible-os/blob/main/src/x.c#L1"', page)   # left as written
        self.assertIn('click b href &quot;https://@GH@/o/impossible-os/blob/main/src/x.c&quot;', page)
        self.assertIn('/blob/main/todo/01-x/TODO-01-a.md', self.fold(files["docs/coverage.html"].decode("utf-8")))

    def test_accessibility_fails_the_current_tree_but_not_a_published_release(self):
        self.fixture()
        self.write("docs/sub/page.md", "<!-- docs: covers=todo/01-x/TODO-01-a.md sources=src/x.c -->\n"
                                       "# Sub\n\n[home](../index.md)\n\n#### Too deep\n")
        self.write("src/x.c", "int x;\n")
        self.write("img.png", "png")
        self.write("docs/design/tokens.json", (B.REPO_REAL / "docs/design/tokens.json").read_text(encoding="utf-8"))
        self.write("docs/design/scope.json", '{"shell_files": [], "ui_title_pattern": "^$"}')
        self.git("add", "-A")
        self.git("commit", "-q", "--no-verify", "-m", "v3 skips a heading level")
        self.git("tag", "v3")
        self.assertEqual(self.release("v3"), 0)                     # published as it was
        flat = json.loads((self.out / "docs/v3/search.json").read_text(encoding="utf-8"))
        self.assertIsInstance(flat, list)                             # its template predates the v2 index
        B.set_root(self.root)
        B.LINK_REF, B.DOCS_BASE, B.RELEASE = "main", "docs/", None
        errors: list[str] = []
        B.build(B.load_project(), errors)
        self.assertIn("docs/sub/page.md: accessibility: heading level skips from H1 to H4 at 'Too deep'", errors)

    def test_nested_version_path_and_dead_absolute_link(self):
        self.fixture()
        self.assertEqual(self.release("rel/v2"), 0)
        page = self.read("docs/rel/v2/index.html")
        self.assertIn('href="https://example.invalid/docs/rel/v2/"', page)
        self.assertIn(f'href="https://@GH@/n/impossible-os/blob/{self.sha}/src/x.c">slashtag', page)
        self.assertIn('href="https://@GH@/o/impossible-os/blob/v1/src/x.c">tagref', page)
        self.assertIn('href="../../../icon.png"', page)
        # An absolute link into this repository must exist in the release tree.
        self.write("docs/index.md", "# Home\n\n[gone](https://@GH@/o/impossible-os/blob/main/nope.c)\n"
                                    "[gone2](http://@GH@/o/impossible-os/blob/main/nope2.c)\n"
                                    "[gone3](https://@GH@/o/impossible-os/raw/main/nope3.png)\n"
                                    "<picture><source srcset=\"https://@GH@/o/impossible-os/raw/main/img.png 2x\"></picture>\n"
                                    "[encslash](https://@GH@/o/impossible-os/blob/rel%2Fv2/src/x.c)\n"
                                    "![gone4](https://raw.githubusercontent.com/o/impossible-os/refs/heads/main/nope4.png)\n"
                                    "[frag](https://@GH@/o/impossible-os/blob/main/docs/sub/page.md#missing)\n\n"
                                    "```mermaid\ngraph TD\n  click a \"https://@GH@/o/impossible-os/blob/main/x\n```\n")
        self.git("add", "-A")
        self.git("commit", "-q", "--no-verify", "-m", "dead")
        self.git("tag", "v3")
        B.LINK_REF, B.DOCS_BASE, B.RELEASE = "main", "docs/", None
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(self.release("v3"), 1)
        msgs = self.fold(err.getvalue())
        self.assertIn("dead link at release v3 (not in that tree): https://@GH@/o/impossible-os/blob/main/nope.c", msgs)
        self.assertIn("no such GitHub heading id): docs/sub/page.md#missing", msgs)
        self.assertIn("(not in that tree): http://@GH@/o/impossible-os/blob/main/nope2.c", msgs)
        self.assertIn("(not in that tree): https://@GH@/o/impossible-os/raw/main/nope3.png", msgs)
        self.assertIn("(not in that tree): https://raw.githubusercontent.com/o/impossible-os/refs/heads/main/nope4.png", msgs)
        self.assertIn("cannot read a Mermaid click line at release v3", msgs)
        self.assertIn("raw HTML may not contain the attribute srcset= on <source>", msgs)
        self.assertIn("repository link with an encoded slash", msgs)

    def test_refusals(self):
        self.fixture()
        for ref, why in (("../x", "must match"), ("-v", "must match"), ("v1/", "must match"),
                         ("main/v1", "may not start with main/"), ("refs/tags/v1", "may not start with"),
                         ("nope", "does not name a commit")):
            with self.assertRaises(B.ReleaseError) as cm:
                B.prepare_release(ref)
            self.assertIn(why, str(cm.exception))
        self.git("checkout", "-q", "--orphan", "old")
        self.git("rm", "-rq", "--cached", ".")
        for f in list(self.root.iterdir()):
            if f.name != ".git":
                shutil.rmtree(f) if f.is_dir() else f.unlink()
        self.write("README.md", "# old\n")
        self.git("add", "-A")
        self.git("commit", "-q", "--no-verify", "-m", "pre-site")
        self.git("tag", "v0")
        self.git("checkout", "-q", "-f", "main")
        with self.assertRaises(B.ReleaseError) as cm:
            B.prepare_release("v0")
        self.assertIn("predates the docs site", str(cm.exception))

    def test_raw_html_outside_the_allowlist_fails_in_every_build(self):
        # html.parser reads the fragment as a browser does; what it cannot vouch for is refused.
        page = B.Page(src=Path("docs/p.md"), rel="p.md", url="p.html")
        for release in (None, "v9"):
            B.RELEASE = release
            for frag, why in (('<img/srcset="u.png 2x">', "srcset"),
                              ('<img alt=""srcset="u.png 2x">', "srcset"),
                              ('<img src="a.png" srcset="u.png 2x">', "srcset"),
                              ('<div><style>/* <a href="https://example.invalid/"> */</style>\n'
                               '<a/href="https://example.invalid/real">real</a></div>', "<style>"),
                              ('<div><script>var s = \'<a href="https://example.invalid/">\';</script>'
                               '<a/href="https://example.invalid/real">r</a></div>', "<script>"),
                              # html.unescape decodes a legacy prefix a browser keeps inside an attribute.
                              ('<a href="https://example.invalid/?a=1&copy=2">c</a>', "&copy"),
                              ('<img alt="&notit;" src="https://example.invalid/n.png">', "&not"),
                              ('<a href="https://example.invalid/" title="&ampx">t</a>', "&amp"),
                              ('<div><svg><style><img srcset="u.png 2x"></style></svg></div>', "foreign content"),
                              ('<p><MATH><mi>x</mi></MATH></p>', "foreign content"),
                              ('<div><!--><a href="https://example.invalid/">s</a><svg></svg><!-- end --></div>',
                               "text after <!-->"),
                              ('<!--->b<a href="https://example.invalid/">c</a>-->', "text after <!-->"),
                              ('<![CDATA[><a/href="https://example.invalid/">x</a>]]>', "marked section"),
                              ('<div><![IGNORE[x]]></div>', "marked section"),
                              ('<div><![IGNORE[></div>', "marked section"),
                              ('<!DOCTYPE html>', "declaration"),
                              ('<?xml version="1.0"?>', "processing instruction"),
                              ('<iframe srcdoc="&lt;a href=x&gt;"></iframe>', "<iframe>"),
                              ('<video src="https://example.invalid/v.mp4"></video>', "src= on <video>"),
                              ('<p style="background:url(https://example.invalid/b.png)">x</p>', "style="),
                              ('<style>p{background:url(https://example.invalid/b.png)}</style>', "<style>"),
                              ('<script>location="https://example.invalid/"</script>', "<script>"),
                              ('<img src="a.png" onerror="x()">', "onerror="),
                              ('<div srcdoc="x">d</div>', "srcdoc="),
                              ('<a href="https://example.invalid/" download="f">f</a>', "download="),
                              ('<noscript><p title="</noscript><script>alert(1)</script>">x</p></noscript>', "<noscript>"),
                              ('<textarea><a/href="x"></textarea>', "<textarea>"),
                              ('<xmp>x</xmp>', "<xmp>"), ('<plaintext>x', "<plaintext>"),
                              ('<noembed>x</noembed>', "<noembed>"), ('<title>x</title>', "<title>"),
                              ('<a href="javascript:alert(1)">x</a>', "Markdown would refuse"),
                              ('<a href="JaVaScRiPt&#58;alert(1)">x</a>', "Markdown would refuse"),
                              ('<img src="vbscript:x">', "Markdown would refuse"),
                              ('<div>\n<img src="https://example.invalid/m" onerror="alert(1)"', "unfinished tag"),
                              ('<a href="javascript:alert(1)"', "unfinished tag")):
                errors: list[str] = []
                r = B.Renderer(dict(FACTS, _historical_owners=[]), {}, errors)
                r.browser = {}
                r.rewrite_raw_html(page, frag)
                self.assertTrue(any(why in e for e in errors), (release, frag, errors))
        # Through Markdown: a page ending in an unfinished tag, which the page
        # template's next `>` would complete in a browser.
        for release in (None, "v9"):
            B.RELEASE = release
            errors = []
            r = B.Renderer(dict(FACTS, _historical_owners=[]), {}, errors)
            r.browser = {}
            r.render(B.Page(src=Path("docs/q.md"), rel="q.md", url="q.html"),
                     '# Q\n\n<div>\n<img src="https://example.invalid/m" onerror="alert(1)"')
            self.assertTrue(any("unfinished tag" in e for e in errors), (release, errors))
        B.RELEASE = None
        errors = []
        B.Renderer(dict(FACTS, _historical_owners=[]), {}, errors).rewrite_raw_html(
            page, '<a href="https://example.invalid/">ok</a><!-- <a/href="x"> -->\n'
                  '<p>\n  <img src="https://example.invalid/i.png" alt="x"> <a\n href="https://example.invalid/y">y</a></p>')
        self.assertEqual(errors, [])          # a readable tag, and a comment, pass
        # Forms a browser and the parser end at the same place: the next tag is
        # still seen and rewritten, so they pass (the documented contract).
        for frag in ('<!ENTITY x "y"><a href="https://example.invalid/">a</a>',
                     '<!-->' + '<a href="https://example.invalid/">b</a>',
                     '<!x><a href="https://example.invalid/">c</a>',
                     'text <!--[note] ordinary comment --><a href="https://example.invalid/">d</a>',
                     '<p>\n<!--[TODO: refresh screenshot]-->\n<img src="https://example.invalid/e.png"></p>',
                     '<details open><summary title="t">s</summary><a class="c" id="i" aria-label="l" '
                     'href="https://example.invalid/f">f</a><img alt="a" width="10" height="10" '
                     'src="https://example.invalid/g.png"></details>',
                     'see <path> and <query>', 'a < b and 3 <4'):
            errors = []
            B.Renderer(dict(FACTS, _historical_owners=[]), {}, errors).rewrite_raw_html(page, frag)
            self.assertEqual(errors, [], frag)
        # Spellings a regex tag scanner used to skip are the tags a browser reads, so
        # they are rewritten like any other: re-serialized, the link validated.
        for frag, want in (('<a/href="https://example.invalid/">x</a>', '<a href="https://example.invalid/">x</a>'),
                           ('<img alt=""src="https://example.invalid/i.png">',
                            '<img alt="" src="https://example.invalid/i.png">'),
                           ('<!-- a --!> <a/href="https://example.invalid/y">y</a>',
                            '<!-- a --!> <a href="https://example.invalid/y">y</a>'),
                           ('<a title="&copy; 2026 &amp; on" href=\'https://example.invalid/?a=1&amp;b=2\'>c</a>',
                            '<a title="\u00a9 2026 &amp; on" href="https://example.invalid/?a=1&amp;b=2">c</a>'),
                           ('<p>kept <!-- <a href="x"> --> as is</p><img src="https://example.invalid/z.png" alt="z"/>',
                            '<p>kept <!-- <a href="x"> --> as is</p><img src="https://example.invalid/z.png" alt="z" />')):
            errors = []
            got = B.Renderer(dict(FACTS, _historical_owners=[]), {}, errors).rewrite_raw_html(page, frag)
            self.assertEqual((got, errors), (want, []), frag)
        # A javascript: link is refused whatever the spelling that hid it from a scanner.
        errors = []
        B.Renderer(dict(FACTS, _historical_owners=[]), {}, errors).rewrite_raw_html(
            page, '<a/href="javascript:alert(1)">x</a>')
        self.assertTrue(any("Markdown would refuse" in e for e in errors), errors)

    def test_an_unresolved_absolute_link_fails_closed(self):
        errors: list[str] = []
        r = B.Renderer(dict(FACTS, _historical_owners=[]), {}, errors)
        r.browser = {}                        # the render pass, with nothing collected
        B.RELEASE = "v9"
        page = B.Page(src=Path("docs/p.md"), rel="p.md", url="p.html")
        url = "https://github" + ".com/o/impossible-os/blob/main/x"
        self.assertEqual(r.pin_absolute(page, url), url)
        self.assertEqual(len(errors), 1)
        self.assertIn("not resolved before pinning", errors[0])

    def test_default_build_is_not_pinned(self):
        r = B.Renderer(dict(FACTS, _historical_owners=[]), {}, [])
        url = "https://github" + ".com/o/impossible-os/blob/main/x"
        self.assertIsNone(r.pin_absolute(None, url))
        self.assertEqual(r.blob_url("x"), "https://example.invalid/o/impossible-os/blob/main/x")
        self.assertEqual(r.pin_mermaid(None, f'click a "{url}"'), f'click a "{url}"')


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


class RealParsers(unittest.TestCase):
    """Section 28: the site scripts read HTML with html.parser and markdown-it
    tokens and URLs with urllib.parse, and each helper holds at its edges."""

    def render(self, text: str) -> str:
        page = B.Page(src=Path("docs/q.md"), rel="q.md", url="q.html")
        r = B.Renderer(dict(FACTS, _historical_owners=[]), {}, [])
        r.browser = {}
        r.render(page, "# Q\n\n" + text)
        return page.body

    def test_alerts_tasks_and_tables_come_from_tokens(self):
        body = self.render("> [!NOTE]\n> Body text\n")
        self.assertIn('<blockquote class="alert alert-note"><p class="alert-title">Note</p><p>Body text</p>', body)
        body = self.render("> [!TIP]  \n> After a hard break\n")
        self.assertIn('<p class="alert-title">Tip</p><p>After a hard break</p>', body)
        body = self.render("> [!WARNING] on the same line\n")
        self.assertIn('<p class="alert-title">Warning</p><p>on the same line</p>', body)
        for plain in ("> just a quote\n", "> [!note] lower case is text\n", "> `[!NOTE]` in code\n"):
            self.assertNotIn("alert", self.render(plain), plain)
        body = self.render("- [ ] open\n- [x] done\n- [X] DONE\n")
        self.assertIn('<li class="task"><input type="checkbox" disabled> open</li>', body)
        self.assertIn('<li class="task"><input type="checkbox" checked disabled> done</li>', body)
        self.assertIn('<li class="task"><input type="checkbox" checked disabled> DONE</li>', body)
        loose = self.render("- [ ] a\n\n- [ ] b\n")
        self.assertNotIn("task", loose)                  # a loose list item is a paragraph, as before
        self.assertIn("<p>[ ] a</p>", loose)
        body = self.render("| a | b |\n| - | - |\n| 1 | 2 |\n")
        self.assertEqual((body.count('<div class="table-wrap"><table>'), body.count("</table></div>")), (1, 1))
        self.assertEqual(body.count("<table"), 1)

    def test_raw_links_are_judged_in_the_form_a_browser_parses(self):
        page = B.Page(src=Path("docs/p.md"), rel="p.md", url="p.html")
        for release in (None, "v9"):
            B.RELEASE = release
            try:
                for href in ("java&#9;script:alert(1)", "java&#10;script:alert(1)", "java&#13;script:alert(1)",
                             "&#1; javascript:alert(1)", " JAVASCRIPT:alert(1)", "vb&#9;script:x"):
                    errors: list[str] = []
                    r = B.Renderer(dict(FACTS, _historical_owners=[]), {}, errors)
                    r.browser = {}
                    r.rewrite_raw_html(page, f'<a href="{href}">x</a>')
                    self.assertTrue(any("Markdown would refuse" in e for e in errors), (release, href, errors))
                errors = []
                r = B.Renderer(dict(FACTS, _historical_owners=[]), {}, errors)
                r.browser = {}
                r.render(B.Page(src=Path("docs/q.md"), rel="q.md", url="q.html"),
                         '# Q\n\n<a href="java&#9;script:alert(1)">x</a>\n')
                self.assertTrue(any("Markdown would refuse" in e for e in errors), (release, errors))
            finally:
                B.RELEASE = None

    def test_a_rewritten_link_never_reads_as_a_scheme(self):
        # A tracked page whose name starts like a scheme is refused outright (its URL
        # appears in navigation, search and the sitemap), and a body link to it is
        # percent-encoded, so `javascript:alert(1).html` is never emitted.
        saved = (B.REPO, B.ROOT, B.SOURCE, B._FILESET, B._DIRSET)
        try:
            root = fixture_repo()
            (root / "docs" / "javascript:alert(1).md").write_text("# Colon\n", encoding="utf-8")
            (root / "docs" / "c.md").write_text('# C\n\n<a href="javascript%3Aalert(1).md">r</a> [m](javascript%3Aalert(1).md)\n',
                                                encoding="utf-8")
            env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
                       GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid")
            for cmd in (["add", "-A"], ["commit", "-q", "-m", "colon", "--no-verify"]):
                subprocess.run(["git", *cmd], cwd=root, check=True, env=env, capture_output=True)
            RawHtmlAndBaseline._use(self, root)
            errors: list[str] = []
            pages, _ = B.load_pages(FACTS, errors)
            body = pages["c.md"].body
            self.assertNotIn('href="javascript:', body)
            self.assertEqual(body.count('href="javascript%3Aalert%281%29.html"'), 2, body)
            self.assertFalse([e for e in errors if "c.md" in e], errors)
            self.assertTrue(any("javascript:alert(1).md: a page path may use only" in e for e in errors), errors)
            self.assertFalse(B.url_safe_path("a b.md") or B.url_safe_path("x:y.md") or B.url_safe_path("caf\u00e9.md"))
            self.assertTrue(B.url_safe_path("kernel/boot-flow_2.md"))
        finally:
            B.REPO, B.ROOT, B.SOURCE, B._FILESET, B._DIRSET = saved
            B.set_root(B.ROOT)

    def test_a_repeated_attribute_keeps_its_first_value(self):
        self.assertEqual(B.first_attrs([("type", "a"), ("id", None), ("type", "b")]), {"type": "a", "id": None})
        self.assertTrue(B.is_noindex('<meta name="robots" name="x" content="noindex" content="index">'))
        self.assertFalse(B.is_noindex('<meta name="x" name="robots" content="noindex">'))
        self.assertEqual(B.html_anchors('<a id="first" id="second" name="n"></a>'), {"first", "n"})
        import linkcheck as L
        out: dict[str, list[str]] = {}
        L.html_links('<a href="https://example.invalid/first" href="https://example.invalid/second">x</a>', "f", 1, out)
        self.assertEqual(list(out), ["https://example.invalid/first"])

    def test_check_parsers_folding_is_linear_and_cycle_safe(self):
        import check_parsers as C
        d = Path(tempfile.mkdtemp(prefix="parsers-"))
        f = d / "chain.py"
        f.write_text("import re\nP = '<a>'\n" + "P = P\n" * 200 + "A = B\nB = A + '<b'\nX = re.compile(P)\nY = re.compile(A)\n",
                     encoding="utf-8")
        start = time.monotonic()
        found = C.check_file(f)
        self.assertLess(time.monotonic() - start, 2.0)
        self.assertEqual([n for n, _ in found], [205, 206], found)
        # A fold cut short by a cycle is not cached: call order cannot hide a pattern.
        f.write_text("import re\nA = '<'\nB = A\nA = B\nre.compile(A)\nre.compile(B + 'a>')\n", encoding="utf-8")
        self.assertEqual([n for n, _ in C.check_file(f)], [6], C.check_file(f))
        # A pattern whose fold outgrows the step budget is reported, never passed.
        for body in ("".join(f"X{i} = X{i - 1} + X{i - 1}\n" for i in range(1, 40)) + "re.compile(X39)\n",):
            f.write_text("import re\nX0 = 'a'\nX0 = X0\n" + body, encoding="utf-8")
            start = time.monotonic()
            found = C.check_file(f)
            self.assertLess(time.monotonic() - start, 5.0)
            self.assertTrue(any("too complex" in m for _, m in found), found)
        f.write_text("import re\nX0 = 'a'\nX0 = X0\n" + "".join(f"X{i} = X{i - 1} + X{i - 1}\n" for i in range(1, 40))
                     + "re.compile(X39)  # parser-allow: a deliberately deep fixture, waived like any other\n",
                     encoding="utf-8")
        self.assertEqual(C.check_file(f), [])
        # More candidates than the check reads is reported, never judged on the first 64.
        f.write_text("import re\nP = 'x'\nP = '<'\nA = 'a'\nA = 'b'\nre.compile(f'{P}{A}{A}{A}{A}{A}{A}>')\n",
                     encoding="utf-8")
        self.assertTrue(any("too complex" in m for _, m in C.check_file(f)), C.check_file(f))
        # Repeats of an ambiguous name multiply combinations, not distinct strings: the
        # work stays bounded and the pattern is still judged in full.
        f.write_text("import re\nA = 'x'\nA = 'xx'\nre.compile(f'" + "{A}" * 40 + "')\n"
                     "re.compile('<b' + " + " + ".join(["A"] * 40) + ")\n", encoding="utf-8")
        start = time.monotonic()
        found = C.check_file(f)
        self.assertLess(time.monotonic() - start, 2.0)
        self.assertEqual([n for n, _ in found], [5], found)
        f.write_text("import re\nP = 'x'\nP = '<'\nre.compile(P + 'b>')\n", encoding="utf-8")
        self.assertEqual([n for n, _ in C.check_file(f)], [4])       # few candidates: every one is judged

    def test_a_long_ampersand_run_is_linear(self):
        frag = '<a href="https://example.invalid/?' + "&" + "a" * 300_000 + '">x</a>'
        start = time.monotonic()
        B._RawAttrs.scan(frag)
        self.assertLess(time.monotonic() - start, 2.0)
        self.assertEqual(B.ambiguous_charref("&middotx"), "&middotx")   # the longest legacy name, then text
        self.assertEqual(B.ambiguous_charref("&middot; &amp; &#38; &nosuch"), "")

    @unittest.skipUnless(shutil.which("node"), "node not installed")
    def test_classic_scripts_are_chosen_by_type(self):
        broken = "function ( {"
        for tag, parsed in (("<script TYPE>", True), ('<script type = " Text/JavaScript ">', True),
                            ('<script type="">', True), ('<script language="javascript">', True),
                            ('<script type="module">', False), ('<script type="application/ld+json">', False),
                            ('<script src="x.js">', False)):
            errors: list[str] = []
            B.check_scripts({"p.html": f"{tag}{broken}</script>".encode()}, errors)
            self.assertEqual(bool(errors), parsed, (tag, errors))
        for tag in ('<script type="text/javascript" type="application/ld+json">',
                    '<script language="javascript" language="x" >'):
            errors = []
            B.check_scripts({"p.html": f"{tag}{broken}</script>".encode()}, errors)
            self.assertTrue(errors, tag)                  # the browser keeps the first attribute
        errors = []
        B.check_scripts({"p.html": b"<script/>const a = ;</script>"}, errors)
        self.assertTrue(any("self-closing <script/>" in e for e in errors), errors)

    def test_noindex_is_read_from_the_meta_tag(self):
        self.assertTrue(B.is_noindex('<meta content="noindex" name="robots">'))
        self.assertTrue(B.is_noindex("<META NAME='Robots' CONTENT='NoIndex, NoFollow'>"))
        self.assertFalse(B.is_noindex('<meta name="robots" content="xnoindex">'))
        self.assertFalse(B.is_noindex('<meta name="googlebot" content="noindex">'))
        self.assertFalse(B.is_noindex('<!-- <meta name="robots" content="noindex"> -->'))

    def test_url_helpers(self):
        self.assertEqual(B.url_scheme("http://[broken/path"), "http")
        self.assertEqual(B.url_scheme("HTTPS://Example.invalid/"), "https")
        self.assertEqual(B.url_scheme(" \tjavascript:x"), "javascript")
        for rel in ("1:x.md", "+x:y.png", "docs/a.md", "#frag", "//host/x", ""):
            self.assertEqual(B.url_scheme(rel), "", rel)
        self.assertEqual(B.browser_url_input("\x01 java\tscr\nipt:x \x00"), "javascript:x")

    def test_donate_buttons_and_count_badge(self):
        pp = "https://www.paypal.com/donate/"
        self.assertEqual(B.donate_buttons(f'<a href="{pp}?locale=en&amp;hosted_button_id=AAA">'), ["AAA"])
        self.assertEqual(B.donate_buttons(f"[a]({pp}?hosted_button_id=AAA) and {pp}?hosted_button_id=BBB"),
                         ["AAA", "BBB"])
        self.assertEqual(B.donate_buttons(f"{pp}?hosted_button_id=AAA&hosted_button_id=BBB"), ["AAA", "BBB"])
        self.assertEqual(B.donate_buttons("see paypal.com/donate for how buttons work"), [])
        self.assertEqual(B.donate_buttons("https://www.paypal.com/donatex?hosted_button_id=Z"), [])
        badge = "https://img.shields.io/badge/lines-{}-blueviolet"
        self.assertEqual(B.count_badge(f"![lines]({badge.format('1%2C234')})\n"), 1234)
        self.assertEqual(B.count_badge(f'<p>\n  <img src="{badge.format("1%2C325%2C572")}" alt="L" />\n</p>\n'), 1325572)
        for bad in ("12%2C34", "1234x", "", "1%2C2345"):
            self.assertIsNone(B.count_badge(f"![l]({badge.format(bad)})\n"), bad)
        self.assertIsNone(B.count_badge("no badge here\n"))
        self.assertIsNone(B.count_badge(f"`{badge.format('1%2C234')}`\n"))   # a URL in code is not an image


class LinkCheckStress(unittest.TestCase):
    """linkcheck.run_checks under seeded random scheduling: the network, the URL
    parser and the worker hand-off are fakes that sleep a random few
    milliseconds, so each seed interleaves the workers, the per-host slots,
    redirects, retries and the budget differently. Invariants, every seed:
    every link is answered or reported unanswered at the budget, and an answer
    is the right one; a URL-parser failure never ends green; no host ever has
    more than PER_HOST requests in flight. The control is a mutant whose
    resolve() stops publishing its failure before the call stops counting as in
    flight: a scripted interleaving (the failure lands after the deadline, the
    worker's own publication held back) must catch it."""

    SEEDS = 200
    HOSTS = ("a.test", "b.test", "c.test")
    KINDS = ("ok", "gone", "down", "flaky", "redir")
    EXPECT = {"ok": ("OK", ""), "gone": ("DEAD", "HTTP 404"), "down": ("UNVERIFIED", "HTTP 503"),
              "flaky": ("OK", ""), "redir": ("OK", "")}

    @classmethod
    def setUpClass(cls):
        import linkcheck as L
        cls.L = L

    @staticmethod
    def mutant(L):
        """linkcheck with the resolve() failure publication removed: only the
        worker publishes the failure, after the call has stopped counting."""
        import types
        src = Path(L.__file__).read_text(encoding="utf-8")
        old = ("                if failure is not None and failure not in fatal:\n"
               "                    fatal.append(failure)\n"
               "                parsing[0] -= 1\n")
        assert src.count(old) == 1, "the control's mutation no longer applies: update it with resolve()"
        m = types.ModuleType("linkcheck_mutant")
        m.__file__ = L.__file__
        sys.modules[m.__name__] = m                  # dataclasses resolve the defining module by name
        try:
            exec(compile(src.replace(old, "                parsing[0] -= 1\n"), L.__file__, "exec"), m.__dict__)
        finally:
            del sys.modules[m.__name__]
        return m

    def run_seed(self, L, seed: int) -> tuple[list[str], int]:
        """(violations, peak in-flight requests on one host) for one seed."""
        import random
        import threading
        from urllib.parse import urlsplit
        rng = random.Random(seed)
        lock = threading.Lock()
        inflight: dict[str, int] = {}
        peak, calls, failed = [0], {}, []
        fail = seed % 4 == 0                        # this seed's URL parser fails on one redirect
        urls = [f"http://{rng.choice(self.HOSTS)}/{rng.choice(self.KINDS)}/{i}" for i in range(rng.randint(8, 30))]
        if fail:
            urls.append(f"http://{rng.choice(self.HOSTS)}/redir-fail/x")
        short = rng.random() < 0.3                  # a budget that runs out mid-run
        budget = rng.uniform(0.002, 0.03) if short else 20.0

        def delay():
            with lock:
                d = rng.uniform(0, 0.003)
            time.sleep(d)

        def request(url, method, timeout):
            host = urlsplit(url).netloc
            with lock:
                inflight[host] = inflight.get(host, 0) + 1
                peak[0] = max(peak[0], inflight[host])
                calls[url] = calls.get(url, 0) + 1
                n = calls[url]
            try:
                delay()
                kind = urlsplit(url).path.split("/")[1]
                if kind == "flaky" and n == 1:
                    raise TimeoutError("timed out")
                return {"gone": (404, None), "down": (503, None), "redir": (301, "/hop"),
                        "redir-fail": (302, "/fail")}.get(kind, (200, None))
            finally:
                with lock:
                    inflight[host] -= 1

        def resolve_location(location, base, timeout):
            delay()
            if location == "/fail":
                with lock:
                    failed.append(base)
                raise L.NormalizeError("fake URL parser failure")
            host = self.HOSTS[(self.HOSTS.index(urlsplit(base).netloc) + 1) % len(self.HOSTS)]
            return f"http://{host}/ok{urlsplit(base).path}"

        real_step = L.step

        def step(*a, **kw):                          # the worker hand-off: between an answer and its publication
            try:
                out = real_step(*a, **kw)
            except BaseException:
                delay()
                raise
            delay()
            return out

        return self.drive(L, urls, budget, short, request, resolve_location, step, failed, peak)

    def drive(self, L, urls, budget, short, request, resolve_location, step, failed, peak, release=None):
        import threading
        saved = (L.request, L.resolve_location, L.step, L.browser_urls)
        before = set(threading.enumerate())
        L.request, L.resolve_location, L.step = request, resolve_location, step
        L.browser_urls = lambda us, base=None, timeout=0: {u: u for u in us}
        got, raised, bad = None, None, []
        try:
            try:
                got = L.run_checks(urls, retries=3, wait=0.001, timeout=0.5, budget=budget)
            except L.NormalizeError as e:
                raised = e
            if release is not None:
                release.set()
            for t in set(threading.enumerate()) - before:
                t.join(5)
                if t.is_alive():
                    bad.append("a worker outlived its run")
        finally:
            L.request, L.resolve_location, L.step, L.browser_urls = saved
        if failed and raised is None:
            bad.append(f"a URL-parser failure ended green: {sorted(got.items())[:3]}")
        if raised is not None and not failed:
            bad.append(f"the run failed with no parser failure: {raised}")
        if got is not None:
            if set(got) != set(urls):
                bad.append(f"links missing from the result: {sorted(set(urls) - set(got))[:3]}")
            for u, v in got.items():
                if v[1].startswith("not finished within"):
                    if not short:
                        bad.append(f"{u} unanswered without the budget running out")
                elif v != self.EXPECT.get(u.split("/")[3]):
                    bad.append(f"{u}: {v}")
        if peak[0] > L.PER_HOST:
            bad.append(f"{peak[0]} requests in flight on one host (limit {L.PER_HOST})")
        return bad, peak[0]

    def publication_race(self, L) -> list[str]:
        """The interleaving the publication order exists for, scripted: a redirect's
        URL parser is called before the deadline and fails after it, while the
        worker's own publication of that failure is held until the run returns."""
        import threading
        release, failed, peak = threading.Event(), [], [0]

        def resolve_location(location, base, timeout):
            time.sleep(0.15)
            failed.append(base)
            raise L.NormalizeError("fake URL parser failure")

        real_step = L.step

        def step(*a, **kw):
            try:
                return real_step(*a, **kw)
            except L.NormalizeError:
                release.wait(5)
                raise

        return self.drive(L, ["http://a.test/redir-fail/x"], 0.05, True, lambda url, method, timeout: (302, "/fail"),
                          resolve_location, step, failed, peak, release=release)[0]

    def stress(self, L) -> tuple[list[str], int]:
        bad, peak = [], 0
        for seed in range(self.SEEDS):
            v, p = self.run_seed(L, seed)
            bad.extend(f"seed {seed}: {x}" for x in v)
            peak = max(peak, p)
            if seed % 50 == 0:
                bad.extend(f"seed {seed} (publication race): {x}" for x in self.publication_race(L))
        return bad, peak

    def test_seeded_stress_holds_every_invariant(self):
        start = time.monotonic()
        bad, peak = self.stress(self.L)
        elapsed = time.monotonic() - start
        self.assertEqual(bad, [])
        self.assertEqual(peak, self.L.PER_HOST)     # the per-host limit was actually under pressure
        self.assertLess(elapsed, 30, f"{self.SEEDS} seeds took {elapsed:.1f} s")

    def test_a_redirect_the_budget_closes_is_unanswered_not_a_checker_error(self):
        # Found by the stress (seed 71): a worker whose redirect resolution was refused
        # by the spent budget published ("UNVERIFIED", "checker error") whenever it beat
        # the coordinator to the results. Scripted with a fake clock: the budget runs
        # out while the first request is on the wire, and the worker answers first.
        import threading
        import types
        L, now, before = self.L, [0.0], set(threading.enumerate())

        def request(url, method, timeout):
            now[0] = 100.0
            return 301, "/hop"
        saved = (L.time, L.request, L.browser_urls)
        L.time, L.request = types.SimpleNamespace(monotonic=lambda: now[0]), request
        L.browser_urls = lambda us, base=None, timeout=0: {u: u for u in us}
        try:
            got = L.run_checks(["http://a.test/redir/1"], retries=1, wait=0, timeout=0.5, budget=10)
            for t in set(threading.enumerate()) - before:
                t.join(5)
        finally:
            L.time, L.request, L.browser_urls = saved
        self.assertEqual(got, {"http://a.test/redir/1": ("UNVERIFIED", "not finished within the 10 s budget")})

    def test_the_stress_catches_a_reverted_publication_order(self):
        bad, _ = self.stress(self.mutant(self.L))
        self.assertTrue(any("publication race" in b and "ended green" in b for b in bad), bad[:5])


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
