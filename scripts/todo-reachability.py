#!/usr/bin/env python3
"""Can a future pass still SEE this item? One question, one detector.

WHY THIS EXISTS RATHER THAN A FOURTH PARTIAL CHECK. Three checks already guard
pieces of this, each correct and each blind to the others:

  * `todo-orphan-check.py`  -- `- [ ]` in a section stamped BOTH Verified and
                               Quality-reviewed, exempting Deferred sections
  * `validate.py`           -- dangling sections / orphan IO rows
  * `sequencer_triage.py`   -- the classifier those checks are policing

Between them, three shapes of invisible work were found on 2026-08-02, all in
one morning, all real:

  1. A `## N.` body with NO Implementation Order row. PROVEN against the real
     completed `00-infrastructure/TODO-05`: adding a section body with an open
     item left the file classified DONE and the work unreachable; the SAME
     section with an IO row flipped it to NEEDS_WORK. The oracle classifies
     from the row and never reads the body.
  2. A bare `- [ ]` in a stamped, DONE section -- the shape orphan-check owns.
  3. Open `- [ ]` items inside DEFERRED-stamped sections. orphan-check exempts
     these BY DESIGN ("recorded blockers with a named owner"), and the exemption
     is right -- but 109 such items exist, and an item parked without naming an
     owner is indistinguishable from one that names one. It is work in the
     wrong SHAPE: the doctrine's repair is `- [/]` with the blocker named.

The question a reader actually has is not "which of three rules did this break"
but "will anyone ever come back to this?". That is what this answers.

REACHABLE means: some future pass will re-read this item. Concretely, the item
lives in a section the triage oracle does not classify DONE, OR in a Deferred
section whose parked item names an owner the owner-side sweep can act on.

Usage:
  todo-reachability.py [--json] [paths...]    exit 1 if unreachable work exists
  todo-reachability.py --selftest
Stdlib only; reads the todo-graph cache when present for the oracle's verdict.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath

_REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_REPO_ROOT / "scripts" / "todo-graph"))
try:
    import cache_schema as _cs
except ImportError as _exc:                                  # pragma: no cover
    sys.stderr.write(f"todo-reachability: cannot import the shared "
                     f"cache-schema validator: {_exc}\n")
    raise

# The shared fence tracker (section 38). It reuses the `cache_schema` imported
# just above rather than loading a second copy -- see `todo_fence._cache_schema`.
sys.path.insert(0, str(_REPO_ROOT / "scripts"))
try:
    import todo_fence as _fence
except ImportError as _exc:                                  # pragma: no cover
    sys.stderr.write(f"todo-reachability: cannot import the shared "
                     f"fence tracker: {_exc}\n")
    raise

# EXIT CODES. 0 = clean, 1 = FINDINGS, 2 = infrastructure.
#
# `2` is NEW with the shared-cache-validator routing, and exists because 1 was
# already taken by a VERDICT. Before this, a cache that would not parse was
# swallowed by a bare `except Exception` and the audit silently fell back to
# per-file parsing, so an unusable cache produced a clean-looking finding count
# computed from a different source than the one this tool documents. Mapping
# the failure onto 1 would have been no better: an infrastructure refusal would
# then be indistinguishable from "there are unreachable items", which is the
# exact defect the shared validator exists to prevent (Codex design review of
# the cache-validation-reach work, [high]).
#
# A MISSING cache is deliberately NOT an error. Absence is not corruption: a
# fresh clone has no `build/todo-cache.json` until something builds it, and this
# audit is expected to work there via its per-file fallback. Every OTHER reason
# -- unreadable, wrong shape, empty, stale -- means a cache EXISTS and cannot be
# trusted, and silently preferring the fallback would hide that.
EXIT_INFRA = 2
_CACHE_FALLBACK_REASONS = frozenset((_cs.REASON_MISSING,))

# One reason tag this tool raises itself rather than receiving from the shared
# validator: the cache is schema-VALID but cannot answer for a file being
# audited. See `_io_status`. It is deliberately a reason of the same shape, so
# every refusal reads the same way in the lint's error line.
REASON_COVERAGE = "COVERAGE"
# ...and one for a node whose identity is not a canonical repo-relative path.
REASON_IDENTITY = "IDENTITY"


class CacheUnusable(RuntimeError):
    """A cache exists but cannot be trusted. Carries a reason tag -- the shared
    validator's, or this module's `REASON_COVERAGE` -- so the message names
    WHICH rule refused, and is caught in `main` and mapped to `EXIT_INFRA` --
    never to the findings code."""

    def __init__(self, reason: str, message: str):
        super().__init__(message)
        self.reason = reason

# NO LOCAL `## N.` GRAMMAR ANY MORE (section 43). This file used to carry
# `^## (\d+)\.` -- column-0 anchored where the canonical rule allows CommonMark's
# 0-3 space indent, and feeding an unbounded digit run to `int()`. Both are now
# `todo_fence.classify_heading`, which MATCHES an over-long heading (so it still
# delimits its section and nothing after it is re-attributed) and REPORTS it
# instead of raising. Measured before the switch: 0 corpus headings are indented
# 1-3 spaces and 0 exceed 5 digits, so adopting the wider, safer rule changed no
# live verdict. `scripts/tests/test_todo_fence.py` walks this file's AST and
# FAILS if a heading matcher is defined here again.
OPEN_ITEM_RE = re.compile(r"^\s*- \[ \]")
PARKED_ITEM_RE = re.compile(r"^\s*- \[/\]")
# AUTHOR-TIME marker for a recurring/standing task -- work that is
# deliberately never "done" (an annual re-review, a periodic audit).
# Such an item is CORRECTLY a bare `- [ ]` and must not be parked, but
# the gate below would otherwise refuse fixpoint on it forever.
# NOTE this is not the inferred `parked-ownerless` rule that was removed
# (see the note at the end of audit()): the distinction is that a human
# WRITES this marker when authoring the item, rather than a detector
# guessing intent from shape. Unmarked items still flag -- fail-closed.
STANDING_RE = re.compile(r"\bstanding:", re.I)
# ONE STAMP GRAMMAR, matching the two authorities this gate has to agree with:
# `sequencer_triage.section_stamps()` (`.claude/hooks/sequencer_triage.py:56`),
# which is what `_is_done` below mirrors, and `todo-staged-check._STAMP_RE`.
# These three used to differ, and in BOTH directions (Codex consistency,
# section 47, [high]): this file required exactly one ASCII space after `>` and
# did not require the closing `**`, so `>**Verified:**` -- valid CommonMark --
# was DONE to the sequencer and unstamped here, hiding an `open-in-done`; while
# a malformed `> **Verified:` was stamped here and not there, which is a false
# blocking verdict from the gate that holds `phase FIXPOINT` open. Keeping the
# match on physical lines establishes nothing if the patterns disagree.
VERIFIED_RE = re.compile(r"^>\s*\*\*Verified:\*\*")
DEFERRED_RE = re.compile(r"^>\s*\*\*Deferred:\*\*")
QUALITY_RE = re.compile(r"^>\s*\*\*Quality reviewed:\*\*")
AWAITING_RE = re.compile(r"awaiting-[a-z]+")
IO_ROW_RE = re.compile(r"^\|[^|]*\|\s*(\d+)\s*\|")
# An Implementation Order "Section" cell: the section marker glyph plus digits,
# optionally wrapped in backticks or a link label. Built from an escape so the
# repo's no-bare-section-refs rule is not tripped by the source itself.
SECTION_MARKER_RE = re.compile(r"^[`\[]*§\s*(\d+)\b")

# An owner is anything a later pass can act on: an XREF, a named TODO, an
# explicit awaiting-* token, or a file path. Deliberately generous -- the point
# is to separate "parked WITH a handle" from "parked into nothing", not to
# grade prose.
OWNER_RE = re.compile(
    r"XREF|awaiting-[a-z]+|TODO-\d+|todo/[0-9]{2}-[a-z-]+/|`[a-z_]+\.(c|h|py|sh)`",
    re.IGNORECASE)


# The BOUNDARY rule comes from the shared module too (section 43 review). A
# column-0 terminator against a 0-3-indent start rule made an indented section
# start without ending the previous one, so its items were counted in BOTH
# bodies -- once under the wrong section number, which is a false finding in
# the gate that holds `phase FIXPOINT` open.


def _sections(scan):
    """(head, line, (start, end)) with the body ending at the next `## ` of ANY kind.

    THE SECTION SET COMES FROM THE PRODUCER'S PROJECTION (section 47), not from
    physical lines. `build.py` moved its five section-context walks onto
    `ScanResult.leaf_views` in section 45 and this walk stayed behind, so the
    producer and the gate disagreed about what a section IS. For `## 1. Root` /
    `- ## 2. Nested` / an indented open item the producer saw sections 1 and 2
    while this saw only section 1 and filed the item under it -- and with
    section 1 open and section 2 DONE the fixpoint gate then emitted no
    `open-in-done` finding at all (Codex consistency, section 45 post-ship,
    [high]).

    BLOCKQUOTED HEADINGS ARE SKIPPED, matching `build.py:1096`, and that is not
    bookkeeping: `leaf_views` consumes the marker the line opens, so
    `> ## 99. Example` projects to `## 99. Example` and classifies as a real
    heading where the physical line never could. Adopting the projection
    WITHOUT the blockquote predicate would promote every quoted example in the
    corpus into a section boundary -- a regression created by the fix (Codex
    design review, section 47, [high]).

    YIELDS BODY BOUNDS `(start, end)` rather than lines or indices, because the
    body's matchers do NOT agree on a projection and must not be forced to --
    and because materialising the range costs one boxed integer per line, which
    at the scanner's valid-input ceiling is hundreds of megabytes for a walk
    that only ever streams. `audit` makes ONE pass over the range.

    Ending only at the next NUMBERED section is wrong and was caught on
    2026-08-02 before it caused a bad edit: the LAST numbered section's body
    then runs to EOF, swallowing `## OS Comparison`, `## Unit Tests`,
    `## Verification` and `## History` -- all of which legitimately contain
    `- [ ]` items. That made every file's final section look like it held
    unreachable open work, and produced 11 confident findings that evaporated
    on inspection (each section actually had 0 open items of its own).

    FENCE-AWARE (section 38): `mask` comes from `todo_fence.fence_scan` and
    marks fenced-code and HTML-comment lines. A `## 99.` inside a fence is an
    EXAMPLE, and reading it as a real heading splits a real section so its
    later items are attributed to a number with no Implementation Order row --
    a blocking `no-io-row` verdict on text that is a code sample, from the
    parser that gates `phase FIXPOINT`. The BODY is filtered too, not just the
    boundary: the item and stamp scans in `audit` run over what is yielded
    here, so a fenced `- [ ]` or a fenced `> **Verified:**` would otherwise
    still be counted.
    """
    mask, leaves = scan.mask, scan.leaf_views
    in_bq = scan.in_blockquote
    # Yields the CLASSIFICATION, not a number: an over-long heading has no
    # usable number and still delimits its section, so a caller has to be able
    # to tell those apart. Handing back an int would force this walk to either
    # invent one or drop the heading, and dropping it re-parents every item
    # after it -- the exact misattribution the shared rule exists to refuse.
    starts = [(i, h) for i, l in enumerate(leaves)
              if not mask[i] and not in_bq(i)
              and (h := _fence.classify_heading(l)).kind != "none"]
    n = len(scan.lines)
    for ln, head in starts:
        end = n
        for j in range(ln + 1, n):
            if not mask[j] and not in_bq(j) and _fence.is_h2(leaves[j]):
                end = j
                break
        yield head, ln, (ln, end)


def _io_rows(scan):
    """Section numbers listed in the `## Implementation Order` table.

    SCOPED to that table, and tolerant of BOTH row shapes found in the corpus
    (2026-08-02) -- the number sits in column 2 behind a status glyph in most
    files (`| 💎 | 1 | Deliverable | ... |`) and in column 1 in others
    (`| 1 | Section | Tag | Dep |`). Scanning every table in the file instead
    produced 304 false "no IO row" findings, because an Inputs table's rows
    never carry a section number.

    FENCE-AWARE (section 38), in BOTH directions. A fenced `## Implementation
    Order` heading would flip `inside` on for an example table -- inventing
    rows -- and a fenced `## Something` inside the real table's span would flip
    it back off, dropping every row after it. Skipping masked lines entirely
    leaves the toggle driven only by real headings.

    CONTAINER-AWARE WITH THE REST OF THE CLOSURE (section 47). This walk stayed
    on physical lines when `_sections` moved onto the projections, which is the
    same half-migration section 45 proved dangerous, one walk over: `build.py`
    reads Implementation Order rows through `views`/`leaf_views`
    (`build.py:1449`), so a list-contained `- ## Implementation Order` and its
    indented rows are a real table to the producer and invisible here -- and a
    section whose row this walk cannot see gets a blocking `no-io-row` verdict
    on work that is correctly filed (Codex consistency, section 47, [high]).
    The BOUNDARY reads `leaf_views` (a heading question) while the ROWS read
    `views` (a content question), matching the producer's split exactly.
    """
    mask, views, leaves = scan.mask, scan.views, scan.leaf_views
    in_bq = scan.in_blockquote
    rows = set()
    inside = False
    sec_col = None            # index of the header's "Section" column, if any
    for i in range(len(scan.lines)):
        # Blockquoted rows are quoted EXAMPLES, excluded for the same reason
        # the heading walk excludes quoted headings.
        if mask[i] or in_bq(i):
            continue
        l = views[i]
        # Shared rule here too (section 43 post-ship review): a fixed `l[3:]`
        # slice reads `# mplementation Order` off an indented heading, so the
        # table would never be entered and every row in it would go unseen.
        _h2 = _fence.h2_title(leaves[i])
        if _h2 is not None:
            inside = _h2.lower().startswith("implementation order")
            sec_col = None
            continue
        if not inside:
            continue
        if not l.startswith("|"):
            # A NON-TABLE LINE ENDS THE TABLE, so the next one declares its own
            # header. Latching `sec_col` for the whole `## Implementation Order`
            # section instead meant a legend or example table placed before the
            # real one fixed the column permanently, and the real header was
            # then consumed as data with no way to correct it (Codex
            # adversarial, section 38 review, [medium]).
            sec_col = None
            continue
        cells = [c.strip() for c in l.strip().strip("|").split("|")]
        # THE HEADER DECLARES WHICH COLUMN HOLDS THE SECTION, and the marker is
        # read from THAT column only. Scanning the whole row for a marker looks
        # equivalent and is not: the `Depends On` cell carries section markers
        # too, so on the many tables with no Section column at all (TODO-01,
        # TODO-04) a row's DEPENDENCY was read as the row's own section -- which
        # produced 59 phantom `no-io-row` refusals against the live corpus the
        # first time this was tried. Measured, not reasoned.
        #
        # THE HEADER IS FOUND BY CONTENT, NEVER BY POSITION. Skipping "the first
        # row after the heading" is the obvious reading and it silently ate the
        # only data row of a header-less table -- which is exactly the shape
        # `test_build.sh`'s routed-reader fixture uses, so the file declared no
        # rows at all and the cache-COVERAGE refusal it exists to prove stopped
        # firing. A separator row needs no special case: its cells are neither
        # digits nor markers, so it contributes nothing on its own.
        low = [c.lower() for c in cells]
        if sec_col is None and "section" in low:
            sec_col = low.index("section")
            continue                      # this row IS the header
        # THE EXPLICIT SECTION MARKER WINS; the digit rule is the FALLBACK for
        # layouts that carry no marker, not an additional source.
        #
        # The digit rule takes the first all-digit cell of the first two
        # columns, which in the dominant six-column layout is the ORDER column,
        # while the caller compares this set against SECTION numbers from the
        # `## N.` headings. The two agree only while a file's order and section
        # numbers cover the same range, and they stop agreeing the moment a
        # section is re-sequenced.
        #
        # COLLECTING BOTH LOOKS CONSERVATIVE AND IS THE OPPOSITE. The first
        # repair of this function did exactly that, on the reasoning that a
        # larger set can only REMOVE a false refusal -- which is true only when
        # ABSENCE is the actionable signal. Here PRESENCE is: `num in rows` is
        # the integrity check, so a row with order 1 and marker 41 lets a real
        # section 1 that has open work and no IO row of its own pass silently
        # (Codex adversarial, section 38 round 2, [high], against that repair).
        # THE MARKER GLYPH IS WHAT MAKES THE COLUMN AUTHORITATIVE, not the
        # header word. `Section` names two different things in this corpus: the
        # section NUMBER in the six-column layout, where the cell carries the
        # marker glyph; and the section TITLE in the older four-column one
        # (`| 1 | Win32 Type Definitions | [Sonnet] | ... |`, where the number
        # is column 0). Keying on the header word alone read 1,522 titles as
        # unparseable and produced a refusal for nearly every section in those
        # files -- measured, and caught only because the corpus differential was
        # re-run after the change rather than reasoned about.
        #
        # So: a marker in the declared column WINS and is used alone, which is
        # what keeps an order number out of the membership set. Anything else
        # falls through to the digit rule, which is correct for the legacy
        # layout because there the digit IS the section number.
        if sec_col is not None and sec_col < len(cells):
            m = SECTION_MARKER_RE.match(cells[sec_col])
            if m:
                rows.add(int(m.group(1)))
                continue
        for cell in cells[:2]:
            if cell.isdigit():
                rows.add(int(cell))
                break
    return rows


_CACHE = {}
# The generation fingerprint taken when the cache was loaded, held so `main`
# can re-verify the corpus AFTER the audit walk. Empty when no cache was
# loaded (missing-cache fallback), in which case there is nothing to bind.
_CACHE_CORPUS = {}
# CACHE ABSENCE IS OUT-OF-BAND STATE, not a key inside `_CACHE`. It was
# `_CACHE["__missing__"] = True`, which put a control signal in the same
# namespace as CACHE-CONTROLLED data: `file_path` is any non-empty string, so a
# node literally named `__missing__` made every lookup report "no cache" and
# sent the whole audit down the weak pre-cache fallback -- with a cache loaded,
# and past every coverage check (Codex re-adversarial, section 19 review,
# [high]). A separate flag cannot be spelled by an input.
#
# `resolved` PINS THE VERDICT FOR THE WHOLE RUN, and it is the other half of
# that fix. With only `missing`, a first call that found no cache left `_CACHE`
# empty -- so the `if _CACHE` early return did not fire, a later in-process call
# RETRIED the load, and a cache created in between (a concurrent build) was
# published together with its fingerprint while `missing` stayed True. Every
# lookup then took the weak pre-cache fallback against a cache that had loaded
# fine, and the post-walk corpus check attested to a generation nothing was
# reading (Codex re-adversarial round 4, section 19 review, [high]). Both fields
# are written together, only at a settled outcome, and an audit that begins
# without a cache finishes without one rather than straddling two generations.
#
# `root` BINDS THE VERDICT TO THE TREE IT WAS TAKEN FROM. `resolved` alone was
# process-global, so an in-process call sequence over two trees -- `audit(p,
# root=A)` and then `_load_cache(".")` -- silently served A's statuses for
# bodies read under `.`, and the post-walk fingerprint attested to A's corpus.
# The missing variant was worse: a missing verdict under A made a CORRUPT cache
# under `.` read as absent, suppressing the infrastructure refusal entirely
# (Codex re-adversarial round 5, section 19 review, [high]). A different root is
# a different corpus, never a cache hit, so it resets and reloads.
_CACHE_LOADED = {"missing": False, "resolved": False, "root": None}


def _reset_cache_state():
    """Drop every cached generation. `main` calls this on entry so two runs in
    one interpreter are independent rather than the second inheriting the
    first's pinned verdict."""
    _CACHE.clear()
    _CACHE_CORPUS.clear()
    _CACHE_LOADED.update(missing=False, resolved=False, root=None)


def _canonical_root(root):
    """The ONE resolved root everything in a run is anchored to.

    Resolved STRICTLY, and a failure is an infrastructure refusal rather than a
    fallback to the caller's spelling. `str(root)` was the fallback, which made
    a relative root such as `"."` the same key in every working directory --
    physical-tree identity silently gone -- and, because only the KEY was
    canonicalized while `cache_path` and `todo_root` kept using the unresolved
    root, a symlink retargeted in between could file one tree's cache and
    fingerprint under another tree's key (Codex re-adversarial round 6, section
    19 review, [medium]). Every filesystem path in the run is derived from this
    value now, so the key and the reads cannot describe different trees.
    """
    try:
        return Path(root).resolve(strict=True)
    except OSError as exc:
        raise CacheUnusable(
            REASON_IDENTITY,
            f"cannot resolve the audit root {root!r}: {exc}") from exc


def _load_cache(root):
    """Section status straight from the todo-graph cache: {path: {n: status}}.

    Spawning `sequencer_triage --classify` per file cost 232 subprocesses and
    made this unusable from lint (it silently timed out on 2026-08-02 and the
    check emitted nothing at all). The cache already carries what is needed --
    `sections[].status` is the Implementation Order marker the classifier reads
    -- so one JSON load replaces the whole fan-out.
    """
    root_path = _canonical_root(root)
    canonical = str(root_path)
    if _CACHE_LOADED["resolved"]:
        if _CACHE_LOADED["root"] == canonical:
            return _CACHE
        # A DIFFERENT TREE IS NEVER A CACHE HIT. Serving the previous root's
        # nodes here is what let one corpus' statuses be applied to another's
        # bodies; dropping the state and reloading is the only answer that
        # cannot mix generations.
        _reset_cache_state()
    cache_path = root_path / "build" / "todo-cache.json"
    # ROUTED THROUGH THE SHARED CACHE-SCHEMA VALIDATOR, under the profile that
    # declares what this reader actually consumes: `sections[].n/.status`. The
    # default stamped-items profile would REFUSE a cache in which nothing has
    # shipped yet, which is a perfectly usable cache for a reachability audit.
    try:
        data, info = _cs.load_and_validate(
            cache_path, root_path / "todo", profile=_cs.PROFILE_SECTIONS)
    except _cs.CacheSchemaError as exc:
        if exc.reason in _CACHE_FALLBACK_REASONS:
            _CACHE_LOADED.update(missing=True, resolved=True,
                                 root=canonical)
            # LABEL the degraded read (v19 capture, 2026-09-03): without the
            # cache the per-file fallback reported 192 open items where the
            # truth was 1417, exit 1 and empty stderr, so every consumer read
            # an 8x undercount as an honest verdict. The JSON is unchanged;
            # the notice is on stderr, where lint and humans see it.
            sys.stderr.write(
                "todo-reachability: build/todo-cache.json is missing -- per-file "
                "fallback, counts are PARTIAL; rebuild with "
                "bash scripts/todo-graph/build-and-validate.sh --keep-cache\n")
            return _CACHE
        # NOT a silent degrade. See EXIT_INFRA above: the cache exists and
        # cannot be trusted, so the audit refuses rather than quietly answering
        # from a different source.
        raise CacheUnusable(exc.reason, str(exc)) from exc
    built = {}
    for node in data:
        # `file_path` and the `sections[]` shape are now guaranteed by the
        # validator, so the defensive `if not fp` / `isinstance(sec, dict)`
        # skips this walk used to need are gone: skipping is exactly the silent
        # narrowing the routing exists to end.
        # `n` MAY BE NULL and that is legal: the schema declares it nullable and
        # `build.py` emits null when a row's Section column will not parse. Such
        # a row is dropped rather than kept under a `None` key, because the
        # lookup side is `_io_rows`, which only ever yields parsed INTEGER
        # section numbers -- a `None` key could never be hit, and keeping it
        # would put an entry in the map that no reader can reach. (Rejecting
        # null outright was the reviewer's suggestion and is NOT taken: it would
        # turn an unparseable IO row -- an ordinary lint finding -- into an
        # infrastructure refusal of the whole audit.)
        # NODE IDENTITY IS A CANONICAL REPO-RELATIVE POSIX PATH, and anything
        # else is refused rather than stored. `build.py` emits exactly that
        # spelling, so an absolute path or one containing a `..` segment cannot
        # come from the producer -- but the shared validator only requires a
        # non-empty string, and a second spelling of the SAME file is not a
        # duplicate `file_path`, so it evades the node-identity rule. Stored, it
        # becomes an alias that `_lookup` could prefer over the real node,
        # letting a decoy status map suppress or fabricate findings (Codex
        # re-adversarial, section 19 review, [medium]).
        fp = node["file_path"]
        # THE SPELLING MUST ALREADY BE CANONICAL -- normalising it here is what
        # made the first version of this rule an alias FACTORY rather than a
        # guard: `./todo/x.md`, `todo//x.md`, `todo/./x.md` and `todo/x.md/` are
        # four distinct `file_path` strings (so neither the shared validator's
        # duplicate rule nor this one sees a repeat) that `as_posix()` collapses
        # onto ONE key, letting a decoy node overwrite the real one on insert
        # (Codex re-adversarial round 3, section 19 review, [high]).
        if (fp != PurePosixPath(fp).as_posix()
                or PurePosixPath(fp).is_absolute()
                or ".." in PurePosixPath(fp).parts
                or Path(fp).is_absolute()):
            raise CacheUnusable(
                REASON_IDENTITY,
                f"cache node file_path {fp!r} is not a canonical repo-relative "
                f"path; a second spelling of a file already in the cache is an "
                f"alias the node-identity rule cannot see")
        if fp in built:
            raise CacheUnusable(
                REASON_IDENTITY,
                f"two cache nodes resolve to the identity {fp!r}; the second "
                f"would silently replace the first")
        # NAMED BY THE SHARED CONSTANT, not a local literal. A retyped copy lets
        # a subtree rename validate one field and read another: the profile
        # would keep declaring `SUBTREE_SECTIONS` while this indexed a key
        # nothing emits, and the KeyError would escape under this tool's VERDICT
        # code rather than its infrastructure one (Codex consistency, section 19
        # review, [medium]).
        built[fp] = {
            sec["n"]: (sec["status"] or "").strip()
            for sec in node[_cs.SUBTREE_SECTIONS] if sec["n"] is not None
        }
    # PUBLISH ATOMICALLY. `_CACHE` used to be filled node by node, so a refusal
    # part-way through left a VALID PREFIX in the global -- and the `if _CACHE`
    # early return at the top then served that prefix to every later call in the
    # same process without re-validating or re-raising, deriving a clean verdict
    # from a cache already declared unusable (Codex re-adversarial round 3,
    # section 19 review, [medium]). Nothing is visible until every node passed.
    _CACHE.update(built)
    # RETAINED FOR POST-WALK RE-VERIFICATION, not decoration. The loader's
    # freshness scan bounds only the milliseconds inside itself; this tool then
    # reads every TODO body and stamp in the corpus, which is the window that
    # actually matters. A TODO edited during that walk would pair OLD cached
    # section statuses with NEW body items and either suppress or fabricate a
    # finding, at a normal exit code. `main` calls `check_corpus_unchanged` with
    # this fingerprint before publishing. Assigned HERE, with the cache, so a
    # refusal cannot leave a fingerprint bound to nodes nobody accepted.
    _CACHE_CORPUS["corpus"] = info.corpus
    _CACHE_CORPUS["todo_root"] = root_path / "todo"
    _CACHE_LOADED.update(missing=False, resolved=True, root=canonical)
    return _CACHE


def _lookup(cache, path, root):
    """The cache node for `path`, matched on the spelling the PRODUCER used.

    `build.py` keys nodes on repo-relative POSIX paths (`todo/01-x/TODO-01.md`),
    so a caller passing an ABSOLUTE path -- which is exactly how an ad-hoc run
    and the test fixtures invoke this tool -- missed every node and fell through
    to the weak pre-cache rule without saying so. That silent miss is what made
    the coverage refusal below look like a false positive when it was added; the
    honest repair is to match the key rather than to refuse a usable cache
    (found by this section's own positive-control fixture).
    """
    # THE CANONICAL KEY IS THE ONLY KEY. Trying the caller's raw spelling as a
    # fallback looked harmless -- it was the ordering fix for an alias decoy --
    # but it defeated the root binding outright: with the process cwd in tree A,
    # `audit("todo/x.md", root=B)` resolves the BODY against the cwd (tree A)
    # while the raw string `todo/x.md` matches tree B's cache node, so B's
    # statuses were applied to A's body and B's fingerprint could not detect it
    # (Codex re-adversarial round 6, section 19 review, [high]). A target that
    # does not live under the audited root has no answer in this cache, and
    # saying so is the only honest result.
    base = Path(_CACHE_LOADED["root"]) if _CACHE_LOADED["root"] \
        else _canonical_root(root)
    try:
        key = Path(path).resolve().relative_to(base).as_posix()
    except (OSError, ValueError) as exc:
        raise CacheUnusable(
            REASON_IDENTITY,
            f"{path} does not live under the audited root {base}; its status "
            f"cannot come from that tree's cache ({exc})") from exc
    return cache.get(key)


def _io_status(path, root, rows=()):
    """{section: IO-table status} from the cache, or None when NO cache exists.

    A LOADED CACHE THAT CANNOT ANSWER FOR THIS FILE IS AN INFRASTRUCTURE
    REFUSAL, not a quiet downgrade. Two shapes reach that, and both used to
    resolve to a status of `""`, which `_is_done` reads as "not done" -- so the
    `open-in-done` / `open-in-deferred` findings for those sections were simply
    never raised, and the audit reported success (Codex adversarial, section 19
    review, [high]):

      * the file has Implementation Order rows but NO node in the cache, so the
        weak pre-cache fallback (`stamped and quality`) silently replaced the
        real oracle rule -- the same approximation this module's `_is_done`
        docstring records as having given a wrong answer twice;
      * the node exists but omits a row the file itself declares, so that one
        section drops out of the comparison.

    Measured across the live corpus before this check was added: zero
    occurrences of either. `todo/TODO-00-INDEX.md` has no node AND no rows, so
    it is not one -- it is a file with nothing to audit.
    """
    cache = _load_cache(root)
    if _CACHE_LOADED["missing"]:
        return None
    status_map = _lookup(cache, path, root)
    if status_map is None:
        if rows:
            raise CacheUnusable(
                REASON_COVERAGE,
                f"the cache has no node for {path}, which declares "
                f"{len(rows)} Implementation Order row(s); falling back to the "
                f"pre-cache rule would answer from a different source than this "
                f"audit documents")
        return None
    gap = sorted(n for n in rows if n not in status_map)
    if gap:
        raise CacheUnusable(
            REASON_COVERAGE,
            f"the cache node for {path} is missing a status for Implementation "
            f"Order row(s) {gap[:5]}; an absent status reads as 'not done' and "
            f"silently suppresses this section's findings")
    return status_map


def _is_done(status, verified, quality, deferred, awaiting):
    """Mirror of sequencer_triage's DONE rule -- deliberately, not approximately.

    From its own docstring: DONE is a shipped `[x]`/`[/]` carrying BOTH
    Verified AND Quality-reviewed, OR an `[x]`/`[/]` carrying a TERMINAL
    Deferred stamp. A Deferred stamp bearing an `awaiting-*` token is BLOCKED,
    not DONE -- the run stays armed and fixpoint refuses -- so those sections
    ARE revisited and their items are reachable.

    Approximating this cost a wrong answer once already: keying on `[x]` alone
    reported 23 findings where the real rule gives a different set, and keying
    on the Verified stamp alone (the pre-cache fallback) gave 350.
    """
    if status not in ("x", "/"):
        return False
    if deferred:
        return not awaiting          # awaiting-* => BLOCKED => still revisited
    return verified and quality


def audit(path, root="."):
    """[(kind, section, detail)] for unreachable work in one file."""
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError:
        return []
    scan = _fence.scan_text(text)
    lines, mask = scan.lines, scan.mask
    # A malformed document masks from its opener to EOF, so EVERY walk below
    # returns empty and this gate -- the one that holds `phase FIXPOINT` open --
    # would report the file clean precisely because it could not read it. Name
    # it instead. `build.py:1030` makes the same call on the producer side; a
    # gate that went quiet here would silently disagree with it about the same
    # file (Codex design review, section 38, [high]).
    reason = scan.unclosed_reason()
    if reason:
        # The CATEGORY comes from the shared mapping too, not from a local
        # if/else over two flags -- with seven HTML block types a hand-written
        # ternary here would silently label an unclosed `<script>` a comment.
        return [(_fence.terminal_category(scan.terminal), 0, reason)]
    rows = _io_rows(scan)
    status_map = _io_status(path, root, rows)
    out = []
    # TWO PROJECTIONS, DELIBERATELY, because the body's matchers disagree about
    # what a container prefix means and section 47 measured the disagreement
    # rather than picking a winner:
    #
    #   ITEMS read `views` and SKIP blockquoted lines. `views` strips outer
    #   containers, so an indented `  - [x]` under a nested heading matches and
    #   is owned by that heading -- which is the whole point. But it also strips
    #   the `> ` from a quoted item's CONTINUATION lines (the opener keeps its
    #   marker, the rest do not), so `> - [ ] quoted example` on line 2 of a
    #   blockquote would become a live open item. The predicate, not the
    #   projection, is what keeps a quoted example out.
    #
    #   STAMPS read PHYSICAL lines. `VERIFIED_RE`/`DEFERRED_RE`/`QUALITY_RE` are
    #   anchored `^> \*\*` and `views` keeps that marker only on the line that
    #   OPENS the blockquote -- every CONTINUATION line loses it. Measured
    #   2026-08-12 on the ordinary two-line stamp block: `> **Verified:**`
    #   matches and the `> **Quality reviewed:**` under it does NOT, so a fully
    #   stamped section would read verified-but-unreviewed and silently stop
    #   being DONE. That is a partial, asymmetric corruption rather than a loud
    #   one, which is what makes it dangerous in the gate holding `phase
    #   FIXPOINT` open. Staying physical also keeps this in agreement with
    #   `sequencer_triage.section_stamps()`
    #   (`.claude/hooks/sequencer_triage.py:56`), the authority `_is_done`
    #   mirrors, which reads physical lines at column zero (Codex design
    #   review, section 47, [medium]).
    #
    # This is the same split `build.py` already makes by excluding
    # `_walk_stamps_xrefs` from the projection closure (`build.py:1077`): a walk
    # that matches ON the container marker cannot be given a projection that
    # removes it.
    #
    # ONE STREAMING PASS PER SECTION, over BOUNDS rather than a materialised
    # index list. The first version yielded `[j for j in range(...)]` and built
    # both projections from it, which replaced a list of pointers to existing
    # strings with a list of freshly boxed integers plus two more full lists:
    # measured on a 200,002-line single-section fixture at 8,016,688 traced
    # bytes for the indices and 3,248,192 for the projections, extrapolating to
    # roughly 945 MB at the scanner's 16 MiB valid-input ceiling. It is linear
    # rather than quadratic (section ranges are disjoint), but a gate that
    # exhausts memory on a valid maximum-size document is refusing the tree for
    # the wrong reason (Codex perf, section 47, [high]).
    views, in_bq = scan.views, scan.in_blockquote
    for head, ln, (start, end) in _sections(scan):
        # An unusable heading number is REPORTED, never dropped. Dropping it
        # would merge this section's items into the previous section's body and
        # then judge them against the wrong Implementation Order row, which is a
        # wrong answer rather than a missing one. The remaining per-section
        # checks all key on the number, so there is nothing further to say about
        # this one until it is renumbered.
        if head.kind == "over-long":
            out.append(("unusable-heading", 0,
                        _fence.heading_report(ln + 1, head)))
            continue
        num = head.n
        opens, parked = [], []
        deferred = stamped = quality = awaiting = False
        for j in range(start, end):
            if mask[j]:
                continue
            # STAMPS off the physical line. Every stamp pattern is anchored at
            # column 0 on `>`, so the cheap prefix test skips three regex calls
            # on the overwhelming majority of lines.
            phys = lines[j]
            if phys[:1] == ">":
                if DEFERRED_RE.match(phys):
                    deferred = True
                    if AWAITING_RE.search(phys):
                        awaiting = True
                elif VERIFIED_RE.match(phys):
                    stamped = True
                elif QUALITY_RE.match(phys):
                    quality = True
            # ITEMS off the projection, quoted examples excluded.
            if in_bq(j):
                continue
            v = views[j]
            if OPEN_ITEM_RE.match(v):
                opens.append(v.strip()[:90])
            elif PARKED_ITEM_RE.match(v):
                parked.append(v.strip())

        # 1. body with no Implementation Order row -- invisible to the oracle
        if num not in rows and (opens or parked):
            out.append(("no-io-row", num,
                        f"section {num} has {len(opens)+len(parked)} item(s) but no "
                        f"Implementation Order row; the oracle classifies from the "
                        f"row and never reads the body"))
            continue

        status = (status_map or {}).get(num, "")
        is_done = (_is_done(status, stamped, quality, deferred, awaiting)
                   if status_map is not None else (stamped and quality))

        # 2. open items in a DONE section that is not a recorded deferral
        if is_done and opens and not deferred:
            out.append(("open-in-done", num,
                        f"{len(opens)} open item(s) in a DONE section: {opens[0]}"))

        # 3. open items parked in a Deferred section -- wrong shape.
        # A `standing:`-marked item is EXCLUDED: it is recurring work that is
        # correctly a bare `- [ ]` and must never be converted to a park. Without
        # this the doctrine and the gate contradicted each other -- the sequencer
        # skill instructs the run to LEAVE such an item alone, and this detector
        # then refused fixpoint on it, so a correctly-shaped corpus could never
        # complete. Found by the run itself, 2026-08-05, on an annual
        # trigger-review item.
        real_opens = [b for b in opens
                      if not STANDING_RE.search(b)]
        if is_done and real_opens and deferred:
            out.append(("open-in-deferred", num,
                        f"{len(real_opens)} open `- [ ]` item(s) inside a Deferred "
                        f"section (should be `- [/]` naming the blocker): "
                        f"{real_opens[0]}"))

        # NO "parked-ownerless" RULE. It was tried on 2026-08-02 and REMOVED:
        # `[/]` means IN PROGRESS in this repo, not "parked awaiting an owner"
        # (implement-todo-section: "`[x]` fully done or `[/]` in progress"), so
        # requiring every `[/]` to name a blocker misread 329 ordinary
        # progress markers -- e.g. `- [/] GetEnvironmentVariableA(...)` -- as
        # defects. A detector that cries wolf 329 times is worse than none.
    return out


def _targets(argv):
    paths = [a for a in argv if not a.startswith("-")]
    if paths:
        return paths
    return sorted(str(p) for p in Path("todo").rglob("TODO-*.md"))


def main(argv) -> int:
    as_json = "--json" in argv
    findings = {}
    # ONE RUN, ONE GENERATION. A second `main()` in the same interpreter must
    # re-derive everything rather than inherit the first run's pinned verdict --
    # including a pinned "no cache", which would otherwise make a cache built
    # between the two runs invisible to the second (Codex re-adversarial round
    # 5, section 19 review, [high]).
    _reset_cache_state()
    try:
        # PRIME THE CACHE BEFORE ANY TODO BODY IS READ. `audit()` reads its
        # file and only THEN calls `_io_status`, which is what initialises the
        # fingerprint -- so on the first target the body was read BEFORE the
        # generation was pinned. A concurrent rebuild landing in that gap paired
        # old body lines with a new cache AND a new fingerprint, so the
        # post-walk check saw an unchanged corpus and published a
        # mixed-generation verdict: precisely the race this binding exists to
        # close (Codex adversarial round 2, [medium]).
        _load_cache(".")
        # ROOT CONTAINMENT IS CHECKED HERE, NOT ONLY IN THE CACHE LOOKUP.
        # Refusing a target from another tree used to depend on a cache being
        # loaded: the check lived in `_cache_status`, so with NO cache the
        # missing-cache fallback audited the foreign file per-file and answered
        # rc 1 with a confident finding about it. Reproduced 2026-08-08 -- a
        # file under /tmp was reported as `[no-io-row] section 1 ...`, which is
        # "an answer that looks authoritative and is not", the exact thing the
        # refusal exists to prevent. The two rationales were never in conflict:
        # the missing-cache fallback is for auditing YOUR OWN tree before a
        # cache exists (a fresh clone), and containment is knowable with no
        # cache at all, so hoisting it satisfies both.
        base = Path(_CACHE_LOADED["root"]) if _CACHE_LOADED["root"] \
            else _canonical_root(".")
        for path in _targets(argv):
            try:
                Path(path).resolve().relative_to(base)
            except (OSError, ValueError) as exc:
                raise CacheUnusable(
                    REASON_IDENTITY,
                    f"{path} does not live under the audited root {base}; "
                    f"this audit describes one tree and cannot answer for a "
                    f"file from another ({exc})") from exc
            hits = audit(path)
            if hits:
                findings[path] = hits
        # CLOSE THE GENERATION WINDOW BEFORE PUBLISHING. Verdicts derived from
        # a corpus that moved under the walk are not verdicts.
        if _CACHE_CORPUS.get("corpus") is not None:
            _cs.check_corpus_unchanged(_CACHE_CORPUS["todo_root"],
                                       _CACHE_CORPUS["corpus"])
    except _cs.CacheSchemaError as exc:
        # `check_corpus_unchanged` raises the shared error directly. Same
        # destination as a load failure: infrastructure, never a verdict.
        sys.stderr.write(f"todo-reachability: corpus moved during the audit "
                         f"[{exc.reason}]: {exc}\n")
        return EXIT_INFRA
    except CacheUnusable as exc:
        # EXIT_INFRA, never 1. A caller distinguishing "unreachable items exist"
        # from "the audit could not run" depends on this separation.
        sys.stderr.write(f"todo-reachability: cache unusable "
                         f"[{exc.reason}]: {exc}\n")
        return EXIT_INFRA
    if as_json:
        print(json.dumps(findings, indent=2))
    else:
        total = sum(len(v) for v in findings.values())
        by_kind = {}
        for hits in findings.values():
            for kind, _, _ in hits:
                by_kind[kind] = by_kind.get(kind, 0) + 1
        for path, hits in sorted(findings.items()):
            print(f"\n{path}")
            for kind, num, detail in hits:
                print(f"  [{kind}] {detail}")
        print(f"\n{total} unreachable finding(s) across {len(findings)} file(s)")
        for k, v in sorted(by_kind.items(), key=lambda kv: -kv[1]):
            print(f"  {v:4d}  {k}")
    return 1 if findings else 0


def _selftest() -> int:
    import tempfile
    ok = True
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "TODO-99-x.md"
        p.write_text(
            "# X\n\n## Implementation Order\n\n| 💎 | 1 | a | -- | [x] |\n\n"
            "## 1. Stamped and done\n\n- [x] done\n- [ ] sneaked in\n"
            "> **Verified:** 2026-01-01 | commit `x`\n"
            # BOTH stamps are required for DONE -- a shipped-but-unreviewed
            # section is NEEDS_WORK, so a fixture with Verified alone is
            # correctly NOT flagged (this fixture originally omitted the
            # Quality-reviewed line and the selftest failed for the right
            # reason).
            "> **Quality reviewed:** 2026-01-01 | Codex\n\n"
            "## 2. No IO row at all\n\n- [ ] invisible work\n")
        hits = {k for k, _, _ in audit(str(p), root=td)}
        for want in ("open-in-done", "no-io-row"):
            if want not in hits:
                print(f"FAIL: {want} not detected ({hits})"); ok = False
    print("todo-reachability selftest", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(_selftest() if "--selftest" in sys.argv else main(sys.argv[1:]))
