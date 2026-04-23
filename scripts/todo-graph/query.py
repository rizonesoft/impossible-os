#!/usr/bin/env python3
# ============================================================================
# scripts/todo-graph/query.py -- TODO graph query CLI (TODO-06 §4).
#
# Consumes build/todo-cache.json produced by build.py. Ten human-facing
# subcommands that answer "what can I work on?", "what references this?",
# and "what is going stale?" without hand-grepping.
#
# Stdlib-only. inotifywait is probed via shutil.which() and used as a
# subprocess when present; otherwise --watch falls back to 2s mtime polling.
#
# Subcommands:
#   ready        -- draft TODOs whose deps are all done
#   blocked      -- active TODOs with at least one unmet dep
#   blocking     -- TODOs ranked by inbound depends_on count (critical path)
#   by-domain    -- all TODOs grouped by domain + first unfinished section
#   backlinks    -- every TODO that references <id> (any edge kind)
#   deferred     -- outbound Accepted/Deferred stamps from <id>
#   deferred-by  -- inbound Accepted/Deferred stamps pointing at <id>
#   orphans      -- TODOs with zero inbound edges of any kind
#   stale        -- TODOs whose last_active_at is older than N days (default 90)
#   stats        -- repo-wide summary (total, by-status, by-domain, top-N)
#   code         -- source paths claimed by <id>'s file_patterns + Notes grep
#   code-by      -- reverse: TODOs whose file_patterns match <path>
#
# Flags:
#   --json               structured output (list of objects; dict for stats)
#   --format markdown    copy-pasteable GFM table (stats emits nested sections)
#   --watch              re-run on todo/*.md change (inotifywait or polling)
#   --days N             stale threshold (default 90; only for `stale`)
#   --cache PATH         override cache path (for tests)
#   --repo-root PATH     override repo root (for tests)
#   --quiet              suppress [query.py] prefix + pre-migration notice
#
# Pre-migration mode: until §5 ships, the 223 TODO cache nodes carry
# id=null, status="no-frontmatter". Status-gated subcommands (ready /
# blocked / blocking) print a one-shot pre-migration notice on stderr
# and emit empty output. Subcommands keyed on file_path / filename-stem /
# slug (backlinks / orphans / stale / by-domain / deferred / code /
# code-by) work today.
#
# Owner: TODO-06 §4 in todo/00-infrastructure/TODO-06-todo-metadata-layer.md.
# Reuses cache loader + id/path index + XREF resolver from validate.py.
# ============================================================================

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

# Import reused helpers from the §3 validator. validate.py is stdlib-only
# and has no module-level side effects (argparse + cache load live inside
# functions); import is safe. Test 9q defends against future drift.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
from validate import (  # noqa: E402
    load_or_rebuild_cache,
    build_id_index,
    build_path_index,
    resolve_xref_target,
)


# ----------------------------------------------------------------------
# Constants
# ----------------------------------------------------------------------

DEFAULT_STALE_DAYS = 90
POLL_INTERVAL_SECONDS = 2.0

# Match `(src/...)` or `(include/...)` path references inside Notes blocks.
# Path body accepts a broad file-path charset without the trailing close-paren.
# Literal placeholder forms like `(src/...)` are filtered post-match via
# `..` containment -- a real file path never has three consecutive dots and
# rarely has `..` outside a relative-path segment that would also not appear
# inside parenthetical prose.
NOTES_PATH_RE = re.compile(r"\((?P<path>(?:src|include)/[A-Za-z0-9_./-]+)\)")

# Filename-stem to slug: drop the leading `TODO-NN-` prefix.
FILENAME_SLUG_RE = re.compile(r"^TODO-\d{1,2}-(?P<slug>.+)$")

# Edge kinds used in the inbound/backlink index.
EDGE_DEPENDS_ON = "depends_on"
EDGE_SATISFIES = "satisfies"
EDGE_SUPERSEDED_BY = "superseded_by"
EDGE_INPUTS = "inputs"
EDGE_STAMPS = "stamps"
EDGE_SECTIONS_DEP = "sections-dep"


# ----------------------------------------------------------------------
# Context object -- carries runtime config + shared indexes
# ----------------------------------------------------------------------

def _safe_list(v) -> list:
    """Return v if it is a list, else []. Guards every downstream `for x
    in field or []:` iteration against hand-edited caches that replaced a
    list-typed field with a scalar (e.g. `sections: 42`) which would
    otherwise raise TypeError from the for-loop."""
    return v if isinstance(v, list) else []


def _validate_nodes(nodes: list) -> list:
    """Drop rows that lack the minimum cache contract (dict + file_path
    string). Build.py's forbidden-field + schema checks already fence
    authored TODO files; this is belt-and-braces against a hand-edited
    or interrupted-write cache that would otherwise crash downstream
    with KeyError."""
    clean = []
    for n in nodes or []:
        if not isinstance(n, dict):
            continue
        fp = n.get("file_path")
        if not isinstance(fp, str) or not fp:
            continue
        clean.append(n)
    return clean


class Ctx:
    __slots__ = (
        "nodes", "id_index", "path_index", "slug_index", "slug_collisions",
        "inbound", "repo_root", "quiet", "pre_notice_fired",
    )

    def __init__(self, nodes, repo_root, quiet):
        self.nodes = _validate_nodes(nodes)
        self.id_index = build_id_index(self.nodes)
        self.path_index = build_path_index(self.nodes)
        self.slug_index, self.slug_collisions = build_slug_index(self.nodes)
        self.repo_root = repo_root
        self.quiet = quiet
        self.pre_notice_fired = False
        self.inbound = collect_inbound_edges(self.nodes, self.id_index, self.path_index)

    def stderr(self, msg):
        if self.quiet:
            return
        sys.stderr.write(f"[query.py] {msg}\n")

    def pre_migration_notice_once(self):
        if self.pre_notice_fired or self.quiet:
            return
        no_fm = sum(1 for n in self.nodes if n.get("status") == "no-frontmatter")
        total = len(self.nodes)
        if total and no_fm * 2 > total:  # majority
            sys.stderr.write(
                f"[query.py] pre-migration notice: {no_fm}/{total} nodes have no "
                "frontmatter; status-gated queries (ready/blocked/blocking) require "
                "§5 back-fill. Showing 0 results.\n"
            )
        self.pre_notice_fired = True


# ----------------------------------------------------------------------
# Indexes
# ----------------------------------------------------------------------

def build_slug_index(nodes: list) -> tuple:
    """Return ({slug: file_path}, {slug: [all matching file_paths]}) where
    slug is the `TODO-NN-<slug>` suffix.

    The second mapping tracks ambiguity so resolve_id_to_node can refuse
    to silently pick one when the same slug maps to multiple files.
    Ambiguity is unlikely post-§5 (frontmatter ids are globally unique),
    but the pre-migration tree has no enforcement -- two domains could
    legally ship `TODO-NN-same-slug.md` files until §5 + §6 land."""
    out: dict = {}
    collisions: dict = {}
    for n in nodes:
        rel = n.get("file_path")
        if not rel:
            continue
        stem = Path(rel).stem
        m = FILENAME_SLUG_RE.match(stem)
        if not m:
            continue
        slug = m.group("slug")
        collisions.setdefault(slug, []).append(rel)
        if slug not in out:
            out[slug] = rel
    return out, collisions


def _resolve_edge_target(raw: str, source_file: str, id_index: dict, path_index: dict) -> Optional[str]:
    """Wrapper over resolve_xref_target that tolerates trailing punctuation
    and markdown anchor suffixes.

    The cache preserves Inputs XREF target_path strings verbatim (e.g.
    'T05', 'TODO-02-ai-development-system.md#9-ai-workflow-...'), but the
    underlying resolver in validate.py does not currently strip trailing
    `#anchor` from filename forms. We strip it here so §4 backlinks
    resolve today; validate.py's stale-XREF count at §3 baseline is
    inflated by the same class of refs and can be tightened separately."""
    if raw is None:
        return None
    t = raw.strip().strip("`").strip(",").strip()
    if not t:
        return None
    # Strip markdown anchor suffix (`TODO-02-foo.md#9-bar` -> `TODO-02-foo.md`).
    if "#" in t:
        t = t.split("#", 1)[0]
    return resolve_xref_target(t, source_file, id_index, path_index)


def collect_inbound_edges(nodes: list, id_index: dict, path_index: dict) -> dict:
    """Precompute inbound edges keyed by target file_path.

    Returns {file_path: [edge_dict, ...]} where each edge_dict has:
      - source:  file_path of the referencing node
      - kind:    one of EDGE_* constants
      - section: Optional[str] -- the `§N` suffix when present

    Self-references are excluded so a TODO's own internal XREFs don't
    make it look inbound-popular."""
    inbound: dict = {}
    for n in nodes:
        src = n["file_path"]

        def _add(target: Optional[str], kind: str, section: Optional[str]):
            if not target or target == src:
                return
            inbound.setdefault(target, []).append({
                "source": src, "kind": kind, "section": section,
            })

        # Frontmatter depends_on / satisfies
        for ref in _safe_list(n.get("depends_on")):
            tgt = id_index.get(ref)
            _add(tgt, EDGE_DEPENDS_ON, None)
        for ref in _safe_list(n.get("satisfies")):
            tgt = id_index.get(ref)
            _add(tgt, EDGE_SATISFIES, None)
        sby = n.get("superseded_by")
        if sby:
            _add(id_index.get(sby), EDGE_SUPERSEDED_BY, None)

        # Inputs XREFs (guard each element -- hand-edited caches may
        # contain None / scalar entries; isinstance the whole way down).
        for x in _safe_list(n.get("inputs_xrefs")):
            if not isinstance(x, dict):
                continue
            tgt = _resolve_edge_target(x.get("target_path"), src, id_index, path_index)
            _add(tgt, EDGE_INPUTS, x.get("target_section"))

        # Stamp XREFs (Accepted / Deferred)
        for x in _safe_list(n.get("stamps_xrefs")):
            if not isinstance(x, dict):
                continue
            tgt = _resolve_edge_target(x.get("target_path"), src, id_index, path_index)
            _add(tgt, EDGE_STAMPS, x.get("target_section"))

        # sections[].depends_on -- structured {target, sections[int]}
        for sec in _safe_list(n.get("sections")):
            if not isinstance(sec, dict):
                continue
            for grp in _safe_list(sec.get("depends_on")):
                if not isinstance(grp, dict):
                    continue
                tgt_tok = grp.get("target")
                if not tgt_tok or tgt_tok == "self":
                    continue
                tgt = _resolve_edge_target(tgt_tok, src, id_index, path_index)
                sections = _safe_list(grp.get("sections"))
                section_str = ",".join(f"§{s}" for s in sections) if sections else None
                _add(tgt, EDGE_SECTIONS_DEP, section_str)
    return inbound


# ----------------------------------------------------------------------
# Id resolution
# ----------------------------------------------------------------------

def resolve_id_to_node(target: str, ctx: Ctx) -> Optional[dict]:
    """Map a user-supplied <id> to a cache node. Pre-migration tree is the
    hot path: filename stem and slug are the working forms until §5 ships.

    Resolution order:
      1. Frontmatter id
      2. Filename stem (TODO-02-ai-development-system)
      3. Slug (ai-development-system)
      4. Full cache file_path (todo/00-infrastructure/TODO-02-ai.md)
      5. resolve_xref_target() last resort (handles T02, D00T02, etc.)

    Ambiguous slug matches (same slug used across domains) exit 2 with
    a listing rather than silently picking one. Frontmatter id collisions
    are caught by the §6 CI gate, not here."""
    if not target:
        return None
    t = target.strip()

    # 1. Frontmatter id
    fp = ctx.id_index.get(t)
    # 2. Filename stem
    if not fp:
        fp = ctx.path_index["by_filename"].get(t)
    # 3. Slug (with ambiguity check)
    if not fp:
        collisions = ctx.slug_collisions.get(t)
        if collisions and len(collisions) > 1:
            sys.stderr.write(
                f"[query.py] error: id '{t}' is ambiguous; matches:\n"
                + "\n".join(f"  - {p}" for p in collisions) + "\n"
                + "[query.py] disambiguate by passing the full filename stem or file_path.\n"
            )
            sys.exit(2)
        fp = ctx.slug_index.get(t)
    # 4. Full cache file_path
    if not fp:
        for n in ctx.nodes:
            if n.get("file_path") == t:
                fp = t
                break
    # 5. resolve_xref_target last resort
    if not fp:
        fp = resolve_xref_target(t, "", ctx.id_index, ctx.path_index)

    if not fp:
        return None
    for n in ctx.nodes:
        if n.get("file_path") == fp:
            return n
    return None


def _id_hint(target: str, ctx: Ctx, limit: int = 3) -> str:
    """Fuzzy substring suggestion from the slug index. Cheap enough; avoids
    bringing in difflib for a one-liner hint."""
    t = target.lower()
    cands = [s for s in ctx.slug_index if t in s or s in t]
    cands.sort(key=lambda s: (abs(len(s) - len(t)), s))
    return ", ".join(cands[:limit]) if cands else "(no near matches)"


def display_id(node: dict) -> str:
    """Human-readable identifier for a node: frontmatter id if available,
    otherwise filename slug. Filename stem is the last-resort fallback."""
    if node.get("id"):
        return node["id"]
    stem = Path(node["file_path"]).stem
    m = FILENAME_SLUG_RE.match(stem)
    return m.group("slug") if m else stem


# ----------------------------------------------------------------------
# Subcommands
# ----------------------------------------------------------------------

def cmd_ready(ctx: Ctx, args) -> tuple:
    rows = []
    for n in ctx.nodes:
        if n.get("status") != "draft":
            continue
        deps = _safe_list(n.get("depends_on"))
        all_done = True
        for ref in deps:
            tgt_path = ctx.id_index.get(ref)
            if not tgt_path:
                all_done = False
                break
            tgt = next((x for x in ctx.nodes if x["file_path"] == tgt_path), None)
            if not tgt or tgt.get("status") != "done":
                all_done = False
                break
        if all_done:
            rows.append({
                "domain": n.get("domain") or "",
                "id": display_id(n),
                "title": n.get("title") or "",
            })
    rows.sort(key=lambda r: (r["domain"], r["id"]))
    return rows, ["domain", "id", "title"]


def cmd_blocked(ctx: Ctx, args) -> tuple:
    rows = []
    for n in ctx.nodes:
        if n.get("status") != "active":
            continue
        blockers = []
        for ref in _safe_list(n.get("depends_on")):
            tgt_path = ctx.id_index.get(ref)
            tgt = next((x for x in ctx.nodes if x["file_path"] == tgt_path), None) if tgt_path else None
            if not tgt or tgt.get("status") != "done":
                blockers.append(ref)
        if blockers:
            rows.append({
                "domain": n.get("domain") or "",
                "id": display_id(n),
                "title": n.get("title") or "",
                "blocked_by": ",".join(blockers),
            })
    rows.sort(key=lambda r: (r["domain"], r["id"]))
    return rows, ["domain", "id", "title", "blocked_by"]


def cmd_blocking(ctx: Ctx, args) -> tuple:
    counts: dict = {}
    for n in ctx.nodes:
        if n.get("status") not in ("active", "draft"):
            continue
        for ref in _safe_list(n.get("depends_on")):
            tgt_path = ctx.id_index.get(ref)
            if tgt_path:
                counts[tgt_path] = counts.get(tgt_path, 0) + 1
    rows = []
    for n in ctx.nodes:
        c = counts.get(n["file_path"], 0)
        if c == 0:
            continue
        rows.append({
            "id": display_id(n),
            "title": n.get("title") or "",
            "inbound_count": c,
        })
    rows.sort(key=lambda r: (-r["inbound_count"], r["id"]))
    return rows, ["id", "title", "inbound_count"]


def cmd_by_domain(ctx: Ctx, args) -> tuple:
    domain = getattr(args, "domain", None)
    rows = []
    for n in ctx.nodes:
        d = n.get("domain") or ""
        if domain and d != domain:
            continue
        first_unfinished = ""
        for sec in _safe_list(n.get("sections")):
            if not isinstance(sec, dict):
                continue
            if sec.get("status") != "done":
                fn = sec.get("n")
                if fn is not None:
                    first_unfinished = f"§{fn}"
                    break
        rows.append({
            "domain": d,
            "id": display_id(n),
            "status": n.get("status") or "",
            "title": n.get("title") or "",
            "first_unfinished": first_unfinished,
        })
    rows.sort(key=lambda r: (r["domain"], r["id"]))
    return rows, ["domain", "id", "status", "title", "first_unfinished"]


def cmd_backlinks(ctx: Ctx, args) -> tuple:
    node = resolve_id_to_node(args.target, ctx)
    if not node:
        sys.stderr.write(
            f"[query.py] error: id '{args.target}' not found. Try: {_id_hint(args.target, ctx)}\n"
        )
        sys.exit(2)
    edges = ctx.inbound.get(node["file_path"], [])
    rows = []
    for e in edges:
        src_node = next((x for x in ctx.nodes if x["file_path"] == e["source"]), None)
        if not src_node:
            continue
        rows.append({
            "domain": src_node.get("domain") or "",
            "id": display_id(src_node),
            "kind": e["kind"],
            "section": e.get("section") or "",
        })
    rows.sort(key=lambda r: (r["domain"], r["id"], r["kind"]))
    return rows, ["domain", "id", "kind", "section"]


def cmd_deferred(ctx: Ctx, args) -> tuple:
    node = resolve_id_to_node(args.target, ctx)
    if not node:
        sys.stderr.write(
            f"[query.py] error: id '{args.target}' not found. Try: {_id_hint(args.target, ctx)}\n"
        )
        sys.exit(2)
    rows = []
    for x in _safe_list(node.get("stamps_xrefs")):
        if not isinstance(x, dict):
            continue
        tgt_path = _resolve_edge_target(
            x.get("target_path"), node["file_path"], ctx.id_index, ctx.path_index,
        )
        # Only include outbound (pointing at a DIFFERENT node)
        if not tgt_path or tgt_path == node["file_path"]:
            continue
        tgt_node = next((nn for nn in ctx.nodes if nn["file_path"] == tgt_path), None)
        rows.append({
            "kind": x.get("kind") or "",
            "severity": x.get("severity") or "",
            "target": display_id(tgt_node) if tgt_node else (x.get("target_path") or ""),
            "section": x.get("target_section") or "",
            "item_name": x.get("item_name") or "",
        })
    rows.sort(key=lambda r: (r["kind"], r["target"], r["section"]))
    return rows, ["kind", "severity", "target", "section", "item_name"]


def cmd_deferred_by(ctx: Ctx, args) -> tuple:
    node = resolve_id_to_node(args.target, ctx)
    if not node:
        sys.stderr.write(
            f"[query.py] error: id '{args.target}' not found. Try: {_id_hint(args.target, ctx)}\n"
        )
        sys.exit(2)
    rows = []
    for n in ctx.nodes:
        if n["file_path"] == node["file_path"]:
            continue
        for x in _safe_list(n.get("stamps_xrefs")):
            if not isinstance(x, dict):
                continue
            tgt_path = _resolve_edge_target(
                x.get("target_path"), n["file_path"], ctx.id_index, ctx.path_index,
            )
            if tgt_path != node["file_path"]:
                continue
            rows.append({
                "source": display_id(n),
                "kind": x.get("kind") or "",
                "severity": x.get("severity") or "",
                "section": x.get("target_section") or "",
                "item_name": x.get("item_name") or "",
            })
    rows.sort(key=lambda r: (r["source"], r["kind"], r["section"]))
    return rows, ["source", "kind", "severity", "section", "item_name"]


def cmd_orphans(ctx: Ctx, args) -> tuple:
    rows = []
    for n in ctx.nodes:
        if ctx.inbound.get(n["file_path"]):
            continue
        rows.append({
            "domain": n.get("domain") or "",
            "id": display_id(n),
            "title": n.get("title") or "",
        })
    rows.sort(key=lambda r: (r["domain"], r["id"]))
    return rows, ["domain", "id", "title"]


def _parse_iso(ts: Optional[str]) -> Optional[datetime]:
    if not ts:
        return None
    # build.py emits "...Z" ISO-8601; accept the "+00:00" form too.
    try:
        return datetime.fromisoformat(ts.replace("Z", "+00:00"))
    except ValueError:
        return None


def cmd_stale(ctx: Ctx, args) -> tuple:
    days = args.days if args.days is not None else DEFAULT_STALE_DAYS
    now = datetime.now(timezone.utc)
    rows = []
    for n in ctx.nodes:
        t = _parse_iso(n.get("last_active_at"))
        if not t:
            continue
        age_days = (now - t).total_seconds() / 86400.0
        if age_days < days:
            continue
        rows.append({
            "domain": n.get("domain") or "",
            "id": display_id(n),
            "last_active_at": n.get("last_active_at") or "",
            "age_days": int(age_days),
        })
    rows.sort(key=lambda r: (-r["age_days"], r["domain"], r["id"]))
    return rows, ["domain", "id", "last_active_at", "age_days"]


def _dep_depth(node: dict, ctx: Ctx, memo: dict, stack: set) -> int:
    """Longest depends_on chain length rooted at node. Cycles bounded by
    the stack set (contributes 0 for the back-edge)."""
    fp = node["file_path"]
    if fp in memo:
        return memo[fp]
    if fp in stack:
        return 0
    stack.add(fp)
    best = 0
    for ref in _safe_list(node.get("depends_on")):
        tgt_path = ctx.id_index.get(ref)
        if not tgt_path:
            continue
        tgt = next((x for x in ctx.nodes if x["file_path"] == tgt_path), None)
        if not tgt:
            continue
        d = _dep_depth(tgt, ctx, memo, stack) + 1
        if d > best:
            best = d
    stack.discard(fp)
    memo[fp] = best
    return best


def cmd_stats(ctx: Ctx, args) -> tuple:
    by_status: dict = {}
    by_domain: dict = {}
    for n in ctx.nodes:
        s = n.get("status") or "unknown"
        by_status[s] = by_status.get(s, 0) + 1
        d = n.get("domain") or "unknown"
        by_domain[d] = by_domain.get(d, 0) + 1

    # top_blocking: reuse cmd_blocking
    blocking_rows, _ = cmd_blocking(ctx, args)
    top_blocking = [
        {"id": r["id"], "inbound_count": r["inbound_count"]}
        for r in blocking_rows[:3]
    ]

    # top_longest_deferred: rank nodes with >=1 outbound kind=deferred stamp
    # by oldest last_active_at (proxy for how long the deferral has sat).
    deferred_nodes = []
    for n in ctx.nodes:
        has_deferred = any(
            (isinstance(x, dict) and x.get("kind") == "deferred")
            for x in (_safe_list(n.get("stamps_xrefs")))
        )
        if not has_deferred:
            continue
        t = _parse_iso(n.get("last_active_at"))
        if not t:
            continue
        deferred_nodes.append((t, n))
    deferred_nodes.sort(key=lambda p: p[0])
    top_longest_deferred = [
        {"id": display_id(n), "last_active_at": n.get("last_active_at")}
        for _, n in deferred_nodes[:3]
    ]

    # avg_dep_depth
    memo: dict = {}
    depths = []
    for n in ctx.nodes:
        depths.append(_dep_depth(n, ctx, memo, set()))
    avg = (sum(depths) / len(depths)) if depths else 0.0

    # orphan count
    orphan_rows, _ = cmd_orphans(ctx, args)
    orphan_count = len(orphan_rows)

    stats = {
        "total_nodes": len(ctx.nodes),
        "by_status": dict(sorted(by_status.items())),
        "by_domain": dict(sorted(by_domain.items())),
        "top_blocking": top_blocking,
        "top_longest_deferred": top_longest_deferred,
        "avg_dep_depth": round(avg, 2),
        "orphan_count": orphan_count,
    }
    return stats, None  # None columns == scalar/nested output


def cmd_code(ctx: Ctx, args) -> tuple:
    node = resolve_id_to_node(args.target, ctx)
    if not node:
        sys.stderr.write(
            f"[query.py] error: id '{args.target}' not found. Try: {_id_hint(args.target, ctx)}\n"
        )
        sys.exit(2)
    rows = []
    seen = set()

    # file_patterns (post-§5)
    for pat in _safe_list(node.get("file_patterns")):
        if not isinstance(pat, str) or not pat:
            continue
        key = (pat, "pattern")
        if key in seen:
            continue
        seen.add(key)
        rows.append({"path": pat, "source": "pattern"})

    # Notes-grep for (src/...) / (include/...) references. Enforce a
    # repo-local path boundary on the cache-derived file_path so a
    # poisoned cache entry like `../../etc/passwd` can never be read.
    text = ""
    try:
        resolved = (ctx.repo_root / node["file_path"]).resolve(strict=False)
        repo_resolved = ctx.repo_root.resolve(strict=False)
        resolved.relative_to(repo_resolved)
        text = resolved.read_text(encoding="utf-8", errors="replace")
    except (OSError, ValueError):
        # ValueError: path escapes repo_root; OSError: file missing / unreadable.
        text = ""
    for m in NOTES_PATH_RE.finditer(text):
        p = m.group("path")
        # Skip placeholder forms: `(src/...)`, `(include/../bar)`, bare
        # `src/` with a trailing slash and nothing else. Require at least
        # one proper filename segment with a file-ish extension or a real
        # (non-dot) basename.
        if ".." in p:
            continue
        basename = p.rsplit("/", 1)[-1]
        if not basename or basename == "..." or basename.startswith("."):
            continue
        key = (p, "notes-grep")
        if key in seen:
            continue
        seen.add(key)
        rows.append({"path": p, "source": "notes-grep"})

    rows.sort(key=lambda r: (r["path"], r["source"]))
    return rows, ["path", "source"]


def cmd_code_by(ctx: Ctx, args) -> tuple:
    target_path = args.target
    rows = []
    for n in ctx.nodes:
        for pat in _safe_list(n.get("file_patterns")):
            if not isinstance(pat, str) or not pat:
                continue
            if fnmatch.fnmatch(target_path, pat):
                rows.append({
                    "domain": n.get("domain") or "",
                    "id": display_id(n),
                    "matched_pattern": pat,
                })
                break
    rows.sort(key=lambda r: (r["domain"], r["id"]))
    return rows, ["domain", "id", "matched_pattern"]


# ----------------------------------------------------------------------
# Emitters
# ----------------------------------------------------------------------

def _md_cell(v) -> str:
    s = str(v if v is not None else "")
    return s.replace("|", "\\|").replace("\n", " ").replace("\r", " ").replace("\t", " ")


def _tsv_cell(v) -> str:
    """Sanitize embedded control characters from TSV fields. A cell
    containing a raw tab would forge a column; a raw newline would forge
    a row. Neither is a legitimate value in the cache surface (titles,
    ids, file paths) but defensive stripping costs nothing and keeps
    shell pipelines safe."""
    s = str(v if v is not None else "")
    return s.replace("\t", " ").replace("\r", " ").replace("\n", " ")


def emit(rows, columns, fmt: str):
    """Render rows in the requested format. rows is either a list of dicts
    (when columns is not None) or a scalar dict (when columns is None, for
    stats). fmt is 'tsv' | 'json' | 'markdown'."""
    if columns is None:
        # Scalar/nested output (stats).
        if fmt == "json":
            sys.stdout.write(json.dumps(rows, indent=2, sort_keys=True) + "\n")
            return
        if fmt == "markdown":
            _emit_stats_markdown(rows)
            return
        _emit_stats_tsv(rows)
        return

    if fmt == "json":
        sys.stdout.write(json.dumps(rows, indent=2, sort_keys=True) + "\n")
        return
    if fmt == "markdown":
        sys.stdout.write("| " + " | ".join(columns) + " |\n")
        sys.stdout.write("| " + " | ".join(["---"] * len(columns)) + " |\n")
        for r in rows:
            sys.stdout.write("| " + " | ".join(_md_cell(r.get(c)) for c in columns) + " |\n")
        return

    # Default TSV: one row per line, tab-separated, no header. Sanitize
    # every cell -- embedded tabs/newlines in a title or item_name would
    # otherwise forge extra columns or rows.
    for r in rows:
        sys.stdout.write("\t".join(_tsv_cell(r.get(c, "")) for c in columns) + "\n")


def _emit_stats_tsv(s: dict):
    sys.stdout.write(f"total_nodes\t{s['total_nodes']}\n")
    sys.stdout.write(f"orphan_count\t{s['orphan_count']}\n")
    sys.stdout.write(f"avg_dep_depth\t{s['avg_dep_depth']}\n")
    for k, v in s["by_status"].items():
        sys.stdout.write(f"by_status\t{_tsv_cell(k)}\t{v}\n")
    for k, v in s["by_domain"].items():
        sys.stdout.write(f"by_domain\t{_tsv_cell(k)}\t{v}\n")
    for b in s["top_blocking"]:
        sys.stdout.write(f"top_blocking\t{_tsv_cell(b['id'])}\t{b['inbound_count']}\n")
    for d in s["top_longest_deferred"]:
        sys.stdout.write(f"top_longest_deferred\t{_tsv_cell(d['id'])}\t{_tsv_cell(d['last_active_at'])}\n")


def _emit_stats_markdown(s: dict):
    sys.stdout.write(f"## TODO graph stats\n\n")
    sys.stdout.write(f"- **total_nodes:** {s['total_nodes']}\n")
    sys.stdout.write(f"- **orphan_count:** {s['orphan_count']}\n")
    sys.stdout.write(f"- **avg_dep_depth:** {s['avg_dep_depth']}\n\n")
    sys.stdout.write("### By status\n\n| status | count |\n| --- | --- |\n")
    for k, v in s["by_status"].items():
        sys.stdout.write(f"| {_md_cell(k)} | {v} |\n")
    sys.stdout.write("\n### By domain\n\n| domain | count |\n| --- | --- |\n")
    for k, v in s["by_domain"].items():
        sys.stdout.write(f"| {_md_cell(k)} | {v} |\n")
    sys.stdout.write("\n### Top blocking\n\n| id | inbound_count |\n| --- | --- |\n")
    for b in s["top_blocking"]:
        sys.stdout.write(f"| {_md_cell(b['id'])} | {b['inbound_count']} |\n")
    sys.stdout.write("\n### Top longest-deferred\n\n| id | last_active_at |\n| --- | --- |\n")
    for d in s["top_longest_deferred"]:
        sys.stdout.write(f"| {_md_cell(d['id'])} | {_md_cell(d['last_active_at'])} |\n")


# ----------------------------------------------------------------------
# Watch loop
# ----------------------------------------------------------------------

def _snapshot_mtimes(todo_root: Path) -> dict:
    out = {}
    for dirpath, _dirs, files in os.walk(todo_root):
        for f in files:
            if not f.endswith(".md"):
                continue
            p = os.path.join(dirpath, f)
            try:
                out[p] = os.stat(p).st_mtime_ns
            except OSError:
                continue
    return out


def watch_loop(run_once, todo_root: Path, quiet: bool):
    """Run run_once() once, then re-run whenever a .md file under todo_root
    changes. Prefer inotifywait; fall back to mtime polling every 2s."""
    def _banner():
        if quiet:
            return
        sys.stderr.write(f"---- {datetime.now(timezone.utc).isoformat(timespec='seconds')}\n")

    # Initial run.
    run_once()

    inw = shutil.which("inotifywait")
    if inw:
        try:
            proc = subprocess.Popen(
                [inw, "-e", "close_write,moved_to,create,delete",
                 "-r", "-m", "--format", "%w%f %e", str(todo_root)],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
            )
        except OSError:
            proc = None
        if proc is not None:
            try:
                while True:
                    line = proc.stdout.readline()
                    if not line:
                        break
                    path = line.split(" ", 1)[0]
                    if path.endswith(".md"):
                        _banner()
                        run_once()
            except KeyboardInterrupt:
                pass
            finally:
                try:
                    proc.terminate()
                    proc.wait(timeout=1)
                except (OSError, subprocess.TimeoutExpired):
                    try:
                        proc.kill()
                    except OSError:
                        pass
            return

    # Polling fallback.
    prev = _snapshot_mtimes(todo_root)
    try:
        while True:
            time.sleep(POLL_INTERVAL_SECONDS)
            cur = _snapshot_mtimes(todo_root)
            if cur != prev:
                _banner()
                run_once()
                prev = cur
    except KeyboardInterrupt:
        return


# ----------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------

SUBCOMMANDS = {
    "ready":       (cmd_ready, []),
    "blocked":     (cmd_blocked, []),
    "blocking":    (cmd_blocking, []),
    "by-domain":   (cmd_by_domain, [("domain", "?", None)]),
    "backlinks":   (cmd_backlinks, [("target", None, None)]),
    "deferred":    (cmd_deferred, [("target", None, None)]),
    "deferred-by": (cmd_deferred_by, [("target", None, None)]),
    "orphans":     (cmd_orphans, []),
    "stale":       (cmd_stale, [("days", "optional-int", None)]),
    "stats":       (cmd_stats, []),
    "code":        (cmd_code, [("target", None, None)]),
    "code-by":     (cmd_code_by, [("target", None, None)]),
}


def _add_shared_flags(ap: argparse.ArgumentParser, *, is_subparser: bool):
    """Global flags are declared on both the top-level parser and every
    subparser so callers can place them on either side of the subcommand
    (e.g. `query.py --json backlinks X` OR `query.py backlinks X --json`).

    On subparsers we set `default=argparse.SUPPRESS` so a flag that is
    missing on the subcommand side does NOT overwrite the value already
    parsed by the top-level layer. Without SUPPRESS, `--quiet` passed
    before the subcommand gets silently reset to False when the
    subparser fills in its own store_true default."""
    common = dict(default=argparse.SUPPRESS) if is_subparser else {}
    ap.add_argument("--cache", help="cache path (default: build/todo-cache.json)",
                    **(common if is_subparser else {"default": "build/todo-cache.json"}))
    ap.add_argument("--repo-root", help="repo root override (default: auto-detect)",
                    **(common if is_subparser else {"default": None}))
    ap.add_argument("--json", action="store_true", help="JSON output",
                    **common)
    ap.add_argument("--format", choices=("tsv", "markdown"),
                    help="output format (default: tsv; --json overrides)",
                    **(common if is_subparser else {"default": "tsv"}))
    ap.add_argument("--watch", action="store_true",
                    help="re-run on todo/*.md change (inotifywait or 2s polling)",
                    **common)
    ap.add_argument("--quiet", action="store_true",
                    help="suppress stderr notices + [query.py] prefix",
                    **common)


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="query.py",
        description="TODO graph query CLI (TODO-06 §4). Consumes build/todo-cache.json.",
    )
    _add_shared_flags(p, is_subparser=False)

    sub = p.add_subparsers(dest="subcommand", required=True, metavar="SUBCOMMAND")
    for name, (_fn, positional) in SUBCOMMANDS.items():
        sp = sub.add_parser(name)
        _add_shared_flags(sp, is_subparser=True)
        for pos_name, nargs, default in positional:
            if nargs == "optional-int":
                sp.add_argument("--days", type=int, default=default,
                                help=f"days threshold (default: {DEFAULT_STALE_DAYS})")
            elif nargs == "?":
                sp.add_argument(pos_name, nargs="?", default=default)
            else:
                sp.add_argument(pos_name)
    return p


def _rebuild_cache(cache_path: Path, repo_root: Path, quiet: bool) -> None:
    """Force-rebuild build/todo-cache.json by shelling out to build.py.
    Used between --watch ticks so the query reflects the user's latest
    TODO edits (load_or_rebuild_cache alone won't trigger a rebuild for
    a schema-stable cache; it only regenerates on absence or stale shape).
    Errors bubble up as a stderr notice but don't abort the watch loop --
    a transient build failure should not kill an interactive watcher."""
    build_py = Path(__file__).resolve().parent / "build.py"
    if not build_py.exists():
        if not quiet:
            sys.stderr.write(f"[query.py] WARN: build.py missing at {build_py}; skipping cache rebuild\n")
        return
    result = subprocess.run(
        [sys.executable, str(build_py),
         "--quiet", "--output", str(cache_path),
         "--root", str(repo_root / "todo"),
         "--repo-root", str(repo_root)],
        cwd=str(repo_root),
    )
    if result.returncode != 0 and not quiet:
        sys.stderr.write(f"[query.py] WARN: build.py exited {result.returncode}; using last successful cache\n")


def _find_repo_root(explicit: Optional[str]) -> Path:
    if explicit:
        return Path(explicit).resolve()
    # Walk upward from this script's directory looking for a `todo/` folder.
    here = Path(__file__).resolve().parent
    for cand in (here, *here.parents):
        if (cand / "todo").is_dir() and (cand / "scripts" / "todo-graph").is_dir():
            return cand
    return Path.cwd()


def main(argv=None) -> int:
    # Restore default SIGPIPE so piping into `head` doesn't print a BrokenPipeError.
    try:
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    except (AttributeError, ValueError):
        pass  # Windows / non-main-thread

    parser = _build_parser()
    args = parser.parse_args(argv)

    repo_root = _find_repo_root(args.repo_root)
    cache_path = Path(args.cache)
    if not cache_path.is_absolute():
        cache_path = (repo_root / cache_path).resolve()

    fn, _positional = SUBCOMMANDS[args.subcommand]
    fmt = "json" if args.json else args.format
    status_gated = args.subcommand in ("ready", "blocked", "blocking")

    def _build_ctx():
        """(Re)load the cache and build a fresh Ctx. Called once up-front
        for a single-shot run; called every tick in --watch mode so the
        user sees output reflecting the latest TODO edits, not the snapshot
        taken at process start."""
        fresh_nodes = load_or_rebuild_cache(cache_path, repo_root, args.quiet)
        if not isinstance(fresh_nodes, list):
            sys.stderr.write(
                f"[query.py] FATAL: cache at {cache_path} is not a list; "
                f"got {type(fresh_nodes).__name__}\n"
            )
            sys.exit(2)
        c = Ctx(fresh_nodes, repo_root, args.quiet)
        if status_gated:
            c.pre_migration_notice_once()
        return c

    def _run_once():
        ctx = _build_ctx()
        rows, columns = fn(ctx, args)
        emit(rows, columns, fmt)

    if args.watch:
        # Ensure the todo/ directory exists before watching. Also regenerate
        # the cache BEFORE each tick so build.py picks up the user's edit;
        # do this inside _watch_rebuild since load_or_rebuild_cache alone
        # won't trigger a rebuild for a schema-stable cache.
        def _watch_rebuild_and_run():
            _rebuild_cache(cache_path, repo_root, args.quiet)
            _run_once()
        # First run uses the cache already primed at startup (may be stale
        # if the user edited a file between last build and invoking watch).
        _rebuild_cache(cache_path, repo_root, args.quiet)
        watch_loop(_watch_rebuild_and_run, repo_root / "todo", args.quiet)
    else:
        _run_once()
    return 0


if __name__ == "__main__":
    sys.exit(main())
