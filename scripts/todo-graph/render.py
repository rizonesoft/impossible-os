#!/usr/bin/env python3
# ============================================================================
# scripts/todo-graph/render.py -- TODO graph visualization (TODO-06 §7).
#
# Five output formats consumed by query.py's `render` subcommand:
#   mermaid   -- GitHub-renderable flowchart, status-colored, clickable
#   dot       -- graphviz digraph for `dot -Tsvg | open` workflows
#   ascii     -- indented tree rooted at <id> or the first orphan
#   gantt     -- mermaid Gantt chart from created_at + effort + depends_on
#   markdown  -- compact table for PR descriptions + AGENTS.md insertions
#
# Shared surface with query.py: consumes the same validated nodes list
# and reuses Ctx's inbound + resolution indexes. Stdlib-only. Edge
# semantics:
#   - depends_on / satisfies / superseded_by: solid arrows
#     (different labels but all structural)
#   - Accepted / Deferred stamps: dotted arrows with the stamp kind on
#     the edge label (the two have different triage meaning -- see
#     review-todo-section skill grammar)
#   - Inputs XREFs: omitted by default (too many; spec doesn't require)
#
# Color palette (Tableau-ish, works on light + dark GitHub themes):
#   done        #d4edda / #28a745     (green)
#   active      #fff3cd / #ffc107     (amber)
#   blocked     #f8d7da / #dc3545     (red)
#   draft       #e9ecef / #6c757d     (gray)
#   superseded  #e2e3e5, dashed       (strikethrough visual cue)
# ============================================================================

from __future__ import annotations

import hashlib
import re
import sys
from pathlib import Path
from typing import Optional

# The Gantt duration grammar is the PRODUCER's rule, so it is imported rather
# than restated: a copy here could accept a value `build.py` refuses, or draw
# one it never emitted (TODO-06 section 25). Same sys.path shape build.py uses,
# so this module works standalone as well as imported by `query.py`.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import cache_schema as _cs  # noqa: E402

EFFORT_REGEX = _cs.EFFORT_REGEX
EFFORT_DEFAULT = _cs.EFFORT_DEFAULT


# Repo URL for clickable mermaid nodes. Spec pins this to the Impossible OS
# GitHub repo; no override flag today (deferrable to a follow-up when the
# graph ships on a fork or under a non-main default branch).
REPO_URL_BASE = "https://github.com/rizonetech/impossible-os/blob/main"

STATUS_STYLES = {
    "done":       {"fill": "#d4edda", "stroke": "#28a745"},
    "active":     {"fill": "#fff3cd", "stroke": "#ffc107"},
    "blocked":    {"fill": "#f8d7da", "stroke": "#dc3545"},
    "draft":      {"fill": "#e9ecef", "stroke": "#6c757d"},
    "superseded": {"fill": "#e2e3e5", "stroke": "#6c757d", "dashed": True},
}

# Mermaid node ids must be identifier-safe. Kebab slugs are fine in
# modern mermaid, but we sanitize just in case (replace any non
# [A-Za-z0-9_] with underscore) and prefix with `n_` so a slug starting
# with a digit (unlikely but possible) still parses.
_MERMAID_ID_BAD = re.compile(r"[^A-Za-z0-9_]")


def _mermaid_id(s: str) -> str:
    """Return a mermaid-safe identifier for `s`. The sanitized form is
    appended with a 6-char md5 prefix hash so paths that differ only in
    sanitized-to-underscore characters (`foo-bar.md` vs `foo_bar.md`,
    `x<y.md` vs `x>y.md`) never collide. Codex pass 16 H1."""
    if not s:
        return "n_unknown"
    safe = _MERMAID_ID_BAD.sub("_", s)
    if safe and safe[0].isdigit():
        safe = "n_" + safe
    h = hashlib.md5(s.encode("utf-8")).hexdigest()[:6]
    return f"{safe}_{h}"


def _label(node: dict, display_id: str) -> str:
    """Human-visible label: id on one line, TODO-NN on the next."""
    fp = node.get("file_path") or ""
    stem = Path(fp).stem if fp else ""
    m = re.match(r"^(TODO-[0-9A-Z]{1,3})-", stem)
    prefix = m.group(1) if m else stem
    # Escape any characters mermaid treats as markup. `<br/>` must be
    # the HTML tag, so label mid-strings with pipe/quote get escaped.
    safe_id = display_id.replace('"', "&quot;")
    return f'{safe_id}<br/>{prefix}'


# ---------------------------------------------------------------------
# Shared scope filter
# ---------------------------------------------------------------------

def filter_by_scope(nodes: list, scope: Optional[str]) -> list:
    """Return the subset of nodes whose domain matches `scope`. When
    scope is None the whole list is returned. Edges will be filtered
    per-emitter so only in-scope endpoints render."""
    if not scope:
        return nodes
    return [n for n in nodes if n.get("domain") == scope]


def _display_id_for(node: dict) -> str:
    """Same derivation query.py.display_id uses -- frontmatter id if
    present, else the filename slug. render.py duplicates it locally
    so this module stays importable without pulling in the whole CLI."""
    if node.get("id"):
        return node["id"]
    fp = node.get("file_path") or ""
    stem = Path(fp).stem
    m = re.match(r"^TODO-[0-9A-Z]{1,3}-(.+)$", stem)
    return m.group(1) if m else stem


# ---------------------------------------------------------------------
# Mermaid flowchart
# ---------------------------------------------------------------------

def render_mermaid(nodes: list, ctx) -> str:
    """Emit a GitHub-renderable mermaid flowchart. Nodes are keyed by
    file_path (unique in the cache), labeled with id + TODO-NN prefix,
    colored by status, and clickable to the GitHub blob URL."""
    out = ["flowchart TD"]
    # Class definitions once, up front.
    for status, style in STATUS_STYLES.items():
        extra = ",stroke-dasharray:5 5" if style.get("dashed") else ""
        out.append(
            f"  classDef {status} "
            f"fill:{style['fill']},stroke:{style['stroke']}{extra};"
        )

    in_scope = {n["file_path"] for n in nodes}

    # Node declarations + click handlers.
    for n in nodes:
        did = _display_id_for(n)
        nid = _mermaid_id(n["file_path"])
        label = _label(n, did)
        out.append(f'  {nid}["{label}"]')
        status = n.get("status") or "draft"
        if status in STATUS_STYLES:
            out.append(f"  class {nid} {status}")
        fp = n.get("file_path")
        if fp:
            out.append(f'  click {nid} "{REPO_URL_BASE}/{fp}"')

    # Structural edges: depends_on, satisfies, superseded_by. Solid
    # arrows. Only within-scope endpoints render.
    for n in nodes:
        src_fp = n["file_path"]
        src_nid = _mermaid_id(src_fp)
        for field in ("depends_on", "satisfies"):
            for ref in (n.get(field) or []):
                tgt_fp = ctx.id_index.get(ref)
                if not tgt_fp or tgt_fp not in in_scope:
                    continue
                tgt_nid = _mermaid_id(tgt_fp)
                label = f"|{field}|" if field != "depends_on" else ""
                out.append(f"  {src_nid} -->{label} {tgt_nid}")
        sby = n.get("superseded_by")
        if sby:
            tgt_fp = ctx.id_index.get(sby)
            if tgt_fp and tgt_fp in in_scope:
                tgt_nid = _mermaid_id(tgt_fp)
                out.append(f"  {src_nid} -->|superseded_by| {tgt_nid}")

    # Stamp edges: Accepted / Deferred. Dotted arrows with label.
    # Targets are resolved via the same helper query.py uses.
    from query import _resolve_edge_target  # local import to avoid cycle at module load
    for n in nodes:
        src_fp = n["file_path"]
        src_nid = _mermaid_id(src_fp)
        for x in (n.get("stamps_xrefs") or []):
            if not isinstance(x, dict):
                continue
            tgt = _resolve_edge_target(x.get("target_path"), src_fp,
                                       ctx.id_index, ctx.path_index)
            if not tgt or tgt == src_fp or tgt not in in_scope:
                continue
            tgt_nid = _mermaid_id(tgt)
            kind = x.get("kind") or "stamp"
            out.append(f"  {src_nid} -.->|{kind}| {tgt_nid}")

    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------
# Graphviz dot
# ---------------------------------------------------------------------

def render_dot(nodes: list, ctx) -> str:
    out = ["digraph todo_graph {",
           "  rankdir=LR;",
           '  node [shape=box, style=filled, fontname="Helvetica"];']
    in_scope = {n["file_path"] for n in nodes}

    for n in nodes:
        did = _display_id_for(n)
        fp = n["file_path"]
        stem = Path(fp).stem
        m = re.match(r"^(TODO-[0-9A-Z]{1,3})-", stem)
        prefix = m.group(1) if m else stem
        status = n.get("status") or "draft"
        style = STATUS_STYLES.get(status, STATUS_STYLES["draft"])
        extra_style = ',style="filled,dashed"' if style.get("dashed") else ""
        safe_label = did.replace('"', '\\"') + "\\n" + prefix
        out.append(
            f'  "{fp}" [label="{safe_label}", fillcolor="{style["fill"]}", '
            f'color="{style["stroke"]}"{extra_style}];'
        )

    for n in nodes:
        src_fp = n["file_path"]
        for field in ("depends_on", "satisfies"):
            for ref in (n.get(field) or []):
                tgt_fp = ctx.id_index.get(ref)
                if tgt_fp and tgt_fp in in_scope:
                    label = f' [label="{field}"]' if field != "depends_on" else ""
                    out.append(f'  "{src_fp}" -> "{tgt_fp}"{label};')
        sby = n.get("superseded_by")
        if sby:
            tgt_fp = ctx.id_index.get(sby)
            if tgt_fp and tgt_fp in in_scope:
                out.append(f'  "{src_fp}" -> "{tgt_fp}" [label="superseded_by"];')

    from query import _resolve_edge_target
    for n in nodes:
        src_fp = n["file_path"]
        for x in (n.get("stamps_xrefs") or []):
            if not isinstance(x, dict):
                continue
            tgt = _resolve_edge_target(x.get("target_path"), src_fp,
                                       ctx.id_index, ctx.path_index)
            if not tgt or tgt == src_fp or tgt not in in_scope:
                continue
            kind = x.get("kind") or "stamp"
            out.append(f'  "{src_fp}" -> "{tgt}" [style=dotted, label="{kind}"];')

    out.append("}")
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------
# ASCII tree
# ---------------------------------------------------------------------

def render_ascii(nodes: list, ctx, root_id: Optional[str] = None) -> str:
    """Indented hierarchical tree rooted at `root_id`. When root_id is
    None, use the first orphan (no inbound edges) for stability.

    Cycle-safe via a visited set; a back-edge renders as
    `    |-- <child> (cycle)`.

    The tree follows the OUTBOUND depends_on direction from root: what
    `root` depends on. Each child expands recursively. Non-depends-on
    edge kinds are NOT expanded (too noisy for a tree view)."""
    nodes_by_fp = {n["file_path"]: n for n in nodes}
    id_to_fp = ctx.id_index

    def _resolve_root() -> Optional[dict]:
        if root_id:
            # Accept frontmatter id, filename stem, or file_path.
            fp = id_to_fp.get(root_id)
            if not fp and root_id in nodes_by_fp:
                fp = root_id
            if not fp:
                for n in nodes:
                    stem = Path(n["file_path"]).stem
                    m = re.match(r"^TODO-[0-9A-Z]{1,3}-(.+)$", stem)
                    if m and m.group(1) == root_id:
                        fp = n["file_path"]
                        break
                    if stem == root_id:
                        fp = n["file_path"]
                        break
            return nodes_by_fp.get(fp) if fp else None
        # No root supplied: pick the first file_path by sort order that
        # has zero inbound edges in ctx.inbound.
        for n in sorted(nodes, key=lambda x: x["file_path"]):
            if not ctx.inbound.get(n["file_path"]):
                return n
        return nodes[0] if nodes else None

    root = _resolve_root()
    if not root:
        return "(no nodes to render)\n"

    lines = []
    # Codex pass 16 M1: separate the recursion stack from the
    # already-rendered set. A diamond DAG (A -> B, A -> C, B -> D,
    # C -> D) is NOT a cycle; D just has fan-in. We render D once the
    # first time it's reached (with full expansion) and tag subsequent
    # encounters with `(seen)` to signal "already-rendered elsewhere"
    # without chasing the same subtree again. Only a back-edge to a
    # node currently in the recursion stack gets `(cycle)`.
    stack: set = set()
    rendered: set = set()

    def _walk(node: dict, indent: int):
        """Pre-order DFS, EXPLICIT-STACK rather than recursive.

        Same defect and same reason as `query._dep_depth`: one frame per
        dependency LEVEL meant a chain longer than Python's recursion limit
        raised `RecursionError` on input the shared validator accepts, so
        `render --render-format ascii` was unavailable for a valid cache. The
        query CLI now maps that onto its rc-2 refusal rather than an ambiguous
        rc 1, but "refuses cleanly" is not the same as "works" (Codex
        adversarial, section 22 round 2, [medium]).

        `(cycle)` vs `(seen)` semantics are preserved exactly: a back-edge to a
        node still on the current path is a cycle, a node finished elsewhere is
        seen, and `stack` is popped only after a node's children are done --
        which is what the `_POP` sentinel frames below exist to schedule.
        """
        _POP = object()
        frames = [(node, indent)]
        while frames:
            item, depth = frames.pop()
            if item is _POP:
                stack.discard(depth)  # `depth` carries the file_path here
                continue
            did = _display_id_for(item)
            status = item.get("status") or "?"
            prefix = "    " * depth + ("|-- " if depth > 0 else "")
            fp = item["file_path"]
            if fp in stack:
                lines.append(f"{prefix}{did} [{status}] (cycle)")
                continue
            if fp in rendered:
                lines.append(f"{prefix}{did} [{status}] (seen)")
                continue
            stack.add(fp)
            rendered.add(fp)
            lines.append(f"{prefix}{did} [{status}]")
            # The pop marker goes on FIRST so it runs after every child, and
            # children are pushed in REVERSE so they emit in declaration order.
            frames.append((_POP, fp))
            children = []
            for ref in (item.get("depends_on") or []):
                tgt_fp = id_to_fp.get(ref)
                child = nodes_by_fp.get(tgt_fp) if tgt_fp else None
                if child:
                    children.append((child, depth + 1))
            frames.extend(reversed(children))

    _walk(root, 0)
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------
# Gantt
# ---------------------------------------------------------------------

def _parse_iso_date(ts) -> Optional[str]:
    """Return `YYYY-MM-DD` extracted from an ISO-8601 timestamp, or None."""
    if not isinstance(ts, str) or len(ts) < 10:
        return None
    # Cheap validation: YYYY-MM-DD prefix + decimal characters.
    head = ts[:10]
    if re.match(r"^\d{4}-\d{2}-\d{2}$", head):
        return head
    return None


def _gantt_effort(node: dict) -> str:
    """Row duration for one node: the declared `effort`, else the default.

    UNREACHABLE BY DESIGN, and kept anyway. Every route into this renderer runs
    through `cache_schema` under a profile that validates `effort` against the
    same grammar `build.py` enforces on the frontmatter, so a bad value refuses
    at load. This is the last guard before the value is interpolated into a
    mermaid line, and it RAISES rather than falling back: a silent fallback here
    would restore precisely the failure TODO-06 section 25 closed -- a duration
    nobody authored, rendered as though somebody had. `ValueError` because the
    caller has already decided this cache is trustworthy, so reaching here is a
    programming error in the pipeline, not a corpus problem.
    """
    # ABSENCE, NOT FALSINESS. `node.get("effort")` conflated a missing key with
    # a present `null`, so `{"effort": null}` rendered the default -- and both
    # the cache schema and `_validate_node_fields` REFUSE a present null, so
    # this guard was more permissive than the validator it exists to backstop.
    # That matters precisely because this function is the last line for a caller
    # that imported the renderer without routing through `cache_schema` (Codex
    # adversarial, section 25, [medium]).
    if "effort" not in node:
        return EFFORT_DEFAULT
    effort = node["effort"]
    if not isinstance(effort, str) or not EFFORT_REGEX.match(effort):
        raise ValueError(
            f"node {node.get('file_path') or node.get('id')!r} carries "
            f"effort={effort!r}, which does not match "
            f"{EFFORT_REGEX.pattern}; a validated cache cannot contain this, "
            f"so the reader that produced these nodes skipped cache_schema")
    return effort


def render_gantt(nodes: list, ctx) -> str:
    """Emit a mermaid Gantt chart. Rows are grouped by domain; each row
    uses `created_at` as start + optional frontmatter `effort` field
    for duration (default `1w`). Status mapped to Mermaid Gantt's
    built-in keywords: done -> done, active -> active, rest default."""
    out = ["gantt",
           "  title TODO Roadmap",
           "  dateFormat YYYY-MM-DD"]
    by_domain: dict = {}
    for n in nodes:
        by_domain.setdefault(n.get("domain") or "(unknown)", []).append(n)
    for dom in sorted(by_domain):
        out.append(f"  section {dom}")
        for n in sorted(by_domain[dom], key=lambda x: x["file_path"]):
            did = _display_id_for(n)
            start = _parse_iso_date(n.get("created_at")) or "2026-01-01"
            effort = _gantt_effort(n)
            status = n.get("status") or "draft"
            # Mermaid Gantt status keywords
            gantt_status = ""
            if status == "done":
                gantt_status = "done, "
            elif status == "active":
                gantt_status = "active, "
            out.append(f"  {did} :{gantt_status}{did}, {start}, {effort}")
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------
# Markdown table
# ---------------------------------------------------------------------

def _md_cell(v) -> str:
    s = str(v if v is not None else "")
    return s.replace("|", "\\|").replace("\n", " ").replace("\r", " ").replace("\t", " ")


def render_markdown(nodes: list, ctx) -> str:
    """Emit a GFM table: domain | id | status | depends_on | first_unfinished."""
    out = ["| domain | id | status | depends_on | first_unfinished |",
           "| --- | --- | --- | --- | --- |"]
    for n in sorted(nodes, key=lambda x: (x.get("domain") or "", x["file_path"])):
        did = _display_id_for(n)
        dom = n.get("domain") or ""
        status = n.get("status") or ""
        deps = ",".join(n.get("depends_on") or []) or "--"
        first_unfinished = ""
        for sec in (n.get("sections") or []):
            if isinstance(sec, dict) and sec.get("status") != "done":
                fn = sec.get("n")
                if fn is not None:
                    first_unfinished = f"§{fn}"
                    break
        out.append(
            f"| {_md_cell(dom)} | {_md_cell(did)} | {_md_cell(status)} | "
            f"{_md_cell(deps)} | {_md_cell(first_unfinished)} |"
        )
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------
# Dispatcher
# ---------------------------------------------------------------------

RENDERERS = {
    "mermaid":  render_mermaid,
    "dot":      render_dot,
    "ascii":    render_ascii,
    "gantt":    render_gantt,
    "markdown": render_markdown,
}


def render(nodes: list, ctx, fmt: str, root_id: Optional[str] = None) -> str:
    fn = RENDERERS.get(fmt)
    if fn is None:
        raise ValueError(f"unknown render format: {fmt!r} (try {list(RENDERERS)})")
    if fmt == "ascii":
        return fn(nodes, ctx, root_id)
    return fn(nodes, ctx)
