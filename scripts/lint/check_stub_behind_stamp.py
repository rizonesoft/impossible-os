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
# probe, schema (`stamped_items` presence) check, mtime staleness check,
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
#   3  cache lacks `stamped_items` field (lint.sh treats as WARN -- old cache)
#   4  cache stale relative to todo/**/*.md (lint.sh treats as ERROR)
#   5  cache unreadable / malformed JSON (lint.sh treats as ERROR)
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
    import resolve_symbol as _rs
    import ref_resolution as _rr
except ImportError as exc:
    sys.stderr.write(f"[check_stub_behind_stamp] FATAL: cannot import resolve_symbol: {exc}\n")
    sys.exit(6)


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


def _check_staleness(cache_path: Path, todo_root: Path) -> bool:
    """True if cache is stale relative to any TODO-*.md mtime."""
    try:
        cache_m = cache_path.stat().st_mtime
    except OSError:
        return False
    newest = 0.0
    for dirpath, _, files in os.walk(todo_root):
        for f in files:
            if f.startswith("TODO-") and f.endswith(".md"):
                try:
                    m = os.stat(os.path.join(dirpath, f)).st_mtime
                    if m > newest:
                        newest = m
                except OSError:
                    pass
    return newest > cache_m


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
    cov = {
        "occurrences": 0,
        "resolved": 0,
        "unresolved_calllike": [],
        "no_calllike_token": [],
        "missing_file": [],
        "unsupported_lang": [],
        "path_escape": [],
        "unpaired_ref": [],
    }
    for node in nodes:
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

    if not cache_path.is_file():
        sys.stderr.write(f"[check_stub_behind_stamp] cache missing: {cache_path}\n")
        return 2

    try:
        # ValueError, not just JSONDecodeError: a cache containing invalid UTF-8
        # raises UnicodeDecodeError, which is a ValueError and NOT an OSError,
        # so the narrower tuple let it escape as an undocumented rc 1 traceback
        # rather than the documented rc 5.
        nodes = json.loads(cache_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        sys.stderr.write(f"[check_stub_behind_stamp] cache unreadable: {exc}\n")
        return 5
    if not isinstance(nodes, list):
        sys.stderr.write("[check_stub_behind_stamp] cache is not a JSON array\n")
        return 5
    # Every node must be a mapping before anything probes it. A well-formed but
    # wrong-shaped cache such as `[1]` parses cleanly and then raises deep in
    # the walk, surfacing as rc 1 instead of the rc 5 the contract promises.
    if not all(isinstance(n, dict) for n in nodes):
        sys.stderr.write("[check_stub_behind_stamp] cache has non-object nodes\n")
        return 5

    if not any("stamped_items" in (n or {}) for n in nodes):
        sys.stderr.write("[check_stub_behind_stamp] cache lacks stamped_items field\n")
        return 3

    if todo_root.is_dir() and _check_staleness(cache_path, todo_root):
        sys.stderr.write("[check_stub_behind_stamp] cache stale relative to todo/**/*.md\n")
        return 4

    try:
        # ONE WALK, ONE CACHE LIFETIME. `walk_scope` releases the resolver's
        # topology-derived caches on both entry and exit, so a second walk in
        # the same interpreter cannot answer from the previous walk's tree
        # snapshot (Codex adversarial, section 15: the lifecycle shipped as an
        # API that no production caller entered).
        with _rr.walk_scope():
            findings, cov = _walk(nodes, repo_root)
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
    buckets = ("unresolved_calllike", "no_calllike_token", "missing_file",
               "unsupported_lang", "path_escape", "unpaired_ref")

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
        if baseline_err is None and baseline < 0:
            baseline_err = f"`resolved` is not a valid count ({baseline!r})"
        # THE DENOMINATOR IS PART OF THE FLOOR, not decoration. `total` was
        # recorded but never read, so the ratio was protected only from above:
        # if extraction stopped emitting UNRESOLVED refs, `occurrences` and the
        # buckets shrank together, `resolved` stayed at its floor, the published
        # ratio IMPROVED, and the check exited 0 -- fail-open on precisely the
        # corpus-rot scenario the counted-population work exists to catch
        # (Codex adversarial, post-commit). A denominator that moves is a
        # finding, in EITHER direction: growth is new stamped work and gets
        # recorded, shrinkage is data loss.
        if baseline_err is None and (not isinstance(baseline_total, int)
                                     or isinstance(baseline_total, bool)
                                     or baseline_total < 0):
            baseline_err = f"`total` is not a valid count ({baseline_total!r})"

    # STDERR, not stdout. stdout is this check's FINDINGS channel -- lint.sh
    # routes every line of it through error() on rc 0 -- so informational
    # coverage output on stdout becomes 6 phantom lint errors. Learned by
    # doing exactly that, 2026-08-05.
    uniq = len(set(cov["unresolved_calllike"]) | set(cov["no_calllike_token"]))
    unresolved_occ = len(cov["unresolved_calllike"]) + len(cov["no_calllike_token"])
    sys.stderr.write(f"stub-behind-stamp:coverage resolved={resolved_n}/{total} "
                     f"unresolved={unresolved_occ} ({uniq} unique)"
                     + (f" baseline={baseline}" if isinstance(baseline, int) else "")
                     + "\n")
    sys.stderr.write(
        f"  buckets: calllike-unresolved={len(cov['unresolved_calllike'])} "
        f"no-calllike-token={len(cov['no_calllike_token'])} "
        f"missing-file={len(cov['missing_file'])} "
        f"non-c-suffix={len(cov['unsupported_lang'])} "
        f"path-escape={len(cov['path_escape'])} "
        f"unpaired-ref={len(cov['unpaired_ref'])}\n")

    if baseline_err is not None:
        sys.stderr.write(
            f"[check_stub_behind_stamp] BASELINE INVALID: {base_path.name} "
            f"{baseline_err}. This file is tracked repo state and gates the "
            f"only coverage floor; treating its absence as 'no floor' would "
            f"disable the gate silently. Restore it from git, or set "
            f"STUB_LINT_ALLOW_NO_BASELINE=1 to proceed deliberately.\n")
        return 8

    samples = sorted(set(cov["unresolved_calllike"]))[:3] \
        + sorted(set(cov["no_calllike_token"]))[:2]
    for u in samples:
        sys.stderr.write(f"  unresolved (NOT checked): {u}\n")
    if uniq > len(samples):
        sys.stderr.write(f"  ... +{uniq - len(samples)} more unresolved (unique)\n")
    if cov["missing_file"]:
        for u in sorted(set(cov["missing_file"]))[:3]:
            sys.stderr.write(f"  missing file (ref points nowhere): {u}\n")

    # DENOMINATOR FIRST -- a shrunken population invalidates the ratio the
    # floor below is expressed in, so reporting the floor as clean while the
    # corpus lost refs would be the fail-open this check exists to prevent.
    if isinstance(baseline_total, int) and cov["occurrences"] != baseline_total:
        direction = ("SHRANK" if cov["occurrences"] < baseline_total
                     else "GREW")
        sys.stderr.write(
            f"[check_stub_behind_stamp] POPULATION {direction}: "
            f"{cov['occurrences']} kind=symbol refs vs baseline total "
            f"{baseline_total}. The resolved floor is a RATIO, so a moving "
            f"denominator changes what passing means: refs lost from the "
            f"cache shrink the buckets and the total together, leaving "
            f"`resolved` at its floor while the published ratio improves. "
            f"If stamped work really was added or removed, update "
            f"{base_path.name}'s `total` DELIBERATELY in the same commit "
            f"and say what changed.\n")
        return 7
    if isinstance(baseline, int) and resolved_n < baseline:
        sys.stderr.write(
            f"[check_stub_behind_stamp] COVERAGE REGRESSION: resolved "
            f"{resolved_n} < baseline {baseline}. A change made this lint "
            f"blinder -- an unresolved symbol yields no finding, so losing "
            f"resolution looks identical to passing. Fix the resolver, or "
            f"update {base_path.name} DELIBERATELY with the reason.\n")
        return 7
    if isinstance(baseline, int) and resolved_n > baseline:
        sys.stderr.write(f"stub-behind-stamp:coverage improved {baseline} -> "
                         f"{resolved_n}; update stub-lint-baseline.json to lock "
                         f"it in\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
