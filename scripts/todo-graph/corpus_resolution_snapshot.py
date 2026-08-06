#!/usr/bin/env python3
# ============================================================================
# corpus_resolution_snapshot.py -- record what resolve_symbol() ACTUALLY
# resolves across the live TODO corpus, as (file, symbol) -> (path, start, end).
#
# Why this exists. Check 7's coverage gate (scripts/lint/stub-lint-baseline.json)
# records a COUNT, and a count is not a mapping. A change can hold the count at
# 55 while silently re-pointing an existing symbol at the wrong line range, or
# while a wrong new resolution offsets a lost one. That is exactly the blind spot
# that let TODO-06 section 10's first attempt pass 115/115 tests plus its own
# mutation-checked fixtures and still take coverage from 52/186 to 1/186
# (reverted, 5cff59cc): synthetic fixtures proved the NEW shapes parsed, and
# nothing checked that the shapes which already worked still did.
#
# So the contract this tool enforces is stronger than the count:
#   every (file, symbol) resolved BEFORE a change must resolve to the SAME
#   (path, line_start, line_end) AFTER it. New resolutions are additions only,
#   and are printed so a human ground-truths them rather than trusting the count.
#
# Usage:
#   corpus_resolution_snapshot.py write <out.json>       # snapshot current tree
#   corpus_resolution_snapshot.py compare <before.json>  # diff vs current tree
#   corpus_resolution_snapshot.py compare <before.json> --strict
#       As above, but GAINED and ADDED fail too. For an AUTOMATED caller (the
#       section 16 identity gate), which has no human to act on the
#       "(GROUND-TRUTH BY HAND)" lines that make them non-failing below.
#
# Exit codes:
#   0  compare: no prior verdict was dropped, lost, moved or reclassified
#      (and, under --strict, nothing was gained or added either)
#   1  compare: at least one prior VERDICT changed for the worse -- any of
#      DROPPED (the occurrence is gone), LOST (it no longer resolves), MOVED
#      (different coordinates) or CHANGED (a different unresolved bucket). All
#      four are real regressions. GAINED and ADDED are not, and exit 0 --
#      EXCEPT under --strict, where both also return 1. Stating that here
#      unqualified contradicted the --strict line above and the code (Codex
#      consistency, section 16).
#   2  usage error (wrong argv shape)
#   3  INFRASTRUCTURE failure -- the walk could not complete at all, so the
#      exit code above is NOT a verdict on regression: an invalid/missing/
#      truncated cache or baseline (CacheError), OR the resolver itself
#      refused an input (ResolverInputError -- an oversized file, or one
#      rewritten mid-run so returned coordinates no longer describe it).
#      Distinct from 0/1 by design: a caller must never read "the gate
#      could not run" as "the gate passed".
# ============================================================================

import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ref_resolution as _rr  # noqa: E402
from resolve_symbol import ResolverInputError  # noqa: E402


def _repo_root() -> Path:
    return Path(os.environ.get("STUB_LINT_REPO_ROOT", ".")).resolve()


def _cache_path() -> Path:
    return Path(os.environ.get("STUB_LINT_CACHE", "build/todo-cache.json"))


# 2: every `kind=symbol` occurrence carries a verdict, so a mapping value is
# either a [path, start, end] list OR a bucket string, and keys carry the
# AUTHORED spelling rather than the effective path. A schema-1 baseline is
# REFUSED with a regenerate pointer rather than mis-compared: under schema 1 a
# ref that merely failed to resolve was recorded as `null` and one that never
# reached resolution was absent entirely, so silently reading it here would
# report every newly-verdicted ref as an addition and every former null as a
# bucket change -- thousands of lines of noise around any real regression.
SNAPSHOT_SCHEMA = 2


class CacheError(RuntimeError):
    """The cache cannot support a trustworthy snapshot."""


def _load_nodes() -> list:
    """Parse AND VALIDATE the cache.

    A gate that fails OPEN is worse than no gate. `collect()` originally trusted
    any JSON: a cache of `[]` or `[{}]` yielded zero refs, `write` exited 0, and
    the resulting empty baseline made `compare` report "OK, 54 added" -- a
    vacuous pass in which a resolver regression on any ref absent from that
    baseline is never checked. Semantic emptiness and staleness are the COMMON
    corruption paths (a syntax error already failed loudly), so both are refused
    here rather than silently producing a passing snapshot.
    """
    p = _cache_path()
    try:
        nodes = json.loads(p.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise CacheError(f"cache not found: {p} (run build-and-validate.sh)")
    except (OSError, ValueError, RecursionError) as exc:
        # ValueError, not JSONDecodeError: invalid UTF-8 raises
        # UnicodeDecodeError, which is a ValueError and neither an OSError nor a
        # decode error, so it escaped this handler entirely. The identical
        # defect was fixed on the BASELINE reader below; leaving the CACHE
        # reader narrower meant the same corrupt-input class exited 1 here and
        # 3 there (Codex adversarial, section 14).
        raise CacheError(f"cache unreadable: {p}: {exc}")
    if not isinstance(nodes, list) or not nodes:
        raise CacheError(f"cache is not a non-empty node array: {p}")
    if not any(isinstance(n, dict) and n.get("stamped_items") for n in nodes):
        raise CacheError(
            f"cache carries no stamped_items -- it predates the section 9 "
            f"extension, or came from a different generator: {p}")
    # EVERY node, item and ref must have the shape `collect()` will `.get` on.
    # The `any(...)` above only proves ONE node is well-formed, so a mixed cache
    # -- `[{good}, "junk"]` -- passed here and then raised AttributeError deep in
    # the walk, which `main()` does not catch: it escapes as a traceback and a
    # bare exit 1, colliding with exit 1's documented "a prior verdict changed".
    # `check_stub_behind_stamp` maps unexpected walk failures to an
    # infrastructure code; this caller only caught CacheError and
    # ResolverInputError, so the shape is rejected UP FRONT instead (Codex
    # adversarial, section 14).
    for i, n in enumerate(nodes):
        if not isinstance(n, dict):
            raise CacheError(f"cache node {i} is not an object ({type(n).__name__}): {p}")
        items = n.get("stamped_items")
        if items is None:
            continue
        if not isinstance(items, list):
            raise CacheError(f"cache node {i} stamped_items is not a list: {p}")
        for j, it in enumerate(items):
            if not isinstance(it, dict):
                raise CacheError(
                    f"cache node {i} item {j} is not an object: {p}")
            # ITEM-LEVEL SCALARS TOO, for the same reason the ref scalars are
            # checked below. `collect()` uses `section_n` as a DICT KEY
            # (`by_section.setdefault`), so a list- or dict-valued one is
            # unhashable and raises TypeError from inside the walk -- uncaught,
            # so the process exits 1, which is this tool's DOCUMENTED code for
            # "a prior verdict changed". A malformed cache would therefore be
            # reported to the section 16 gate as a resolver REGRESSION rather
            # than as the infrastructure failure the header promises (Codex
            # consistency, section 16).
            for field in ("section_n", "item_idx"):
                v = it.get(field)
                if v is not None and not isinstance(v, (str, int, float)):
                    raise CacheError(
                        f"cache node {i} item {j} `{field}` is "
                        f"{type(v).__name__}, expected a scalar: {p}")
                if isinstance(v, bool):
                    raise CacheError(
                        f"cache node {i} item {j} `{field}` is a bool, "
                        f"expected a scalar: {p}")
            refs = it.get("refs")
            if refs is None:
                continue
            if not isinstance(refs, list):
                raise CacheError(
                    f"cache node {i} item {j} refs is not a list: {p}")
            for k, r in enumerate(refs):
                if not isinstance(r, dict):
                    raise CacheError(
                        f"cache node {i} item {j} ref {k} is not an object: {p}")
                # SCALAR TYPES TOO, not just containers. `classify_ref` calls
                # `rel.startswith(...)` on a ref's `file`, so an integer there
                # passed container validation and then raised AttributeError
                # from inside the walk -- escaping as rc 1 and colliding with
                # exit 1's documented "a prior verdict changed" (Codex
                # adversarial, section 14).
                for field in ("kind", "file", "symbol"):
                    v = r.get(field)
                    if v is not None and not isinstance(v, str):
                        raise CacheError(
                            f"cache node {i} item {j} ref {k} `{field}` is "
                            f"{type(v).__name__}, expected string: {p}")
    # Staleness, mirroring check_stub_behind_stamp.py: a cache older than the
    # newest TODO describes a tree that no longer exists, and a baseline taken
    # from it silently omits live refs.
    # Walk the SELECTED repository's todo/, not the process CWD's. Using a bare
    # relative "todo" meant the staleness check silently measured nothing
    # whenever the tool ran from anywhere but the repo root -- exactly the
    # fail-open the rest of this function exists to close.
    #
    # os.walk SWALLOWS traversal errors unless given an onerror callback -- a
    # missing root simply yields nothing and an unreadable subtree is skipped.
    # Wrapping the walk in try/except therefore caught nothing: an unreadable
    # TODO subtree would leave `newest` low and an old cache would look fresh,
    # which is the exact fail-open this check exists to close.
    def _walk_err(exc):
        raise CacheError(f"cannot traverse todo tree: {exc}")

    todo_root = _repo_root() / "todo"
    if not todo_root.is_dir():
        raise CacheError(f"todo root is not a readable directory: {todo_root}")
    try:
        cache_m = p.stat().st_mtime
        newest = 0.0
        for dirpath, _, files in os.walk(todo_root, onerror=_walk_err):
            for f in files:
                if f.startswith("TODO-") and f.endswith(".md"):
                    newest = max(newest,
                                 os.stat(os.path.join(dirpath, f)).st_mtime)
    except OSError as exc:
        # A stat failure is an INFRASTRUCTURE error, not "fresh enough".
        raise CacheError(f"cannot determine cache freshness: {exc}")
    if newest == 0.0:
        raise CacheError(f"no TODO files found under {todo_root} -- refusing to "
                         f"call a cache fresh against an empty corpus")
    if newest > cache_m:
        raise CacheError(
            f"cache is STALE (a TODO is newer than {p}); rebuild via "
            f"scripts/todo-graph/build-and-validate.sh --keep-cache")
    return nodes


def collect() -> dict:
    """Return {"<todo>#<sec>.<item>r<n> <file>::<symbol>": [path, start, end]
    | "<bucket>"} for EVERY `kind=symbol` ref in the cache's stamped_items --
    the exact ref population lint Check 7 walks, with no ref left unverdicted.

    THE POPULATION IS THE WHOLE POPULATION. This used to `continue` on any ref
    that did not reach resolution and record a bare `None` for one that reached
    it and failed, so the gate covered 695 of Check 7's 1,625 occurrences: 930
    refs had no entry at all (unpaired-ref 858, missing-file 8, non-C-suffix 63,
    path-escape 1) and the 221 nulls did not say WHICH bucket they landed in.
    Section 13 is the worked example of the hole -- it moved refs between
    `calllike-unresolved` (35 -> 132) and `no_calllike_token` (47 -> 89) at
    scale, and nothing gated the movement: the resolved floor only saw the total
    rise, and the bucket line is printed but was never compared. A change that
    quietly reclassified hundreds of refs the other way would have read as clean.

    KEYED PER OCCURRENCE, not per (file, symbol). Deduping looked harmless --
    the same pair always resolves the same way -- but it quietly weakened the
    contract. Seven pairs in the live corpus appear twice; if one occurrence
    disappears or changes away from `kind=symbol`, the surviving occurrence
    holds the key and the tuple, so `compare` reports OK while a stamped
    reference really did regress.

    THE KEY CARRIES THE AUTHORED SPELLING, NOT RESOLVER OUTPUT. It used to
    carry the EFFECTIVE path, which made the key a function of the very thing
    the gate measures: a basename-repaired ref that stopped resolving lost its
    effective path, so its key changed and the regression reported as a DROPPED
    plus an ADDED instead of a LOST, obscuring what actually happened. The
    authored spelling is a pure function of the TODO text, so an occurrence
    keeps one key across every verdict it can ever have. Byte-compatibility
    with a schema-1 baseline was considered and is worth nothing here: `compare`
    refuses any baseline whose `schema` is not SNAPSHOT_SCHEMA, so an old
    baseline can never be compared against these keys anyway (Codex design
    review, section 14).
    """
    root = _repo_root()
    nodes = _load_nodes()
    out = {}
    for node in nodes:
        todo_path = node.get("file_path") or "?"
        items = node.get("stamped_items") or []
        by_section = {}
        for it in items:
            by_section.setdefault(it.get("section_n"), []).append(it)
        # LAZY, and the laziness now lives INSIDE the scope rather than in a
        # `_needs` set copied into both callers: only a section that actually
        # holds an unpaired symbol builds candidates, and 357 of the corpus's
        # 830 sections do (Codex perf, post-commit). The scope also carries the
        # scope is also where the section's unpaired-symbol set is derived, so
        # both gates agree on the pairing population without either computing it.
        sec_scopes = {
            sec: _rr.section_scope(sec_items, root)
            for sec, sec_items in by_section.items()
        }
        for it in items:
            sec = it.get("section_n", "?")
            idx = it.get("item_idx", "?")
            scope = sec_scopes.get(it.get("section_n")) or _rr.EMPTY_SCOPE
            for ref_i, ref in enumerate(it.get("refs") or []):
                if ref.get("kind") != "symbol":
                    continue
                sym = ref.get("symbol")
                # ONE SHARED END-TO-END RULE, not a mirrored walk. Check 7 and
                # this snapshot both call `resolve_ref` and neither orchestrates
                # a resolver step itself. Mirroring by hand is what shipped an
                # incomplete proof twice: a snapshot over a NARROWER population
                # than Check 7 walks silently omits the mappings a change added
                # (the decl-following draft covered 36 of 54), and one over a
                # WIDER population is not a gate on Check 7 at all. Sharing only
                # the pre-resolution half left the same hazard one level down --
                # each caller ran its own direct-resolve -> follow_declaration
                # sequence, so a fallback added to one and not the other
                # recreated the defect exactly (Codex design review, section 14).
                result = _rr.resolve_ref(ref, scope, root)
                # AUTHORED spelling, with an explicit sentinel when the TODO
                # names none -- see the key rationale in the docstring. `-` is
                # not ambiguous with a real value: a ref's stored file is either
                # a path or absent, and a symbol is either present or the ref is
                # `unpaired_ref` by definition.
                a_file = ref.get("file") or "-"
                key = f"{todo_path}#{sec}.{idx}r{ref_i} {a_file}::{sym or '-'}"
                # A COLLISION IS A HARD ERROR, never a silent overwrite. The key
                # is assembled from stringified fields, so distinct occurrences
                # CAN collide -- `section_n` 1 and "1" render identically, a
                # field carrying the `#`/`::`/`r` delimiters is ambiguous, and a
                # TODO with two headings of the same number restarts item_idx.
                # `out[key] = ...` would drop one of them while Check 7 still
                # counts both, so `refs` would record the collapsed total and
                # the resulting baseline would be SELF-CONSISTENT: compare()
                # passes forever over a population quietly smaller than the
                # lint's. That is precisely the divergence this section exists
                # to make impossible, so it fails loudly instead (rc 3). The
                # live corpus has 1,626 unique keys, but nothing except this
                # check keeps that true (Codex adversarial, section 14).
                if key in out:
                    raise CacheError(
                        f"duplicate occurrence key {key!r} -- two refs share one "
                        f"identity, so the gate would silently verdict fewer "
                        f"refs than lint Check 7 counts")
                out[key] = (result.bucket if result.bucket is not None
                            else [result.def_rel, result.line_start,
                                  result.line_end])
    return out


def main(argv) -> int:
    # `--strict` is parsed OUT of argv before the shape check, so the positional
    # contract below is unchanged. It exists for the section 16 wiring: an
    # AUTOMATED caller has no human to read "(GROUND-TRUTH BY HAND)", so the
    # outcomes that are advisory for a human must be failing ones for it. The
    # alternative -- having the driver grep this tool's stdout for "  GAINED "
    # lines -- would let a print-format tweak silently disable the gate, which
    # is the exact fail-open class this file exists to close (Codex design
    # review, section 16).
    strict = "--strict" in argv
    argv = [a for a in argv if a != "--strict"]
    if len(argv) != 2 or argv[0] not in ("write", "compare"):
        sys.stderr.write(
            "usage: corpus_resolution_snapshot.py write <out.json>\n"
            "       corpus_resolution_snapshot.py compare <before.json> "
            "[--strict]\n")
        return 2
    if strict and argv[0] != "compare":
        # A no-op flag a caller BELIEVES is protecting them is worse than an
        # absent one: `write --strict` would look gated and gate nothing.
        sys.stderr.write("[corpus_resolution_snapshot] --strict applies to "
                         "`compare` only\n")
        return 2

    try:
        # ONE WALK, ONE CACHE LIFETIME -- see the same wrapping in
        # check_stub_behind_stamp.main. Both gates must share the lifecycle for
        # the same reason they share the resolution rule.
        with _rr.walk_scope():
            now = collect()
    except CacheError as exc:
        # Exit 3 == INFRASTRUCTURE, deliberately distinct from 0 (pass) and
        # 1 (regression). A caller must never read "the gate could not run" as
        # "the gate passed".
        sys.stderr.write(f"[corpus_resolution_snapshot] {exc}\n")
        return 3
    except ResolverInputError as exc:
        # `collect()` calls resolve_symbol() per ref, and resolve_symbol() now
        # raises this for an input it refuses to answer about (oversized file,
        # or one that changed on disk mid-run) rather than silently returning
        # None. Left uncaught, this surfaced as an unhandled traceback and a
        # bare process exit 1 -- colliding with exit 1's DOCUMENTED meaning
        # here (a prior mapping was lost or moved), which a caller reading
        # only the exit code would misread as a real regression instead of
        # "the walk could not complete". Same INFRASTRUCTURE code as
        # CacheError, for the same reason: not a result, not a pass.
        sys.stderr.write(f"[corpus_resolution_snapshot] resolver input "
                         f"refused: {exc}\n")
        return 3
    # A RESOLVED entry is a list; every other entry is a bucket string. Under
    # schema 1 this test was `v is not None`, which is exactly the test that
    # stops working once an unresolved ref carries a bucket instead of a null --
    # left unchanged it would have counted all 1,625 refs as resolved.
    resolved_now = {k: v for k, v in now.items() if isinstance(v, list)}
    if not now:
        sys.stderr.write("[corpus_resolution_snapshot] no eligible refs -- "
                         "refusing to treat an empty population as a result\n")
        return 3

    if argv[0] == "write":
        # SELF-DESCRIBING. The bare mapping-only format could not distinguish a
        # complete baseline from a truncated one: a single-entry file compared
        # clean, reporting the other 56 resolved mappings as ADDED and exiting 0.
        # Recording the population lets `compare` detect truncation instead of
        # trusting the file's own size.
        # A FAILED WRITE IS INFRASTRUCTURE, not a regression verdict. An
        # unwritable path or a directory target raised straight out of
        # `write_text` as a traceback and a bare exit 1 -- the one code that
        # means "a prior verdict changed" (reproduced with a directory target,
        # Codex consistency, section 14).
        try:
            Path(argv[1]).write_text(json.dumps({
                "schema": SNAPSHOT_SCHEMA,
                "refs": len(now),
                "resolved": len(resolved_now),
                "mappings": now,
            }, indent=1, sort_keys=True), encoding="utf-8")
        except (OSError, ValueError) as exc:
            sys.stderr.write(f"[corpus_resolution_snapshot] cannot write "
                             f"snapshot {argv[1]}: {exc}\n")
            return 3
        print(f"snapshot: {len(resolved_now)} resolved of {len(now)} refs "
              f"-> {argv[1]}")
        return 0

    # VALIDATE THE BASELINE TOO. Validating only the live cache left the other
    # input fail-open: a `{}` baseline, or one whose mappings are all null,
    # yields an empty `resolved_before`, so every current mapping classifies as
    # ADDED, `lost` and `moved` stay empty, and compare exits 0 having checked
    # nothing. That is the same vacuous pass the cache validation above exists
    # to prevent, on the input the gate is actually comparing against.
    try:
        before = json.loads(Path(argv[1]).read_text(encoding="utf-8"))
    except (OSError, ValueError, RecursionError) as exc:
        # ValueError, not just JSONDecodeError: invalid UTF-8 raises
        # UnicodeDecodeError, which is a ValueError and NOT an OSError, so the
        # narrower tuple let it escape as an uncaught traceback and a bare
        # process exit 1 -- colliding with exit 1's DOCUMENTED meaning here (a
        # prior verdict changed), which a caller reading only the exit code
        # would misread as a real regression instead of "the gate could not
        # run". Deeply-nested JSON raises RecursionError, which is not a
        # ValueError at all. `check_stub_behind_stamp._read_baseline` already
        # catches ValueError for precisely this reason; this is the same
        # contract on the other input (Codex adversarial, section 14).
        sys.stderr.write(f"[corpus_resolution_snapshot] baseline unreadable: "
                         f"{argv[1]}: {exc}\n")
        return 3
    if not isinstance(before, dict) or not before:
        sys.stderr.write(f"[corpus_resolution_snapshot] baseline is not a "
                         f"non-empty object: {argv[1]}\n")
        return 3
    if before.get("schema") != SNAPSHOT_SCHEMA:
        sys.stderr.write(f"[corpus_resolution_snapshot] baseline schema is "
                         f"{before.get('schema')!r}, expected {SNAPSHOT_SCHEMA}"
                         f" -- regenerate it with `write`\n")
        return 3
    mappings = before.get("mappings")
    if not isinstance(mappings, dict) or not mappings:
        sys.stderr.write("[corpus_resolution_snapshot] baseline has no "
                         "mappings object\n")
        return 3
    # TRUNCATION: the recorded population must match what the file actually
    # holds. Without this a one-entry baseline passed cleanly.
    if before.get("refs") != len(mappings):
        sys.stderr.write(f"[corpus_resolution_snapshot] baseline is TRUNCATED: "
                         f"declares {before.get('refs')} refs, holds "
                         f"{len(mappings)}\n")
        return 3
    # ELEMENT TYPES, not just arity. `[path, 411.0, 442.0]` satisfied a
    # length-3 check and compared EQUAL to the integer tuple under Python, so
    # an arity-only check let a malformed baseline through.
    #
    # A BUCKET IS VALIDATED AGAINST THE EXACT ENUM, not merely "is a string".
    # The bucket names are ref_resolution's published contract; accepting an
    # unrecognised one would let a typo'd or retired bucket sit in the baseline
    # comparing unequal against every future run forever -- a permanent CHANGED
    # that no amount of correct behaviour can clear.
    bad = []
    for k, v in mappings.items():
        if isinstance(v, str):
            if v not in _rr.ALL_BUCKETS:
                bad.append(k)
            continue
        if not (isinstance(v, list) and len(v) == 3):
            bad.append(k)
            continue
        pth, a, b = v
        if not isinstance(pth, str) or not pth:
            bad.append(k)
        elif isinstance(a, bool) or isinstance(b, bool):
            bad.append(k)
        elif not isinstance(a, int) or not isinstance(b, int):
            bad.append(k)
        elif a < 1 or b < a:
            bad.append(k)
    if bad:
        sys.stderr.write(f"[corpus_resolution_snapshot] baseline has "
                         f"{len(bad)} malformed mapping(s), e.g. {bad[0]!r}\n")
        return 3
    before = mappings
    resolved_before = {k: v for k, v in before.items() if isinstance(v, list)}
    if not resolved_before:
        sys.stderr.write("[corpus_resolution_snapshot] baseline records ZERO "
                         "resolved mappings -- there is nothing to protect, so "
                         "a pass would be vacuous\n")
        return 3

    # SIX OUTCOMES, and the split between them is the point of section 14.
    # Before, a ref that stopped resolving and a ref that never had a verdict
    # were the same `None`, and a ref moving between unresolved buckets was not
    # recorded at all.
    #
    #   DROPPED  the occurrence is gone from the corpus entirely
    #   LOST     resolved before, now only a bucket
    #   MOVED    resolved before and after, at different coordinates
    #   CHANGED  unresolved before and after, in a DIFFERENT bucket
    #   GAINED   unresolved before, resolved now -- a coverage win
    #   ADDED    a key the baseline did not carry
    dropped, lost, moved, changed, gained = [], [], [], [], []
    for k, v in before.items():
        nv = now.get(k)
        if nv is None:
            dropped.append((k, v))
        elif isinstance(v, list) and isinstance(nv, list):
            if nv != v:
                moved.append((k, v, nv))
        elif isinstance(v, list):
            lost.append((k, v, nv))
        elif isinstance(nv, list):
            gained.append((k, v, nv))
        elif nv != v:
            changed.append((k, v, nv))
    added = sorted(k for k in now if k not in before)

    print(f"before: {len(resolved_before)} resolved of {len(before)} refs")
    print(f"after:  {len(resolved_now)} resolved of {len(now)} refs")
    for k, v in dropped:
        print(f"  DROPPED {k} was {v}")
    for k, v, nv in lost:
        print(f"  LOST    {k} was {v}, now bucket {nv!r}")
    for k, v, nv in moved:
        print(f"  MOVED   {k} {v} -> {nv}")
    for k, v, nv in changed:
        print(f"  CHANGED {k} bucket {v!r} -> {nv!r}")
    for k, v, nv in gained:
        print(f"  GAINED  {k} bucket {v!r} -> {nv}  (GROUND-TRUTH BY HAND)")
    for k in added:
        print(f"  ADDED   {k} -> {now[k]}  (GROUND-TRUTH BY HAND)")

    # CHANGED FAILS. It is not automatically a regression -- section 13's bucket
    # movements were the intended effect of a coverage win -- but the exit code
    # is the only part of this tool a wired gate can read, so reporting a
    # reclassification while exiting 0 would let a resolver defect that moved
    # hundreds of refs between buckets ship unattended (Codex design review,
    # section 14). The clearing path is the one this roadmap already uses for
    # every ADDED mapping: ground-truth the movement by hand, then regenerate
    # the baseline with `write`. GAINED and ADDED stay non-failing because
    # neither can hide a loss: every prior verdict is still accounted for.
    regressions = len(dropped) + len(lost) + len(moved) + len(changed)
    if regressions:
        print(f"\nFAIL: {len(dropped)} dropped, {len(lost)} lost, "
              f"{len(moved)} moved, {len(changed)} bucket-changed. A prior "
              f"verdict changed -- the resolved COUNT alone would not have "
              f"shown this. Ground-truth each line, then regenerate with "
              f"`write` if the change is intended.")
        return 1
    # STRICT: what is advisory for a human is failing for a machine. GAINED and
    # ADDED are non-failing above because neither can hide a LOSS, and that
    # reasoning is intact -- but it silently assumes a human reads the
    # "(GROUND-TRUTH BY HAND)" lines. A resolver defect that binds a previously
    # unresolved ref to the WRONG definition produces a GAINED, and under an
    # automated caller reading only the exit code it would ship green while
    # Check 7 goes on to inspect the wrong body (Codex design review, section
    # 16). Under --strict there is deliberately NO in-band clearing path: an
    # approval token a committer can mint is not approval, and this repo's
    # committer is frequently an unattended agent. Clearing a gain is a human
    # act -- ground-truth it, then regenerate the baseline with `write`.
    if strict and (gained or added):
        print(f"\nFAIL (--strict): {len(gained)} gained, {len(added)} added. "
              f"Neither can hide a loss, so both are non-failing for a HUMAN "
              f"caller -- but nothing here has ground-truthed them, and a "
              f"wrong new binding looks exactly like a coverage win. Verify "
              f"each line above by hand, then regenerate with `write`.")
        return 1
    print(f"\nOK: no prior verdict dropped, lost, moved or reclassified; "
          f"{len(gained)} gained, {len(added)} added.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
