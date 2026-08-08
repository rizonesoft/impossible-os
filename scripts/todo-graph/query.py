#!/usr/bin/env python3
# ============================================================================
# scripts/todo-graph/query.py -- TODO graph query CLI (TODO-06 §4).
#
# Consumes build/todo-cache.json produced by build.py. Thirteen human-facing
# subcommands that answer "what can I work on?", "what references this?",
# and "what is going stale?" without hand-grepping.
#
# Stdlib-only. inotifywait is probed via shutil.which() and used as a
# subprocess when present; otherwise --watch falls back to 2s mtime polling.
#
# Subcommands -- this list is the documented inventory and must match
# SUBCOMMANDS below. It said "ten", listed twelve, and omitted `render` while
# the dispatch table defined thirteen (Codex consistency, section 24,
# [medium]); the per-verb history selector in `_profile_for` is keyed off this
# same set, so drift here is how a timestamp consumer goes uncharged.
# The three marked (history) dereference a git-derived timestamp and are the
# ones routed through PROFILE_QUERY_HISTORY.
#   ready        -- draft TODOs whose deps are all done
#   blocked      -- active TODOs with at least one unmet dep
#   blocking     -- TODOs ranked by inbound depends_on count (critical path)
#   by-domain    -- all TODOs grouped by domain + first unfinished section
#   backlinks    -- every TODO that references <id> (any edge kind)
#   deferred     -- outbound Accepted/Deferred stamps from <id>
#   deferred-by  -- inbound Accepted/Deferred stamps pointing at <id>
#   orphans      -- TODOs with zero inbound edges of any kind
#   stale        -- TODOs whose last_active_at is older than N days (default 90)
#                   (history)
#   stats        -- repo-wide summary (total, by-status, by-domain, top-N)
#                   (history: the longest-deferred table reads last_active_at)
#   code         -- source paths claimed by <id>'s file_patterns + Notes grep
#   code-by      -- reverse: TODOs whose file_patterns match <path>
#   render       -- mermaid / dot / ascii / gantt / markdown views of the graph
#                   (history: ONLY --render-format gantt, which uses created_at)
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
# Output bounds (row-returning subcommands; stats + render are exempt):
#   --limit N            max rows (default 50; `0` = complete set, CLI only)
#   --offset N           skip N rows -- pages are re-runnable (total order)
#   --scope DOMAIN       restrict to one domain (verbs with a domain column)
#   --fields a,b,c       return only these columns
#   --max-bytes N        hard ceiling (default 24000); breaching it FAILS
#                        CLOSED with the bound + total_matching + the exact
#                        narrowing flags, never a silent partial set
#
# --json responses carry returned / total_matching / truncated (+ limit,
# offset, and `next` when truncated) alongside `rows`, so a page can never
# be mistaken for the complete set. TSV/markdown stay pure columnar data;
# their envelope goes to stderr, and a TRUNCATED result is announced there
# even under --quiet.
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
import io
import json
import os
import re
import shutil
import signal
import stat
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
    build_id_index,
    build_path_index,
    resolve_xref_target,
)
# THE SHARED CACHE-SCHEMA VALIDATOR (section 17 / 19 / 22). `load_or_rebuild_cache`
# is deliberately NO LONGER IMPORTED: it ends at `validate.py:169` with a bare
# `json.loads(cache_path.read_text())`, which has none of the bounded read, the
# fstat generation binding, the freshness comparison or the corpus fingerprint
# this module needs. Validating the nodes it returned would have been routing in
# name only -- a well-shaped STALE cache still served, and a cache rewritten
# between the read and the answer still published (Codex design review, section
# 22, [high]). `validate.py` keeps that helper for its own recovery path, which
# is section 23's to harden.
import cache_schema as _cs  # noqa: E402


# ----------------------------------------------------------------------
# Constants
# ----------------------------------------------------------------------

DEFAULT_STALE_DAYS = 90
POLL_INTERVAL_SECONDS = 2.0

# ----------------------------------------------------------------------
# Output bounding (owner TODO-06, query-surface bound)
#
# Every row-returning subcommand scales with the node count, so an
# unbounded result set lands whole in whatever consumes it -- including a
# model context via mcp_server.py. These constants bound what comes OUT
# of the index; nothing about the index itself changes.
#
# DEFAULT_ROW_LIMIT applies when --limit is absent. `--limit 0` is an
# explicit, CLI-only request for the complete set: it is how a human or a
# script says "I want all of it" (and how the completeness assertions in
# tests/test_build.sh opt back in). The MCP surface clamps into
# [1, MCP_MAX_ROW_LIMIT] and can never reach the unlimited form.
#
# BYTES_PER_TOKEN is a deliberately crude divisor. It is not a tokenizer;
# it is a stable, uniformly-applied estimator so before/after numbers are
# comparable to each other.
# ----------------------------------------------------------------------
DEFAULT_ROW_LIMIT = 50
UNLIMITED_ROW_LIMIT = 0
# Chosen so a max-limit request FITS under OUTPUT_CEILING_BYTES for every
# verb rather than tripping the fail-closed path. The widest row measured
# is by-domain at ~193 bytes, so 24000 bytes holds ~124 rows; 100 leaves
# headroom. A cap above that would make "ask for the most you may" a
# reliable way to get zero rows back and force a second call -- the
# ceiling should be a backstop for pathological rows, not the normal path.
MCP_MAX_ROW_LIMIT = 100
OUTPUT_CEILING_BYTES = 24000
BYTES_PER_TOKEN = 3.9

# Ceiling on the `code` verb's Notes scan of a cache-named file. 16 MiB is the
# same bound `resolve_symbol` puts on a source file, and ~80x the largest TODO
# in the corpus, so it refuses nothing real while keeping a hostile or
# mistyped `file_path` from being read without limit.
NOTES_READ_MAX_BYTES = 16 * 1024 * 1024

# Subcommands exempt from row bounding, with the reason each is exempt:
#   stats  -- shape is O(taxonomy), not O(nodes): scalars plus dicts keyed
#             by status/domain plus two hardcoded [:3] top-N lists. It
#             cannot grow with the tree, and tests/test_build.sh 9l pins
#             its seven top-level keys, so it stays unwrapped too.
#   render -- returns one self-delimiting diagram string, not rows, and is
#             deliberately absent from the MCP tool surface. It already
#             takes --scope, and commonly writes to --output <file>.
UNBOUNDED_SUBCOMMANDS = frozenset({"stats", "render"})

# Severity rank for the two stamp verbs (deferred / deferred-by). These
# answer "what is blocking me", so the most severe rows must sort FIRST:
# a truncated page that dropped a Critical while keeping Mediums would be
# a bound that hides a blocker, which is worse than no bound at all.
# Measured on the live tree: `deferred native-api-ssdt` returns 58 rows of
# which 6 are Critical, and a severity-blind first-50 page lost 2 of them.
# Unknown / empty severities sort last but are never dropped ahead of a
# ranked one. Vocabulary observed in the cache: Critical, H, M, L, "".
SEVERITY_RANK = {"critical": 0, "h": 1, "high": 1, "m": 2, "medium": 2,
                 "l": 3, "low": 3}
SEVERITY_UNKNOWN_RANK = 4


def _severity_rank(v) -> int:
    return SEVERITY_RANK.get(str(v or "").strip().lower(), SEVERITY_UNKNOWN_RANK)

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


# EXIT CODES. rc 2 is the INFRASTRUCTURE code -- the cache exists and cannot be
# trusted -- and it already meant exactly that here (the non-list guard in
# `_build_ctx` has exited 2 since the CLI shipped). rc 3 stays the ceiling
# breach, which is a bounded successful answer rather than a refusal, and rc 1
# stays a real verdict. Keeping the refusal on 2 means no consumer's existing
# exit-code handling changes meaning.
EXIT_CACHE_UNUSABLE = 2

# rc 4 is THE REQUEST, not the cache. The command handlers exited 2 for an
# unresolvable target, an ambiguous name, an unknown `--fields` column and an
# out-of-range `--offset`, which collided head-on with the documented meaning
# of 2: a caller told "the cache exists and cannot be trusted" was in fact
# looking at a perfectly good cache and a typo in its own argv, and the two
# were indistinguishable because the input errors wrote no body at all.
# Splitting the code is what lets a consumer act differently -- rebuild the
# cache versus fix the request (Codex consistency, section 22 review, [medium]).
EXIT_QUERY_INPUT = 4


class QueryInputError(Exception):
    """The REQUEST cannot be answered; the cache is fine -- PROBABLY.

    CARRIES its reason and detail instead of emitting them, because whether
    the cache is fine is not knowable at the raise site. Target resolution and
    ambiguity checks run INSIDE the walk, before `_run_once`'s post-walk
    `check_corpus_unchanged`, so a corpus edit landing mid-walk can make a
    perfectly good target look absent. Emitting there published "the cache is
    usable; correct the request" over what was actually a stale generation --
    telling the caller to fix a request that was never wrong, and skipping the
    generation binding that is this section's headline invariant. The caller
    re-verifies first and decides which envelope is true (Codex
    re-adversarial, section 22 review round 7, [medium]).

    Raised rather than `sys.exit()` for the same reason `CacheRefused` is, and
    the omission was the bug: ten command-handler `sys.exit(2)` calls unwound
    straight through `_run_once_guarded` (which catches `CacheRefused`,
    `CeilingExceeded`, `RecursionError` and `MemoryError`, but never
    `SystemExit`) and KILLED the watcher. So `backlinks <id> --watch` on a
    target that is later renamed -- an ordinary, valid edit -- ended the
    session, which is precisely the interactive-tool failure this section
    pinned watch-mode behaviour to avoid (Codex adversarial, section 22
    review, [medium]).
    """

    def __init__(self, reason: str, message: str, quiet: bool = False):
        super().__init__(message)
        self.reason = reason
        self.detail = message
        self.quiet = quiet
        # Set once the body has been written, so the guard can emit the ones
        # that never passed through the post-walk check (the argv-shape errors
        # from `bound_rows`, which do not depend on cache CONTENT and so have
        # nothing to re-verify) without double-printing the ones that did.
        self.emitted = False


def _write_input_error(exc: "QueryInputError") -> None:
    """Emit the machine-readable request-error body on stdout.

    Same stdout discipline as `_write_refusal`, and for the same transport
    reason: `mcp_server.py` captures stdout in-process, so a stderr-only
    message leaves the buffer empty. The `error` key is what makes the MCP
    layer classify this as a failure rather than a result.
    """
    sys.stdout.write(json.dumps({
        "error": "query-input",
        "reason": exc.reason,
        "detail": exc.detail,
        "hint": "the cache is usable; correct the request and re-run",
    }, indent=2, sort_keys=True) + "\n")
    if not exc.quiet:
        sys.stderr.write(f"[query.py] error: {exc.reason}: {exc.detail}\n")
    exc.emitted = True


def _input_error(reason: str, message: str, quiet: bool = False):
    """Raise a request error. Emission is the CALLER's, after re-verification."""
    raise QueryInputError(reason, message, quiet)


class CacheRefused(Exception):
    """The cache cannot be trusted, so no answer is produced.

    RAISED RATHER THAN `sys.exit()`, which matters for exactly one caller:
    `--watch`. A `sys.exit(2)` inside a tick unwinds straight through
    `watch_loop` and KILLS the watcher, so a single half-saved TODO file would
    end an interactive session -- while the refusal is supposed to be a
    per-tick event the next good edit clears. `_run_once_guarded` maps this to
    rc 2 for the single-shot and MCP transports, and the watch tick simply
    continues; one refusal path, three transport behaviors, no duplicated
    envelope.
    """


def _write_refusal(reason: str, message: str, quiet: bool) -> None:
    """Emit the MACHINE-READABLE refusal body on stdout.

    The body goes to STDOUT, not just stderr, and that is load-bearing rather
    than cosmetic: `mcp_server.py` runs `main()` in-process with stdout
    redirected into a buffer, so a refusal written only to stderr would leave
    that buffer EMPTY -- which is precisely the `buf.getvalue() or "[]"` path
    that turned a refusal into a successful-looking empty answer. Writing the
    envelope here means every transport gets the same body from one place.

    Split from `_refuse` because the resource handlers in `_run_once_guarded`
    need the body WITHOUT the exception: they are already unwinding, and
    raising from inside an `except` would escape the guard it is standing in.
    """
    sys.stdout.write(json.dumps({
        "error": "cache-unusable",
        "reason": reason,
        "detail": message,
        "hint": "rebuild with bash scripts/todo-graph/build-and-validate.sh",
    }, indent=2, sort_keys=True) + "\n")
    if not quiet:
        sys.stderr.write(f"[query.py] FATAL: {reason}: {message}\n")


def _refuse(reason: str, message: str, quiet: bool):
    """Write the refusal body, then raise `CacheRefused` to abandon the walk."""
    _write_refusal(reason, message, quiet)
    raise CacheRefused(message)


class Ctx:
    __slots__ = (
        "nodes", "id_index", "path_index", "slug_index", "slug_collisions",
        "stem_collisions", "inbound", "repo_root", "quiet", "pre_notice_fired",
        "node_by_path", "ambiguous_edges", "dep_notice_fired",
        "dep_edges_empty",
    )

    def __init__(self, nodes, repo_root, quiet):
        # NODES ARRIVE VALIDATED. The old `_validate_nodes` silently dropped
        # rows that were not dicts or lacked `file_path`, so every answer below
        # -- stats, ready, backlinks -- could be computed from a SUBSET of the
        # corpus and still look complete. The shared validator REFUSES what that
        # one discarded; see `_build_ctx`.
        self.nodes = nodes
        # file_path -> node. The dependency walks resolved an id to a path and
        # then LINEAR-SCANNED `nodes` for it, once per edge, which is O(n^2)
        # over the graph. `file_path` uniqueness is guaranteed by the shared
        # validator, so this map is exact rather than best-effort.
        self.node_by_path = {n["file_path"]: n for n in self.nodes}
        self.id_index = build_id_index(self.nodes)
        self.path_index = build_path_index(self.nodes)
        self.slug_index, self.slug_collisions = build_slug_index(self.nodes)
        self.stem_collisions = _build_stem_collisions(self.nodes)
        self.repo_root = repo_root
        self.quiet = quiet
        self.pre_notice_fired = False
        self.dep_notice_fired = False
        # None until a readiness verb asks; then True/False. Carried
        # into the --json envelope so a transport that drops stderr
        # still receives the caveat.
        self.dep_edges_empty = None
        # Edge tokens naming a BARE stem that more than one file carries are
        # collected, not silently bound to whichever file the flattened index
        # happened to keep. `_build_ctx` turns a non-empty list into a refusal.
        self.ambiguous_edges = []
        self.inbound = collect_inbound_edges(
            self.nodes, self.id_index, self.path_index,
            self.stem_collisions, self.ambiguous_edges)

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

    def dependency_source_notice_once(self):
        """Say what `ready` / `blocked` / `blocking` ranked against.

        AN EMPTY ANSWER MUST NOT READ AS A VERDICT (TODO-06 section 25). These
        three verbs consult the FILE-level `depends_on` frontmatter key and
        nothing else, and as measured on 2026-08-08 zero of the 232 live TODOs
        author it -- so `blocked`
        and `blocking` return nothing for every possible corpus, and a caller
        cannot tell "no TODO is blocked" from "this ranking has no edges to
        rank". That is the same conflation section 22 removed from the refusal
        path, arriving here through a silent default instead of a dropped row.
        The counterpart is the docs entry naming the OTHER dependency evidence
        and why it is deliberately not consulted; see
        docs/infrastructure/todo-metadata.md ("Which dependency evidence the
        readiness verbs consult"). NOT todo-graph.md, which is the GENERATED
        mermaid artifact rather than a prose doc.

        `stats` INHERITS IT, and that is wanted rather than incidental: it
        reuses `cmd_blocking` for `top_blocking` and reports `avg_dep_depth`,
        both computed from the same empty edge set, so its 0.0 needs the same
        caveat its source does.

        Fires ONCE per invocation and only when the edge set is EMPTY. A
        corpus that authors the field gets the answer it asked for with no
        commentary, so this cannot become noise that trains a reader to skip
        stderr -- it appears exactly while the degradation it describes is
        real. It stays on stderr and under `--quiet`, so no machine-readable
        stdout changes shape (`--json` consumers are untouched).
        """
        if self.dep_notice_fired:
            return
        self.dep_notice_fired = True
        # RECORD BEFORE THE `quiet` GATE, and this ordering is the whole fix.
        # The note was written to stderr and suppressed by `--quiet`, and
        # `mcp_server.py` invokes this CLI with `--quiet` unconditionally and
        # reads only stdout -- so an agent asking `blocked` over MCP got an
        # ordinary empty success with no caveat at all, which is precisely the
        # ambiguity this note exists to remove and precisely the transport
        # section 22 hardened for the same reason (Codex adversarial, [high]).
        # The flag is what `_bounded_page` puts into the `--json` envelope.
        self.dep_edges_empty = not any(
            _safe_list(n.get("depends_on")) for n in self.nodes)
        if not self.dep_edges_empty or self.quiet:
            return
        sys.stderr.write(
            "[query.py] note: ranked against 0 file-level `depends_on` edges "
            f"({len(self.nodes)} nodes, none authoring the field), so this "
            "answer describes the absence of a graph rather than the state of "
            "one. Section-level dependencies are NOT consulted here and that "
            "is deliberate: their targets are unqualified (`TODO-05` matches "
            "one file per domain). See docs/infrastructure/todo-metadata.md "
            "\"Which dependency evidence the readiness verbs consult\".\n")


def _build_stem_collisions(nodes: list) -> dict:
    """Return {stem: [file_paths]} for every filename stem that appears
    more than once. Mirrors build_slug_index's collision tracking --
    validate.build_path_index silently overwrites on duplicate stems,
    so we track ambiguity separately here and refuse stem-based
    resolution when >1 file shares the stem."""
    seen: dict = {}
    for n in nodes:
        rel = n.get("file_path")
        if not rel:
            continue
        stem = Path(rel).stem
        seen.setdefault(stem, []).append(rel)
    return {s: paths for s, paths in seen.items() if len(paths) > 1}


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


def _normalize_ref_token(raw) -> str:
    """The ONE normalization both ambiguity classification and resolution use.

    These were two ordered strip chains that had to be kept identical, and they
    were not. `_note_ambiguous` stripped backticks then commas in a single
    pass, while `_resolve_edge_target` hands its result to
    `resolve_xref_target`, which strips backticks AGAIN -- so the realistic
    token `` `TODO-A-Shared.md`, `` kept a trailing backtick during
    classification (recording no ambiguity) and lost it during resolution
    (returning None for the duplicate stem). The edge was then silently omitted
    at rc 0 rather than refused (Codex adversarial, section 22 round 7, [high]).

    Looping to a fixed point is what makes the order irrelevant: any
    interleaving of backticks, commas, whitespace, an `#anchor` and a trailing
    slash converges on the same token.
    """
    if raw is None:
        return ""
    token = str(raw)
    while True:
        before = token
        token = token.strip().strip("`").strip(",").rstrip("/")
        if "#" in token:
            token = token.split("#", 1)[0]
        if token == before:
            return token


def _edge_uses_ambiguous_stem(token: str, stem_collisions: dict) -> Optional[str]:
    """Return the ambiguous stem when `token` is a BARE filename reference to a
    stem more than one file carries, else None.

    `validate.build_path_index` flattens `{stem: file_path}`, so when two files
    share a stem only the LAST survives and every bare-stem edge naming either
    one resolves to that survivor -- at rc 0, with the other file showing no
    inbound edge at all. REPRODUCED with `todo/01-a/TODO-A-Shared.md` and
    `todo/02-b/TODO-A-Shared.md`: `backlinks first` returned 0 rows and
    `backlinks second` returned 1, for an Inputs XREF naming the bare stem
    (Codex adversarial, section 22 round 4, [high]).

    `_build_stem_collisions` already computes this map and its docstring claims
    the reader "refuses stem-based resolution when >1 file shares the stem" --
    but the only consumer was `resolve_id_to_node`, i.e. the target a HUMAN
    types. Edge resolution never asked, which is why the graph could be wrong
    while the CLI politely refused the same ambiguous name.

    ONLY BARE STEMS ARE AMBIGUOUS. Duplicate stems across domains are a
    supported layout, and a domain-qualified reference (`01-a/TODO-A-Shared.md`)
    or an id names exactly one file -- so refusing those would be a false
    positive on a corpus that is doing nothing wrong.
    """
    if not stem_collisions or "/" in token or "\\" in token:
        return None
    stem = token[:-3] if token.endswith(".md") else token
    return stem if stem in stem_collisions else None


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
    t = _normalize_ref_token(raw)
    if not t:
        return None
    return resolve_xref_target(t, source_file, id_index, path_index)


def collect_inbound_edges(nodes: list, id_index: dict, path_index: dict,
                          stem_collisions: Optional[dict] = None,
                          ambiguous: Optional[list] = None) -> dict:
    """Precompute inbound edges keyed by target file_path.

    Returns {file_path: [edge_dict, ...]} where each edge_dict has:
      - source:  file_path of the referencing node
      - kind:    one of EDGE_* constants
      - section: Optional[str] -- the `§N` suffix when present

    Self-references are excluded so a TODO's own internal XREFs don't
    make it look inbound-popular.

    When `stem_collisions` and `ambiguous` are supplied, every edge token that
    names a BARE ambiguous stem is appended to `ambiguous` as
    `(source, token, [candidate paths])`. Collected rather than raised here so
    the caller reports ALL of them at once instead of one per run -- and so
    this function stays a pure index builder."""
    inbound: dict = {}
    for n in nodes:
        src = n["file_path"]

        def _note_ambiguous(raw):
            if ambiguous is None or raw is None:
                return
            token = _normalize_ref_token(raw)
            stem = _edge_uses_ambiguous_stem(token, stem_collisions)
            if stem:
                ambiguous.append((src, token, stem_collisions[stem]))

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
            _note_ambiguous(x.get("target_path"))
            tgt = _resolve_edge_target(x.get("target_path"), src, id_index, path_index)
            _add(tgt, EDGE_INPUTS, x.get("target_section"))

        # Stamp XREFs (Accepted / Deferred)
        for x in _safe_list(n.get("stamps_xrefs")):
            if not isinstance(x, dict):
                continue
            _note_ambiguous(x.get("target_path"))
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
                _note_ambiguous(tgt_tok)
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
    # 2. Filename stem (with ambiguity check -- validate.build_path_index
    # silently overwrites on duplicate stems, so we refuse here rather
    # than silently returning whichever node was indexed last).
    if not fp:
        stem_collisions = ctx.stem_collisions.get(t)
        if stem_collisions and len(stem_collisions) > 1:
            _input_error(
                "AMBIGUOUS_STEM",
                f"id '{t}' matches multiple filename stems ("
                + ", ".join(stem_collisions)
                + "); disambiguate by passing the full file_path",
                ctx.quiet)
        fp = ctx.path_index["by_filename"].get(t)
    # 3. Slug (with ambiguity check)
    if not fp:
        collisions = ctx.slug_collisions.get(t)
        if collisions and len(collisions) > 1:
            _input_error(
                "AMBIGUOUS_SLUG",
                f"id '{t}' is ambiguous; matches "
                + ", ".join(collisions)
                + "; disambiguate by passing the full file_path",
                ctx.quiet)
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
    """TODOs whose every declared dependency is done.

    THE DEPENDENCY SOURCE IS THE FILE-LEVEL `depends_on` FRONTMATTER KEY, and
    section 25 pinned that deliberately rather than leaving it as the state
    nobody had chosen. The cache also carries `sections[].depends_on` -- 2,529
    groups from the Implementation Order tables, far richer than the file-level
    field, which zero live TODOs author. Redirecting these verbs at it was
    considered and REJECTED on two measurements: 1,806 of those groups target
    `self`, and of the 145 distinct cross-file targets, 26 name a bare
    `TODO-NN` that matches MORE THAN ONE file (`TODO-01` matches 18). The two
    fields are also different NAMESPACES -- frontmatter `depends_on` holds TODO
    ids, section groups hold filename stems -- so this is not a source swap but
    an unresolved-reference problem, and qualified resolution is section 26's.

    Until that lands the honest behaviour is to consult one source and SAY so,
    which `dependency_source_notice_once` does. See
    docs/infrastructure/todo-graph.md.
    """
    ctx.dependency_source_notice_once()
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
    """Active TODOs with an unmet dependency. Same single source as `cmd_ready`
    and the same section 25 reasoning; the notice fires when there are no edges
    to rank, because an unconditionally empty answer here reads exactly like
    "nothing is blocked"."""
    ctx.dependency_source_notice_once()
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
    """TODOs ranked by inbound dependency count. Same single source as
    `cmd_ready`; see section 25. This is the verb the OS Comparison table
    advertises as the critical-path rank, so an empty result that looks like a
    verdict is the most misleading of the three."""
    ctx.dependency_source_notice_once()
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
        _input_error(
            "TARGET_NOT_FOUND",
            f"id '{args.target}' not found. Try: {_id_hint(args.target, ctx)}",
            ctx.quiet)
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
    # Total order: the trailing column is appended so a bounded page is
    # reproducible across cache rebuilds, not merely within one run.
    rows.sort(key=lambda r: (r["domain"], r["id"], r["kind"],
                             str(r.get("section") or "")))
    return rows, ["domain", "id", "kind", "section"]


def cmd_deferred(ctx: Ctx, args) -> tuple:
    node = resolve_id_to_node(args.target, ctx)
    if not node:
        _input_error(
            "TARGET_NOT_FOUND",
            f"id '{args.target}' not found. Try: {_id_hint(args.target, ctx)}",
            ctx.quiet)
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
    # Severity FIRST so a bounded page cannot drop a Critical while
    # keeping a Medium; remaining columns give a total order so the page
    # is reproducible across cache rebuilds.
    rows.sort(key=lambda r: (_severity_rank(r.get("severity")),
                             r["kind"], r["target"], r["section"],
                             str(r.get("item_name") or "")))
    return rows, ["kind", "severity", "target", "section", "item_name"]


def cmd_deferred_by(ctx: Ctx, args) -> tuple:
    node = resolve_id_to_node(args.target, ctx)
    if not node:
        _input_error(
            "TARGET_NOT_FOUND",
            f"id '{args.target}' not found. Try: {_id_hint(args.target, ctx)}",
            ctx.quiet)
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
    # Severity FIRST (see cmd_deferred): an inbound blocker must survive
    # truncation. Remaining columns give a total, reproducible order.
    rows.sort(key=lambda r: (_severity_rank(r.get("severity")),
                             r["source"], r["kind"], r["section"],
                             str(r.get("item_name") or "")))
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
            if days <= 0:
                rows.append({
                    "domain": n.get("domain") or "",
                    "id": display_id(n),
                    "last_active_at": n.get("last_active_at") or "",
                    "age_days": 0,
                })
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
    the stack set (contributes 0 for the back-edge).

    ITERATIVE, NOT RECURSIVE, AND THE DIFFERENCE IS REACHABLE (Codex
    adversarial, section 22, [high]). The recursive form recursed once per link,
    so a cache describing a chain longer than Python's recursion limit raised
    `RecursionError` -- which `_run_once_guarded` did not catch, so the CLI
    exited **1**, the code this reader documents as a real VERDICT, with an
    empty body. A 2,000-node chain reproduces it.

    IT HID BEHIND NODE ORDER, which is why it survived this long: `cmd_stats`
    walks `ctx.nodes` in order, so a cache listing the chain root FIRST fills
    `memo` bottom-up and never nests more than one frame deep. Reverse the same
    2,000 nodes -- an ordering an untrusted cache picks for itself -- and the
    first call descends the entire chain. Bounding the input depth instead
    would mean choosing an arbitrary limit for a shape the producer may
    legitimately emit; an explicit stack has no limit to choose.
    """
    fp = node["file_path"]
    if fp in memo:
        return memo[fp]
    # Each frame is [node, targets-or-None, best-so-far, next-child-index]. A
    # frame is EXPANDED once (targets None -> list), then drained one child at
    # a time via the INDEX, then resolved -- the recursive control flow written
    # out.
    #
    # THE INDEX IS NOT A STYLE CHOICE. The first version of this rewrite did
    # `pending = pending[1:]` per child, which copies the remaining list every
    # step and turns a node with k dependencies from O(k) into O(k^2). A valid
    # 20,000-leaf star measured 1.36s that way against 0.03s recursively, and
    # the schema puts no `maxItems` on `depends_on` while the cache ceiling is
    # 64 MiB -- so far wider graphs are accepted. Under MCP the `_CALL_LOCK`
    # makes one such `stats` call block every other query (Codex adversarial,
    # section 22 round 2, [medium]).
    frames = [[node, None, 0, 0]]
    while frames:
        cur, pending, best, idx = frames[-1]
        cur_fp = cur["file_path"]
        if pending is None:
            if cur_fp in memo:
                frames.pop()
                if frames:
                    frames[-1][2] = max(frames[-1][2], memo[cur_fp] + 1)
                continue
            if cur_fp in stack:
                # Back-edge: contributes 0 to the ancestor, so the edge itself
                # still counts as 1. Same as the recursive `0 + 1`.
                frames.pop()
                if frames:
                    frames[-1][2] = max(frames[-1][2], 1)
                continue
            stack.add(cur_fp)
            targets = []
            for ref in _safe_list(cur.get("depends_on")):
                tgt_path = ctx.id_index.get(ref)
                if not tgt_path:
                    continue
                tgt = ctx.node_by_path.get(tgt_path)
                if tgt is not None:
                    targets.append(tgt)
            frames[-1][1] = targets
            continue
        if idx < len(pending):
            frames[-1][3] = idx + 1
            frames.append([pending[idx], None, 0, 0])
            continue
        stack.discard(cur_fp)
        memo[cur_fp] = best
        frames.pop()
        if frames:
            frames[-1][2] = max(frames[-1][2], best + 1)
    return memo.get(fp, 0)


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

    # top_longest_deferred: rank nodes with >=1 genuinely-outbound
    # kind=deferred stamp (resolves to a DIFFERENT node) by oldest
    # last_active_at. Uses the same outbound-resolution filter as
    # cmd_deferred so `stats` and `deferred <id>` cannot contradict each
    # other (unresolved or self-pointing stamps don't qualify).
    deferred_nodes = []
    for n in ctx.nodes:
        has_outbound_deferred = False
        for x in _safe_list(n.get("stamps_xrefs")):
            if not isinstance(x, dict):
                continue
            if x.get("kind") != "deferred":
                continue
            tgt = _resolve_edge_target(
                x.get("target_path"), n["file_path"], ctx.id_index, ctx.path_index,
            )
            if tgt and tgt != n["file_path"]:
                has_outbound_deferred = True
                break
        if not has_outbound_deferred:
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
        _input_error(
            "TARGET_NOT_FOUND",
            f"id '{args.target}' not found. Try: {_id_hint(args.target, ctx)}",
            ctx.quiet)
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
    #
    # THE BOUNDARY IS NOT ENOUGH ON ITS OWN, and that gap was real: an
    # explicitly-supplied `--cache` is validated for SHAPE but deliberately not
    # for freshness against THIS corpus, so a node's `file_path` is
    # attacker-influenced text that only has to stay repo-LOCAL. A FIFO at that
    # path blocks `read_text()` forever and a multi-gigabyte regular file
    # exhausts memory -- both before any ceiling this reader enforces. So the
    # read is bounded the same way `cache_schema` bounds its own: stat the
    # opened descriptor, require a regular file, and cap the bytes (Codex
    # adversarial, section 22 review, [medium]).
    text = ""
    try:
        resolved = (ctx.repo_root / node["file_path"]).resolve(strict=False)
        repo_resolved = ctx.repo_root.resolve(strict=False)
        resolved.relative_to(repo_resolved)
        # O_NONBLOCK IS THE LOAD-BEARING FLAG, and getting this order wrong is
        # how the first attempt at this fix defended nothing: a plain
        # `open(path, "rb")` on a FIFO with no writer BLOCKS INSIDE THE OPEN,
        # so an `fstat` guard placed after it is unreachable and the hang it
        # was written to prevent happens at the same line as before. Opening
        # non-blocking returns a descriptor immediately for every file type,
        # and the fstat then rejects anything that is not a regular file.
        # `fstat` on the DESCRIPTOR (not a path stat) is what makes this
        # TOCTOU-free: the object measured is the object read.
        fd = os.open(resolved, os.O_RDONLY | os.O_NONBLOCK)
        with os.fdopen(fd, "rb") as fh:
            st = os.fstat(fh.fileno())
            if not stat.S_ISREG(st.st_mode):
                raise OSError(f"not a regular file: {resolved}")
            if st.st_size > NOTES_READ_MAX_BYTES:
                raise OSError(
                    f"{resolved} is {st.st_size} bytes, over the "
                    f"{NOTES_READ_MAX_BYTES}-byte notes-scan ceiling")
            text = fh.read(NOTES_READ_MAX_BYTES).decode("utf-8", errors="replace")
    except (OSError, ValueError):
        # ValueError: path escapes repo_root; OSError: file missing, unreadable,
        # not a regular file, or over the ceiling.
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


def cmd_render(ctx: Ctx, args) -> tuple:
    """Dispatch to scripts/todo-graph/render.py for the requested format.
    Returns (rendered_text, None) so emit() skips its columnar path and
    writes the string verbatim (render formats are self-delimiting)."""
    import render as _render_mod  # sibling module; added in TODO-06 §7
    scope = getattr(args, "scope", None) or None
    fmt = getattr(args, "render_format", None) or "mermaid"
    target = getattr(args, "target", None) or None  # ascii root id
    nodes = _render_mod.filter_by_scope(ctx.nodes, scope)
    text = _render_mod.render(nodes, ctx, fmt, root_id=target)
    # Signal RAW_TEXT output via a sentinel columns value; emit() handles.
    return text, "__RAW_TEXT__"


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
    stats). fmt is 'tsv' | 'json' | 'markdown'.

    Sentinel `columns == "__RAW_TEXT__"` means `rows` is already a
    fully-rendered string (e.g. mermaid / dot / ascii output from the
    render subcommand); write it verbatim without format translation."""
    if columns == "__RAW_TEXT__":
        sys.stdout.write(rows if isinstance(rows, str) else str(rows))
        return
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
# Output bounding
#
# One choke point for every row-returning subcommand: scope filter, then
# field projection, then offset/limit, then a hard byte ceiling. Verbs in
# UNBOUNDED_SUBCOMMANDS bypass all of it (see the constant for why).
# ----------------------------------------------------------------------

def est_tokens(n_bytes: int) -> int:
    """Uniform crude estimator. Not a tokenizer -- a stable divisor so the
    before/after numbers in the bounding report are comparable."""
    return int(n_bytes / BYTES_PER_TOKEN)


class CeilingExceeded(Exception):
    """Raised instead of emitting a silently-truncated set. Carries the
    envelope that main() writes before returning a nonzero exit code."""

    def __init__(self, envelope: dict):
        super().__init__(envelope.get("error", "output-ceiling-exceeded"))
        self.envelope = envelope


def _narrowing_flags(columns, scope_supported: bool, limit: int) -> list:
    """The exact flags a caller can add to bring an over-ceiling query
    under it. Returned in the failure envelope so the next call is
    mechanical rather than guesswork."""
    flags = [f"--limit {max(1, limit // 2)}"]
    if scope_supported:
        flags.append("--scope <domain>")
    if columns:
        keep = ",".join(list(columns)[:2])
        flags.append(f"--fields {keep}")
    flags.append("--offset N  (page through the remainder)")
    return flags


def _apply_scope(rows, columns, scope: str, subcommand: str, quiet: bool = False):
    """Filter rows to a single domain. Fails closed when the subcommand
    exposes no domain column: silently ignoring --scope would let a caller
    believe they narrowed a query that in fact returned everything."""
    if "domain" not in (columns or []):
        _input_error(
            "SCOPE_UNSUPPORTED",
            f"--scope is not supported by '{subcommand}' (no domain column; "
            f"columns are: {','.join(columns or [])}). Supported on: ready, "
            f"blocked, blocking, by-domain, backlinks, orphans, stale, code-by",
            quiet)
    return [r for r in rows if (r.get("domain") or "") == scope]


def _apply_fields(rows, columns, fields: str, subcommand: str, quiet: bool = False):
    """Project rows onto a caller-chosen column subset. An unknown field
    is an error, not a silently-dropped request."""
    want = [f.strip() for f in fields.split(",") if f.strip()]
    unknown = [f for f in want if f not in (columns or [])]
    if unknown:
        _input_error(
            "UNKNOWN_FIELDS",
            f"unknown --fields for '{subcommand}': {','.join(unknown)}. "
            f"Valid columns: {','.join(columns or [])}", quiet)
    projected = [{k: r.get(k) for k in want} for r in rows]
    return projected, want


# ONE statement of the caveat, consumed by BOTH emission paths -- the bounded
# envelope and the unbounded `stats` dict. Two copies is how one of them comes
# to say something the other does not.
_DEPENDENCY_SOURCE_CAVEAT = {
    "field": "depends_on",
    "level": "file",
    "edges": 0,
    "note": "ranked against 0 file-level depends_on edges: this describes the "
            "absence of a graph, not the state of one. Section-level "
            "dependencies are deliberately not consulted (their targets are "
            "unqualified). See docs/infrastructure/todo-metadata.md.",
}


def bound_rows(rows, columns, args, subcommand: str, ctx=None):
    """Apply scope -> fields -> offset/limit. Returns (rows, columns, meta).

    meta always carries returned / total_matching / truncated so a caller
    can never mistake a bounded page for the complete set."""
    total_before_scope = len(rows)
    # THE CALLER'S --quiet REACHES EVERY rc-4 PATH. These four are raised
    # AFTER the generation check, so they emit from the guard rather than from
    # `_run_once`; leaving `quiet` to its default there printed a human
    # diagnostic to stderr under `--quiet`, which the MCP transport always
    # passes. A consumer merging the streams then read JSON followed by prose,
    # and the same error class behaved differently depending on where in the
    # pipeline it was raised (Codex re-adversarial, section 22 review round 8).
    quiet = bool(getattr(args, "quiet", False))
    scope = getattr(args, "scope", None) or None
    if scope:
        rows = _apply_scope(rows, columns, scope, subcommand, quiet)

    if getattr(args, "fields", None):
        rows, columns = _apply_fields(rows, columns, args.fields, subcommand, quiet)

    total_matching = len(rows)

    limit = getattr(args, "limit", None)
    limit = DEFAULT_ROW_LIMIT if limit is None else int(limit)
    offset = int(getattr(args, "offset", None) or 0)
    if offset < 0:
        _input_error("BAD_OFFSET", "--offset must be >= 0", quiet)

    if limit == UNLIMITED_ROW_LIMIT:
        page = rows[offset:] if offset else rows
    else:
        if limit < 0:
            _input_error("BAD_LIMIT", "--limit must be >= 0", quiet)
        page = rows[offset:offset + limit]

    truncated = (offset + len(page)) < total_matching or offset > 0
    meta = {
        "subcommand": subcommand,
        "returned": len(page),
        "total_matching": total_matching,
        "truncated": bool(truncated),
        "limit": limit,
        "offset": offset,
        "scope": scope,
        "unbounded": limit == UNLIMITED_ROW_LIMIT,
    }
    if scope:
        meta["total_before_scope"] = total_before_scope
    # THE CAVEAT TRAVELS IN THE DATA, not only on stderr. `--quiet` is what
    # the MCP transport always passes, so a stderr-only note is invisible to
    # exactly the caller least able to notice an empty ranking is structural
    # (Codex adversarial, [high]). Present only when a readiness verb actually
    # consulted the graph and found no edges, so no other subcommand's
    # envelope changes shape.
    if ctx is not None and getattr(ctx, "dep_edges_empty", None):
        meta["dependency_source"] = _DEPENDENCY_SOURCE_CAVEAT
    return page, columns, meta


def _render_to_string(rows, columns, fmt: str) -> str:
    """Run the existing emitters into a buffer so the byte ceiling can be
    measured on the real output rather than an approximation."""
    buf = io.StringIO()
    saved = sys.stdout
    sys.stdout = buf
    try:
        emit(rows, columns, fmt)
    finally:
        sys.stdout = saved
    return buf.getvalue()


def emit_bounded(rows, columns, fmt: str, meta: dict, ceiling: int,
                 scope_supported: bool = True, quiet: bool = False) -> None:
    """Emit a bounded result, or raise CeilingExceeded.

    JSON carries the envelope inline, always, including for an empty
    result: that is the machine surface (and the only one MCP serves), so
    the completeness fields must travel with the data.

    TSV / markdown are COLUMNAR contracts -- callers run awk / cut / wc -l
    over them, so stdout stays pure data and the envelope goes to stderr.
    An inline trailer here corrupted `awk -F'\\t' '{print $1}' | sort -u`
    into seeing a second value. Suppression rule: --quiet hides the
    informational envelope, but a truncated result is announced even under
    --quiet, because a silently-truncated set reading as complete is the
    exact failure this layer exists to prevent."""
    if fmt == "json":
        # Only load-bearing fields travel. The envelope is a flat cost on
        # every response, so it dominates a small result set: carrying
        # `subcommand` (the caller already knows it) plus null/default
        # `scope` / `unbounded` cost ~68 bytes per call for nothing.
        # returned / total_matching / truncated are always present, as is
        # the limit+offset pair that makes a page re-runnable.
        envelope = {
            "returned": meta["returned"],
            "total_matching": meta["total_matching"],
            "truncated": meta["truncated"],
            "limit": meta["limit"],
            "offset": meta["offset"],
            "rows": rows,
        }
        if meta.get("scope"):
            envelope["scope"] = meta["scope"]
            envelope["total_before_scope"] = meta.get("total_before_scope")
        if meta.get("unbounded"):
            envelope["unbounded"] = True
        if meta.get("dependency_source"):
            envelope["dependency_source"] = meta["dependency_source"]
        if meta["truncated"]:
            envelope["next"] = (
                f"--limit {meta['limit']} --offset "
                f"{meta['offset'] + meta['returned']}"
            )
        body = json.dumps(envelope, indent=2, sort_keys=True) + "\n"
    else:
        body = _render_to_string(rows, columns, fmt)
        if not meta.get("unbounded") and (meta.get("truncated") or not quiet):
            label = "WARN: truncated" if meta.get("truncated") else "bounded"
            sys.stderr.write(
                f"[query.py] {label}: returned={meta['returned']} "
                f"total_matching={meta['total_matching']} "
                f"truncated={str(meta['truncated']).lower()} "
                f"limit={meta['limit']} offset={meta['offset']}"
                + ("  (add --limit 0 for the complete set)"
                   if meta.get("truncated") else "")
                + "\n"
            )

    n_bytes = len(body.encode("utf-8"))
    # `--limit 0` is the explicit, CLI-only "give me the complete set"
    # request, so the ceiling does not apply to it: a caller who asked for
    # everything must be able to receive everything, or the no-lost-answers
    # guarantee has no terminating path. The MCP surface cannot reach this
    # branch -- _clamp_limit maps 0 onto the default before argv is built.
    if n_bytes > ceiling and not meta.get("unbounded"):
        raise CeilingExceeded({
            "error": "output-ceiling-exceeded",
            "subcommand": meta["subcommand"],
            "ceiling_bytes": ceiling,
            "would_emit_bytes": n_bytes,
            "would_emit_est_tokens": est_tokens(n_bytes),
            "returned": 0,
            "total_matching": meta["total_matching"],
            "truncated": True,
            "limit": meta["limit"],
            "offset": meta["offset"],
            "narrow_with": _narrowing_flags(columns, scope_supported,
                                            meta["limit"] or DEFAULT_ROW_LIMIT),
            "hint": "no rows were emitted; this response is the bound, not a "
                    "partial answer. Re-run with one of narrow_with.",
        })
    sys.stdout.write(body)


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


# How long to wait for the NEXT event before deciding the burst is over. Long
# enough that a corpus-wide sweep lands as one batch (a 232-file rewrite
# finishes well inside it), short enough to stay imperceptible for the ordinary
# case of a single file being saved.
WATCH_SETTLE_SECS = 0.25

# Absolute ceilings on ONE drain, so coalescing can never postpone the answer
# indefinitely under a sustained event stream. Whichever is reached first ends
# the drain and the tick runs; anything still queued simply coalesces into the
# NEXT drain, so no event is lost -- only deferred by one tick.
WATCH_DRAIN_MAX_SECS = 2.0
WATCH_DRAIN_MAX_EVENTS = 5000

# Hard ceiling on ONE unterminated event record. An inotify line is a path plus
# an event-name list, so anything approaching this is malformed by
# construction -- and without the ceiling a producer emitting bytes and no
# newline grows the reader's buffer without limit. The idle read is
# deliberately deadline-FREE (the watcher must be able to block until something
# happens), so this bound is the only thing between a malformed stream and a
# watcher that consumes memory until it dies. Measured 2026-08-08 before the
# bound: 1.9 MB, 2.8 MB, 3.5 MB across three successive 50ms reads, still
# climbing.
WATCH_MAX_RECORD_BYTES = 64 * 1024


class _EventLineReader:
    """Line reader over a RAW descriptor, with its own buffer.

    SELECT AND A BUFFERED FILE OBJECT CANNOT BE MIXED, and doing it made the
    first version of this coalescing completely inert. `Popen(..., text=True)`
    hands back a buffered stream, so one `readline()` can pull an entire burst
    out of the kernel pipe into Python's buffer; `select()` on the underlying
    fd then reports NOTHING READABLE -- correctly, the pipe really is empty --
    while 49 complete lines sit in user space waiting. The drain returned 0 and
    every event still got its own full rebuild.

    Measured 2026-08-08 with the writer held open, which is the shape
    `inotifywait -m` actually has: 50 lines written, first line read, drain
    returned 0. The fixture that "proved" coalescing worked had CLOSED its
    writer, and the resulting EOF is what kept the descriptor readable -- so it
    passed while testing the one condition that never occurs live.

    Owning the buffer removes the split: readability is decided by what this
    class holds plus what `select` says about the fd, which are the same two
    places the data can be.
    """

    def __init__(self, fd: int):
        self.fd = fd
        # THE TWO STATES ARE HELD APART ON PURPOSE. `_ready` is finished
        # records; `_buf` is the in-progress tail and NEVER contains a newline
        # once `_absorb` has run. Keeping them in one buffer is what made the
        # size ceiling and the resync interfere with each other: the ceiling
        # measured finished records it had no business discarding, and the
        # resync could consume a good record's newline as the terminator of the
        # bad record before it. With the split, each rule can only reach the
        # bytes it is actually about.
        self._ready: list = []
        self._buf = b""
        self._eof = False
        # Set when a record blew the size ceiling: bytes up to and including
        # the NEXT newline belong to that discarded record and must be dropped,
        # not surfaced as a truncated phantom event.
        self._resyncing = False

    def _absorb(self, chunk: bytes) -> None:
        """Fold a raw read into `_ready` + `_buf`, applying resync and ceiling."""
        data = self._buf + chunk
        self._buf = b""
        if self._resyncing:
            idx = data.find(b"\n")
            if idx < 0:
                # Still inside the discarded record, and nothing in `data` can
                # be anything else -- drop it whole and stay in resync.
                return
            data = data[idx + 1:]
            self._resyncing = False
        *complete, tail = data.split(b"\n")
        # A RECORD OVER THE CEILING IS MALFORMED WHETHER OR NOT IT TERMINATED.
        # Bounding only the unfinished tail is not enough: an oversized record
        # whose closing newline arrives in the same read becomes a "complete"
        # line and is emitted as an event, so the ceiling would reject it only
        # when the writer happened to pause mid-record -- a timing accident,
        # not a rule. Filtering here makes the bound a property of the record.
        self._ready.extend(
            c.decode("utf-8", errors="replace") + "\n"
            for c in complete if len(c) <= WATCH_MAX_RECORD_BYTES)
        # `tail` is by construction the only unfinished record, so it is the
        # only thing the ceiling may judge -- and the finished records ahead of
        # it are already safe in `_ready`.
        if len(tail) > WATCH_MAX_RECORD_BYTES:
            self._resyncing = True
        else:
            self._buf = tail

    def _take_buffered_line(self) -> Optional[str]:
        if self._ready:
            return self._ready.pop(0)
        return None

    def next_line(self, timeout: Optional[float]) -> Optional[str]:
        """Return the next complete line, or None on timeout/EOF.

        `timeout=None` blocks indefinitely, which is what the outer watch loop
        wants while idle; a finite timeout is what the drain wants.

        A FINITE TIMEOUT IS ABSOLUTE, not per-read. Handing the same `timeout`
        to every `select` looks equivalent and is not: a writer producing bytes
        with no newline keeps the descriptor readable forever, so each read
        succeeds, `_take_buffered_line` never does, and this loop never returns
        to the caller whose deadline is supposed to bound it. The drain's
        ceiling is then unreachable -- the same unbounded-coalescing failure it
        exists to prevent, one level down, plus an unbounded `_buf`.
        """
        try:
            import select
        except ImportError:  # pragma: no cover -- POSIX-only fallback
            return None
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            line = self._take_buffered_line()
            if line is not None:
                return line
            if self._eof:
                # A trailing fragment with no newline is still an event --
                # unless it belongs to a record already being discarded, in
                # which case surfacing it would emit exactly the truncated
                # phantom the resync exists to suppress.
                if self._buf and not self._resyncing:
                    rest, self._buf = self._buf, b""
                    return rest.decode("utf-8", errors="replace")
                return None
            if deadline is None:
                remaining = None
            else:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    # Partial bytes STAY BUFFERED: the line is incomplete, not
                    # discarded, and the next call resumes where this stopped.
                    return None
            try:
                ready, _, _ = select.select([self.fd], [], [], remaining)
            except (OSError, ValueError):
                return None
            if not ready:
                return None
            try:
                chunk = os.read(self.fd, 65536)
            except OSError:
                return None
            if not chunk:
                self._eof = True
                continue
            self._absorb(chunk)


def _drain_pending_events(reader: _EventLineReader) -> int:
    """Consume events already queued on `reader`, returning how many.

    BOUNDED BY AN ABSOLUTE DEADLINE, not by a per-event window. Re-arming the
    full settle window on every event -- an earlier shape of this function --
    reads as "absorb the whole sweep" and is in fact unbounded: any stream
    producing events faster than the window keeps the drain running forever, so
    the watcher never rebuilds and never answers. That is a worse failure than
    the burst it was added to fix, because it has no end state. The whole drain
    therefore gets ONE deadline, each wait is given only the time remaining,
    and a hard event cap bounds it a second way.

    Nothing is lost when a bound is hit: whatever is still queued coalesces
    into the NEXT drain, so an event is at worst deferred by one tick.
    """
    deadline = time.monotonic() + WATCH_DRAIN_MAX_SECS
    drained = 0
    while drained < WATCH_DRAIN_MAX_EVENTS:
        remaining = min(WATCH_SETTLE_SECS, deadline - time.monotonic())
        if remaining <= 0:
            return drained
        if reader.next_line(remaining) is None:
            return drained
        drained += 1
    return drained


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
            # Read through the RAW descriptor. Calling `proc.stdout.readline()`
            # here would buffer the burst in user space where `select` cannot
            # see it, which is exactly what made the drain below return 0.
            reader = _EventLineReader(proc.stdout.fileno())
            try:
                while True:
                    line = reader.next_line(None)
                    if line is None:
                        break
                    path = line.split(" ", 1)[0]
                    if not path.endswith(".md"):
                        continue
                    # COALESCE THE BURST. One event used to mean one full tick,
                    # and a tick rebuilds the whole cache and re-validates it.
                    # A formatter sweep, a branch checkout or a migration
                    # touches the corpus wholesale -- 232 files in this repo --
                    # so the watcher spent minutes replaying obsolete
                    # intermediate states while the user waited for the answer
                    # to the LAST edit, which is the only one they asked about
                    # (Codex perf, section 22 review, [medium]).
                    #
                    # Drain whatever else is already readable within a short
                    # settle window, then run ONE tick for the batch. The
                    # per-tick refusal semantics above are untouched: a batch
                    # whose rebuild fails still refuses exactly once.
                    _drain_pending_events(reader)
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
    "render":      (cmd_render, [("render-format", "render-fmt", None),
                                  ("scope", "scope-opt", None),
                                  ("output", "output-opt", None),
                                  ("target", "?", None)]),
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
    # --- Output bounding ------------------------------------------------
    # `--scope` doubles as the render subcommand's domain filter: the
    # semantics were already identical ("restrict to a single domain"), so
    # it is declared once here rather than twice with a name collision.
    ap.add_argument("--limit", type=int,
                    help=f"max rows to return (default: {DEFAULT_ROW_LIMIT}; "
                         f"0 = no limit, CLI only)",
                    **(common if is_subparser else {"default": None}))
    ap.add_argument("--offset", type=int,
                    help="skip this many rows before returning (default: 0)",
                    **(common if is_subparser else {"default": None}))
    ap.add_argument("--scope",
                    help="restrict to a single domain (e.g. 00-infrastructure)",
                    **(common if is_subparser else {"default": None}))
    ap.add_argument("--fields",
                    help="comma-separated subset of columns to return "
                         "(default: all columns for the subcommand)",
                    **(common if is_subparser else {"default": None}))
    ap.add_argument("--max-bytes", dest="max_bytes", type=int,
                    help=f"hard output ceiling in bytes "
                         f"(default: {OUTPUT_CEILING_BYTES}); breaching it "
                         f"fails closed rather than truncating",
                    **(common if is_subparser else {"default": None}))


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
            elif nargs == "render-fmt":
                sp.add_argument("--render-format", dest="render_format",
                                choices=("mermaid", "dot", "ascii", "gantt", "markdown"),
                                default="mermaid",
                                help="render format (default: mermaid)")
            elif nargs == "scope-opt":
                # Now supplied by _add_shared_flags for every subcommand;
                # re-adding it here would collide on the option string.
                continue
            elif nargs == "output-opt":
                sp.add_argument("--output", dest="output_path", default=None,
                                help="write rendered output to this file (default: stdout)")
            elif nargs == "?":
                sp.add_argument(pos_name, nargs="?", default=default)
            else:
                sp.add_argument(pos_name)
    return p


def _cache_path_is_writable_from_here(cache_path: Path, repo_root: Path) -> bool:
    """Return True only when cache_path is safely under repo_root. The
    watch-mode auto-rebuild writes to cache_path via build.py; accepting
    arbitrary user-supplied absolute paths would turn a supposedly
    read-only query into a file-write primitive anywhere on disk. Users
    pointing at an external cache for inspection (e.g. a CI fixture)
    simply skip the rebuild and get read-only behavior."""
    try:
        cp_resolved = cache_path.resolve(strict=False)
        rr_resolved = repo_root.resolve(strict=False)
        cp_resolved.relative_to(rr_resolved)
        return True
    except (OSError, ValueError):
        return False


REBUILD_OK = "ok"           # build.py ran and succeeded
REBUILD_FAILED = "failed"   # build.py ran and FAILED -- the cache is suspect
REBUILD_SKIPPED = "skipped"  # no rebuild was applicable here


def _rebuild_cache(cache_path: Path, repo_root: Path, quiet: bool) -> str:
    """Force-rebuild build/todo-cache.json by shelling out to build.py.
    Used between --watch ticks so the query reflects the user's latest
    TODO edits, and once on a MISSING cache for a fresh clone.

    RETURNS A TRI-STATE, and the distinction between the last two is
    load-bearing. It used to return None and print `using last successful
    cache` on a build.py failure, which is the defect: after a FAILED rebuild
    the previous cache is, by definition, the one that does not reflect the
    edit that just broke the build, so answering from it serves a stale graph
    as though it were fresh.

    But "no rebuild happened" is NOT the same event. An out-of-repo `--cache`
    and an absent build.py are both deliberate read-only situations in which
    nothing was attempted and nothing is suspect -- collapsing them into the
    failure code makes an inspection-mode watcher refuse EVERY tick and serve
    nothing at all, which deletes the read-only workflow this function's own
    guard exists to support.

    Refuses to write outside repo_root. A user who pointed --cache at an
    external path may inspect that cache read-only, but auto-rebuild is
    disabled so a typo or hostile fixture cannot overwrite arbitrary
    host files via the watch loop."""
    if not _cache_path_is_writable_from_here(cache_path, repo_root):
        if not quiet:
            sys.stderr.write(
                f"[query.py] WARN: --cache {cache_path} is outside repo_root "
                f"{repo_root}; skipping auto-rebuild (read-only mode)\n"
            )
        return REBUILD_SKIPPED
    build_py = Path(__file__).resolve().parent / "build.py"
    if not build_py.exists():
        if not quiet:
            sys.stderr.write(f"[query.py] WARN: build.py missing at {build_py}; skipping cache rebuild\n")
        return REBUILD_SKIPPED
    result = subprocess.run(
        [sys.executable, str(build_py),
         "--quiet", "--output", str(cache_path),
         "--root", str(repo_root / "todo"),
         "--repo-root", str(repo_root)],
        cwd=str(repo_root),
    )
    if result.returncode != 0:
        if not quiet:
            sys.stderr.write(
                f"[query.py] WARN: build.py exited {result.returncode}; "
                f"the cache on disk no longer reflects the corpus\n")
        return REBUILD_FAILED
    return REBUILD_OK


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

    todo_root = repo_root / "todo"
    # THE CANONICAL CACHE IS THE ONE FRESHNESS CAN BE ESTABLISHED FOR.
    # `check_freshness` compares the cache's recorded corpus binding against
    # THIS repo's `todo/` tree, which is a meaningful question only for the
    # cache this repo's producer writes. A cache the user pointed at with
    # `--cache` (a CI fixture, a captured snapshot, another checkout) describes
    # a DIFFERENT corpus by construction, so checking it against ours would
    # refuse every such cache and delete the read-only inspection path the
    # `--cache` flag exists for (`_cache_path_is_writable_from_here` documents
    # that same boundary for the rebuild side).
    #
    # THIS IS NOT THE "TWO RULES THAT CAN DISAGREE" THE SECTION FORBIDS. There
    # is exactly ONE shape rule -- the shared validator, fail-closed, applied to
    # every cache on every transport. What varies is whether a FRESHNESS
    # question that is undefined for a foreign corpus gets asked at all, and
    # when it is skipped the reader says so on stderr rather than implying the
    # cache was certified. The bypass a plain flag would create is closed by
    # comparing the RESOLVED path: `--cache build/todo-cache.json` resolves onto
    # the canonical path and is checked exactly like the default.
    canonical_cache = (repo_root / "build" / "todo-cache.json").resolve()
    check_stale = cache_path.resolve() == canonical_cache

    # THE HISTORY AXIS IS PER-VERB, NOT PER-READER (section 24). Only three of
    # this CLI's thirteen commands dereference a git-derived timestamp, and
    # making the whole reader pay `corpus_history_id` (47.9ms measured, against
    # a 169ms invocation) to protect fields the other ten never read is a ~28%
    # toll for nothing. `stale` and `stats` read `last_active_at`; `render` reads
    # `created_at` ONLY in the Gantt renderer (render.py:375), which is why the
    # format is part of the test rather than the command name alone.
    def _profile_for(args):
        sub = getattr(args, "subcommand", None)
        if sub in ("stale", "stats"):
            return _cs.PROFILE_QUERY_HISTORY
        if sub == "render" and getattr(args, "render_format", None) == "gantt":
            return _cs.PROFILE_QUERY_HISTORY
        return _cs.PROFILE_QUERY

    profile = _profile_for(args)

    def _build_ctx():
        """(Re)load the cache and build a fresh Ctx. Called once up-front
        for a single-shot run; called every tick in --watch mode so the
        user sees output reflecting the latest TODO edits, not the snapshot
        taken at process start.

        ROUTED THROUGH THE SHARED VALIDATOR under `PROFILE_QUERY`, which
        declares what this reader actually consumes: the section rows, the
        dependency groups inside them, the stamp XREFs, and the node-level
        scalars and edge collections. The load is generation-bound (one
        descriptor, fstat-based freshness and rewrite detection, a byte
        ceiling), so a well-shaped but STALE cache is refused rather than
        served -- which the previous `load_or_rebuild_cache` + drop-bad-rows
        path could not do.

        ONE REBUILD ON A MISSING CACHE, and only on MISSING. A fresh clone has
        no `build/todo-cache.json` and building it is the useful behavior the
        old loader provided. Every OTHER reason refuses: rebuilding over a
        cache that is unreadable, stale or shape-invalid would destroy the
        evidence and re-answer from a file the validator just rejected, which
        is the fail-open shape this section closes. (Recovering from a corrupt
        cache by regenerating it is `validate.py`'s documented behavior and is
        section 23's to harden -- not something to reproduce here by accident.)
        """
        if not check_stale and not args.quiet and not _build_ctx.notified:
            sys.stderr.write(
                f"[query.py] NOTE: --cache {cache_path} is not this repo's "
                f"build/todo-cache.json; validating its shape but NOT its "
                f"freshness against {todo_root}\n")
            _build_ctx.notified = True
        try:
            fresh_nodes, info = _cs.load_and_validate(
                cache_path, todo_root, check_stale=check_stale,
                profile=profile)
        except _cs.CacheSchemaError as exc:
            if exc.reason != _cs.REASON_MISSING:
                _refuse(exc.reason, str(exc), args.quiet)
            # Anything but a clean rebuild leaves the cache still absent, so
            # both FAILED and SKIPPED refuse here -- unlike the watch tick,
            # there is no previous answer to fall back to.
            if _rebuild_cache(cache_path, repo_root, args.quiet) != REBUILD_OK:
                _refuse(_cs.REASON_MISSING,
                        f"cache absent at {cache_path} and build.py could not "
                        f"produce it", args.quiet)
            try:
                fresh_nodes, info = _cs.load_and_validate(
                    cache_path, todo_root, check_stale=check_stale,
                    profile=profile)
            except _cs.CacheSchemaError as exc2:
                _refuse(exc2.reason, str(exc2), args.quiet)
        c = Ctx(fresh_nodes, repo_root, args.quiet)
        if c.ambiguous_edges:
            # The graph CANNOT be built correctly: each of these tokens names a
            # stem several files carry, and the flattened index would bind it
            # to whichever file came last while the others show no inbound edge
            # at all. Refusing names the collision instead; a domain-qualified
            # reference or an id resolves it.
            first = c.ambiguous_edges[0]
            _refuse("AMBIGUOUS_EDGE",
                    f"{len(c.ambiguous_edges)} edge(s) name a filename stem "
                    f"carried by more than one file -- e.g. {first[0]} -> "
                    f"{first[1]!r}, which matches {', '.join(first[2])}; "
                    f"use the target's frontmatter id, which names exactly "
                    f"one file", args.quiet)
        if status_gated:
            c.pre_migration_notice_once()
        return c, info

    # Once per process, not once per watch tick -- an inspection-mode watcher
    # would otherwise repeat the same notice every 2 seconds forever.
    _build_ctx.notified = False

    def _run_once():
        ctx, info = _build_ctx()
        # A REQUEST ERROR IS ALSO A RESULT DERIVED FROM THE CACHE, so it gets
        # the same generation binding as a successful answer. "id not found"
        # and "ambiguous" are computed from the indexes the walk was given; if
        # the corpus moved underneath, the honest answer is that the cache is
        # stale, NOT that the caller's request is wrong. Holding the error here
        # and deciding after re-verification is what keeps rc 4 from
        # overruling rc 2.
        input_error = None
        try:
            rows, columns = fn(ctx, args)
        except QueryInputError as exc:
            input_error = exc
        # THE GENERATION BINDING IS ONLY CLOSED HERE. The validated read proves
        # the cache matched the corpus when it was opened; this proves the
        # corpus did not move while the walk above computed an answer from it.
        # Publishing first and checking after would report a result derived
        # from a corpus that no longer exists (section 17's contract; the same
        # post-walk re-verification `todo-reachability.py:542` performs).
        try:
            # HISTORY FIRST, CONTENT LAST, and the ORDER is the point (Codex
            # adversarial, section 24, [medium]). The history probe forks git
            # and costs ~48ms; running it AFTER the content check put that whole
            # window between "the corpus is unchanged" and publication, so a
            # TODO edit landing inside it changed no git history, passed both
            # checks, and shipped anyway. Checking history first keeps the
            # content fingerprint the LAST thing verified before the answer is
            # emitted, which is the property the generation binding rests on.
            # `info.history` is None for every verb outside the three that
            # consume the derived timestamps, making this a no-op there rather
            # than a second 47.9ms probe.
            _cs.check_history_unchanged(todo_root, info.history)
            _cs.check_corpus_unchanged(todo_root, info.corpus)
        except _cs.CacheSchemaError as exc:
            # Refuses whether or not a request error is pending: a stale
            # generation is the stronger and truer statement.
            _refuse(exc.reason, str(exc), args.quiet)
        if input_error is not None:
            # The corpus held, so the request really was the problem.
            _write_input_error(input_error)
            raise input_error
        output_path = getattr(args, "output_path", None)

        # Bounding applies to row-returning subcommands only. `stats`
        # (scalar dict, columns is None) and `render` (raw text sentinel)
        # are exempt by shape as well as by name -- see
        # UNBOUNDED_SUBCOMMANDS for the reasoning.
        bounded = (
            args.subcommand not in UNBOUNDED_SUBCOMMANDS
            and columns is not None
            and columns != "__RAW_TEXT__"
        )

        if not bounded:
            # THE UNBOUNDED PATH NEEDS THE CAVEAT TOO, and missing it was the
            # first fix's blind spot: `stats` bypasses `bound_rows` entirely,
            # yet `cmd_stats` reuses `cmd_blocking` for `top_blocking` and
            # reports `avg_dep_depth` -- both computed from the same empty edge
            # set. An MCP caller therefore read those scalars as real graph
            # conclusions while `blocking` beside them carried the warning
            # (Codex re-adversarial, [high]). Attached to the dict `stats`
            # already returns, so no other unbounded subcommand changes shape.
            if (isinstance(rows, dict)
                    and getattr(ctx, "dep_edges_empty", None)
                    and "dependency_source" not in rows):
                rows = dict(rows)
                rows["dependency_source"] = _DEPENDENCY_SOURCE_CAVEAT
            for flag in ("limit", "offset", "fields"):
                if getattr(args, flag, None) not in (None, ""):
                    sys.stderr.write(
                        f"[query.py] WARN: --{flag} does not apply to "
                        f"'{args.subcommand}' (unbounded by design); ignored\n"
                        if not args.quiet else ""
                    )
            if output_path:
                with open(output_path, "w", encoding="utf-8") as f:
                    saved = sys.stdout
                    sys.stdout = f
                    try:
                        emit(rows, columns, fmt)
                    finally:
                        sys.stdout = saved
            else:
                emit(rows, columns, fmt)
            return

        page, out_columns, meta = bound_rows(rows, columns, args,
                                             args.subcommand, ctx)
        ceiling = getattr(args, "max_bytes", None) or OUTPUT_CEILING_BYTES
        scope_supported = "domain" in (columns or [])

        if output_path:
            # A file sink is not a model context; the ceiling exists to
            # protect the latter. Row bounds still apply so the file
            # matches what the same flags would print.
            with open(output_path, "w", encoding="utf-8") as f:
                saved = sys.stdout
                sys.stdout = f
                try:
                    emit_bounded(page, out_columns, fmt, meta,
                                 ceiling=max(ceiling, 1 << 30),
                                 scope_supported=scope_supported,
                                 quiet=args.quiet)
                finally:
                    sys.stdout = saved
        else:
            emit_bounded(page, out_columns, fmt, meta, ceiling=ceiling,
                         scope_supported=scope_supported, quiet=args.quiet)

    def _run_once_guarded():
        """Ceiling breach is a normal, expected outcome -- it must render
        as a machine-readable envelope, not a traceback. Returns the exit
        code so mcp_server.py (which calls main() in-process) receives the
        envelope on stdout rather than an exception."""
        try:
            _run_once()
            return 0
        except CeilingExceeded as exc:
            sys.stdout.write(json.dumps(exc.envelope, indent=2,
                                        sort_keys=True) + "\n")
            return 3
        except CacheRefused:
            # `_refuse` already wrote the envelope to stdout; this only maps it
            # onto the documented infrastructure exit code.
            return EXIT_CACHE_UNUSABLE
        except QueryInputError as exc:
            # Catching it HERE is what keeps a watch tick alive: the handlers
            # used to `sys.exit(2)` and `SystemExit` is not an `Exception`, so
            # it unwound past every guard below and terminated the watcher on
            # an ordinary rename of the watched target.
            if not exc.emitted:
                # Raised after the generation check (the argv-shape errors in
                # `bound_rows`), so nothing re-verified it and nothing emitted
                # it yet.
                _write_input_error(exc)
            return EXIT_QUERY_INPUT
        except RecursionError as exc:
            # A RESOURCE failure is infrastructure, not a verdict. Letting it
            # escape exited 1 -- which this reader documents as "the graph has
            # findings" -- with an empty body, so an unusable cache read as a
            # clean run with nothing to report. `_dep_depth` is iterative now,
            # so this is the backstop for any walk that grows one later rather
            # than the live path (Codex adversarial, section 22, [high]).
            _write_refusal("RECURSION",
                           f"exhausted the interpreter stack while walking "
                           f"the graph: {exc}", args.quiet)
            return EXIT_CACHE_UNUSABLE
        except MemoryError:
            # Deliberately no interpolation: formatting allocates, and this
            # handler runs precisely when allocation is failing. Same contract
            # `cache_schema.load_and_validate` already applies to its own read.
            _write_refusal("MEMORY", "out of memory while answering the query",
                           args.quiet)
            return EXIT_CACHE_UNUSABLE

    if args.watch:
        # WATCH-MODE BEHAVIOR AFTER A FAILED REBUILD IS PINNED HERE (section 22):
        # refuse THIS TICK, keep the watcher alive. The two alternatives are
        # both worse. Serving the previous cache -- what this did until now --
        # publishes a graph that predates the edit which broke the build, with
        # nothing on stdout saying so, so the user reads a stale answer as a
        # fresh one. Killing the watcher on a transient error makes an
        # interactive tool unusable, since a half-saved TODO file is a normal
        # intermediate state while editing. Refusing the tick keeps both
        # properties: no stale rows are ever emitted, and the next valid edit
        # produces a fresh successful answer with no restart.
        def _watch_tick():
            # Bound to a name rather than tested inline so the per-rule
            # mutation harness has a needle unique to the WATCH guard: the
            # identical `if not _rebuild_cache(...)` call also appears on the
            # missing-cache path in `_build_ctx`, and a needle matching both
            # would neuter two rules at once and verify neither.
            rebuilt = _rebuild_cache(cache_path, repo_root, args.quiet)
            # ONLY an attempted-and-failed rebuild refuses the tick. A SKIPPED
            # rebuild (inspection-mode `--cache`, absent build.py) attempted
            # nothing and casts no doubt on the cache, so the tick proceeds and
            # the loader's own freshness rule remains the judge.
            if rebuilt == REBUILD_FAILED:
                sys.stdout.write(json.dumps({
                    "error": "cache-rebuild-failed",
                    "reason": "REBUILD",
                    "detail": "build.py failed; refusing to serve the previous "
                              "cache, which predates the edit that broke it",
                    "hint": "fix the TODO tree build.py rejected; the watcher "
                            "is still running and will answer on the next "
                            "successful rebuild",
                }, indent=2, sort_keys=True) + "\n")
                sys.stdout.flush()
                return
            # A ceiling breach must not kill an interactive watcher; the
            # envelope is printed and the loop continues to the next tick.
            _run_once_guarded()
        # NO EXPLICIT FIRST TICK -- `watch_loop` performs the initial
        # `run_once()` itself. The code this replaced called `_rebuild_cache`
        # here (a rebuild, no output) before handing a separate runner to the
        # loop; swapping in `_watch_tick`, which rebuilds AND answers, made the
        # opening result be emitted TWICE, with no separator in quiet TSV/JSON
        # streaming to tell a consumer the rows were one snapshot rather than
        # two (Codex adversarial, section 22 round 2, [medium]). Passing the
        # tick to the loop still gives the intended property: a watcher started
        # against an already-broken tree refuses its first tick instead of
        # opening with a stale answer.
        watch_loop(_watch_tick, todo_root, args.quiet)
        return 0
    return _run_once_guarded()


if __name__ == "__main__":
    sys.exit(main())
