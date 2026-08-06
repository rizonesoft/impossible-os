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

import base64
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cache_schema as _cs  # noqa: E402
import ref_resolution as _rr  # noqa: E402
import snapshot_protocol as _protocol  # noqa: E402
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
#
# RE-EXPORTED, no longer DEFINED here (section 18). It moved to the
# inert `snapshot_protocol.json` for the same reason the buckets did: it sat in
# THIS file beside `collect()` and `--strict`, both verdict-affecting, so the
# identity gate could not tell a schema migration from a resolver change and
# failed closed on every one (section 16). Existing `SNAPSHOT_SCHEMA` references
# in this module are unchanged.
SNAPSHOT_SCHEMA = _protocol.SNAPSHOT_SCHEMA


class CacheError(RuntimeError):
    """The cache cannot support a trustworthy snapshot."""


def _load_nodes() -> list:
    """Parse AND VALIDATE the cache, via the SHARED validator.

    A gate that fails OPEN is worse than no gate. `collect()` originally trusted
    any JSON: a cache of `[]` or `[{}]` yielded zero refs, `write` exited 0, and
    the resulting empty baseline made `compare` report "OK, 54 added" -- a
    vacuous pass in which a resolver regression on any ref absent from that
    baseline is never checked.

    The RULES now live in `cache_schema` so this reader and lint Check 7 agree
    about which caches are valid at all (section 17). The CODE does not move:
    every reason maps to this tool's single documented infrastructure exit 3,
    because a caller must never read "the gate could not run" as "the gate
    passed". `CacheError` is retained as this module's public failure type so
    `main()` and its tests keep their existing contract.
    """
    p = _cache_path()
    try:
        nodes, info = _cs.load_and_validate(p, _repo_root() / "todo")
    except _cs.CacheSchemaError as exc:
        raise CacheError(str(exc))
    # POLICY, not shape -- and it is THIS reader's policy, which is why the
    # shared module reports the population instead of ruling on it. A snapshot
    # taken over an empty stamped population is a vacuous baseline: every later
    # `compare` would report "OK, N added" and check nothing. The lint makes the
    # opposite call on the same fact (it lets the walk proceed so its own
    # population-regression gate adjudicates the loss), which is exactly the
    # divergence the shared RULE is allowed to keep (Codex design review,
    # section 17).
    if info.population == 0:
        raise CacheError(
            f"cache carries zero stamped items -- refusing to write a vacuous "
            f"baseline that would make every later compare report a clean "
            f"pass over nothing: {p}")
    return nodes, info.corpus


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
    nodes, corpus = _load_nodes()
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
    # RE-VERIFY THE CORPUS GENERATION NOW THE WALK IS DONE. The freshness check
    # inside `_load_nodes` bounds only its own few milliseconds; the window that
    # matters is the ~1s resolution walk just completed, during which a TODO
    # edit would leave these results describing a tree that no longer exists.
    # Raising here is correct: an infrastructure refusal (rc 3) rather than a
    # verdict computed from stale nodes (Codex adversarial, section 17 review).
    try:
        _cs.check_corpus_unchanged(root / "todo", corpus)
    except _cs.CacheSchemaError as exc:
        raise CacheError(str(exc))
    return out


class SnapshotInvalid(RuntimeError):
    """A snapshot FILE is unreadable, truncated, or malformed. Always rc 3."""


def _vocabulary_of(obj, label):
    """The bucket vocabulary a snapshot was written under.

    EACH SNAPSHOT DECLARES ITS OWN, which is what makes a bucket rename
    comparable at all. Validating both sides against HEAD's `ALL_BUCKETS` --
    the rule before section 18 -- meant that after a rename the base
    snapshot necessarily held the retired name, so the comparison died at
    validation with rc 3 (infrastructure) instead of reporting the rename as
    the CHANGED verdicts it actually is. The gate would refuse exactly the
    migration the protocol extraction exists to make possible (Codex design
    review, section 18).

    ABSENT is not an error: it means a snapshot written before this field
    existed, and its meaning is unambiguously the pre-existing rule -- it was
    written under the vocabulary of its own tree, and HEAD's is the only
    vocabulary available to check it against. This is the EXPAND step of
    expand/migrate/contract; the first rename AFTER both sides carry the field
    compares correctly. That compatibility is also why SNAPSHOT_SCHEMA is NOT
    bumped for this addition: a bump would make this section's own landing
    commit a protocol-AND-resolver change, which the separation rule must
    refuse -- the gate would refuse the very commit that installs it.
    """
    raw = obj.get("buckets")
    if raw is None:
        return tuple(_rr.ALL_BUCKETS)
    if not isinstance(raw, list) or not raw:
        raise SnapshotInvalid(f"{label}: 'buckets' is present but not a "
                              f"non-empty list")
    for v in raw:
        if not isinstance(v, str) or not v:
            raise SnapshotInvalid(f"{label}: 'buckets' holds a non-string or "
                                  f"empty entry {v!r}")
    if len(set(raw)) != len(raw):
        raise SnapshotInvalid(f"{label}: 'buckets' holds duplicate names")
    return tuple(raw)


def _halves_from_b64(blob, label, vocab):
    """Decode a caller-supplied [pre, post] pair and BIND it to `vocab`.

    Values alone are not evidence either. Without this binding a caller could
    supply fabricated-but-well-formed halves -- identical on both sides, say --
    and the relocation comparison would see nothing, which is the boolean
    attestation this replaced wearing a longer argument (Codex adversarial,
    section 18 round 5). Requiring the ordered concatenation to equal the
    snapshot's OWN declared vocabulary means the caller can only ever tell this
    tool how that vocabulary was SPLIT, never invent what it contains.
    """
    try:
        pair = json.loads(base64.b64decode(blob))
    except Exception as exc:  # noqa: BLE001 -- any decode failure is infra
        raise SnapshotInvalid(f"{label}: cannot decode the supplied bucket "
                              f"halves: {exc}") from exc
    if not isinstance(pair, list) or len(pair) != 2:
        raise SnapshotInvalid(f"{label}: supplied halves must be a [pre, post] "
                              f"pair")
    for half in pair:
        if not isinstance(half, list) or not half \
                or not all(isinstance(b, str) and b for b in half):
            raise SnapshotInvalid(f"{label}: supplied halves must each be a "
                                  f"non-empty list of names")
    pre, post = tuple(pair[0]), tuple(pair[1])
    if set(pre) & set(post):
        raise SnapshotInvalid(f"{label}: supplied halves overlap")
    if len(set(pre)) != len(pre) or len(set(post)) != len(post):
        raise SnapshotInvalid(f"{label}: supplied halves hold duplicates")
    if pre + post != tuple(vocab):
        raise SnapshotInvalid(
            f"{label}: supplied halves {list(pre)} + {list(post)} do not "
            f"reproduce the snapshot's own declared vocabulary "
            f"{list(vocab)} -- a caller may say how the vocabulary is SPLIT, "
            f"never what it contains")
    return pre, post


def _halves_of(obj, label, vocab):
    """The (pre, post) split a snapshot was written under, or None if absent.

    Absent means a snapshot written before section 18 recorded the split; the
    caller treats that as "cannot adjudicate a relocation" rather than "no
    relocation happened", because the two are not the same claim.
    """
    pre, post = obj.get("buckets_pre"), obj.get("buckets_post")
    if pre is None and post is None:
        return None
    for half, name in ((pre, "buckets_pre"), (post, "buckets_post")):
        if not isinstance(half, list) or not half \
                or not all(isinstance(b, str) and b for b in half):
            raise SnapshotInvalid(f"{label}: {name!r} is present but not a "
                                  f"non-empty list of names")
    if tuple(list(pre) + list(post)) != tuple(vocab):
        raise SnapshotInvalid(
            f"{label}: buckets_pre + buckets_post does not equal buckets -- "
            f"the snapshot contradicts itself about its own vocabulary")
    return tuple(pre), tuple(post)


def _read_snapshot(path, role="baseline"):
    """Load + fully validate a snapshot file. Returns (mappings, vocabulary).

    `role` names the side in every message. A two-file comparison validates two
    snapshots with identical rules, and calling the after-file a "baseline"
    would misreport which input a reader has to go and fix.

    Factored out of `compare` so BOTH sides of a two-file comparison get the
    identical treatment. When only the baseline was validated, the other input
    was fail-open by omission -- and a two-file mode with one validated side
    would reintroduce that asymmetry on the side the gate newly trusts.
    """
    try:
        obj = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError, RecursionError) as exc:
        # ValueError, not just JSONDecodeError: invalid UTF-8 raises
        # UnicodeDecodeError, which is a ValueError and NOT an OSError, so the
        # narrower tuple let it escape as an uncaught traceback and a bare
        # process exit 1 -- colliding with exit 1's DOCUMENTED meaning here (a
        # prior verdict changed), which a caller reading only the exit code
        # would misread as a real regression instead of "the gate could not
        # run". Deeply-nested JSON raises RecursionError, which is not a
        # ValueError at all (Codex adversarial, section 14).
        raise SnapshotInvalid(f"{role} unreadable: {path}: {exc}") from exc
    if not isinstance(obj, dict) or not obj:
        raise SnapshotInvalid(f"{role} is not a non-empty object: {path}")
    if obj.get("schema") != SNAPSHOT_SCHEMA:
        raise SnapshotInvalid(
            f"{role} schema is {obj.get('schema')!r}, expected "
            f"{SNAPSHOT_SCHEMA} -- regenerate it with `write`: {path}")
    mappings = obj.get("mappings")
    if not isinstance(mappings, dict) or not mappings:
        raise SnapshotInvalid(f"{role} has no mappings object: {path}")
    # TRUNCATION: the recorded population must match what the file actually
    # holds. Without this a one-entry baseline passed cleanly.
    if obj.get("refs") != len(mappings):
        raise SnapshotInvalid(
            f"{role} is TRUNCATED: declares {obj.get('refs')} refs, holds "
            f"{len(mappings)}: {path}")
    vocab = _vocabulary_of(obj, str(path))
    halves = _halves_of(obj, str(path), vocab)
    # ELEMENT TYPES, not just arity. `[path, 411.0, 442.0]` satisfied a
    # length-3 check and compared EQUAL to the integer tuple under Python, so
    # an arity-only check let a malformed snapshot through.
    #
    # A BUCKET IS VALIDATED AGAINST THE EXACT ENUM -- now the enum THIS
    # snapshot declares, not the reader's. Accepting an unrecognised name would
    # let a typo'd bucket sit in the file comparing unequal against every
    # future run forever: a permanent CHANGED that no correct behaviour clears.
    bad = []
    for k, v in mappings.items():
        if isinstance(v, str):
            if v not in vocab:
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
        raise SnapshotInvalid(
            f"{role} has {len(bad)} malformed mapping(s), e.g. {bad[0]!r}: "
            f"{path}")
    return mappings, vocab, halves


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
    # A CALLER MAY SUPPLY THE HALVES A SNAPSHOT DOES NOT CARRY -- but it hands
    # over the VALUES, which this tool then compares itself. The first cut took
    # a bare `--halves-checked` attestation instead, and that was fail-open by
    # construction: presence of a flag became trusted evidence, so any future
    # caller could silence rc 3 with one word rather than doing the comparison
    # (Codex adversarial, section 18 round 4). A tool that cannot verify a
    # claim must not accept the claim.
    supplied = {"base": None, "head": None}
    rest = []
    _pending = None
    for a in argv:
        if _pending:
            supplied[_pending] = a
            _pending = None
        elif a == "--base-halves-b64":
            _pending = "base"
        elif a == "--head-halves-b64":
            _pending = "head"
        elif a != "--strict":
            rest.append(a)
    if _pending:
        sys.stderr.write(f"[corpus_resolution_snapshot] --{_pending}-halves-b64 "
                         f"needs a value\n")
        return 2
    argv = rest
    # THREE SHAPES. `compare <before> <after>` compares two ALREADY-WRITTEN
    # snapshots and performs NO walk of its own -- which is what lets the
    # identity gate run its two walks CONCURRENTLY instead of serially
    # (section 18). The one-file form is unchanged: it walks the live
    # tree and compares against the baseline.
    ok_shape = (
        (len(argv) == 2 and argv[0] in ("write", "compare"))
        or (len(argv) == 3 and argv[0] == "compare")
    )
    if not ok_shape:
        sys.stderr.write(
            "usage: corpus_resolution_snapshot.py write <out.json>\n"
            "       corpus_resolution_snapshot.py compare <before.json> "
            "[--strict]\n"
            "       corpus_resolution_snapshot.py compare <before.json> "
            "<after.json> [--strict] [--halves-checked]\n")
        return 2
    if strict and argv[0] != "compare":
        # A no-op flag a caller BELIEVES is protecting them is worse than an
        # absent one: `write --strict` would look gated and gate nothing.
        sys.stderr.write("[corpus_resolution_snapshot] --strict applies to "
                         "`compare` only\n")
        return 2

    two_file = len(argv) == 3
    if two_file:
        # NO WALK AT ALL on this path. Both sides are files, both are validated
        # identically, and neither the cache nor the resolver is consulted --
        # so this mode cannot be influenced by the tree it happens to run in.
        try:
            before, before_vocab, before_halves = _read_snapshot(
                argv[1], "baseline")
            now, now_vocab, now_halves = _read_snapshot(
                argv[2], "after snapshot")
            if before_halves is None and supplied["base"]:
                before_halves = _halves_from_b64(supplied["base"], "baseline",
                                                 before_vocab)
            if now_halves is None and supplied["head"]:
                now_halves = _halves_from_b64(supplied["head"],
                                              "after snapshot", now_vocab)
        except SnapshotInvalid as exc:
            sys.stderr.write(f"[corpus_resolution_snapshot] {exc}\n")
            return 3
        resolved_now = {k: v for k, v in now.items() if isinstance(v, list)}
        if before_vocab != now_vocab:
            # NOT an error -- it is the migration the protocol extraction
            # exists to allow. Say so loudly, because every bucket string
            # differing between the two sides will now surface as CHANGED and
            # a reader must know those lines are the rename, not a defect.
            print(f"note: bucket vocabulary changed {list(before_vocab)} -> "
                  f"{list(now_vocab)}; bucket-valued verdicts differing across "
                  f"the two sides are reported as CHANGED below")
        return _compare(before, now, resolved_now, strict,
                        before_halves, now_halves)

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
                # THE VOCABULARY THIS SNAPSHOT WAS WRITTEN UNDER. Recorded so a
                # later reader validates the bucket strings against the enum
                # that produced them rather than its own -- see _vocabulary_of.
                "buckets": list(_rr.ALL_BUCKETS),
                # THE TWO HALVES, SEPARATELY. The flattened tuple cannot
                # express a cross-boundary move: relocating the first POST
                # bucket to the end of PRE leaves `buckets` byte-identical
                # while CHANGING BEHAVIOUR, because the lint consumer branches
                # on POST membership to decide whether it reports the effective
                # path or the authored one. Both snapshots then compared
                # perfectly clean over a real reclassification (Codex
                # adversarial, section 18 round 2).
                "buckets_pre": list(_rr.PRE_RESOLUTION_BUCKETS),
                "buckets_post": list(_rr.POST_RESOLUTION_BUCKETS),
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
    #
    # The validation body now lives in `_read_snapshot` so the two-file mode
    # applies the IDENTICAL rules to both of its sides.
    try:
        before, before_vocab, before_halves = _read_snapshot(argv[1])
        if before_halves is None and supplied["base"]:
            before_halves = _halves_from_b64(supplied["base"], "baseline",
                                             before_vocab)
        # The live side's halves come from the tree being walked, which is
        # authoritative for it.
        now_halves = (tuple(_rr.PRE_RESOLUTION_BUCKETS),
                      tuple(_rr.POST_RESOLUTION_BUCKETS))
    except SnapshotInvalid as exc:
        sys.stderr.write(f"[corpus_resolution_snapshot] {exc}\n")
        return 3
    if tuple(before_vocab) != tuple(_rr.ALL_BUCKETS):
        print(f"note: baseline bucket vocabulary {list(before_vocab)} differs "
              f"from this tree's {list(_rr.ALL_BUCKETS)}; bucket-valued "
              f"verdicts differing across the two sides are reported as "
              f"CHANGED below")
    return _compare(before, now, resolved_now, strict,
                    before_halves, now_halves)


def _compare(before, now, resolved_now, strict, before_halves=None,
             now_halves=None) -> int:
    """Diff two mapping sets. Shared by the live-walk and two-file modes."""
    if before_halves is None or now_halves is None:
        # ABSENCE IS NOT EQUALITY. A snapshot written before the halves were
        # recorded cannot answer whether a bucket moved across the pre/post
        # boundary -- and a move leaves every bucket STRING identical, so the
        # mapping diff below cannot answer it either. Passing here would report
        # "no prior verdict changed" about a question nothing asked (Codex
        # adversarial, section 18 round 3, reproduced at rc 0).
        sys.stderr.write(
            "[corpus_resolution_snapshot] a snapshot does not record its "
            "pre/post bucket halves, so a cross-boundary relocation cannot be "
            "adjudicated -- and a relocation changes real verdicts while "
            "leaving every mapping identical. Regenerate both snapshots with "
            "`write`, or supply the missing side with "
            "--base-halves-b64/--head-halves-b64 so this tool can compare "
            "them itself.\n")
        return 3
    relocated = []
    if before_halves is not None and now_halves is not None:
        # ONLY A BUCKET PRESENT ON BOTH SIDES CAN HAVE MOVED. Comparing the
        # halves wholesale flagged an ADDED bucket as a relocation, which is
        # wrong on the merits and broke the data-only-addition case: growing
        # the vocabulary changes no existing ref's half, and the mapping diff
        # below already adjudicates whether any verdict actually moved.
        b_half = {b: "pre" for b in before_halves[0]}
        b_half.update({b: "post" for b in before_halves[1]})
        n_half = {b: "pre" for b in now_halves[0]}
        n_half.update({b: "post" for b in now_halves[1]})
        # SAME-NAME MOVES ONLY. Pairing every removed name with every added
        # one marked a legitimate atomic migration (retire an unused post
        # bucket, add an unrelated pre bucket) as a relocation with no way to
        # clear it. It is also unnecessary: this branch is only reachable with
        # the resolver byte-identical, so it still EMITS the retired name and
        # the head snapshot fails its own vocabulary validation at rc 3 --
        # reproduced 2026-08-06 (Codex adversarial, section 18 round 4).
        relocated = sorted(b for b in b_half
                           if b in n_half and b_half[b] != n_half[b])
    if relocated:
        # A CROSS-BOUNDARY MOVE IS A REAL RECLASSIFICATION, and it is invisible
        # in the mappings: every bucket STRING is unchanged, so the diff below
        # would report nothing while the consumer silently switched which path
        # it reports for every ref in the moved bucket. It fails here, and the
        # clearing path is the usual one -- ground-truth the move by hand, then
        # regenerate with `write`.
        print(f"\nFAIL: bucket(s) {relocated} were RELOCATED across the "
              f"pre/post-resolution boundary: {list(before_halves[0])} / "
              f"{list(before_halves[1])} -> {list(now_halves[0])} / "
              f"{list(now_halves[1])}. Every bucket STRING is unchanged, "
              f"so no mapping differs -- but POST membership decides whether a "
              f"repaired ref reports its effective path or its authored one, "
              f"so this moves real verdicts. Ground-truth the move, then "
              f"regenerate the baseline with `write`.")
        return 1
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
