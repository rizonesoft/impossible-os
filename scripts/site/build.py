#!/usr/bin/env python3
"""Build and drift-check the Impossible OS website and documentation site.

One generator owns every published surface, so no fact is maintained in two
places by hand:

* ``project.json`` is the single source of truth for project facts (owner,
  repository URL, site URL, release date). The landing page templates under
  ``gh-pages/`` reference them as ``{{key}}``; Markdown files carry them in
  ``<!-- project:key -->value<!-- /project -->`` regions that stay readable on
  GitHub and are rewritten by ``--sync``.
* ``docs/**/*.md`` is rendered to ``<out>/docs/`` with a generated navigation
  tree, per-page table of contents, client-side search index, and a
  documentation-coverage page computed from ``<!-- docs: covers=... -->``
  directives against every ``todo/**/TODO-*.md`` file.

Modes:
  (default)          build the site into build/site/ (never committed)
  --check            build in memory and fail on any drift: stale project
                     regions, dead doc links, a README line-count badge that
                     disagrees with COUNT.md, an unknown owner in a live repo
                     URL, or a documentation-coverage baseline that is stale
                     or missing a new undocumented TODO
  --sync             rewrite project regions in tracked Markdown in place
  --update-baseline  SHRINK the coverage baseline (never adds entries)
  --release REF      render only the docs tree as it was at commit REF into
                     <out>/docs/<REF>/, every link to main or to REF itself
                     pinned to REF's full SHA (keeping release trees published
                     is roadmap work)

Stdlib plus the vendored markdown-it-py under tools/vendor/, so the output is
identical on every host and in CI. Deterministic: stable ordering, no
timestamps.
"""

from __future__ import annotations

import argparse
from html.parser import HTMLParser
import datetime as _dt
import hashlib
import html
import json
import os
import posixpath
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unicodedata
from urllib.parse import unquote, urlsplit
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]   # the real checkout; every git call runs here
ROOT = REPO                                  # where inputs are READ; set_root() moves it to a snapshot
sys.path.insert(0, str(REPO / "tools" / "vendor"))
sys.path.insert(0, str(REPO / "scripts"))
import todo_fence  # noqa: E402  (the shared `## N.` heading rule)

from markdown_it import MarkdownIt  # noqa: E402  (vendored, path set above)

PROJECT_FILE = ROOT / "project.json"
SITE_SRC = ROOT / "gh-pages"
DOCS = ROOT / "docs"
TODO = ROOT / "todo"
BASELINE = DOCS / ".coverage-baseline.json"
DEFAULT_OUT = REPO / "build" / "site"

# What the site reads. A snapshot (--staged, --sync-head) materialises only these
# plus every tracked *.md, so a check judges exactly what a commit contains.
SNAP_PATHS = ["project.json", "COUNT.md", "docs", "todo", "gh-pages", "resources/icons/src",
              "resources/brand", "resources/backgrounds", "resources/icons/color", "include/desktop/theme_tokens.h", "*.md"]
SOURCE = "worktree"          # "worktree" | "index" | a commit ref
# Release rendering (--release). The defaults reproduce the main-branch site byte
# for byte; a release build pins every repository link to the release commit and
# publishes under docs/<version>/ instead of docs/.
LINK_REF = "main"            # the git ref every blob/tree/raw link names
DOCS_BASE = "docs/"          # site path the docs tree is published under
RELEASE: str | None = None   # the version path segment(s) of a release build
VERSION_SEGMENT_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")
PUBLISH_KEYS = ("site_url", "docs_url", "repo_url", "owner", "repo")
_FILESET: set[str] | None = None
_DIRSET: set[str] | None = None


def git(*args: str, binary: bool = False):
    out = subprocess.run(["git", *args], cwd=REPO, check=True, capture_output=True).stdout
    return out if binary else out.decode()


def fileset() -> tuple[set[str], set[str]]:
    """Repo-relative files that exist in SOURCE, and their parent directories.
    Link existence is judged against this, never the filesystem, so an ignored
    local artifact (build/...) can never satisfy a link."""
    global _FILESET, _DIRSET
    if _FILESET is None:
        if SOURCE == "worktree":
            raw = git("ls-files", "-z", "--cached", "--others", "--exclude-standard")
        elif SOURCE == "index":
            raw = git("ls-files", "-z")
        else:
            raw = git("ls-tree", "-r", "-z", "--name-only", SOURCE)
        files = {p for p in raw.split("\0") if p}
        if SOURCE == "worktree":
            files = {p for p in files if (REPO / p).exists()}
        dirs: set[str] = set()
        for f in files:
            parts = f.split("/")
            for i in range(1, len(parts)):
                dirs.add("/".join(parts[:i]))
        _FILESET, _DIRSET = files, dirs
    return _FILESET, _DIRSET  # type: ignore[return-value]


def set_root(root: Path) -> None:
    global ROOT, PROJECT_FILE, SITE_SRC, DOCS, TODO, BASELINE, _TRACKED
    ROOT = root
    _TRACKED = None
    PROJECT_FILE, SITE_SRC, DOCS, TODO = root / "project.json", root / "gh-pages", root / "docs", root / "todo"
    BASELINE = DOCS / ".coverage-baseline.json"
    sys.path.insert(0, str(REPO / "scripts" / "site"))
    import gen_theme_header as g  # noqa: E402  (sibling module)
    g.ROOT, g.TOKENS, g.HEADER = root, root / "docs" / "design" / "tokens.json", root / "include" / "desktop" / "theme_tokens.h"


def snapshot(source: str) -> Path:
    """Materialise the site inputs from the index ("index") or a commit into a temp dir."""
    global SOURCE, _FILESET, _DIRSET
    SOURCE, _FILESET, _DIRSET = source, None, None
    tmp = Path(tempfile.mkdtemp(prefix="site-snap-"))
    files, _ = fileset()
    want = sorted(f for f in files if any(
        f == p or f.startswith(p + "/") or (p == "*.md" and f.endswith(".md")) for p in SNAP_PATHS))
    if source == "index":
        subprocess.run(["git", "checkout-index", "-z", "--stdin", f"--prefix={tmp}/"], cwd=REPO, check=True,
                       input="\0".join(want).encode(), capture_output=True)
    else:
        # One `git cat-file --batch` for every blob instead of a `git show` per file
        # (measured: 797 processes / 3.2 s down to one process / ~0.5 s).
        proc = subprocess.run(["git", "cat-file", "--batch"], cwd=REPO, check=True, capture_output=True,
                              input="".join(f"{source}:{f}\n" for f in want).encode())
        out, pos = proc.stdout, 0
        for f in want:
            nl = out.index(b"\n", pos)
            header = out[pos:nl].split()
            if len(header) != 3:
                raise RuntimeError(f"snapshot: cannot read {source}:{f}")
            size = int(header[2])
            data = out[nl + 1:nl + 1 + size]
            pos = nl + 1 + size + 1
            dest = tmp / f
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
    set_root(tmp)
    return tmp
TEMPLATE_EXTS = {".html", ".js", ".css", ".json", ".xml", ".txt", ".svg", ""}

REGION_RE = re.compile(r"(?<!`)<!-- project:([a-z_]+) -->(.*?)<!-- /project -->", re.S)  # not inside `code`
TEMPLATE_RE = re.compile(r"\{\{([a-z_]+)\}\}")
DIRECTIVE_RE = re.compile(r"<!--\s*docs:\s*(.*?)\s*-->", re.S)
REPO_URL_RE = re.compile(r"github\.com/([A-Za-z0-9-]+)/impossible-os\b")


# --------------------------------------------------------------------------
# Project facts
# --------------------------------------------------------------------------

def load_project() -> dict[str, str]:
    raw = json.loads(PROJECT_FILE.read_text(encoding="utf-8"))
    d = _dt.date.fromisoformat(raw["release_date"])
    facts = {k: v for k, v in raw.items() if isinstance(v, str)}
    facts["release_date_long"] = f"{d.strftime('%B')} {d.day}, {d.year}"
    facts["release_date_ts"] = f"{raw['release_date']}T00:00:00Z"
    facts["release_year"] = str(d.year)
    facts["_historical_owners"] = raw.get("historical_owners", [])
    facts["_historical_owner_files"] = raw.get("historical_owner_files", [])
    facts.update(load_stats())
    return facts


def load_stats() -> dict[str, str]:
    """Computed repository facts (``stat_*``). They move with ordinary work, so the
    post-commit hook re-syncs README.md after every commit and the pre-commit lint
    skips them (--skip-stats); CI still checks them."""
    todos = todo_files()
    domains = {t.split("/")[1] for t in todos}
    docs = sorted(DOCS.rglob("*.md"))
    covered: set[str] = set()
    for d in docs:
        covers = parse_directives(d.read_text(encoding="utf-8")).get("covers", "")
        covered.update(c.strip() for c in covers.split(",") if c.strip())
    cov = json.loads((DOCS / "test-coverage" / "coverage.json").read_text(encoding="utf-8"))
    m = COUNT_TOTAL_RE.search((ROOT / "COUNT.md").read_text(encoding="utf-8"))
    lines = int(m.group(1)) if m else 0
    return {
        "stat_todo_files": str(len(todos)),
        "stat_todo_domains": str(len(domains)),
        "stat_doc_pages": str(len(docs)),
        "stat_docs_covered": str(len(covered & set(todos))),
        "stat_test_suites": f"{cov['total_suites']:,}",
        "stat_test_assertions": f"{cov['total_assertions']:,}",
        # The same exact whole-tree total the README badge shows.
        "stat_lines": f"{lines:,}",
    }


def render_template(text: str, facts: dict, where: str, errors: list[str]) -> str:
    def sub(m: re.Match) -> str:
        key = m.group(1)
        if key not in facts or key.startswith("_"):
            errors.append(f"{where}: unknown template key {{{{{key}}}}}")
            return m.group(0)
        return facts[key]
    return TEMPLATE_RE.sub(sub, text)


def sync_regions(text: str, facts: dict, where: str, errors: list[str]) -> str:
    def sub(m: re.Match) -> str:
        key = m.group(1)
        if key not in facts or key.startswith("_"):
            errors.append(f"{where}: unknown project region key '{key}'")
            return m.group(0)
        return f"<!-- project:{key} -->{facts[key]}<!-- /project -->"
    return REGION_RE.sub(sub, text)


def tracked_files(*pathspecs: str) -> list[Path]:
    """Files in SOURCE matching pathspecs, as paths under ROOT (the snapshot when one is active)."""
    import fnmatch
    files, _ = fileset()
    return [ROOT / f for f in sorted(files) if any(fnmatch.fnmatch(f, p) for p in pathspecs)]


# --------------------------------------------------------------------------
# Markdown rendering
# --------------------------------------------------------------------------

def github_slug(text: str) -> str:
    """GitHub's heading id rule: lowercase, keep word characters as Ruby's
    `\\p{Word}` defines them (letters, combining marks, decimal digits, connector
    punctuation such as `_`), hyphens and spaces, drop everything else, then turn
    spaces into hyphens. Nothing is trimmed: a heading ending in `&#x20;` keeps
    its trailing hyphen, as on github.com."""
    out = []
    for ch in text.lower():
        cat = unicodedata.category(ch)
        if ch in "- " or cat[0] in "LM" or cat in ("Nd", "Pc"):
            out.append(ch)
    return "".join(out).replace(" ", "-")


class Slugger:
    """GitHub's heading ids (github-slugger): a repeated slug takes the first free
    `-N` suffix and every id handed out is reserved, so the headings `Foo`, `Foo`,
    `Foo-1` get `foo`, `foo-1`, `foo-1-1`."""

    def __init__(self) -> None:
        self.seen: dict[str, int] = {}

    def slug(self, text: str) -> str:
        base = s = github_slug(text)
        while s in self.seen:
            self.seen[base] += 1
            s = f"{base}-{self.seen[base]}"
        self.seen[s] = 0
        return s


class _AnchorParser(HTMLParser):
    """`id` and `name` of <a> tags in raw HTML, any attribute order or quoting;
    HTMLParser skips comments, so a commented-out anchor is not an anchor."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.found: set[str] = set()

    def handle_starttag(self, tag, attrs):
        if tag == "a":
            self.found.update(v for k, v in attrs if k in ("id", "name") and v)

    handle_startendtag = handle_starttag


# Raw HTML in docs pages is an ALLOWLIST, not a list of known-bad shapes: every
# denylist round found another construct that hides a link from the checker
# (srcset, srcdoc, CSS url(), foreign content). Links go through `href` on <a>
# and `src` on <img>, which the rewriter validates and pins; the other allowed
# attributes carry no URL. The corpus used none of what this refuses (surveyed
# 2026-09-29: no raw HTML attributes at all outside comments).
RAW_HTML_LINK_ATTRS = {"a": "href", "img": "src"}
RAW_HTML_ATTRS = frozenset({
    "alt", "title", "id", "name", "class", "width", "height", "align", "valign", "border",
    "open", "colspan", "rowspan", "scope", "headers", "start", "reversed", "type", "lang",
    "dir", "role", "hidden"})
# Every element that takes the HTML tokenizer out of normal parsing, per the
# HTML Standard's "in body" insertion mode: RCDATA (title, textarea), RAWTEXT
# (style, xmp, iframe, noembed, noframes, and noscript when scripting is on),
# script data (script), PLAINTEXT (plaintext), and the foreign-content roots
# (svg, math). Inside any of them a browser and html.parser can disagree about
# where markup is, so the set is closed by the standard, not by examples.
RAW_HTML_REFUSED_TAGS = {
    **dict.fromkeys(("title", "textarea"), "RCDATA text"),
    **dict.fromkeys(("style", "xmp", "iframe", "noembed", "noframes", "noscript"), "raw text"),
    "script": "script code", "plaintext": "plain text to the end of the page",
    "svg": "foreign content", "math": "foreign content"}


class _RawAttrs(HTMLParser):
    """The net under Renderer.rewrite_raw_html, reading a raw HTML fragment the
    way a browser does. It records every <a href>/<img src> by character offset
    (so the tag scanner must have rewritten exactly that tag: text inside
    <script>/<style> cannot stand in for it), and `refused` names the first
    thing outside the allowlist: a tag in RAW_HTML_REFUSED_TAGS, an attribute
    outside RAW_HTML_ATTRS (aria-* aside) or a link attribute on another tag.
    It also refuses markup where the parser would read on past the point a
    browser resumes parsing elements: `<!-->` or `<!--->` with more text before
    a later `-->`, and any marked section (`<![...`, however the parser routes
    it); `<!DOCTYPE>` and processing instructions too, as no page needs them.
    Other bogus comments (`<!x>`) and a bare `<!-->` end where a browser ends
    them, so they pass."""

    def __init__(self, text: str) -> None:
        super().__init__(convert_charrefs=True)
        self.links: dict[int, int] = {}
        self.refused = ""
        self._text = text
        self._line_starts = [0] + [m.end() for m in re.finditer("\n", text)]

    def _offset(self) -> int:
        line, col = self.getpos()
        return self._line_starts[line - 1] + col

    def _refuse(self, why: str) -> None:
        self.refused = self.refused or why

    def handle_starttag(self, tag, attrs):
        if tag in RAW_HTML_REFUSED_TAGS:
            self._refuse(f"<{tag}> ({RAW_HTML_REFUSED_TAGS[tag]})")
        n = 0
        for k, v in attrs:
            if k == RAW_HTML_LINK_ATTRS.get(tag):
                if v is not None:
                    n += 1
            elif k not in RAW_HTML_ATTRS and not k.startswith("aria-"):
                self._refuse(f"the attribute {k}= on <{tag}>")
        if n:
            self.links[self._offset()] = n

    handle_startendtag = handle_starttag

    def handle_comment(self, data):
        # Classified by the source OPENER, not the text: `<!--[note]-->` is an
        # ordinary comment, `<![IGNORE[>` a marked section the parser routed here.
        off = self._offset()
        opener = self._text[off:off + 4]
        if opener.startswith("<!["):
            self._refuse("a marked section (<![...)")
        elif opener == "<!--" and data.startswith((">", "->")):
            self._refuse("text after <!--> or <!---> that a browser reads as markup")

    def handle_decl(self, decl):
        self._refuse("a declaration")

    def unknown_decl(self, data):
        self._refuse("a marked section (<![...)")

    def handle_pi(self, data):
        self._refuse("a processing instruction")

    @classmethod
    def scan(cls, html_text: str) -> "_RawAttrs":
        p = cls(html_text)
        p.feed(html_text)
        # html.parser holds an unfinished trailing tag back and drops it at
        # close(), while the page template's next `>` would complete it in a
        # browser, attributes and all. Anything left that opens like markup is
        # refused; a bare `a < b` is data and has already been consumed.
        if re.match(r"<[A-Za-z/!?]", p.rawdata):
            p._refuse("an unfinished tag at the end of a raw HTML block")
        p.close()
        return p


def html_anchors(html_text: str) -> set[str]:
    p = _AnchorParser()
    p.feed(html_text)
    p.close()
    return p.found


# Phrasing elements a browser runs together with the text around them, so the
# search text must not split `boot_<b>health</b>` into two words. Any other tag
# (a paragraph, a cell, a line break) separates words.
PHRASING_TAGS = frozenset({
    "a", "abbr", "b", "bdi", "bdo", "cite", "code", "data", "del", "dfn", "em", "i", "ins", "kbd",
    "mark", "q", "s", "samp", "small", "span", "strong", "sub", "sup", "time", "u", "var", "wbr"})


class _HtmlOutline(HTMLParser):
    """Headings, images and text of a raw HTML fragment, in document order, so the
    accessibility checks and the search index read raw HTML as they read Markdown.
    An <img> records its alt as None when the attribute is absent and "" when it
    is present and empty (the explicit decorative-image form); its alt text is
    part of the searchable text, as it is of what a screen reader announces."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.events: list[tuple[str, object]] = []    # ("h", level) | ("img", alt or None)
        self.parts: list[str] = []

    @property
    def text(self) -> str:
        return "".join(self.parts)

    def handle_starttag(self, tag, attrs):
        if re.fullmatch(r"h[1-6]", tag):
            self.events.append(("h", int(tag[1])))
        elif tag == "img":
            alts = [v for k, v in attrs if k == "alt"]
            alt = (alts[0] or "") if alts else None
            self.events.append(("img", alt))
            self.parts.append(f" {alt or ''} ")
            return
        if tag not in PHRASING_TAGS:
            self.parts.append(" ")

    handle_startendtag = handle_starttag

    def handle_endtag(self, tag):
        if tag not in PHRASING_TAGS:
            self.parts.append(" ")

    def handle_data(self, data):
        self.parts.append(data)

    @classmethod
    def scan(cls, html_text: str) -> "_HtmlOutline":
        p = cls()
        p.feed(html_text)
        p.close()
        return p


def a11y_findings(outline: list[tuple[str, object, str]]) -> list[str]:
    """Static accessibility rules over a page's headings and images in document
    order: exactly one H1, first; no heading deeper than one level below the one
    before it; every image described. A raw HTML heading is refused outright: it
    gets no anchor, no contents entry and no search section of its own. A Markdown image cannot say "decorative"
    apart from "forgotten", so it needs alt text; raw HTML may write alt="" for a
    decorative image (WCAG H67) but may not omit the attribute."""
    found: list[str] = []
    levels = [(v, where) for kind, v, where in outline if kind in ("h", "raw-h")]
    found.extend(f"raw HTML <h{v}>: write it as a Markdown heading, which is what the page's anchors, "
                 f"contents and search sections are built from: {where}" for kind, v, where in outline if kind == "raw-h")
    h1s = sum(1 for v, _ in levels if v == 1)
    if h1s != 1:
        found.append(f"{h1s} H1 headings; a page has exactly one")
    if levels and levels[0][0] != 1:
        found.append(f"the first heading is H{levels[0][0]} ({levels[0][1]!r}); a page starts with its H1")
    for (prev, _), (level, where) in zip(levels, levels[1:]):
        if level > prev + 1:
            found.append(f"heading level skips from H{prev} to H{level} at {where!r}")
    for kind, alt, where in outline:
        if kind == "md-img" and not str(alt).strip():
            found.append(f"image without alt text: {where}")
        elif kind == "img" and alt is None:
            found.append(f"raw <img> without an alt attribute (alt=\"\" marks a decorative one): {where}")
        elif kind == "img" and alt and not str(alt).strip():
            found.append(f"raw <img> with blank alt text: {where}")
    return found


def heading_title(inline, breaks: str = "") -> str:
    """An inline token's text: text and code spans, markup dropped. A line break
    becomes `breaks`: nothing for a heading id (GitHub drops it), a space for
    prose, so a wrapped paragraph does not run its words together."""
    return "".join(c.content if c.type in ("text", "code_inline") else breaks
                   for c in (inline.children or []) if c.type in ("text", "code_inline", "softbreak", "hardbreak"))


def raw_anchors(tokens) -> set[str]:
    """Explicit <a id="..."> / <a name="..."> anchors in raw HTML only, so one quoted
    inside a code example is not mistaken for a real anchor."""
    found: set[str] = set()
    for tok in tokens:
        if tok.type == "html_block":
            found |= html_anchors(tok.content)
        elif tok.type == "inline":
            for c in tok.children or []:
                if c.type == "html_inline":
                    found |= html_anchors(c.content)
    return found


_MD_ANCHORS: dict[str, set[str]] = {}
ANCHOR_CACHE = REPO / "build" / "site-anchor-cache.json"


def _anchor_cache_version() -> str:
    """Anything that could change an anchor set: this file (slug rules) and the parser."""
    import hashlib
    import markdown_it
    return hashlib.sha256(Path(__file__).read_bytes() + markdown_it.__version__.encode()).hexdigest()[:16]


class AnchorCache:
    """Full-parse anchor sets of Markdown link targets, keyed by content hash.

    The gate must agree with a real Markdown parse (a line scanner tried here
    diverged on reference links, list-indented fences and inline HTML), but a
    full parse of the ~12 MB of roadmap files that docs pages link into costs
    ~5 s. Keyed by content, a result can never be stale, so every commit after
    the first re-parses only the targets it changed. The file lives under the
    ignored build/ directory; a missing, corrupt or other-version cache is simply
    rebuilt, and nothing published depends on it. Saving keeps only the entries
    this run used, so editing a large roadmap file replaces its entry instead of
    adding one more per version forever."""

    def __init__(self, path: Path) -> None:
        self.path, self.version, self.dirty = path, _anchor_cache_version(), False
        self.entries: dict[str, list[str]] = {}
        self.used: set[str] = set()
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return
        entries = data.get("entries") if isinstance(data, dict) and data.get("version") == self.version else None
        if isinstance(entries, dict):
            # Keep only well-formed entries; anything else is a miss and is re-parsed.
            self.entries = {k: v for k, v in entries.items()
                            if isinstance(v, list) and all(isinstance(a, str) for a in v)}

    def anchors(self, text: str) -> set[str]:
        import hashlib
        key = hashlib.sha256(text.encode("utf-8")).hexdigest()
        self.used.add(key)
        hit = self.entries.get(key)
        if hit is not None:
            return set(hit)
        tokens = make_md().parse(text, {})
        slugger = Slugger()
        found = {slugger.slug(heading_title(tokens[i + 1])) for i, t in enumerate(tokens) if t.type == "heading_open"}
        found |= raw_anchors(tokens)
        self.entries[key], self.dirty = sorted(found), True
        return found

    def save(self) -> None:
        keep = {k: v for k, v in self.entries.items() if k in self.used}
        if not self.dirty and len(keep) == len(self.entries):
            return
        tmp = self.path.with_suffix(f".{os.getpid()}.tmp")
        try:   # write-then-rename, so a concurrent build never reads half a file
            self.path.parent.mkdir(parents=True, exist_ok=True)
            tmp.write_text(json.dumps({"version": self.version, "entries": keep}), encoding="utf-8")
            os.replace(tmp, self.path)
        except OSError:
            try:   # a cache that cannot be written only costs time, and neither can its cleanup
                tmp.unlink(missing_ok=True)
            except OSError:
                pass
        self.entries, self.dirty = keep, False


def markdown_anchors(repo_rel: str, cache: "AnchorCache | None" = None) -> set[str]:
    """The fragment ids GitHub gives a tracked Markdown file outside docs/ (a link
    from a docs page to it resolves on github.com, not on the site)."""
    path = str(ROOT / repo_rel)   # keyed by full path: ROOT moves to a snapshot
    if path not in _MD_ANCHORS:
        text = Path(path).read_text(encoding="utf-8")
        _MD_ANCHORS[path] = (cache or AnchorCache(ANCHOR_CACHE)).anchors(text)
    return _MD_ANCHORS[path]


@dataclass
class Page:
    src: Path                       # docs/... .md
    rel: str                        # path relative to docs/, posix
    url: str                        # site path relative to /docs/
    title: str = ""
    order: int = 1000
    covers: list[str] = field(default_factory=list)
    sources: list[str] = field(default_factory=list)   # code the page describes (freshness)
    reviewed: str = ""                                 # YYYY-MM-DD, bumped when re-checked unchanged
    body: str = ""
    toc: list[tuple[int, str, str]] = field(default_factory=list)
    anchors: set[str] = field(default_factory=set)
    text: str = ""                                     # plain text tokens only: the legacy search index
    summary: str = ""                                  # first top-level paragraph (meta description)
    records: list[tuple[str, str, str]] = field(default_factory=list)  # search: (anchor, heading path, text)
    a11y: list[str] = field(default_factory=list)      # accessibility findings, reported for the current tree


def out_path(url: str) -> str:
    return url + "index.html" if (url == "" or url.endswith("/")) else url


def page_url(rel: str) -> str:
    p = rel[:-3]  # strip .md
    if p == "index" or p.endswith("/index"):
        return p[: -len("index")]
    return p + ".html"


def make_md() -> MarkdownIt:
    md = MarkdownIt("commonmark", {"html": True, "linkify": False, "typographer": False})
    md.enable(["table", "strikethrough"])
    return md


ALERT_RE = re.compile(
    r"<blockquote>\s*<p>\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\]\s*(?:<br\s*/?>)?\s*", re.S)



def valid_reviewed(value) -> bool:
    """reviewed= is a date (YYYY-MM-DD) or, for a second review on the same day,
    a minute-precision time (YYYY-MM-DDTHH:MM). Staleness is decided by COMMIT,
    so bumping the value is what re-baselines a page; a date-only field left no
    honest way to re-review a page twice in one day."""
    import re as _re
    if not isinstance(value, str) or not _re.fullmatch(r"\d{4}-\d{2}-\d{2}(T\d{2}:\d{2})?", value):
        return False
    try:
        _dt.datetime.fromisoformat(value)
    except ValueError:
        return False
    return True

def parse_directives(text: str) -> dict[str, str]:
    """Directives count only ABOVE the first heading, so an example directive
    quoted later in the page (as the site docs do) is never mistaken for it."""
    head = re.split(r"^#", text, maxsplit=1, flags=re.M)[0]
    found: dict[str, str] = {}
    for m in DIRECTIVE_RE.finditer(head):
        for part in re.split(r"\s+(?=[a-z_]+=)", m.group(1).strip()):
            if "=" in part:
                k, v = part.split("=", 1)
                found[k.strip()] = v.strip()
    return found


def directive_sources(text: str) -> list[str]:
    """The `sources=` list of a page's directive, normalised (no trailing slash)."""
    return [c.strip().rstrip("/") for c in parse_directives(text).get("sources", "").split(",") if c.strip()]


class Renderer:
    def __init__(self, facts: dict, pages: dict[str, Page], errors: list[str]):
        self.facts = facts
        self.pages = pages          # rel -> Page
        self.errors = errors
        self.md = make_md()
        self.pending_anchor_checks: list[tuple[str, str, str]] = []
        self.pending_md_anchor_checks: list[tuple[str, str, str]] = []   # into tracked .md outside docs/
        # Release builds only: absolute links as a browser resolves them. None on
        # the collecting pass, which records every candidate in `absolute`.
        self.browser: dict[str, str | None] | None = None
        self.absolute: set[str] = set()

    def blob_url(self, repo_rel: str) -> str:
        return f"{self.facts['repo_url']}/blob/{LINK_REF}/{repo_rel}"

    def raw_url(self, repo_rel: str) -> str:
        return f"https://raw.githubusercontent.com/{self.facts['owner']}/{self.facts['repo']}/{LINK_REF}/{repo_rel}"

    def pin_absolute(self, page: Page, url: str) -> str | None:
        """A release build pins absolute links into this repository's `main` too:
        a page that spells out https://github.com/<owner>/impossible-os/blob/main/X
        would otherwise send a reader of an old release to today's X. The link is
        matched as a browser resolves it, by Node's WHATWG `URL` (`self.browser`,
        filled between the two release render passes): scheme, host, default
        port, dot segments, backslashes and credentials are the parser's answer,
        never a hand-written approximation. Owners compare case-insensitively and
        include the historical ones. The target must exist in the release tree,
        and a Markdown fragment must be a GitHub heading id there. Returns None for
        anything that is not such a link, and always None outside a release build,
        so the main site is unchanged."""
        if RELEASE is None:
            return None
        if self.browser is None:
            self.absolute.add(url)
            return None
        if url not in self.browser:     # fail closed: an unresolved link must never ship as written
            self.errors.append(f"docs/{page.rel}: absolute link was not resolved before pinning: {url}")
            return url
        resolved = self.browser[url]
        if not resolved:
            return None
        parts = urlsplit(resolved)
        if parts.scheme not in ("http", "https") or parts.port is not None:
            return None
        raw = parts.path.split("/")
        seg = [unquote(x) for x in raw]
        owners = {o.lower() for o in [self.facts["owner"], *self.facts.get("_historical_owners", [])]}
        host = (parts.hostname or "").rstrip(".")
        if len(seg) < 4 or seg[1].lower() not in owners or seg[2].lower() != self.facts["repo"].lower():
            return None
        if host == "github.com" and seg[3] in ("blob", "tree", "raw"):
            kind, at = seg[3], 4
        elif host == "raw.githubusercontent.com":
            kind, at = "rawhost", 3
        else:
            return None
        if any("/" in x for x in seg):     # `rel%2Fv2`: GitHub reads a slash there, the matcher could not
            self.errors.append(f"docs/{page.rel}: repository link with an encoded slash; write it with /: {url}")
            return url
        # GitHub names the branch as `main` or `refs/heads/main`, and this release's
        # own tag as `<tag>` or `refs/tags/<tag>` (a tag can be moved, so it pins
        # too). A link naming any other ref already names what its author meant.
        # The match depends on the commit alone, never on which refs a checkout
        # holds, so a release tree stays byte-reproducible (verify_live.py): a
        # branch named `main/...` or `<tag>/...` would make such a URL ambiguous
        # on GitHub too, and the repository does not create one.
        tag = RELEASE.split("/")
        for ref in (["refs", "heads", "main"], ["main"], ["refs", "tags", *tag], tag):
            if seg[at:at + len(ref)] == ref:
                rest = "/".join(raw[at + len(ref):])
                break
        else:
            return None
        fragment = urlsplit(url).fragment      # the parser dropped it; the written one is what a reader follows
        repo_rel = unquote(rest).rstrip("/")
        files, dirs = fileset()
        exists = (repo_rel in dirs or repo_rel == "") if kind == "tree" else repo_rel in files
        if not exists:
            self.errors.append(f"docs/{page.rel}: dead link at release {RELEASE} (not in that tree): {url}")
            return url
        if kind == "blob" and fragment and repo_rel.endswith(".md"):
            self.pending_md_anchor_checks.append((page.rel, repo_rel, fragment))
        if kind == "rawhost":
            pinned = self.raw_url(rest)
        else:
            pinned = f"{self.facts['repo_url']}/{kind}/{LINK_REF}" + (f"/{rest}" if rest else "")
        return pinned + (f"?{parts.query}" if parts.query else "") + (f"#{fragment}" if fragment else "")

    def pin_mermaid(self, page: Page, source: str) -> str:
        """Diagram click targets are links a reader follows, so a release pins them
        like any other (the TODO graph links every roadmap file). Each line is
        tokenized with shlex, which reads Mermaid's double-quoted strings, with `;`
        as punctuation so statements split there and a terminator never joins the
        URL. Only the URL operand of a `click <node> ["href"] "<url>"` statement is
        pinned; a tooltip or target after it is text. A `%%` line is a Mermaid
        comment and `#` is ordinary text (`fill:#fff`), never a shell comment. A
        line naming `click` that cannot be tokenized fails the release rather than
        keep a `main` link."""
        if RELEASE is None:
            return source
        out = []
        for line in source.split("\n"):
            if line.lstrip().startswith("%%"):
                out.append(line)
                continue
            lex = shlex.shlex(line, posix=True, punctuation_chars=";")
            lex.whitespace_split = True
            lex.quotes, lex.escape = '"', ""   # Mermaid strings: double quotes, no escapes; `Bob's` is text
            lex.commenters = ""
            try:
                tokens = list(lex)
            except ValueError:
                if "click" in line:
                    self.errors.append(f"docs/{page.rel}: cannot read a Mermaid click line at release {RELEASE}: "
                                       f"{line.strip()}")
                out.append(line)
                continue
            stmt: list[str] = []
            for tok in tokens + [";"]:
                if tok != ";":
                    stmt.append(tok)
                    continue
                if len(stmt) >= 3 and stmt[0] == "click":
                    url = stmt[3] if stmt[2] == "href" and len(stmt) >= 4 else stmt[2]
                    pinned = self.pin_absolute(page, url)
                    if pinned is not None and pinned != url:
                        line = line.replace(f'"{url}"', f'"{pinned}"')
                stmt = []
            out.append(line)
        return "\n".join(out)

    def rewrite_href(self, page: Page, href: str) -> str:
        if href.startswith("#") and len(href) > 1:
            self.pending_anchor_checks.append((page.rel, page.rel, href[1:]))
            return href
        if not href or href.startswith(("#", "mailto:")) or re.match(r"^[a-z][a-z0-9+.-]*:", href):
            pinned = self.pin_absolute(page, href) if href else None
            return href if pinned is None else pinned
        path, _, frag = href.partition("#")
        target = Path(os.path.normpath(page.src.parent / unquote(path)))
        try:
            repo_rel = target.relative_to(ROOT).as_posix()
        except ValueError:
            self.errors.append(f"docs/{page.rel}: link escapes the repository: {href}")
            return href
        files, dirs = fileset()
        is_dir = repo_rel in dirs or repo_rel == "."
        if repo_rel not in files and not is_dir:
            self.errors.append(f"docs/{page.rel}: dead link (not a tracked file): {href}")
            return href
        if is_dir:
            idx = repo_rel + "/index.md"
            if repo_rel.startswith("docs/") and idx in files:
                target, repo_rel = ROOT / idx, idx
            else:
                return f"{self.facts['repo_url']}/tree/{LINK_REF}/{repo_rel}"
        if repo_rel.startswith("docs/") and repo_rel.endswith(".md"):
            doc_rel = repo_rel[len("docs/"):]
            if frag:
                self.pending_anchor_checks.append((page.rel, doc_rel, frag))
            here_dir = posixpath.dirname(out_path(page.url)) or "."
            rel = posixpath.relpath(out_path(page_url(doc_rel)), here_dir)
            if rel == "index.html" or rel.endswith("/index.html"):
                rel = rel[: -len("index.html")] or "./"
            return rel + (f"#{frag}" if frag else "")
        if frag and repo_rel.endswith(".md"):
            self.pending_md_anchor_checks.append((page.rel, repo_rel, frag))
        return self.blob_url(repo_rel) + (f"#{frag}" if frag else "")

    def collect(self, page: Page, tokens) -> None:
        """Release builds: offer every link of a parsed page to the same rewriters
        render() uses, discarding the results, so pin_absolute() records each
        absolute link for the browser resolution pass. Tokens are not modified."""
        for tok in tokens:
            if tok.type == "inline":
                for child in tok.children or []:
                    if child.type == "link_open":
                        self.rewrite_href(page, child.attrGet("href") or "")
                    elif child.type == "image":
                        self.rewrite_img(page, child.attrGet("src") or "")
                    elif child.type == "html_inline":
                        self.rewrite_raw_html(page, child.content)
            elif tok.type == "html_block":
                self.rewrite_raw_html(page, tok.content)
            elif tok.type == "fence" and tok.info.strip() == "mermaid":
                self.pin_mermaid(page, tok.content)

    def render(self, page: Page, text: str, tokens=None, env: dict | None = None) -> None:
        if tokens is None:
            env = {}
            tokens = self.md.parse(text, env)
        env = env if env is not None else {}
        slugger = Slugger()
        plain: list[str] = []
        # Search records, one per H2/H3 section plus the part above the first,
        # holding EVERY word a reader can see there: prose, code spans, code
        # blocks, image alt text and raw HTML text. An H3 carries its H2 in the
        # heading path, so a query naming both still lands in one record.
        records: list[tuple[str, str, list[str]]] = [("", "", [])]
        h2 = ""
        heading_inline = -1                   # the inline token of an H1-H3: its text is the heading, not body
        outline: list[tuple[str, object, str]] = []   # headings and images in order, for a11y_findings
        for i, tok in enumerate(tokens):
            if tok.type == "paragraph_open" and tok.level == 0 and not page.summary:
                page.summary = " ".join(heading_title(tokens[i + 1], breaks=" ").split())
            if tok.type == "heading_open":
                inline = tokens[i + 1]
                title = heading_title(inline)
                slug = slugger.slug(title)
                tok.attrSet("id", slug)
                page.anchors.add(slug)
                level = int(tok.tag[1])
                outline.append(("h", level, title.strip()))
                if level == 1 and not page.title:
                    page.title = title.strip()
                if level in (2, 3):
                    page.toc.append((level, slug, title.strip()))
                    h2 = title.strip() if level == 2 else h2
                    path = title.strip() if level == 2 or not h2 else f"{h2} \u203a {title.strip()}"
                    records.append((slug, path, []))
                # An H1 is the page title; its text is body only where a title= directive
                # overrides it, so a word shown there stays findable.
                if level in (2, 3) or (level == 1 and title.strip() == page.title):
                    heading_inline = i + 1
            if tok.type == "inline":
                words: list[str] = []
                shown: list[str] = []         # alt and raw HTML text: the part a heading's title leaves out
                for child in tok.children or []:
                    if child.type == "link_open":
                        child.attrSet("href", self.rewrite_href(page, child.attrGet("href") or ""))
                    elif child.type == "image":
                        # The alt a browser gets: markup stripped, so ![`x`](y) renders alt="".
                        alt = self.md.renderer.renderInlineAsText(child.children, self.md.options, env)
                        outline.append(("md-img", alt, child.attrGet("src") or ""))
                        words.append(f" {alt} ")
                        shown.append(alt)
                        child.attrSet("src", self.rewrite_img(page, child.attrGet("src") or ""))
                    elif child.type == "text":
                        plain.append(child.content)
                        words.append(child.content)
                    elif child.type == "code_inline":
                        words.append(child.content)
                    elif child.type in ("softbreak", "hardbreak"):
                        words.append(" ")
                    elif child.type == "html_inline":
                        seen = _HtmlOutline.scan(child.content)
                        outline.extend(("raw-" + k if k == "h" else k, v, child.content.strip()[:80])
                                       for k, v in seen.events)
                        words.append(seen.text)
                        shown.append(seen.text)
                records[-1][2].append("".join(words) if i != heading_inline else " ".join(shown))
            if tok.type in ("fence", "code_block"):
                records[-1][2].append(tok.content)
            if tok.type == "html_block":
                seen = _HtmlOutline.scan(tok.content)
                outline.extend(("raw-" + k if k == "h" else k, v, " ".join(tok.content.split())[:80])
                               for k, v in seen.events)
                records[-1][2].append(seen.text)
                tok.content = self.rewrite_raw_html(page, tok.content)
            if tok.type == "inline":
                for child in tok.children or []:
                    if child.type == "html_inline":
                        child.content = self.rewrite_raw_html(page, child.content)
            if tok.type == "fence" and tok.info.strip() == "mermaid":
                tok.type = "html_block"
                tok.content = f'<pre class="mermaid">{html.escape(self.pin_mermaid(page, tok.content))}</pre>\n'
        page.anchors |= raw_anchors(tokens)
        body = self.md.renderer.render(tokens, self.md.options, env)
        body = ALERT_RE.sub(lambda m: f'<blockquote class="alert alert-{m.group(1).lower()}"><p class="alert-title">{m.group(1).title()}</p><p>', body)
        body = re.sub(r"<li>\[ \]", '<li class="task"><input type="checkbox" disabled>', body)
        body = re.sub(r"<li>\[[xX]\]", '<li class="task"><input type="checkbox" checked disabled>', body)
        body = re.sub(r"<table>", '<div class="table-wrap"><table>', body)
        body = re.sub(r"</table>", "</table></div>", body)
        page.body = body
        page.text = " ".join(plain)
        page.records = [(anchor, path, " ".join(" ".join(parts).split())) for anchor, path, parts in records]
        page.a11y = a11y_findings(outline)
        if not page.title:
            page.title = page.rel

    # A tag scanner, not a bare attribute regex: comments pass through untouched, and
    # attributes are consumed one name=value pair at a time, so text inside another
    # attribute's value (title="use href=x") is never mistaken for a link.
    _VAL = r'(?:"[^"]*"|\'[^\']*\'|[^\s"\'=<>`]+)'
    TAG_RE = re.compile(r'<!--.*?-->|<([A-Za-z][\w:-]*)((?:\s+[^\s=/>"\']+(?:\s*=\s*' + _VAL + r')?)*)(\s*/?>)', re.S)
    ATTR_RE = re.compile(r'(\s+)([^\s=/>"\']+)(?:(\s*=\s*)(' + _VAL + r'))?')

    def rewrite_raw_html(self, page: Page, html_text: str) -> str:
        """Raw HTML href/src get the same rewriting and validation as Markdown links.
        Values are entity-decoded before use and escaped exactly once on output.

        The rewriter is a tag scanner, and a browser also reads tags it does not
        (`<a/href=...>`), so html.parser is the net (`_RawAttrs`): an <a href> or
        <img src> the scanner did not rewrite at that exact tag fails the build,
        and so does anything outside the raw-HTML allowlist."""
        seen = _RawAttrs.scan(html_text)
        if seen.refused:
            self.errors.append(f"docs/{page.rel}: raw HTML may not contain {seen.refused}: links are "
                               f"checked and pinned only as <a href> and <img src>")
        rewritten: dict[int, int] = {}      # tag offset -> href/src the scanner rewrote there
        here = [0]

        def attr_sub(m: re.Match) -> str:
            name, value = m.group(2), m.group(4)
            if value is None or name.lower() not in ("href", "src"):
                return m.group(0)
            rewritten[here[0]] = rewritten.get(here[0], 0) + 1
            raw = value[1:-1] if value[:1] in ("\"", "\'") else value
            url = html.unescape(raw)
            if not self.md.validateLink(url):     # the scheme policy Markdown links get (javascript:, vbscript:...)
                self.errors.append(f"docs/{page.rel}: raw HTML {name.lower()}= with a URL Markdown would refuse: "
                                   f"{url[:80]}")
                return m.group(0)
            new = self.rewrite_href(page, url) if name.lower() == "href" else self.rewrite_img(page, url)
            return f'{m.group(1)}{name}="{html.escape(new, quote=True)}"'

        def tag_sub(m: re.Match) -> str:
            if m.group(0).startswith("<!--"):
                return m.group(0)
            here[0] = m.start()
            return "<" + m.group(1) + self.ATTR_RE.sub(attr_sub, m.group(2)) + m.group(3)
        out = self.TAG_RE.sub(tag_sub, html_text)
        if any(rewritten.get(off) != n for off, n in seen.links.items()):
            self.errors.append(f"docs/{page.rel}: raw HTML the link rewriter cannot read (an href or src it "
                               f"would leave unchecked); put whitespace between a tag's name and attributes: "
                               f"{' '.join(html_text.split())[:120]}")
        return out

    def rewrite_img(self, page: Page, src: str) -> str:
        if not src or re.match(r"^[a-z][a-z0-9+.-]*:", src):
            pinned = self.pin_absolute(page, src) if src else None
            return src if pinned is None else pinned
        target = Path(os.path.normpath(page.src.parent / unquote(src)))
        try:
            repo_rel = target.relative_to(ROOT).as_posix()
        except ValueError:
            repo_rel = ""
        if repo_rel not in fileset()[0]:
            self.errors.append(f"docs/{page.rel}: missing image (not a tracked file): {src}")
            return src
        return self.raw_url(repo_rel)

    def check_anchors(self) -> None:
        for src_rel, doc_rel, frag in self.pending_anchor_checks:
            target = self.pages.get(doc_rel)
            if target is not None and unquote(frag) not in target.anchors:
                self.errors.append(f"docs/{src_rel}: dead anchor: {doc_rel}#{frag}")
        cache = AnchorCache(ANCHOR_CACHE)
        for src_rel, repo_rel, frag in self.pending_md_anchor_checks:
            if unquote(frag) not in markdown_anchors(repo_rel, cache):
                self.errors.append(f"docs/{src_rel}: dead anchor (no such GitHub heading id): {repo_rel}#{frag}")
        cache.save()


# --------------------------------------------------------------------------
# Coverage
# --------------------------------------------------------------------------

def todo_files() -> list[str]:
    """Domain roadmap files only (todo/NN-domain/TODO-*.md); internal trackers are excluded."""
    return sorted(r for r in (p.relative_to(ROOT).as_posix() for p in TODO.glob("[0-9][0-9]-*/TODO-*.md")))


def todo_title(repo_rel: str) -> str:
    for line in (ROOT / repo_rel).read_text(encoding="utf-8").splitlines():
        if line.startswith("# "):
            return line[2:].strip()
    return Path(repo_rel).stem


def coverage(pages: dict[str, Page], errors: list[str]) -> tuple[dict[str, list[str]], list[str]]:
    todos = todo_files()
    known = set(todos)
    covered: dict[str, list[str]] = {t: [] for t in todos}
    for page in pages.values():
        for t in page.covers:
            if t not in known:
                errors.append(f"docs/{page.rel}: covers= names a TODO that does not exist: {t}")
                continue
            covered[t].append(page.rel)
    undocumented = [t for t, docs in covered.items() if not docs]
    return covered, undocumented


UI_TRIGGER_RE = re.compile(
    r"src/desktop/|include/desktop/|\btaskbar\b|\bstart menu\b|quick settings|context menu|\btitle ?bar\b|"
    r"caption button|desktop icon|wallpaper|file explorer|notification center|\bacrylic\b|\bmica\b", re.I)
DESIGN_LINE_RE = re.compile(r"^\*\*Design( deviation)?:\*\*\s*(.+)$", re.M)
DESIGN_REF_RE = re.compile(r"docs/design/([a-z0-9_-]+\.md)(?:#([A-Za-z0-9_-]+))?")


def todo_sections(text: str):
    """Yield (number, title, body, first_line) for each `## N.` section.

    Headings and boundaries come from the shared rule in `scripts/todo_fence.py`
    (fence-aware, CommonMark indent), the same one the graph producer uses, so a
    `## 9.` inside a code sample is never a section here either."""
    scan = todo_fence.scan_text(text)
    lines, mask = scan.lines, scan.mask
    for i, line in enumerate(lines):
        if mask[i]:
            continue
        head = todo_fence.classify_heading(line)
        if head.kind != "ok" or not head.title:
            continue
        end = next((j for j in range(i + 1, len(lines)) if not mask[j] and todo_fence.is_h2(lines[j])), len(lines))
        yield head.n, head.title.strip(), "\n".join(lines[i + 1:end]), i + 1


def check_design_lines(pages: dict[str, Page], errors: list[str]) -> None:
    """Every OPEN roadmap section in design scope (docs/design/scope.json: the shell
    files, plus any section whose title names a UI surface) must carry a
    `**Design:**` line citing the spec anchors it implements, so an implementer --
    or the unattended run -- builds the design rather than the section's own older
    wording. `**Design:** n/a -- <reason>` opts a section out; `**Design deviation:**
    <reason>` records a deliberate departure. Every cited file and anchor must exist,
    in or out of scope."""
    scope = json.loads((DOCS / "design" / "scope.json").read_text(encoding="utf-8"))
    shell_files = set(scope["shell_files"])
    title_re = re.compile(scope["ui_title_pattern"])
    known = set(todo_files())
    for f in sorted(shell_files - known):
        errors.append(f"docs/design/scope.json: shell file does not exist: {f}")
    for rel in todo_files():
        text = (ROOT / rel).read_text(encoding="utf-8")
        for num, title, body, line in todo_sections(text):
            lines = DESIGN_LINE_RE.findall(body)
            is_open = re.search(r"^\s*- \[[ /]\]", body, re.M)
            in_scope = rel in shell_files or title_re.search(title)
            if is_open and in_scope and not lines:
                errors.append(f"{rel}:{line}: section {num} ({title}) is in design scope but has no **Design:** line "
                              f"(cite docs/design/<file>.md#<anchor>, or '**Design:** n/a -- <reason>')")
            for kind, value in lines:
                if value.lower().startswith("n/a"):
                    if len(value) < 12:
                        errors.append(f"{rel}:{line}: section {num}: '**Design:** n/a' needs a reason")
                    continue
                refs = DESIGN_REF_RE.findall(value)
                if not refs and not kind:
                    errors.append(f"{rel}:{line}: section {num}: **Design:** line cites no docs/design/*.md reference")
                for fname, anchor in refs:
                    page = pages.get(f"design/{fname}")
                    if page is None:
                        errors.append(f"{rel}:{line}: section {num}: docs/design/{fname} does not exist")
                    elif anchor and anchor not in page.anchors:
                        errors.append(f"{rel}:{line}: section {num}: dead design anchor docs/design/{fname}#{anchor}")


def baseline_reference() -> set[str] | None:
    """The previously COMMITTED baseline the candidate is measured against: HEAD's
    copy when the candidate differs from HEAD (a commit being made), otherwise
    HEAD~1's (CI checking a commit that already landed). None when no prior copy
    exists (first creation), which is the only way the list may start non-empty."""
    rel = "docs/.coverage-baseline.json"
    cand = BASELINE.read_bytes() if BASELINE.exists() else b""
    for ref in ("HEAD", "HEAD~1"):
        proc = subprocess.run(["git", "show", f"{ref}:{rel}"], cwd=REPO, capture_output=True)
        if proc.returncode != 0:
            return None
        if ref == "HEAD" and proc.stdout == cand and SOURCE in ("worktree", "index"):
            continue  # unchanged against HEAD: compare with the commit before it
        return set(json.loads(proc.stdout.decode("utf-8"))["undocumented"])
    return None


def shrink_baseline(data: dict | None, undocumented: list[str]) -> dict:
    """The --update-baseline result: entries that are still undocumented AND already
    listed (never an addition), with growth reasons kept only for entries still listed.
    With no existing file, the baseline is created from the current undocumented set."""
    comment = ("TODO files with no documentation page yet. Shrink-only: scripts/site/build.py "
               "--update-baseline removes entries, never adds them (except when creating this file).")
    if data is None:
        return {"_comment": comment, "undocumented": sorted(undocumented)}
    keep = sorted(set(undocumented) & set(data.get("undocumented", [])))
    reasons = {k: v for k, v in data.get("growth_reasons", {}).items() if k in set(keep)}
    out = {"_comment": comment, "undocumented": keep}
    if reasons:
        out["growth_reasons"] = reasons
    return out


def check_baseline(undocumented: list[str], errors: list[str]) -> None:
    baseline = set(json.loads(BASELINE.read_text(encoding="utf-8"))["undocumented"]) if BASELINE.exists() else set()
    prior = baseline_reference()
    reasons = json.loads(BASELINE.read_text(encoding="utf-8")).get("growth_reasons", {}) if BASELINE.exists() else {}
    if prior is not None:
        for t in sorted(baseline - prior):
            if len(str(reasons.get(t, "")).strip()) >= 20:
                continue  # an explicit, reviewable reason in the baseline file itself

            errors.append(f"coverage: baseline GREW: {t} was added to docs/.coverage-baseline.json; the baseline "
                          f"only shrinks -- write the docs page instead, or (only when removing a false claim) "
                          f"record why under \"growth_reasons\" in the baseline file")
    for t in undocumented:
        if t not in baseline:
            errors.append(f"coverage: {t} has no documentation page. Add a docs page with "
                          f"'<!-- docs: covers={t} -->' (the baseline never grows).")
    for t in sorted(baseline - set(undocumented)):
        errors.append(f"coverage: baseline entry is stale (now documented or removed): {t}. "
                      f"Run: python3 scripts/site/build.py --update-baseline")


# --------------------------------------------------------------------------
# Site assembly
# --------------------------------------------------------------------------

_TRACKED: tuple[set[str], set[str]] | None = None


def tracked_sources() -> tuple[set[str], set[str]]:
    """Tracked files and directories of the SOURCE being checked. Stricter than
    fileset(): an untracked local file has no history, so it can be no source."""
    global _TRACKED
    if _TRACKED is None:
        import freshness
        freshness.REPO = REPO
        _TRACKED = freshness.tracked(SOURCE)
    return _TRACKED


DATE_HEADER_RE = re.compile(r"\x01\d{4}-\d{2}-\d{2}")


def last_updated() -> dict[str, str] | None:
    """{repo path: YYYY-MM-DD} for every file under docs/ and gh-pages/: the
    committer date of the last commit on the first-parent line that touched it,
    as `git log -1 --format=%cs -- <path>` gives it, in one git process.

    `--first-parent --diff-merges=first-parent` counts a merge that changed a page (a conflict
    resolution) as a change on the main line; plain `log` prints no files for a
    merge and would date the page earlier. `--no-renames` dates a renamed page
    at its rename. The commit is HEAD for a worktree or index build and the
    snapshot ref otherwise, so a page is dated by what is committed, and a page
    with no commit yet has no date. None in a shallow clone, where the oldest
    fetched commit would be misread as the last change."""
    if git("rev-parse", "--is-shallow-repository").strip() == "true":
        return None
    ref = "HEAD" if SOURCE in ("worktree", "index") else SOURCE
    if subprocess.run(["git", "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}"], cwd=REPO,
                      capture_output=True).returncode != 0:
        return {}   # unborn HEAD: nothing is committed, so nothing has a date
    # Every output-shaping setting is pinned on the command line, so a host's git
    # config (log.diffMerges=combined, log.showSignature) cannot change the bytes.
    out = git("-c", "log.showSignature=false", "log", "--first-parent", "--diff-merges=first-parent",
              "--no-renames", "-z", "--name-only", "--format=%x01%cs", ref, "--", "docs", "gh-pages")
    dates: dict[str, str] = {}
    date = ""
    # NUL-separated tokens. A commit header is exactly "\x01YYYY-MM-DD"; a path
    # is limited to docs/ or gh-pages/, so it can never take that shape, and a
    # path containing \x01 (legal in a file name) stays one token.
    for tok in out.split("\0"):
        tok = tok.lstrip("\n")
        if DATE_HEADER_RE.fullmatch(tok):
            date = tok[1:]
        elif tok and date:
            dates.setdefault(tok, date)
    return dates


def check_sources(where: str, sources: list[str], errors: list[str]) -> None:
    files, dirs = tracked_sources()
    for src in sources:
        if src not in files and src not in dirs:
            errors.append(f"{where}: source path is not a tracked file or directory: {src}")


def load_pages(facts: dict, errors: list[str]) -> tuple[dict[str, Page], Renderer]:
    """Render every docs page. Each page is parsed once. A release build then
    keeps the parsed tokens and walks them through the same link rewriters with
    their results discarded, which only collects the absolute links; one Node
    process resolves them all as a browser would, and the single render pins
    against that answer. The main build renders each page as it is read."""
    pages: dict[str, Page] = {}
    for src in sorted(DOCS.rglob("*.md")):
        rel = src.relative_to(DOCS).as_posix()
        pages[rel] = Page(src=src, rel=rel, url=page_url(rel))
    r = Renderer(facts, pages, errors)
    parsed: dict[str, tuple[str, list, dict]] = {}
    for page in pages.values():
        text = page.src.read_text(encoding="utf-8")
        d = parse_directives(text)
        page.covers = [c.strip() for c in d.get("covers", "").split(",") if c.strip()]
        page.sources = directive_sources(text)
        page.reviewed = d.get("reviewed", "")
        if page.reviewed and not valid_reviewed(page.reviewed):
            errors.append(f"docs/{page.rel}: reviewed= must be YYYY-MM-DD or YYYY-MM-DDTHH:MM, got {page.reviewed!r}")
        if "order" in d:
            page.order = int(d["order"])
        if "title" in d:
            page.title = d["title"]
        if RELEASE is None:
            r.render(page, text)                    # stream: the main build keeps no token trees
        else:
            env: dict = {}
            parsed[page.rel] = (text, r.md.parse(text, env), env)
    if RELEASE is not None:
        probe = Renderer(facts, pages, [])          # its errors and anchor checks are the render's to report
        for page in pages.values():
            probe.collect(page, parsed[page.rel][1])
        r.browser = {}
        if probe.absolute:
            import linkcheck  # noqa: E402  (sibling module, like freshness: the one WHATWG URL bridge)
            try:
                r.browser = linkcheck.browser_urls(sorted(probe.absolute))
            except linkcheck.NormalizeError as e:
                errors.append(f"release {RELEASE}: cannot resolve absolute links, so none could be pinned: {e}")
        for page in pages.values():
            text, tokens, env = parsed.pop(page.rel)
            r.render(page, text, tokens, env)
    r.check_anchors()
    return pages, r


def nav_tree(pages: dict[str, Page]) -> dict:
    tree: dict = {"pages": [], "dirs": {}}
    for page in pages.values():
        parts = page.rel.split("/")
        node = tree
        for part in parts[:-1]:
            node = node["dirs"].setdefault(part, {"pages": [], "dirs": {}})
        node["pages"].append(page)
    return tree


def section_title(pages: dict[str, Page], prefix: str, name: str) -> str:
    idx = pages.get(f"{prefix}{name}/index.md")
    if idx and idx.title:
        return re.sub(r"^[^\w]+", "", idx.title).strip()
    return name.replace("-", " ").title()


def nav_html(pages: dict[str, Page], current: Page, rel_root: str) -> str:
    tree = nav_tree(pages)
    out: list[str] = []

    def emit(node: dict, prefix: str) -> None:
        items = sorted(node["pages"], key=lambda p: (0 if p.rel.endswith("index.md") else 1, p.order, p.title.lower()))
        out.append("<ul>")
        for p in items:
            if prefix and p.rel.endswith("index.md"):
                continue
            cls = ' class="active"' if p is current else ""
            out.append(f'<li><a{cls} href="{rel_root}{p.url or "./"}">{html.escape(p.title)}</a></li>')
        for name in sorted(node["dirs"], key=lambda n: section_title(pages, prefix, n).lower()):
            sub = node["dirs"][name]
            idx = pages.get(f"{prefix}{name}/index.md")
            title = html.escape(section_title(pages, prefix, name))
            open_attr = " open" if current.rel.startswith(f"{prefix}{name}/") else ""
            head = (f'<a href="{rel_root}{idx.url}">{title}</a>' if idx else title)
            out.append(f"<li><details{open_attr}><summary>{head}</summary>")
            emit(sub, f"{prefix}{name}/")
            out.append("</details></li>")
        out.append("</ul>")

    emit(tree, "")
    return "\n".join(out)


def depth_prefix(url: str) -> str:
    return "../" * out_path(url).count("/")


def toc_html(page: Page) -> str:
    if len(page.toc) < 2:
        return ""
    items = "".join(
        f'<li class="l{lvl}"><a href="#{slug}">{html.escape(t)}</a></li>' for lvl, slug, t in page.toc)
    return f'<nav class="toc"><p>On this page</p><ul>{items}</ul></nav>'


def site_url(facts: dict, path: str) -> str:
    """Absolute published URL of a site path; a trailing index.html is the directory."""
    if path == "index.html" or path.endswith("/index.html"):
        path = path[: -len("index.html")]
    return facts["site_url"].rstrip("/") + "/" + path


def description(facts: dict, page: Page) -> str:
    """The page's first paragraph, cut on a word boundary: what a search result or
    a link preview shows. Pages that open with no paragraph get a generic line."""
    text = page.summary or f"{facts['name']} documentation: {page.title}"
    if len(text) > 160:
        text = text[:157].rsplit(" ", 1)[0].rstrip(",;:") + "..."
    return text


def render_page(tpl: str, facts: dict, pages: dict[str, Page], page: Page, updated: str = "") -> str:
    root = depth_prefix(page.url)
    stamp = (f'<span>Last updated <time datetime="{updated}">{updated}</time></span>' if updated else "")
    values = {
        "TITLE": html.escape(page.title),
        "DESCRIPTION": html.escape(description(facts, page)),
        "CANONICAL": html.escape(site_url(facts, DOCS_BASE + out_path(page.url))),
        "ROOT": root,
        "SITE_ROOT": root + "../" * DOCS_BASE.count("/"),
        "NAV": nav_html(pages, page, root),
        "TOC": toc_html(page),
        "BODY": page.body,
        "UPDATED": stamp,
        "EDIT": f"{facts['repo_url']}/blob/{LINK_REF}/docs/{page.rel}",
    }
    # One pass over the TEMPLATE: text substituted in (a page body that mentions
    # %EDIT%, a title with %ROOT%) is never itself rescanned for placeholders.
    return re.sub(r"%([A-Z_]+)%", lambda m: values.get(m.group(1), m.group(0)), tpl)


FRESH_LABEL = {"fresh": "up to date", "stale": "sources changed", "editing": "being updated",
               "new": "not yet committed", "unknown": "unknown (shallow clone)"}


def fresh_label(r) -> str:
    """The state as shown to a reader; an unknown state says WHY."""
    if r.state == "unknown" and getattr(r, "error", ""):
        return f"unknown: {r.error}"
    return FRESH_LABEL.get(r.state, r.state)


def freshness_table(records) -> str:
    if not records:
        return ""
    stale = sum(1 for r in records if r.state == "stale")
    rows = []
    for r in sorted(records, key=lambda r: (r.state != "stale", r.kind, r.name)):
        name = (f'<a href="{{{{repo_url}}}}/blob/{LINK_REF}/{html.escape(r.name)}">{html.escape(r.name[5:])}</a>'
                if r.kind == "page" else f"Landing page card: {html.escape(r.name)}")
        srcs = ", ".join(f"<code>{html.escape(x)}</code>" for x in r.sources)
        state = html.escape(fresh_label(r))
        if r.state == "stale":
            state = f'<span class="missing">{state}</span>'
        rows.append(f"<tr><td>{name}</td><td>{srcs}</td><td>{html.escape(r.baseline_date or '')}</td><td>{state}</td></tr>")
    return ("<h2 id=\"freshness\">Freshness</h2>"
            "<p>Pages and landing-page cards that name the code they describe (<code>sources=</code>). A page is flagged "
            "when that code has changed since the page's content last changed; update it, or bump its "
            "<code>reviewed=</code> date if it is still accurate.</p>"
            f"<p><strong>{stale}</strong> of <strong>{len(records)}</strong> flagged.</p>"
            "<div class=\"table-wrap\"><table><thead><tr><th>Page</th><th>Sources</th><th>Last updated</th>"
            "<th>State</th></tr></thead><tbody>" + "".join(rows) + "</tbody></table></div>")


def coverage_page(pages: dict[str, Page], covered: dict[str, list[str]], records=()) -> Page:
    by_domain: dict[str, list[str]] = {}
    for t in covered:
        parts = t.split("/")
        domain = parts[1] if len(parts) > 2 else "(tracker)"
        by_domain.setdefault(domain, []).append(t)
    total = len(covered)
    done = sum(1 for d in covered.values() if d)
    rows = []
    for domain in sorted(by_domain):
        ts = by_domain[domain]
        n = sum(1 for t in ts if covered[t])
        rows.append(f"<h2 id=\"{html.escape(domain)}\">{html.escape(domain)} <span class=\"pill\">{n}/{len(ts)}</span></h2><div class=\"table-wrap\"><table><thead><tr><th>TODO</th><th>Documentation</th></tr></thead><tbody>")
        for t in ts:
            docs = covered[t]
            links = ", ".join(f'<a href="{pages[d].url or "./"}">{html.escape(pages[d].title)}</a>' for d in docs) \
                or '<span class="missing">not yet documented</span>'
            rows.append(f'<tr><td><a href="{{{{repo_url}}}}/blob/{LINK_REF}/{t}">{html.escape(todo_title(t))}</a></td><td>{links}</td></tr>')
        rows.append("</tbody></table></div>")
    pct = (100 * done // total) if total else 100
    body = (f"<h1 id=\"documentation-coverage\">Documentation coverage</h1>"
            f"<p>Every roadmap file under <code>todo/</code> should have at least one documentation page that declares "
            f"<code>&lt;!-- docs: covers=todo/... --&gt;</code>. This page is generated on every build.</p>"
            f"<p class=\"meter\"><span style=\"width:{pct}%\"></span></p>"
            f"<p><strong>{done}</strong> of <strong>{total}</strong> roadmap files documented ({pct}%).</p>"
            + freshness_table(records) + "".join(rows))
    p = Page(src=DOCS / "coverage.md", rel="coverage.md", url="coverage.html", title="Documentation coverage",
             order=9999)
    p.body = body
    return p


def hex_to_css(value: str) -> str:
    v = value.lstrip("#")
    if len(v) == 6:
        return "#" + v.lower()
    a, rgb = int(v[:2], 16), v[2:]
    r, g, b = (int(rgb[i:i + 2], 16) for i in (0, 2, 4))
    return f"rgba({r}, {g}, {b}, {a / 255:.3f})"


FEATURES_FILE = "features.json"   # under gh-pages/; rendered, never published as-is
_TODO_GRAPH = None


def todo_graph():
    """The todo-graph producer (scripts/todo-graph/build.py), loaded once by path.
    Its `extract_implementation_order` IS the roadmap's status rule, so a feature
    card's progress can never disagree with the graph the overnight run reads."""
    global _TODO_GRAPH
    if _TODO_GRAPH is None:
        import importlib.util
        sys.path.insert(0, str(REPO / "scripts" / "todo-graph"))
        spec = importlib.util.spec_from_file_location("todo_graph_producer", REPO / "scripts" / "todo-graph" / "build.py")
        _TODO_GRAPH = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(_TODO_GRAPH)
    return _TODO_GRAPH


def roadmap_progress(owners: list[str]) -> tuple[int, int, int]:
    """(done, in_progress, total) Implementation Order rows across `owners`."""
    done = partial = total = 0
    for rel in owners:
        for row in todo_graph().extract_implementation_order((ROOT / rel).read_text(encoding="utf-8")):
            total += 1
            done += row["status"] == "x"
            partial += row["status"] == "/"
    return done, partial, total


def feature_cards(facts: dict, errors: list[str]) -> str:
    """Landing page feature cards from gh-pages/features.json. The text is written
    by hand and must state only what the code does today; the roadmap status line
    under it is COMPUTED from the owning roadmap files, and `sources` feed the doc
    freshness check. A missing owner or source path is a drift error."""
    path = SITE_SRC / FEATURES_FILE
    try:
        cards = json.loads(path.read_text(encoding="utf-8"))["cards"]
    except (OSError, ValueError, KeyError) as e:
        errors.append(f"gh-pages/{FEATURES_FILE}: unreadable ({e})")
        return ""
    known = set(todo_files())
    titles = [c.get("title") for c in cards]
    for t in {t for t in titles if titles.count(t) > 1}:
        errors.append(f"gh-pages/{FEATURES_FILE}: duplicate card title {t!r} (titles identify cards for freshness)")
    out = []
    for card in cards:
        where = f"gh-pages/{FEATURES_FILE}: card {card.get('title', '?')!r}"
        if not isinstance(card.get("title"), str) or not card["title"].strip():
            errors.append(f"{where}: every card needs a non-empty string title")
        owners, sources = card.get("owners") or [], card.get("sources") or []
        if not owners:
            errors.append(f"{where}: names no owning roadmap file")
        for o in owners:
            if o not in known:
                errors.append(f"{where}: owner is not a roadmap file: {o}")
        check_sources(where, [x.rstrip("/") for x in sources], errors)
        if card.get("reviewed") and not valid_reviewed(card["reviewed"]):
            errors.append(f"{where}: reviewed must be YYYY-MM-DD or YYYY-MM-DDTHH:MM, got {card['reviewed']!r}")
        for field in ("title", "text"):
            if any(d in card.get(field, "") for d in (chr(0x2014), chr(0x2013))):
                errors.append(f"{where}: {field} contains an em or en dash")
        good = [o for o in owners if o in known]
        # What is AHEAD, not what is done: much of the foundation predates the
        # roadmap, so a done-count would read "0 of 12" on a card whose features
        # run today. The open-section count is true whatever the history.
        done, _partial, total = roadmap_progress(good) if good else (0, 0, 0)
        left = total - done
        status = "Roadmap complete" if total and not left else f"{left} roadmap section{'s' if left != 1 else ''} to go"
        link = f"{facts['repo_url']}/blob/main/{good[0]}" if good else facts["repo_url"]
        out.append(
            '<div class="feature-card">\n'
            f'    <div class="feature-card-icon">{card.get("icon", "")}</div>\n'
            f'    <h4>{html.escape(card.get("title", ""))}</h4>\n'
            f'    <p>{html.escape(card.get("text", ""))}</p>\n'
            f'    <a class="feature-status" href="{html.escape(link)}" target="_blank" '
            f'rel="noopener">{status}</a>\n'
            '</div>')
    return "\n".join(out)


def design_tokens_css() -> str:
    """CSS custom properties generated from docs/design/tokens.json, so the web
    mockup and the C header (gen_theme_header.py) share one source."""
    t = json.loads((DOCS / "design" / "tokens.json").read_text(encoding="utf-8"))
    out: list[str] = []
    default = t["color"]["default_theme"]
    for theme in sorted(t["color"]["themes"], key=lambda n: n != default):
        sel = f':root, [data-theme="{theme}"]' if theme == default else f'[data-theme="{theme}"]'
        decls = [f"--{k.replace('_', '-')}: {hex_to_css(v)};" for k, v in sorted(t["color"]["themes"][theme].items())]
        for surface, spec in sorted(t["material"][theme].items()):
            tint = spec["tint"].lstrip("#")
            r, g, b = (int(tint[i:i + 2], 16) for i in (0, 2, 4))
            decls.append(f"--mat-{surface}-tint: rgba({r}, {g}, {b}, {spec['tint_opacity'] / 255:.3f});")
            if "blur" in spec:
                # gfx_acrylic runs a 3-pass box blur of this radius (about a 1.7x Gaussian).
                decls.append(f"--mat-{surface}-blur: {round(spec['blur'] * 1.7)}px;")
                decls.append(f"--mat-{surface}-noise: {spec['noise'] / 100:.2f};")
        out.append(f"{sel} {{ " + " ".join(decls) + " }")
    decls = []
    for group, prefix in (("size", "size"), ("radius", "radius"), ("spacing", "space")):
        for k, v in sorted(t[group].items()):
            if not k.startswith("_"):
                decls.append(f"--{prefix}-{k.replace('_', '-')}: {v}px;")
    for k, v in sorted(t["motion"].items()):
        if k.startswith("_"):
            continue
        if isinstance(v, list):
            decls.append(f"--{k.replace('_', '-')}: cubic-bezier({', '.join(str(x) for x in v)});")
        else:
            decls.append(f"--motion-{k.replace('_', '-')}: {v}ms;")
    for name, (y, blur, alpha) in sorted((k, v) for k, v in t["elevation"].items() if not k.startswith("_")):
        decls.append(f"--elev-{name.replace('_', '-')}: 0 {y}px {blur}px rgba(0, 0, 0, {alpha / 255:.3f});")
    decls.append(f"--font-ui: {t['type']['family_ui_web']};")
    out.append(":root { " + " ".join(decls) + " }")
    return "\n".join(out)


NOINDEX_RE = re.compile(r'<meta\s+name="robots"\s+content="[^"]*\bnoindex\b', re.I)


def sitemap(facts: dict, files: dict[str, bytes], sources: dict[str, str], dates: dict[str, str]) -> bytes:
    """sitemap.xml over every published HTML page except those whose head says
    noindex (404.html), derived from the output map so a new page is listed
    without being named anywhere. `lastmod` is the source file's last commit
    date; a generated page (the coverage page) has none."""
    rows = []
    for rel in sorted(files):
        if not rel.endswith(".html") or NOINDEX_RE.search(files[rel].decode("utf-8", "replace")):
            continue
        when = dates.get(sources.get(rel, ""), "")
        rows.append(f"  <url><loc>{html.escape(site_url(facts, rel))}</loc>"
                    + (f"<lastmod>{when}</lastmod>" if when else "") + "</url>\n")
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n'
            + "".join(rows) + "</urlset>\n").encode("utf-8")


ASSET_DIRS = (
    ("resources/icons/src", "icons", (".svg",)),
    ("resources/brand", "brand", (".svg", ".png")),
    ("resources/backgrounds", "wallpapers", (".jpg",)),
)


SEARCH_V2_MARK = 'data-search-index="2"'   # the template reads the sharded v2 index
SEARCH_BUDGET = 2 * 1024 * 1024            # bytes, per file: search.json and every shard
SEARCH_SHARD_TARGET = 512 * 1024           # a shard is cut once it would pass this


def _json_bytes(value) -> bytes:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def search_files(pages: dict[str, Page], v2: bool, errors: list[str]) -> dict[str, bytes]:
    """The client-side search index, as {path under the docs root: bytes}.

    v2 (a template carrying SEARCH_V2_MARK): search.json is a manifest of pages
    and sections, [page index, anchor, heading path], and the full text of each
    section lives in shards named by their content hash. The manifest names
    every shard with its record count, so a manifest and a shard from different
    builds can never be read together: a stale manifest names shards the new
    deploy does not have, and the client refuses a count that does not add up.
    Nothing is dropped to meet the budget; a file over it fails the build.

    Legacy: a release tree's own template may predate v2 and read the flat
    [{t,u,h,x}] index it was published with, so it gets that, unchanged."""
    ordered = sorted(pages.values(), key=lambda p: p.rel)
    if not v2:
        return {"search.json": _json_bytes([{"t": p.title, "u": p.url or "./", "h": [t for _, _, t in p.toc],
                                             "x": p.text[:4000]} for p in ordered])}
    plist: list[list[str]] = []
    sections: list[list] = []
    shards: list[list[str]] = [[]]
    size = 0
    for n, page in enumerate(ordered):
        plist.append([page.title, page.url or "./"])
        for anchor, path, text in page.records:
            sections.append([n, anchor, path])
            cost = len(_json_bytes(text)) + 1
            if shards[-1] and size + cost > SEARCH_SHARD_TARGET:
                shards.append([])
                size = 0
            shards[-1].append(text)
            size += cost
    out: dict[str, bytes] = {}
    names: list[list] = []
    for shard in shards:
        if not shard:
            continue
        data = _json_bytes(shard)
        name = f"search-{hashlib.sha256(data).hexdigest()[:16]}.json"
        out[name] = data
        names.append([name, len(shard)])
    out = {"search.json": _json_bytes({"v": 2, "p": plist, "s": sections, "shards": names}), **out}
    for name, data in out.items():
        if len(data) > SEARCH_BUDGET:
            errors.append(f"search index {name} is {len(data):,} bytes, over the {SEARCH_BUDGET:,}-byte budget "
                          f"(content is never dropped to fit; cut more shards or split the page)")
    return out


def build(facts: dict, errors: list[str]) -> dict[str, bytes]:
    """Return {site-relative path: bytes} for the whole site."""
    files: dict[str, bytes] = {}
    sources: dict[str, str] = {}   # site path -> the tracked file it is rendered from ("" = generated)
    # A release build publishes its docs tree only; the landing pages, assets,
    # sitemap and robots.txt belong to the main site.
    landing = RELEASE is None
    facts = dict(facts, design_tokens_css=design_tokens_css()) if landing else dict(facts)
    # Only when a template uses it: a snapshot of an older tree has no features.json.
    if landing and any("{{feature_cards}}" in f.read_text(encoding="utf-8", errors="replace")
                       for f in SITE_SRC.rglob("*.html")):
        facts["feature_cards"] = feature_cards(facts, errors)
    for src_dir, dest, exts in (ASSET_DIRS if landing else ()):
        base = ROOT / src_dir
        if base.is_dir():
            for f in sorted(base.iterdir()):
                if f.is_file() and f.suffix in exts:
                    files[f"{dest}/{f.name}"] = f.read_bytes()
    # 1. Landing site templates.
    for src in (sorted(SITE_SRC.rglob("*")) if landing else ()):
        if src.is_dir():
            continue
        rel = src.relative_to(SITE_SRC).as_posix()
        data = src.read_bytes()
        if src.suffix in TEMPLATE_EXTS and rel not in ("docs-template.html",):
            data = render_template(data.decode("utf-8"), facts, f"gh-pages/{rel}", errors).encode("utf-8")
        if rel not in ("docs-template.html", FEATURES_FILE):
            files[rel] = data
            sources[rel] = f"gh-pages/{rel}"
    # 2. Docs.
    pages, _ = load_pages(facts, errors)
    covered, undocumented = coverage(pages, errors)
    if landing:
        # Roadmap and sources= hygiene judge the CURRENT commit; a release is
        # published as it was, so only rendering errors (dead links, images,
        # anchors, unknown covers=) fail a release build.
        check_design_lines(pages, errors)
        for page in pages.values():
            check_sources(f"docs/{page.rel}", page.sources, errors)
    import freshness
    freshness.REPO = REPO
    page_sources = {f"docs/{p.rel}": p.sources for p in pages.values() if p.sources}
    if SOURCE in ("worktree", "index") and freshness.merging():
        page_sources = freshness.with_head_sources(
            page_sources, [f"docs/{p.rel}" for p in pages.values()], directive_sources, SOURCE)
    records = freshness.check_all(page_sources, SOURCE)
    build.freshness = records  # type: ignore[attr-defined]
    cov = coverage_page(pages, covered, records)
    tpl = render_template((SITE_SRC / "docs-template.html").read_text(encoding="utf-8"), facts,
                          "gh-pages/docs-template.html", errors)
    all_pages = dict(pages)
    all_pages[cov.rel] = cov
    dates = last_updated()
    build.shallow = dates is None  # type: ignore[attr-defined]
    dates = dates or {}
    for page in all_pages.values():
        out = out_path(page.url)
        updated = "" if page is cov else dates.get(f"docs/{page.rel}", "")
        rendered = render_page(tpl, facts, all_pages, page, updated)
        rendered = render_template(rendered, facts, f"docs/{page.rel}", []) if page is cov else rendered
        files[f"{DOCS_BASE}{out}"] = rendered.encode("utf-8")
        sources[f"{DOCS_BASE}{out}"] = "" if page is cov else f"docs/{page.rel}"
    if landing:
        for name in ("sitemap.xml", "robots.txt"):
            if name in files:
                errors.append(f"gh-pages/{name}: generated by build.py; remove the hand-written copy")
        files["sitemap.xml"] = sitemap(facts, files, sources, dates)
        files["robots.txt"] = (f"User-agent: *\nAllow: /\n\nSitemap: {site_url(facts, 'sitemap.xml')}\n").encode("utf-8")
    if landing:
        # Page hygiene of the CURRENT tree, like check_design_lines: a release is
        # published as it was.
        for page in all_pages.values():
            errors.extend(f"docs/{page.rel}: accessibility: {f}" for f in page.a11y)
    for name, data in search_files(pages, SEARCH_V2_MARK in tpl, errors).items():
        files[f"{DOCS_BASE}{name}"] = data
    build.undocumented = undocumented  # type: ignore[attr-defined]
    return files


# --------------------------------------------------------------------------
# Drift checks outside the site
# --------------------------------------------------------------------------

def check_regions(facts: dict, errors: list[str], write: bool, only: list[str] | None = None,
                  skip_stats: bool = False) -> None:
    for path in (tracked_files(*only) if only else tracked_files("*.md")):
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8")
        if "<!-- project:" not in text:
            continue
        rel = path.relative_to(ROOT).as_posix()
        new = sync_regions(text, facts, rel, errors)
        if skip_stats:
            new = merge_stats(text, new)
        if new != text:
            if write:
                path.write_text(new, encoding="utf-8")
                print(f"synced {rel}")
            else:
                errors.append(f"{rel}: project region out of date (run: python3 scripts/site/build.py --sync)")


def merge_stats(old: str, new: str) -> str:
    """Put the ORIGINAL stat_* region values back into ``new`` (used by --skip-stats)."""
    olds = [m.group(0) for m in REGION_RE.finditer(old) if m.group(1).startswith("stat_")]
    it = iter(olds)
    return REGION_RE.sub(lambda m: next(it) if m.group(1).startswith("stat_") else m.group(0), new)


def check_owner_urls(facts: dict, errors: list[str]) -> None:
    allowed_files = set(facts["_historical_owner_files"])
    where = ["--cached"] if SOURCE == "index" else ([SOURCE] if SOURCE != "worktree" else [])
    proc = subprocess.run(["git", "grep", "-nIE", *where, "-e", r"github\.com/[A-Za-z0-9-]+/impossible-os\b", "--",
                           ":!src/libs", ":!src/kernel/acpica"],
                          cwd=REPO, capture_output=True, text=True)
    if proc.returncode not in (0, 1):  # 1 = no match; anything else is a failed search, not a clean one
        errors.append(f"owner URL scan failed (git grep rc={proc.returncode}): {proc.stderr.strip()}")
        return
    out = proc.stdout
    for line in out.splitlines():
        if SOURCE not in ("worktree", "index"):
            line = line.split(":", 1)[1]  # drop the "<ref>:" prefix
        rel, lineno, content = line.split(":", 2)
        if rel in allowed_files:
            continue
        for m in REPO_URL_RE.finditer(content):
            if m.group(1).lower() != facts["owner"].lower():
                errors.append(f"{rel}:{lineno}: repository URL names owner '{m.group(1)}', "
                              f"project.json says '{facts['owner']}'")


DONATE_RE = re.compile(r"paypal\.com/donate/?\?hosted_button_id=([A-Za-z0-9]+)")


def check_donate_links(facts: dict, errors: list[str]) -> None:
    """Every PayPal donate link in the tree must be project.json's `donate_url`:
    templates use {{donate_url}}, and a hand-typed link (README, docs) that
    names another button fails here, so an old donation link cannot survive."""
    want = DONATE_RE.search(facts.get("donate_url", ""))
    if not want:
        errors.append("project.json: donate_url is missing or is not a PayPal donate link")
        return
    where = ["--cached"] if SOURCE == "index" else ([SOURCE] if SOURCE != "worktree" else [])
    proc = subprocess.run(["git", "grep", "-nIE", *where, "-e", DONATE_RE.pattern, "--", ":!src/libs"],
                          cwd=REPO, capture_output=True, text=True)
    if proc.returncode not in (0, 1):
        errors.append(f"donate link scan failed (git grep rc={proc.returncode}): {proc.stderr.strip()}")
        return
    for line in proc.stdout.splitlines():
        if SOURCE not in ("worktree", "index"):
            line = line.split(":", 1)[1]
        rel, lineno, content = line.split(":", 2)
        for m in DONATE_RE.finditer(content):
            if m.group(1) != want.group(1):
                errors.append(f"{rel}:{lineno}: PayPal donate link uses button {m.group(1)}, "
                              f"project.json donate_url uses {want.group(1)}")


ICON_SIZES = (16, 24, 32, 48, 64, 72, 96, 128, 256)  # must match SIZES in scripts/convert-icons.sh


def check_icon_renders(errors: list[str]) -> None:
    """The committed icon PNGs must come from the current SVG sources."""
    import hashlib
    src, stamp = ROOT / "resources" / "icons" / "src", ROOT / "resources" / "icons" / "color" / "SOURCES.sha256"
    if not src.is_dir():
        return
    color = stamp.parent
    # The stamp binds sources AND outputs ("<sha256>  src/<name>.svg" per source,
    # "<sha256>  <size>/<name>.png" per render); both sides are re-hashed from the
    # snapshot, so sources committed without their PNGs, or a missing or
    # hand-edited PNG, fail.
    have: dict[str, str] = {}
    if stamp.exists():
        for line in stamp.read_text(encoding="utf-8").splitlines():
            h, _, name = line.partition("  ")
            have[name.strip()] = h
    want = {f"src/{f.name}": hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(src.glob("*.svg"))}
    for f in sorted(src.glob("*.svg")):
        for size in ICON_SIZES:
            png = color / str(size) / f"{f.stem}.png"
            want[f"{size}/{f.stem}.png"] = hashlib.sha256(png.read_bytes()).hexdigest() if png.exists() else "missing"
    if want != have:
        changed = sorted(n for n in set(want) | set(have) if want.get(n) != have.get(n))
        errors.append(f"resources/icons: rendered PNGs do not match their sources ({', '.join(changed[:6])}"
                      f"{' ...' if len(changed) > 6 else ''}); run: bash scripts/convert-icons.sh and commit the PNGs")


INLINE_SCRIPT_RE = re.compile(r"<script(?![^>]*\b(?:src|type)=)[^>]*>(.*?)</script>", re.S | re.I)
NODE_SYNTAX_CHECK = r"""
const vm = require("vm");
let bad = 0;
for (const [name, src] of JSON.parse(require("fs").readFileSync(0, "utf8"))) {
  try { new vm.Script(src, { filename: name }); }
  catch (e) { bad++; console.log(name + ": " + String(e.message).split("\n")[0]); }
}
process.exit(bad ? 1 : 0);
"""


def check_scripts(files: dict[str, bytes], errors: list[str]) -> None:
    """Every classic script the site ships must PARSE. One syntax error stops the whole
    block, so a stray brace on the landing page froze the countdown and left every
    scroll-revealed section invisible, while every link and fact check stayed green.
    Needs `node` (present locally and on the Actions runner); skipped when absent."""
    node = shutil.which("node")
    if not node:
        return
    seen: dict[str, str] = {}
    for rel, data in sorted(files.items()):
        if rel.endswith(".js"):
            seen.setdefault(data.decode("utf-8", "replace"), rel)
        elif rel.endswith(".html"):
            for i, m in enumerate(INLINE_SCRIPT_RE.finditer(data.decode("utf-8", "replace"))):
                seen.setdefault(m.group(1), f"{rel} (inline script {i + 1})")
    batch = json.dumps([[name, src] for src, name in seen.items()])
    r = subprocess.run([node, "-e", NODE_SYNTAX_CHECK], input=batch, capture_output=True, text=True)
    if r.returncode not in (0, 1):
        errors.append(f"script syntax check could not run: {r.stderr.strip()[:200]}")
    for line in r.stdout.splitlines():
        errors.append(f"JavaScript syntax error in {line}")


COUNT_TOTAL_RE = re.compile(r"\*\*All lines in tree\*\*\s*\|\s*\*\*\d+\*\*\s*\|\s*\*\*(\d+)\*\*")


def check_count_badge(errors: list[str]) -> None:
    count = (ROOT / "COUNT.md").read_text(encoding="utf-8")
    m = COUNT_TOTAL_RE.search(count)
    b = re.search(r"img\.shields\.io/badge/lines-([0-9]+(?:%2C[0-9]{3})*)-", (ROOT / "README.md").read_text(encoding="utf-8"))
    if not m or not b:
        errors.append("README/COUNT.md: line-count badge or COUNT.md total not found")
        return
    shown = int(b.group(1).replace("%2C", ""))      # the exact count, comma-grouped
    if shown != int(m.group(1)):
        errors.append(f"README.md: line-count badge says {shown:,}, COUNT.md says {int(m.group(1)):,} "
                      f"(run: COUNT_ONLY=1 bash .githooks/post-commit)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--sync", nargs="*", metavar="FILE",
                      help="rewrite project regions (all tracked Markdown, or only FILEs)")
    mode.add_argument("--update-baseline", action="store_true")
    mode.add_argument("--freshness", action="store_true",
                      help="list every page and card with sources= and whether its code changed since")
    mode.add_argument("--emit-head", metavar="FILE",
                      help="post-commit: print FILE as committed at HEAD with its project regions synced")
    mode.add_argument("--sync-head", nargs="+", metavar="FILE",
                      help="post-commit: sync FILEs' regions from the HEAD snapshot, only when the worktree copy is clean")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--ref", default="HEAD", help="commit that --emit-head / --sync-head read (default HEAD)")
    ap.add_argument("--staged", action="store_true",
                    help="read every input from the INDEX (pre-commit), not the working tree")
    ap.add_argument("--skip-stats", action="store_true",
                    help="do not compare stat_* regions (pre-commit; post-commit re-syncs them)")
    ap.add_argument("--release", metavar="REF",
                    help="render only the docs tree as it was at commit REF into <out>/docs/<REF>/, "
                         "every repository link pinned to REF's full SHA")
    args = ap.parse_args()
    if args.release is not None and (args.staged or args.sync is not None or args.sync_head or args.emit_head
                                     or args.update_baseline or args.freshness or args.skip_stats):
        ap.error("--release combines only with --check, --out and --quiet")
    args.out = args.out.resolve()
    snap = None
    try:
        if args.release is not None:
            try:
                snap = prepare_release(args.release)
            except ReleaseError as e:
                print(f"ERROR: {e}", file=sys.stderr)
                return 1
        elif args.sync_head or args.emit_head:
            snap = snapshot(args.ref)
        elif args.staged:
            snap = snapshot("index")
        return run(args)
    finally:
        if snap is not None:
            shutil.rmtree(snap, ignore_errors=True)


class ReleaseError(Exception):
    pass


RELEASE_FACTS: dict = {}     # publishing facts of the CURRENT tree, applied over a release's own


def prepare_release(ref: str) -> Path | None:
    """Resolve REF, materialise its tree and switch rendering to release mode.

    The version path is REF itself, so every /-separated part must be a plain
    name (no `..`, no leading dash, nothing git would read as revision syntax).
    Links pin to the full SHA rather than the tag, because a tag can be moved and
    a published page must keep naming the commit it was built from. Publishing
    facts (site, repository, owner) come from the current tree: the repository has
    changed owners before, and the release is served from today's site."""
    global LINK_REF, DOCS_BASE, RELEASE, RELEASE_FACTS
    parts = ref.split("/")
    if not all(VERSION_SEGMENT_RE.fullmatch(p) for p in parts):
        raise ReleaseError(f"release ref {ref!r}: each /-separated part must match "
                           f"{VERSION_SEGMENT_RE.pattern} (it becomes the docs/<version>/ path)")
    if parts[0] in ("main", "refs"):
        # A link naming blob/main/v1/x could mean branch main or tag main/v1;
        # refusing the name keeps every pinned link unambiguous.
        raise ReleaseError(f"release ref {ref!r}: a version may not start with main/ or refs/ "
                           f"(it would be ambiguous with the branch in repository links)")
    r = subprocess.run(["git", "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}"], cwd=REPO,
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise ReleaseError(f"release ref {ref!r} does not name a commit")
    sha = r.stdout.strip()
    current = json.loads((REPO / "project.json").read_text(encoding="utf-8"))
    snap = snapshot(sha)
    missing = [p for p in ("project.json", "gh-pages/docs-template.html", "COUNT.md",
                           "docs/test-coverage/coverage.json") if not (ROOT / p).is_file()]
    if missing or not DOCS.is_dir():
        shutil.rmtree(snap, ignore_errors=True)
        raise ReleaseError(f"release ref {ref!r} ({sha[:12]}) predates the docs site: no "
                           + ", ".join(missing or ["docs/"]))
    LINK_REF, DOCS_BASE, RELEASE = sha, f"docs/{ref}/", ref
    RELEASE_FACTS = {k: current[k] for k in PUBLISH_KEYS if isinstance(current.get(k), str)}
    RELEASE_FACTS["_historical_owners"] = list(current.get("historical_owners", []))
    return snap


def sync_head(files: list[str], facts: dict, errors: list[str]) -> int:
    """Rewrite regions computed from the COMMITTED tree. A file whose working copy
    differs from HEAD is left alone (warned), so the post-commit amend can never
    carry uncommitted edits or facts derived from uncommitted inputs."""
    for rel in files:
        head_path, wt_path = ROOT / rel, REPO / rel
        if not head_path.exists():
            continue
        head_text = head_path.read_text(encoding="utf-8")
        new = sync_regions(head_text, facts, rel, errors)
        if new == head_text:
            continue
        if not wt_path.exists() or wt_path.read_text(encoding="utf-8") != head_text:
            print(f"site: WARNING: {rel} has uncommitted edits; project regions not synced "
                  f"(run: python3 scripts/site/build.py --sync {rel})", file=sys.stderr)
            continue
        wt_path.write_text(new, encoding="utf-8")
        print(f"synced {rel}")
    for e in errors:
        print(f"ERROR: {e}", file=sys.stderr)
    return 1 if errors else 0


def run(args: argparse.Namespace) -> int:
    facts = load_project()
    errors: list[str] = []
    if RELEASE is not None:
        owners = RELEASE_FACTS["_historical_owners"] + facts["_historical_owners"]
        facts.update(RELEASE_FACTS)
        facts["_historical_owners"] = sorted(set(owners))

    if args.sync_head:
        return sync_head(args.sync_head, facts, errors)
    if args.emit_head:
        text = (ROOT / args.emit_head).read_text(encoding="utf-8")
        out = sync_regions(text, facts, args.emit_head, errors)
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        sys.stdout.write(out)
        return 1 if errors else 0

    if args.sync is not None:
        check_regions(facts, errors, write=True, only=args.sync or None)
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1 if errors else 0

    files = build(facts, errors)
    undocumented = build.undocumented  # type: ignore[attr-defined]
    # A shallow clone cannot date pages. A check may still run there (warned), but
    # a written site would publish different bytes for the same commit, which
    # verify_live.py would then report as drift, so writing one is refused.
    shallow = build.shallow  # type: ignore[attr-defined]
    if shallow and not (args.check or args.freshness or args.update_baseline):
        errors.append("shallow clone: page dates need full history (fetch-depth: 0); refusing to write the site")

    import freshness
    records = build.freshness  # type: ignore[attr-defined]
    if args.freshness:
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        for r in records:
            extra = f" ({', '.join(r.changed[:5])})" if r.changed else f" ({r.error})" if r.error else ""
            print(f"{r.state:8} {r.baseline_date or '-':10} {r.kind:4} {r.name}{extra}")
        stale = sum(1 for r in records if r.state == "stale")
        unknown = sum(1 for r in records if r.state == "unknown")
        print(f"freshness: {stale} of {len(records)} stale" + (f", {unknown} unknown" if unknown else ""),
              file=sys.stderr)
        return 1 if errors else 0

    if args.update_baseline:
        data = json.loads(BASELINE.read_text(encoding="utf-8")) if BASELINE.exists() else None
        BASELINE.write_text(json.dumps(shrink_baseline(data, undocumented), indent=2) + "\n", encoding="utf-8")
        print(f"baseline: {len(json.loads(BASELINE.read_text(encoding='utf-8'))['undocumented'])} undocumented TODO file(s)")
        return 0

    check_scripts(files, errors)
    if RELEASE is None:
        # Repository hygiene of the CURRENT commit; a release is published as it was.
        check_regions(facts, errors, write=False, skip_stats=args.skip_stats)
        check_owner_urls(facts, errors)
        check_donate_links(facts, errors)
        check_count_badge(errors)
        check_icon_renders(errors)
        check_baseline(undocumented, errors)
        sys.path.insert(0, str(REPO / "scripts" / "site"))
        import gen_theme_header  # noqa: E402  (sibling module; set_root() repoints it at a snapshot)
        errors.extend(gen_theme_header.check())
        import repo_meta  # noqa: E402  (offline half; the live GitHub comparison is repo-metadata.yml)
        errors.extend(repo_meta.validate(json.loads(PROJECT_FILE.read_text(encoding="utf-8"))))

    if errors:
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        print(f"site: {len(errors)} drift error(s)", file=sys.stderr)
        return 1

    # Warnings, never errors: printed even under --quiet so lint Check 30 can
    # forward them (a code change is not always a docs change).
    for r in records:
        w = freshness.warning(r)
        if w:
            print(f"WARN: {w}", file=sys.stderr)
    if shallow:
        print("WARN: shallow clone: docs pages carry no last-updated dates and sitemap.xml no lastmod",
              file=sys.stderr)

    if not args.check:
        if args.out.exists():
            shutil.rmtree(args.out)
        for rel, data in files.items():
            dest = args.out / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
    if not args.quiet:
        docs = sum(1 for k in files if k.startswith("docs/") and k.endswith(".html"))
        total = len(todo_files())
        print(f"site: OK ({len(files)} files, {docs} doc pages, "
              f"{total - len(undocumented)}/{total} TODO files documented)"
              + (f" [release {RELEASE} @ {LINK_REF[:12]}]" if RELEASE is not None else "")
              + ("" if args.check else f" -> {args.out}"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
