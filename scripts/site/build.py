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

Stdlib plus the vendored markdown-it-py under tools/vendor/, so the output is
identical on every host and in CI. Deterministic: stable ordering, no
timestamps.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import html
import json
import os
import posixpath
import re
import shutil
import subprocess
import sys
import tempfile
from urllib.parse import unquote
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
    global ROOT, PROJECT_FILE, SITE_SRC, DOCS, TODO, BASELINE
    ROOT = root
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
        # The same whole-tree total, floored to thousands, that the README badge shows.
        "stat_lines": f"{lines // 1000 * 1000:,}+",
        "stat_lines_k": f"{lines // 1000:,}K+",
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
    s = text.strip().lower()
    s = re.sub(r"[^\w\- ]", "", s, flags=re.UNICODE)
    return s.replace(" ", "-")


@dataclass
class Page:
    src: Path                       # docs/... .md
    rel: str                        # path relative to docs/, posix
    url: str                        # site path relative to /docs/
    title: str = ""
    order: int = 1000
    covers: list[str] = field(default_factory=list)
    body: str = ""
    toc: list[tuple[int, str, str]] = field(default_factory=list)
    anchors: set[str] = field(default_factory=set)
    text: str = ""


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


class Renderer:
    def __init__(self, facts: dict, pages: dict[str, Page], errors: list[str]):
        self.facts = facts
        self.pages = pages          # rel -> Page
        self.errors = errors
        self.md = make_md()
        self.pending_anchor_checks: list[tuple[str, str, str]] = []

    def blob_url(self, repo_rel: str) -> str:
        return f"{self.facts['repo_url']}/blob/main/{repo_rel}"

    def rewrite_href(self, page: Page, href: str) -> str:
        if href.startswith("#") and len(href) > 1:
            self.pending_anchor_checks.append((page.rel, page.rel, href[1:]))
            return href
        if not href or href.startswith(("#", "mailto:")) or re.match(r"^[a-z][a-z0-9+.-]*:", href):
            return href
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
                return f"{self.facts['repo_url']}/tree/main/{repo_rel}"
        if repo_rel.startswith("docs/") and repo_rel.endswith(".md"):
            doc_rel = repo_rel[len("docs/"):]
            if frag:
                self.pending_anchor_checks.append((page.rel, doc_rel, frag))
            here_dir = posixpath.dirname(out_path(page.url)) or "."
            rel = posixpath.relpath(out_path(page_url(doc_rel)), here_dir)
            if rel == "index.html" or rel.endswith("/index.html"):
                rel = rel[: -len("index.html")] or "./"
            return rel + (f"#{frag}" if frag else "")
        return self.blob_url(repo_rel) + (f"#{frag}" if frag else "")

    def render(self, page: Page, text: str) -> None:
        env: dict = {}
        tokens = self.md.parse(text, env)
        used: dict[str, int] = {}
        plain: list[str] = []
        for i, tok in enumerate(tokens):
            if tok.type == "heading_open":
                inline = tokens[i + 1]
                title = "".join(c.content for c in (inline.children or []) if c.type in ("text", "code_inline"))
                slug = github_slug(title)
                n = used.get(slug, 0)
                used[slug] = n + 1
                if n:
                    slug = f"{slug}-{n}"
                tok.attrSet("id", slug)
                page.anchors.add(slug)
                level = int(tok.tag[1])
                if level == 1 and not page.title:
                    page.title = title.strip()
                if level in (2, 3):
                    page.toc.append((level, slug, title.strip()))
            if tok.type == "inline":
                for child in tok.children or []:
                    if child.type == "link_open":
                        child.attrSet("href", self.rewrite_href(page, child.attrGet("href") or ""))
                    elif child.type == "image":
                        child.attrSet("src", self.rewrite_img(page, child.attrGet("src") or ""))
                    elif child.type == "text":
                        plain.append(child.content)
            if tok.type == "html_block":
                tok.content = self.rewrite_raw_html(page, tok.content)
            if tok.type == "inline":
                for child in tok.children or []:
                    if child.type == "html_inline":
                        child.content = self.rewrite_raw_html(page, child.content)
            if tok.type == "fence" and tok.info.strip() == "mermaid":
                tok.type = "html_block"
                tok.content = f'<pre class="mermaid">{html.escape(tok.content)}</pre>\n'
        # Explicit <a id="..."> / <a name="..."> anchors authored as raw HTML.
        for m in re.finditer(r'<a\s+(?:id|name)="([^"]+)"', text):
            page.anchors.add(m.group(1))
        body = self.md.renderer.render(tokens, self.md.options, env)
        body = ALERT_RE.sub(lambda m: f'<blockquote class="alert alert-{m.group(1).lower()}"><p class="alert-title">{m.group(1).title()}</p><p>', body)
        body = re.sub(r"<li>\[ \]", '<li class="task"><input type="checkbox" disabled>', body)
        body = re.sub(r"<li>\[[xX]\]", '<li class="task"><input type="checkbox" checked disabled>', body)
        body = re.sub(r"<table>", '<div class="table-wrap"><table>', body)
        body = re.sub(r"</table>", "</table></div>", body)
        page.body = body
        page.text = " ".join(plain)
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
        Values are entity-decoded before use and escaped exactly once on output."""
        def attr_sub(m: re.Match) -> str:
            name, value = m.group(2), m.group(4)
            if value is None or name.lower() not in ("href", "src"):
                return m.group(0)
            raw = value[1:-1] if value[:1] in ("\"", "\'") else value
            url = html.unescape(raw)
            new = self.rewrite_href(page, url) if name.lower() == "href" else self.rewrite_img(page, url)
            return f'{m.group(1)}{name}="{html.escape(new, quote=True)}"'

        def tag_sub(m: re.Match) -> str:
            if m.group(0).startswith("<!--"):
                return m.group(0)
            return "<" + m.group(1) + self.ATTR_RE.sub(attr_sub, m.group(2)) + m.group(3)
        return self.TAG_RE.sub(tag_sub, html_text)

    def rewrite_img(self, page: Page, src: str) -> str:
        if not src or re.match(r"^[a-z][a-z0-9+.-]*:", src):
            return src
        target = Path(os.path.normpath(page.src.parent / unquote(src)))
        try:
            repo_rel = target.relative_to(ROOT).as_posix()
        except ValueError:
            repo_rel = ""
        if repo_rel not in fileset()[0]:
            self.errors.append(f"docs/{page.rel}: missing image (not a tracked file): {src}")
            return src
        return f"https://raw.githubusercontent.com/{self.facts['owner']}/{self.facts['repo']}/main/{repo_rel}"

    def check_anchors(self) -> None:
        for src_rel, doc_rel, frag in self.pending_anchor_checks:
            target = self.pages.get(doc_rel)
            if target is not None and unquote(frag) not in target.anchors:
                self.errors.append(f"docs/{src_rel}: dead anchor: {doc_rel}#{frag}")


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

def load_pages(facts: dict, errors: list[str]) -> tuple[dict[str, Page], Renderer]:
    pages: dict[str, Page] = {}
    for src in sorted(DOCS.rglob("*.md")):
        rel = src.relative_to(DOCS).as_posix()
        pages[rel] = Page(src=src, rel=rel, url=page_url(rel))
    r = Renderer(facts, pages, errors)
    for page in pages.values():
        text = page.src.read_text(encoding="utf-8")
        d = parse_directives(text)
        page.covers = [c.strip() for c in d.get("covers", "").split(",") if c.strip()]
        if "order" in d:
            page.order = int(d["order"])
        if "title" in d:
            page.title = d["title"]
        r.render(page, text)
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


def render_page(tpl: str, facts: dict, pages: dict[str, Page], page: Page) -> str:
    root = depth_prefix(page.url)
    edit = f"{facts['repo_url']}/blob/main/docs/{page.rel}"
    return (tpl.replace("%TITLE%", html.escape(page.title))
               .replace("%ROOT%", root)
               .replace("%SITE_ROOT%", root + "../")
               .replace("%NAV%", nav_html(pages, page, root))
               .replace("%TOC%", toc_html(page))
               .replace("%BODY%", page.body)
               .replace("%EDIT%", edit))


def coverage_page(pages: dict[str, Page], covered: dict[str, list[str]]) -> Page:
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
            rows.append(f'<tr><td><a href="{{{{repo_url}}}}/blob/main/{t}">{html.escape(todo_title(t))}</a></td><td>{links}</td></tr>')
        rows.append("</tbody></table></div>")
    pct = (100 * done // total) if total else 100
    body = (f"<h1 id=\"documentation-coverage\">Documentation coverage</h1>"
            f"<p>Every roadmap file under <code>todo/</code> should have at least one documentation page that declares "
            f"<code>&lt;!-- docs: covers=todo/... --&gt;</code>. This page is generated on every build.</p>"
            f"<p class=\"meter\"><span style=\"width:{pct}%\"></span></p>"
            f"<p><strong>{done}</strong> of <strong>{total}</strong> roadmap files documented ({pct}%).</p>" + "".join(rows))
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


ASSET_DIRS = (
    ("resources/icons/src", "icons", (".svg",)),
    ("resources/brand", "brand", (".svg", ".png")),
    ("resources/backgrounds", "wallpapers", (".jpg",)),
)


def build(facts: dict, errors: list[str]) -> dict[str, bytes]:
    """Return {site-relative path: bytes} for the whole site."""
    files: dict[str, bytes] = {}
    facts = dict(facts, design_tokens_css=design_tokens_css())
    for src_dir, dest, exts in ASSET_DIRS:
        base = ROOT / src_dir
        if base.is_dir():
            for f in sorted(base.iterdir()):
                if f.is_file() and f.suffix in exts:
                    files[f"{dest}/{f.name}"] = f.read_bytes()
    # 1. Landing site templates.
    for src in sorted(SITE_SRC.rglob("*")):
        if src.is_dir():
            continue
        rel = src.relative_to(SITE_SRC).as_posix()
        data = src.read_bytes()
        if src.suffix in TEMPLATE_EXTS and rel not in ("docs-template.html",):
            data = render_template(data.decode("utf-8"), facts, f"gh-pages/{rel}", errors).encode("utf-8")
        if rel != "docs-template.html":
            files[rel] = data
    # 2. Docs.
    pages, _ = load_pages(facts, errors)
    check_design_lines(pages, errors)
    covered, undocumented = coverage(pages, errors)
    cov = coverage_page(pages, covered)
    tpl = render_template((SITE_SRC / "docs-template.html").read_text(encoding="utf-8"), facts,
                          "gh-pages/docs-template.html", errors)
    all_pages = dict(pages)
    all_pages[cov.rel] = cov
    for page in all_pages.values():
        out = out_path(page.url)
        rendered = render_page(tpl, facts, all_pages, page)
        rendered = render_template(rendered, facts, f"docs/{page.rel}", []) if page is cov else rendered
        files[f"docs/{out}"] = rendered.encode("utf-8")
    index = [{"t": p.title, "u": p.url or "./", "h": [t for _, _, t in p.toc], "x": p.text[:4000]}
             for p in sorted(pages.values(), key=lambda p: p.rel)]
    files["docs/search.json"] = json.dumps(index, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
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
    b = re.search(r"img\.shields\.io/badge/lines-(\d+)k-", (ROOT / "README.md").read_text(encoding="utf-8"))
    if not m or not b:
        errors.append("README/COUNT.md: line-count badge or COUNT.md total not found")
        return
    want = int(m.group(1)) // 1000  # post-commit writes the floor
    if int(b.group(1)) != want:
        errors.append(f"README.md: line-count badge says {b.group(1)}k, COUNT.md says {want}k "
                      f"(run: COUNT_ONLY=1 bash .githooks/post-commit)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--sync", nargs="*", metavar="FILE",
                      help="rewrite project regions (all tracked Markdown, or only FILEs)")
    mode.add_argument("--update-baseline", action="store_true")
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
    args = ap.parse_args()
    args.out = args.out.resolve()
    snap = None
    try:
        if args.sync_head or args.emit_head:
            snap = snapshot(args.ref)
        elif args.staged:
            snap = snapshot("index")
        return run(args)
    finally:
        if snap is not None:
            shutil.rmtree(snap, ignore_errors=True)


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

    if args.update_baseline:
        data = json.loads(BASELINE.read_text(encoding="utf-8")) if BASELINE.exists() else None
        BASELINE.write_text(json.dumps(shrink_baseline(data, undocumented), indent=2) + "\n", encoding="utf-8")
        print(f"baseline: {len(json.loads(BASELINE.read_text(encoding='utf-8'))['undocumented'])} undocumented TODO file(s)")
        return 0

    check_regions(facts, errors, write=False, skip_stats=args.skip_stats)
    check_owner_urls(facts, errors)
    check_count_badge(errors)
    check_icon_renders(errors)
    check_scripts(files, errors)
    check_baseline(undocumented, errors)
    sys.path.insert(0, str(REPO / "scripts" / "site"))
    import gen_theme_header  # noqa: E402  (sibling module; set_root() repoints it at a snapshot)
    errors.extend(gen_theme_header.check())

    if errors:
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        print(f"site: {len(errors)} drift error(s)", file=sys.stderr)
        return 1

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
              + ("" if args.check else f" -> {args.out}"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
