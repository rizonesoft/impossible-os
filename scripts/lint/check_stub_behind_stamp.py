#!/usr/bin/env python3
# ============================================================================
# check_stub_behind_stamp.py -- lint Check 7 implementation.
#
# Walks build/todo-cache.json (TODO graph cache, per-item extension)
# and finds AI-slop stub-behind-stamp findings: a `[x]` checklist item
# names a function whose body is a <=3-line `return CONSTANT;` placeholder
# without an /* INTENTIONAL-STUB: <reason> */ marker on the body opener.
#
# Single Python entry point for lint Check 7: handles the cache existence
# probe, schema (`stamped_items` presence) check, corpus-binding freshness check,
# AND the actual stub walk in one process. Single JSON parse; per-run
# file-content + symbol-resolution caches. Replaces 3 separate Python
# spawns + bash heredoc that the earlier shape used.
#
# Output: one finding per line in the format
#   <file>:<body_open_line>:stub-behind-stamp:<symbol> stamped in <todo>:<sec>:<item>
#
# Distinct exit codes (consumed by scripts/lint.sh Check 7):
#   0  walk completed (with or without findings -- caller counts findings)
#   2  cache missing (lint.sh treats as WARN -- contributor hasn't built)
#   3  cache lacks the `stamped_items` field on EVERY node (lint.sh treats as
#      WARN -- a legacy cache predating the section 9 extension). Narrowed in
#      section 17: a cache that HAS the field but carries an empty population
#      is a producer regression, not an old cache, and is deliberately left to
#      the rc 7 population gate below rather than downgraded to this warning.
#   4  cache stale relative to todo/**/*.md (lint.sh treats as ERROR)
#   5  cache unreadable / malformed JSON, or a shape that violates
#      scripts/todo-graph/schema/cache.schema.json, or an empty node array
#      (lint.sh treats as ERROR). Decided by the shared `cache_schema` module.
#      rc 5 also covers a cache written against a producer contract this
#      reader does not speak (LEGACY_FORMAT): unreadable in the sense that
#      matters, since its fields may not mean what this code assumes.
#   6  internal helper failure (lint.sh treats as ERROR)
#   7  the coverage gate refused, for EITHER of its two contracts (lint.sh
#      treats as ERROR and routes on the emitted tag): `COVERAGE REGRESSION`
#      -- resolved fell below the recorded floor; or `POPULATION GREW/SHRANK`
#      -- the kind=symbol denominator moved, which changes what the floor's
#      ratio means and must be re-recorded deliberately.
#   8  stub-lint-baseline.json missing/malformed (lint.sh treats as ERROR --
#      the baseline is TRACKED, so its absence disables the gate silently;
#      STUB_LINT_ALLOW_NO_BASELINE=1 is the deliberate, reported bypass)
#   9  resolver refused an input (file past the per-file byte ceiling, or
#      rewritten mid-run so returned coordinates no longer describe it).
#      lint.sh treats as ERROR: it is an infrastructure failure, and the one
#      thing it must never be silently downgraded to is "unresolved".
# ============================================================================

import json
import os
import re
import sys
from functools import lru_cache
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "todo-graph"))

try:
    import cache_schema as _cs
    import resolve_symbol as _rs
    import ref_resolution as _rr
except ImportError as exc:
    sys.stderr.write(f"[check_stub_behind_stamp] FATAL: cannot import resolve_symbol: {exc}\n")
    sys.exit(6)


# The SHARED cache-schema rule (scripts/todo-graph/cache_schema.py) raises with
# a `reason` tag rather than an exit code, precisely so sharing the RULE does
# not collapse the CODES: this tool's 2/3/4/5 split is consumed by lint.sh's
# case statement and by section 16's gate wiring, while the snapshot maps every
# reason to its single rc 3. Each reason is mapped EXPLICITLY -- a `.get`
# default would silently route a newly-added reason to whatever code happened
# to be the fallback.
_CACHE_REASON_RC = {
    _cs.REASON_MISSING: 2,                  # WARN: contributor hasn't built it
    _cs.REASON_LEGACY_NO_STAMPED_ITEMS: 3,  # WARN: cache predates the extension
    _cs.REASON_STALE: 4,                    # ERROR: false-clean risk
    _cs.REASON_UNREADABLE: 5,               # ERROR: corrupt JSON
    _cs.REASON_SHAPE: 5,                    # ERROR: violates cache.schema.json
    _cs.REASON_EMPTY: 5,                    # ERROR: exists but nothing to walk
    _cs.REASON_LEGACY_FORMAT: 5,            # ERROR: another producer contract
}
# The "map every reason EXPLICITLY" rule above is right, and the line below
# exists because its failure mode bit: the cache-format identity work added
# `REASON_LEGACY_FORMAT` and did not extend this table, so an old-contract
# binding printed its useful refusal and THEN died on
# `KeyError('LEGACY_FORMAT')` -- an undocumented traceback out of the very
# handler that exists to keep infrastructure failures out of the verdict
# channel (Codex adversarial, round 2, [medium]). LEGACY_FORMAT
# joins UNREADABLE/SHAPE at 5 rather than STALE at 4: the artifact is not out
# of date relative to the corpus, it is one this reader cannot interpret.
#
# An import-time completeness assert, NOT a `.get` fallback -- the distinction
# is the whole point. A fallback silently routes the next new reason to some
# code; this fails the moment the module loads, naming the reason and the
# table, so the omission is impossible to ship.
_UNMAPPED = sorted(
    name for name in dir(_cs)
    if name.startswith("REASON_") and getattr(_cs, name) not in _CACHE_REASON_RC)
if _UNMAPPED:
    raise AssertionError(
        f"cache_schema declares reason(s) {_UNMAPPED} with no exit code in "
        f"_CACHE_REASON_RC; add them here rather than letting the lookup raise "
        f"KeyError at the moment a bad cache is being reported")


# Per-run dedupe of resolve / stub work. The actual file-line cache lives
# in resolve_symbol.py at module scope (`_load_file_lines`) and is shared
# by both resolve_symbol() and is_stub_body(); these wrappers just memoize
# the parsed-result so the same (file, symbol) tuple does not re-walk the
# regex/brace logic when multiple stamped items name it.
def _resolve_cached(file_path: str, symbol: str):
    # Delegates to ref_resolution's memo rather than holding a second one:
    # the section-scope pairing there already asked this exact question for
    # the file that won, and two independent caches would pay for the same
    # resolve twice (measured: 1,159 resolve_symbol calls for 606 walk-side
    # lookups).
    return _rr._resolve_memoized(file_path, symbol)


# `_resolve_cached` was an `lru_cache` and callers reach for its `.cache_clear`
# (scripts/overnight/tests/test_stub_lint_coverage.py resets all four caches
# between fixture trees). Delegating silently dropped that attribute and broke
# them with an AttributeError, so the contract is preserved explicitly and
# routed at the memo that now holds the entries (Codex adversarial, round 2).
_resolve_cached.cache_clear = _rr.clear_caches


@lru_cache(maxsize=2048)
def _is_stub_cached(file_path: str, line_start: int, line_end: int):
    return _rs.is_stub_body(file_path, line_start, line_end)


# `_check_staleness` lived here until section 17. It was a fail-OPEN copy of
# the snapshot's check -- it swallowed `os.walk` traversal errors (no `onerror`
# callback), returned "not stale" when the cache `stat` failed, and treated a
# corpus with zero TODO files as fresh -- and its sole caller additionally
# skipped it whenever the todo root was absent. The fail-closed rule now lives
# in `cache_schema.check_freshness`, shared with the corpus snapshot.


# `_STRIP_RE`, `_raw_text`, `_code_text` and `_has_calllike_token` MOVED to
# scripts/todo-graph/ref_resolution.py (section 14). They were the second half
# of the resolution rule, and leaving them here kept this walk orchestrating
# direct-resolve -> follow_declaration -> bucket independently of the identity
# snapshot that is supposed to gate it -- the same shape that once let a
# fallback ship covering 36 of 54 mappings. Both callers now consume
# `ref_resolution.resolve_ref` and neither touches the resolver directly.
# The move also fixed a real defect in them: keyed by `lru_cache(file_abs)`,
# a cache HIT never re-entered `_load_file_lines`, so a file rewritten mid-walk
# answered from pre-mutation text instead of raising ResolverInputError. The
# moved versions are content-bound (Codex design review, section 14).



def _walk(nodes: list, repo_root: Path) -> tuple:
    """Returns (findings, coverage). `coverage` buckets EVERY paired symbol
    occurrence into mutually exclusive classes that sum back to `occurrences`.

    COVERAGE IS REPORTED, NOT DISCARDED. An unresolved symbol used to `continue`
    silently, so a ref the check never examined was indistinguishable from one it
    examined and cleared -- the lint printed the same nothing for both. Measured
    2026-08-05: only 52 of 186 stamped symbol refs resolve, so the check was
    ~72% blind and reporting clean.

    EVERY skip is now a BUCKET, not a bare `continue`. MEASURED 2026-08-06: of
    503 paired symbol refs only 193 ever reached resolution -- 262 were dropped
    because the named file does not exist, 47 for a non-C suffix, 1 for escaping
    the repo. Those skips ran BEFORE any counting, so retargeting an unresolved
    ref at a nonexistent path made the published ratio look BETTER. A gate whose
    denominator shrinks as the corpus rots is fail-open by construction.

    BUCKET NAMES DESCRIBE EVIDENCE, NOT CAUSE. `no_calllike_token` and
    `unresolved_calllike` say what was observed in the named file; they
    deliberately do not claim "stale bookkeeping" vs "resolver gap". A call
    site, a prototype and a macro invocation all carry a call-like token while
    the real definition lives in another file -- e.g. src/kernel/test/
    test_alpc.c only CALLS kmalloc_fail_next, defined at src/kernel/mm/
    heap.c:499 -- so a causal label would be wrong for a large share of both.
    Assigning cause needs cross-file definition evidence, which is resolver
    work owned by the todo-metadata-layer roadmap's resolver-coverage section.
    """
    repo_resolved = repo_root.resolve()
    findings = 0
    # KEYED FROM THE PUBLISHED VOCABULARY, not a private copy. Hardcoding the
    # six names here made this file a SECOND definition of the protocol: a
    # data-only bucket ADDITION is a supported migration (the identity gate
    # accepts it, fixture 22j), and the very next `cov[result.bucket]` would
    # then raise KeyError -- so the migration the gate advertises could not
    # pass lint (Codex consistency, section 18 review).
    cov = {"occurrences": 0, "resolved": 0}
    cov.update({b: [] for b in _rr.ALL_BUCKETS})
    # PER-OWNER CONTRIBUTIONS, so the exclusion set can reach the rc-7 gate
    # (section 43). The gate compares the live population against a stored
    # GLOBAL total, so dropping an excluded owner's refs from the walk would
    # guarantee a `POPULATION SHRANK` refusal against a baseline that still
    # counts them -- the wedge with extra steps. Counting per owner instead lets
    # `main` subtract the same owner from BOTH sides, which is the only shape
    # under which excluding a file changes nothing about what the gate means.
    cov["by_owner"] = {}
    for node in nodes:
        owner = node.get("file_path")
        own = cov["by_owner"].setdefault(owner, {"occurrences": 0,
                                                 "resolved": 0})
        items = node.get("stamped_items") or []
        # SECTION-SCOPED candidate files, computed once per section rather than
        # per ref: the section-scope pairing in ref_resolution.classify_ref
        # needs every .c/.h file the section's OWN stamped items name, and a
        # section routinely holds dozens of items.
        by_section = {}
        for it in items:
            by_section.setdefault(it.get("section_n"), []).append(it)
        # LAZY, and the laziness lives INSIDE the scope rather than in a
        # `_needs` set copied into both callers: only a section that actually
        # holds an unpaired symbol builds candidates, and 357 of the corpus's
        # 830 sections do (Codex perf, post-commit). The scope also carries the
        # scope is also where the section's unpaired-symbol set is derived, so
        # both gates agree on the pairing population without either computing it.
        sec_scopes = {
            sec: _rr.section_scope(sec_items, repo_resolved)
            for sec, sec_items in by_section.items()
        }
        for it in items:
            scope = sec_scopes.get(it.get("section_n")) or _rr.EMPTY_SCOPE
            for ref in it.get("refs", []):
                if ref.get("kind") != "symbol":
                    continue
                # POPULATION IS EVERY kind=symbol REF -- count FIRST, classify
                # after. An earlier revision of THIS change incremented
                # `occurrences` only after the file/symbol pairing check, which
                # left the invariant downstream of a drop it therefore could not
                # see: an extractor regression that stopped emitting `file`
                # would move refs out of a counted bucket into nothing, every
                # bucket would still sum, the unchanged resolved floor would
                # still pass, and the published ratio would IMPROVE because its
                # denominator shrank. MEASURED 2026-08-06: 1,625 kind=symbol
                # refs exist and 1,122 carry a symbol with no file -- the blind
                # spot was more than twice the population hiding inside it.
                cov["occurrences"] += 1
                own["occurrences"] += 1
                file_rel = ref.get("file")
                symbol = ref.get("symbol")
                # ONE effective-path rule, shared with the identity snapshot
                # (ref_resolution.classify_ref). It applies the historical
                # order of filters unchanged and adds the two stored-ref
                # repairs -- section-scope pairing for a symbol with no file,
                # and basename repair for a bare filename that does not exist
                # at the repo root. Bucket idents keep the AUTHORED spelling so
                # a ref that stays unresolved reads the same as it always did.
                result = _rr.resolve_ref(ref, scope, repo_resolved)
                if result.bucket is not None:
                    if result.bucket == "unpaired_ref":
                        cov["unpaired_ref"].append(
                            str(symbol or file_rel or "?"))
                    elif result.bucket in _rr.POST_RESOLUTION_BUCKETS:
                        # Bucket the ref at the path actually opened, not the
                        # authored spelling: for a repaired ref the authored
                        # token names nothing (a bare filename, or no file at
                        # all), so reporting it here would name a file nobody
                        # can inspect.
                        cov[result.bucket].append(f"{result.rel_path}:{symbol}")
                    else:
                        cov[result.bucket].append(f"{file_rel}:{symbol}")
                    continue
                cov["resolved"] += 1
                own["resolved"] += 1
                stub = _is_stub_cached(result.def_abs, result.line_start,
                                       result.line_end)
                if stub is None:
                    continue
                ret_const, body_open_line = stub
                item_text = (it.get("item_text") or "").strip()
                if len(item_text) > 100:
                    item_text = item_text[:97] + "..."
                todo_path = node.get("file_path") or "?"
                section_n = it.get("section_n", "?")
                # REPORT AT THE DEFINITION, which is not always the ref's own
                # effective path: a followed declaration resolves in the
                # sibling .c, and the declaring header has nothing at
                # `body_open_line` to inspect. `def_rel` is already that file
                # in repo-relative form, and never the authored spelling -- a
                # basename-repaired or section-paired ref's authored token
                # names no file, so printing it would send a reader nowhere.
                report_rel = result.def_rel
                print(
                    f"{report_rel}:{body_open_line}:stub-behind-stamp:{symbol} "
                    f"returns {ret_const} (stamped [x] in {todo_path} "
                    f"section {section_n}: \"{item_text}\")"
                )
                findings += 1
    return findings, cov


def main() -> int:
    cache_arg = os.environ.get("STUB_LINT_CACHE")
    repo_arg = os.environ.get("STUB_LINT_REPO_ROOT")
    cache_path = Path(cache_arg) if cache_arg else REPO_ROOT / "build" / "todo-cache.json"
    repo_root = Path(repo_arg).resolve() if repo_arg else REPO_ROOT
    todo_root = repo_root / "todo"

    # ONE SHARED RULE, this tool's OWN CODES. Existence, readability, schema
    # shape and staleness are all decided by `cache_schema` so this reader and
    # the corpus snapshot cannot disagree about which caches are valid; the
    # reason -> code mapping above keeps the documented 2/3/4/5 contract intact.
    #
    # This replaced four separate probes that were each WEAKER than the
    # snapshot's (section 17): wrong-typed-but-falsey `stamped_items: {}` and
    # `refs: {}` passed and then silently walked ZERO refs while reporting a
    # clean lint; a non-list/non-dict deeper in the tree crashed into the
    # `except Exception -> rc 6` handler instead of naming itself; a
    # `RecursionError` from `json.loads` escaped as a bare rc 1 traceback; and
    # the staleness check was fail-OPEN three ways (swallowed traversal errors,
    # returned "fresh" on a stat failure, and was skipped entirely when the
    # todo root was absent).
    try:
        nodes, info = _cs.load_and_validate(cache_path, todo_root)
    except _cs.CacheSchemaError as exc:
        sys.stderr.write(f"[check_stub_behind_stamp] {exc}\n")
        return _CACHE_REASON_RC[exc.reason]

    # DELIBERATELY NOT SHORT-CIRCUITED ON AN EMPTY POPULATION (Codex design
    # review, section 17, [high]). The snapshot refuses a zero-item cache
    # outright, because a vacuous baseline is useless to it. This reader must
    # do the OPPOSITE: an emptied stamped population is a producer REGRESSION,
    # and the gate that adjudicates it is this tool's own rc 7 "POPULATION
    # GREW/SHRANK" check further down. Returning a cache-level code here would
    # pre-empt that gate and route a regression through lint.sh as a WARNING --
    # and warnings leave the overall lint exit at 0, so the loss would ship.
    # The walk therefore proceeds and the population gate does its job.
    _ = info

    try:
        # ONE WALK, ONE CACHE LIFETIME. `walk_scope` releases the resolver's
        # topology-derived caches on both entry and exit, so a second walk in
        # the same interpreter cannot answer from the previous walk's tree
        # snapshot (Codex adversarial, section 15: the lifecycle shipped as an
        # API that no production caller entered).
        with _rr.walk_scope():
            findings, cov = _walk(nodes, repo_root)
        # RE-VERIFY THE CORPUS GENERATION NOW THE WALK IS DONE. The freshness
        # check inside the shared loader bounds only its own few milliseconds;
        # the window that matters is the ~1s walk just completed, during which a
        # TODO edit would leave these findings describing a tree that no longer
        # exists. Mapped to the documented STALE code (4) like any other
        # staleness, never to a finding count (Codex adversarial, s17 review).
        _cs.check_corpus_unchanged(todo_root, info.corpus)
    except _cs.CacheSchemaError as exc:
        # BEFORE the generic handler below, which would otherwise swallow the
        # post-walk staleness refusal into rc 6 "internal failure". It is a
        # cache-state reason like any other and maps through the same table, so
        # a corpus that moved under the walk reports the documented rc 4.
        sys.stderr.write(f"[check_stub_behind_stamp] {exc}\n")
        return _CACHE_REASON_RC[exc.reason]
    except _rs.ResolverInputError as exc:
        # SURFACED, never bucketed. The resolver raises this for an input it
        # refuses to answer about -- a file past the per-file byte ceiling, or
        # one rewritten mid-run so that coordinates already returned no longer
        # describe its contents. Letting it fall into the generic handler below
        # would report it as an opaque "internal failure"; letting it become an
        # ordinary unresolved ref would be worse still, since that is the one
        # outcome indistinguishable from a clean pass.
        sys.stderr.write(
            f"[check_stub_behind_stamp] RESOLVER INPUT REFUSED: {exc}\n"
            f"  This is not a stub finding and not a coverage gap: the file "
            f"could not be read in a way that makes line coordinates "
            f"trustworthy. Re-run on a quiescent tree; if a source file really "
            f"is that large, raise _MAX_FILE_BYTES deliberately.\n")
        return 9
    except Exception as exc:  # pragma: no cover -- defensive
        sys.stderr.write(f"[check_stub_behind_stamp] internal failure: {exc}\n")
        return 6

    resolved_n = cov["resolved"]
    # FROM THE PUBLISHED VOCABULARY, exactly as `cov` is keyed at the top of
    # `_walk`. This line used to retype the six names, which made it a SECOND
    # definition of the protocol and re-opened the bug the `cov` comment above
    # describes as fixed: an ADDED bucket that gets emitted is accumulated in
    # `cov` but omitted from `accounted`, so the sum below disagrees with the
    # population and this check exits 6 on a migration the identity gate
    # accepts; a RETIRED name leaves `cov[b]` raising KeyError. So the gate
    # could approve a retirement that leaves its own required lint consumer
    # unusable (Codex design review, section 20).
    buckets = tuple(_rr.ALL_BUCKETS)

    # ONE COUNTING BASE. The old line published `resolved=57/189`, where 57 was
    # counted per OCCURRENCE and the 132 it was added to had been through
    # `sorted(set(...))` -- an occurrence count plus a unique count is not a
    # ratio over any one population. Everything below counts OCCURRENCES; the
    # deduped view appears only as a parenthetical on the human sample list,
    # where collapsing repeats is a readability win rather than a measurement.
    accounted = resolved_n + sum(len(cov[b]) for b in buckets)
    if accounted != cov["occurrences"]:
        # Not defensive paranoia: the bug this whole change repairs was a
        # denominator that silently lost refs. If the buckets ever stop summing
        # to the population, the published number is wrong again and the check
        # must say so rather than print a plausible ratio.
        sys.stderr.write(
            f"[check_stub_behind_stamp] internal failure: buckets sum to "
            f"{accounted} but {cov['occurrences']} occurrences were seen\n")
        return 6

    # COVERAGE BASELINE. The standing gap is a WARN, not an ERROR: it is
    # pre-existing, and an ERROR would block every commit in the repo until it
    # reached zero -- the same reason Check 24 warns rather than blocks.
    #
    # A DROP below the recorded baseline is different, and it is an ERROR. It
    # means a change made the check blinder, which is the one failure this lint
    # cannot report on its own: an unresolved symbol produces no finding, so
    # losing resolution looks exactly like passing. MEASURED 2026-08-05 -- a
    # resolver rewrite took coverage from 52/186 to 1/186 while the full suite
    # stayed green and its own new fixtures passed. This gate is what would have
    # caught it, in one line, at commit time.
    total = cov["occurrences"]
    # STUB_LINT_BASELINE lets a test point the floor at a TEMPORARY file. The
    # mutation fixtures previously rewrote the tracked baseline in place and
    # relied on a `finally` to restore it, so a SIGKILL, an external timeout, a
    # crash mid-write or two concurrent runs could leave the repo's real floor
    # truncated, artificially high (blocking every later commit) or artificially
    # low (silently weakening the gate the tests exist to protect). A regression
    # test must not be able to wedge or weaken production state.
    base_env = os.environ.get("STUB_LINT_BASELINE")
    base_path = (Path(base_env) if base_env
                 else repo_root / "scripts" / "lint" / "stub-lint-baseline.json")

    # FAIL CLOSED on the baseline. This file is TRACKED repo state, so it is
    # never legitimately absent -- a fresh clone has it. The earlier shape
    # swallowed every read/parse error into `baseline = None` and then skipped
    # the floor entirely via the `isinstance(baseline, int)` guard, which meant
    # deleting or corrupting the file disabled the only coverage gate and still
    # exited 0. That is fail-open exactly when health cannot be established,
    # and it is a silent bypass with no record. The bootstrap case the old
    # warn-only behaviour protected cannot occur for a tracked file.
    baseline = None
    baseline_err = None
    # The whole parsed baseline, kept so the exclusion arithmetic below can read
    # its per-owner contributions without a second read of the same file (a
    # second read is a second chance to see a different file).
    _baseline_doc = {}

    def _read_baseline():
        """(value, error). Catches ValueError, not just JSONDecodeError: invalid
        UTF-8 raises UnicodeDecodeError, which is a ValueError and NOT an
        OSError, so the narrower tuple let it escape as an undocumented rc 1
        traceback instead of the rc 8 the contract promises. AttributeError
        covers well-formed JSON of the wrong SHAPE -- `[1]` parses fine and then
        `.get` does not exist on a list."""
        try:
            doc = json.loads(base_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            return None, f"unreadable ({exc})"
        if not isinstance(doc, dict):
            return None, f"is not a JSON object ({type(doc).__name__})"
        _baseline_doc.update(doc)
        return (doc.get("resolved"), doc.get("total")), None

    baseline_total = None
    if os.environ.get("STUB_LINT_ALLOW_NO_BASELINE") == "1":
        # Explicit, visibly reported skip -- the one sanctioned bypass.
        pair, _ = _read_baseline()
        baseline, baseline_total = pair if pair else (None, None)
        sys.stderr.write("[check_stub_behind_stamp] baseline floor SKIPPED via "
                         "STUB_LINT_ALLOW_NO_BASELINE=1\n")
    else:
        pair, baseline_err = _read_baseline()
        baseline, baseline_total = pair if pair else (None, None)
        # bool BEFORE the `< 0` compare: bool is an int subclass, so `True`
        # satisfies isinstance(x, int) and would otherwise pass as a count.
        if baseline_err is None and (not isinstance(baseline, int)
                                     or isinstance(baseline, bool)):
            baseline_err = f"`resolved` is not an integer ({baseline!r})"
        # `<= 0`, for the SAME reason as `total` below and found by the same
        # class of reasoning one round later: a zero RESOLVED floor is not a
        # floor either. With every symbol landing in an unresolved bucket and a
        # baseline of {"resolved": 0, "total": N}, the population comparison
        # matches, `0 < 0` is false, and main returns 0 -- an exit-0 run in
        # which Check 7 examined no function body at all, rendered by lint.sh
        # as a mere warning (Codex adversarial, section 17 review). The
        # snapshot already rejects the equivalent zero-resolved baseline.
        if baseline_err is None and baseline <= 0:
            baseline_err = (f"`resolved` is not a valid floor ({baseline!r})"
                            f" -- a zero floor passes a walk that resolved "
                            f"nothing")
        # THE DENOMINATOR IS PART OF THE FLOOR, not decoration. `total` was
        # recorded but never read, so the ratio was protected only from above:
        # if extraction stopped emitting UNRESOLVED refs, `occurrences` and the
        # buckets shrank together, `resolved` stayed at its floor, the published
        # ratio IMPROVED, and the check exited 0 -- fail-open on precisely the
        # corpus-rot scenario the counted-population work exists to catch
        # (Codex adversarial, post-commit). A denominator that moves is a
        # finding, in EITHER direction: growth is new stamped work and gets
        # recorded, shrinkage is data loss.
        # `<= 0`, NOT `< 0`. A ZERO denominator is not a floor, it is the
        # ABSENCE of one, and it silently disarms the population gate that the
        # section 17 empty-population policy hands off to: with an emptied
        # `stamped_items`, `occurrences` is also 0, the equality below passes,
        # the resolved floor 0 >= 0 passes, main returns 0, and lint.sh renders
        # `resolved=0/0` as full-coverage INFO. So re-recording the baseline
        # while the producer is collapsed would make the collapse permanent and
        # invisible (Codex adversarial, section 17). A corpus with zero stamped
        # symbol refs IS the regression this file exists to catch, so the
        # honest response is rc 8 "baseline invalid" -- and
        # STUB_LINT_ALLOW_NO_BASELINE=1 remains the sanctioned, REPORTED bypass
        # for a tree that genuinely has no floor yet.
        if baseline_err is None and (not isinstance(baseline_total, int)
                                     or isinstance(baseline_total, bool)
                                     or baseline_total <= 0):
            baseline_err = (f"`total` is not a valid count ({baseline_total!r})"
                            f" -- a zero denominator disarms the population "
                            f"gate rather than setting a floor")

    # STDERR, not stdout. stdout is this check's FINDINGS channel -- lint.sh
    # routes every line of it through error() on rc 0 -- so informational
    # coverage output on stdout becomes 6 phantom lint errors. Learned by
    # doing exactly that, 2026-08-05.
    # POST-RESOLUTION IS "the file was opened and the symbol was not found
    # there", which is what `unresolved` has always meant here. Naming the two
    # members made this a third private copy of the vocabulary; deriving it from
    # the published half keeps the meaning and survives a migration.
    post = [b for b in _rr.POST_RESOLUTION_BUCKETS if b in cov]
    uniq = len(set().union(*(set(cov[b]) for b in post)) if post else set())
    unresolved_occ = sum(len(cov[b]) for b in post)
    sys.stderr.write(f"stub-behind-stamp:coverage resolved={resolved_n}/{total} "
                     f"unresolved={unresolved_occ} ({uniq} unique)"
                     + (f" baseline={baseline}" if isinstance(baseline, int) else "")
                     + "\n")
    # The human labels are a PRESENTATION detail, so a bucket without one still
    # reports under its own name rather than vanishing from the line or raising.
    # ORDER IS PART OF THE PUBLISHED LINE. Iterating ALL_BUCKETS reordered it
    # from the established `calllike-unresolved ... unpaired-ref` to the JSON's
    # pre-then-post order -- a silent change to output people read and grep, for
    # no reason (Codex consistency, section 20). Accounting still derives from
    # the vocabulary; only the presentation keeps its historical sequence, with
    # any bucket added later appended in vocabulary order rather than dropped.
    labels = (("unresolved_calllike", "calllike-unresolved"),
              ("no_calllike_token", "no-calllike-token"),
              ("missing_file", "missing-file"),
              ("unsupported_lang", "non-c-suffix"),
              ("path_escape", "path-escape"),
              ("unpaired_ref", "unpaired-ref"))
    ordered = [(b, lbl) for b, lbl in labels if b in cov]
    known = {b for b, _ in labels}
    ordered += [(b, b) for b in buckets if b not in known]
    sys.stderr.write(
        "  buckets: "
        + " ".join(f"{lbl}={len(cov[b])}" for b, lbl in ordered)
        + "\n")

    if baseline_err is not None:
        sys.stderr.write(
            f"[check_stub_behind_stamp] BASELINE INVALID: {base_path.name} "
            f"{baseline_err}. This file is tracked repo state and gates the "
            f"only coverage floor; treating its absence as 'no floor' would "
            f"disable the gate silently. Restore it from git, or set "
            f"STUB_LINT_ALLOW_NO_BASELINE=1 to proceed deliberately.\n")
        return 8

    # Sample the post-resolution halves in vocabulary order, 5 in total, so the
    # shape of the list is unchanged while the bucket names come from the
    # protocol rather than from here.
    samples = []
    for i, b in enumerate(post):
        if len(samples) >= 5:
            break
        samples += sorted(set(cov[b]))[:(3 if i == 0 else 2)]
    samples = samples[:5]
    for u in samples:
        sys.stderr.write(f"  unresolved (NOT checked): {u}\n")
    if uniq > len(samples):
        sys.stderr.write(f"  ... +{uniq - len(samples)} more unresolved (unique)\n")
    for u in sorted(set(cov.get("missing_file") or ()))[:3]:
        sys.stderr.write(f"  missing file (ref points nowhere): {u}\n")

    # THE EXCLUSION SET REACHES THE GATE, SYMMETRICALLY (section 43). Until now
    # `lint.sh` filtered this helper's STDOUT, which excludes FINDINGS and
    # nothing else -- so a TODO the commit does not touch could still move the
    # stamped-symbol population and refuse the commit at rc 7 below, the exact
    # unattended-worktree wedge the set exists to prevent.
    #
    # Subtracting the excluded owners from the LIVE side alone would not fix it,
    # it would invert it: the stored baseline still counts them, so every
    # exclusion would guarantee `POPULATION SHRANK`. Both sides move together or
    # neither does, which is why the baseline carries per-owner contributions.
    live_occ, live_res = cov["occurrences"], resolved_n
    excluded = [p.strip() for p in
                (os.environ.get("STUB_LINT_EXCLUDED") or "").splitlines()
                if p.strip()]
    base_owners = _baseline_doc.get("by_owner")
    live_owners = cov.get("by_owner") or {}

    # THE MAP IS VALIDATED ON EVERY BASELINE-BACKED RUN, not only when an
    # exclusion is present (Codex adversarial, section 43 round 2, [medium]).
    # Nesting the checks under `if excluded` meant ordinary lint and CI -- which
    # pass no exclusion set -- would accept a baseline whose per-owner map was
    # omitted, corrupted or desynchronised so long as the GLOBAL totals still
    # looked right. The corruption then surfaces at the next scoped commit as an
    # rc 7, recreating the unrelated-worktree wedge this change removes, and a
    # sum-preserving misattribution would subtract the wrong owner's share.
    # AND IT IS REQUIRED, not merely checked when present (Codex consistency,
    # section 43 post-ship, [high]). "Accepted when absent" contradicted the
    # tracked baseline's own contract: the key exists there now, so a commit
    # DELETING it would pass ordinary lint and CI and only surface at someone
    # else's scoped commit as an rc 7 -- the same deferred-blame shape the
    # per-owner map was added to remove. The explicit floor bypass
    # (STUB_LINT_ALLOW_NO_BASELINE) remains the one way to proceed without it.
    # A PRESENT MAP IS ALWAYS VALIDATED; ABSENCE IS NOT THIS FUNCTION'S CALL.
    # The finding this addresses is real -- a commit deleting `by_owner` from
    # the TRACKED baseline would pass ordinary lint and surface later as an
    # rc 7 at someone else's scoped commit -- but "reject every baseline that
    # lacks the key" is the wrong place to enforce it: this helper is handed
    # hand-written three-key baselines by fixtures that are measuring the
    # population gate, and a synthetic tree uses the same default path, so the
    # rule cannot be scoped by path either. The contract is a property of the
    # REPO'S OWN FILE, so it is asserted against that file directly, in
    # `scripts/tests/test_todo_fence.py`. Here: if the map exists it must be
    # correct, and an exclusion still requires it (enforced below).
    bad = None
    if isinstance(baseline_total, int) or isinstance(baseline, int):
        if base_owners is None:
            pass
        elif not isinstance(base_owners, dict):
            bad = (f"{base_path.name} `by_owner` is not an object "
                   f"({type(base_owners).__name__})")
        else:
            tot_o = tot_r = 0
            for own, val in base_owners.items():
                if not isinstance(val, dict):
                    bad = f"baseline by_owner[{own!r}] is not an object"
                    break
                o, r = val.get("occurrences"), val.get("resolved")
                if (not isinstance(o, int) or isinstance(o, bool) or o < 0
                        or not isinstance(r, int) or isinstance(r, bool)
                        or r < 0):
                    bad = (f"baseline by_owner[{own!r}] is not a pair of "
                           f"non-negative integers ({o!r}, {r!r})")
                    break
                if r > o:
                    bad = (f"baseline by_owner[{own!r}] resolves more refs "
                           f"than it has ({r} > {o})")
                    break
                tot_o += o
                tot_r += r
            if bad is None and isinstance(baseline_total, int) \
                    and isinstance(baseline, int) \
                    and (tot_o != baseline_total or tot_r != baseline):
                bad = (f"baseline by_owner sums ({tot_o} occurrences, {tot_r} "
                       f"resolved) do not equal the recorded totals "
                       f"({baseline_total}, {baseline}); regenerate them "
                       f"together, never separately")
    if bad:
        sys.stderr.write(
            f"[check_stub_behind_stamp] BASELINE by_owner INVALID: {bad}. The "
            f"per-owner contributions are what let a commit-gate exclusion "
            f"narrow this gate on both sides; an unusable map is refused here "
            f"rather than at the next scoped commit.\n")
        return 8

    if excluded:
        # FAIL CLOSED on what is specific to HAVING an exclusion set. The map
        # itself was validated above, on every run.
        bad = None
        # THE FLOOR FIRST, because it is the root fact. An absent baseline also
        # has no `by_owner` map, and naming the map would send a reader looking
        # for a corrupted file when the real answer is that they asked to scope
        # a gate that is not running (STUB_LINT_ALLOW_NO_BASELINE=1). The
        # arithmetic below would also raise on None.
        if not isinstance(baseline_total, int) or not isinstance(baseline, int):
            bad = ("the baseline floor is not in force (skipped or unreadable), "
                   "so an exclusion set has no gate to narrow")
        elif not isinstance(base_owners, dict):
            bad = (f"{base_path.name} carries no `by_owner` map, so an "
                   f"exclusion cannot be subtracted from the baseline as well "
                   f"as from the live walk")
        elif len(set(excluded)) != len(excluded):
            bad = "the exclusion set contains duplicate entries"
        if bad is None:
            for ident in excluded:
                if (ident.startswith("/") or ".." in Path(ident).parts
                        or not ident.startswith("todo/")):
                    bad = (f"exclusion {ident!r} is not a repo-relative path "
                           f"under todo/")
                    break
                if ident not in base_owners and ident not in live_owners:
                    # A KNOWN NON-OWNER is fine (v14 close-out, 2026-08-16): a
                    # capture file (`todo/overnight-runner-improvements/`,
                    # `todo/token-saver/`) is a legitimate todo file that is
                    # deliberately not a graph node, and the self-improvement
                    # rule REQUIRES filing into it in the same turn a finding
                    # is observed -- so a path-limited commit routinely leaves
                    # it dirty and excluded. Such a path contributes ZERO to
                    # both sides of the arithmetic, so accepting it subtracts
                    # nothing and cannot widen the gate; and a rename in the
                    # unsafe direction stays covered, because the live walk
                    # counts an on-disk owner under its CURRENT name (an
                    # exclusion under that name would then be IN live_owners,
                    # not here). Only a path that does not EXIST is the
                    # renamed/misspelled case the refusal was written for.
                    if (repo_root / ident).is_file():
                        continue
                    bad = (f"exclusion {ident!r} names no TODO in either the "
                           f"baseline or the current cache AND no such file "
                           f"exists -- a renamed or misspelled entry excludes "
                           f"nothing and would widen this gate silently")
                    break
        if bad:
            sys.stderr.write(
                f"[check_stub_behind_stamp] EXCLUSION SET INVALID: {bad}. The "
                f"coverage gate is not evaluated against a set it cannot apply "
                f"to both sides.\n")
            return 7
        for ident in excluded:
            b = base_owners.get(ident) or {}
            l = live_owners.get(ident) or {}
            baseline_total -= int(b.get("occurrences", 0) or 0)
            baseline -= int(b.get("resolved", 0) or 0)
            live_occ -= int(l.get("occurrences", 0) or 0)
            live_res -= int(l.get("resolved", 0) or 0)
        # AND THE ADJUSTED VIEW IS ITSELF VALID. The per-entry checks above
        # cannot see the whole exclusion set at once, and a floor at or below
        # zero passes vacuously -- `live_res < 0` is never true, so the coverage
        # gate would be disabled rather than scoped. Same fail-closed rule the
        # unadjusted baseline already has (a zero floor is rc 8 there).
        if min(live_occ, live_res, baseline_total) < 0 or baseline <= 0:
            sys.stderr.write(
                f"[check_stub_behind_stamp] EXCLUSION SET INVALID: after "
                f"excluding {len(excluded)} owner(s) the gate would compare "
                f"{live_occ}/{live_res} against {baseline_total}/{baseline}, "
                f"which is not a floor this check can enforce. An exclusion "
                f"may narrow the gate; it may not switch it off.\n")
            return 7
        sys.stderr.write(
            f"[check_stub_behind_stamp] coverage gate scope: {len(excluded)} "
            f"todo file(s) excluded from BOTH the live population and the "
            f"baseline; population {live_occ} vs {baseline_total}\n")

    # DENOMINATOR FIRST -- a shrunken population invalidates the ratio the
    # floor below is expressed in, so reporting the floor as clean while the
    # corpus lost refs would be the fail-open this check exists to prevent.
    if isinstance(baseline_total, int) and live_occ != baseline_total:
        direction = ("SHRANK" if live_occ < baseline_total
                     else "GREW")
        sys.stderr.write(
            f"[check_stub_behind_stamp] POPULATION {direction}: "
            f"{live_occ} kind=symbol refs vs baseline total "
            f"{baseline_total}. The resolved floor is a RATIO, so a moving "
            f"denominator changes what passing means: refs lost from the "
            f"cache shrink the buckets and the total together, leaving "
            f"`resolved` at its floor while the published ratio improves. "
            f"If stamped work really was added or removed, update "
            f"{base_path.name}'s `total` DELIBERATELY in the same commit "
            f"and say what changed.\n")
        return 7
    if isinstance(baseline, int) and live_res < baseline:
        sys.stderr.write(
            f"[check_stub_behind_stamp] COVERAGE REGRESSION: resolved "
            f"{live_res} < baseline {baseline}. A change made this lint "
            f"blinder -- an unresolved symbol yields no finding, so losing "
            f"resolution looks identical to passing. Fix the resolver, or "
            f"update {base_path.name} DELIBERATELY with the reason.\n")
        return 7
    if isinstance(baseline, int) and live_res > baseline:
        sys.stderr.write(f"stub-behind-stamp:coverage improved {baseline} -> "
                         f"{live_res}; update stub-lint-baseline.json to lock "
                         f"it in\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
