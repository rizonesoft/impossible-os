#!/usr/bin/env python3
"""Prove the LINT CONSUMER routes every symbol occurrence through the shared rule.

WHY (section 18, closing a section 16 blind spot). The identity gate
walks `corpus_resolution_snapshot.py` and nothing else, so it adjudicates
NOTHING about `scripts/lint/check_stub_behind_stamp.py` -- even though the two
share the resolution rule and section 14 unified them precisely so a fallback
added to one could not diverge from the other. Section 16 deliberately removed
the consumer from its closure rather than list it: a listed file that forces a
walk and then compares two identical snapshots advertises coverage that does not
exist, which is worse than an acknowledged gap.

This is the weaker-but-real half of the choice section 18 offered: prove the
consumer DELEGATES, rather than differential its verdicts. The invariant it
proves is the one section 14 actually promised.

TWO CHECKS, AND THE ORDER OF THEIR IMPORTANCE IS NOT THE ORDER THEY RUN IN.

  SEMANTIC (the load-bearing one). Monkeypatch `ref_resolution.resolve_ref` to
  return distinctive sentinels, drive the consumer's real walk over a real
  corpus, and assert (a) it was called exactly once per symbol occurrence and
  (b) every sentinel verdict propagated verbatim into the consumer's output. A
  consumer that re-implements resolution stops calling the patched function and
  fails (a); one that post-processes a verdict fails (b).

  STRUCTURAL (defence in depth, and ONLY that). One AST assertion: the shared
  rule has exactly one call site. On its own it is trivially bypassable -- a
  refactor can leave one dead `resolve_ref` call to satisfy the count while
  routing verdicts through a differently-named local helper, and the file's
  existing `_resolve_cached` wrapper means a blanket ban on resolve-shaped
  names would already need an exception (Codex design review, section 18). It
  is kept because it is cheap and reads as documentation, NOT because it is
  sufficient. A second structural rule was tried and removed; see the note at
  `structural_violations`.

Exit codes mirror the sibling gates:
    0  the consumer delegates
    1  a VIOLATION: the consumer bypasses or post-processes the shared rule
    2  usage error
    3  INFRASTRUCTURE: the check could not run. Never read as a pass.
"""
from __future__ import annotations

import ast
import contextlib
import io
import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
# A path no corpus ref could produce, so its presence in a coverage entry can
# only mean the consumer propagated what the shared rule returned.
_SENTINEL_REL = "__delegation_sentinel__/effective/path.c"
CONSUMER = REPO_ROOT / "scripts/lint/check_stub_behind_stamp.py"
TODO_GRAPH = REPO_ROOT / "scripts/todo-graph"


class DelegationError(RuntimeError):
    """Infrastructure: the check itself could not run."""


# --------------------------------------------------------------------------
# STRUCTURAL
# --------------------------------------------------------------------------
def structural_violations(path: Path, buckets) -> list:
    try:
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    except (OSError, SyntaxError, ValueError) as exc:
        raise DelegationError(f"cannot parse {path}: {exc}") from exc

    # A BUCKET-LITERAL BAN WAS IMPLEMENTED HERE AND REMOVED, deliberately.
    # The rule was "a bucket name appearing as a string literal means the
    # consumer is retyping vocabulary instead of reading the published tuple".
    # Measured against the real consumer on 2026-08-06 it produced 28 hits, and
    # every one inspected was CORRECT code: `result.bucket == "unpaired_ref"`
    # is a comparison against a published name, and the reporting block near
    # the end of the file names buckets in human-facing text. A lint that fails
    # correct code is worse than no lint -- it trains its own suppression, and
    # satisfying it would mean an unrelated 28-site refactor whose only effect
    # is to please the checker. Same call, and the same reasoning, as the
    # `parked-ownerless` detector this repo built and removed for misreading
    # 329 ordinary items. The semantic check below is the load-bearing one; it
    # does not need this to be sound.
    out = []
    resolve_calls = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            fn = node.func
            name = (fn.attr if isinstance(fn, ast.Attribute)
                    else fn.id if isinstance(fn, ast.Name) else None)
            if name == "resolve_ref":
                resolve_calls.append(node.lineno)

    if len(resolve_calls) != 1:
        out.append(
            f"{path.name}: expected exactly ONE call to resolve_ref, found "
            f"{len(resolve_calls)} (lines {resolve_calls or 'none'}). More than "
            f"one entry point is how the two gates diverged before section 14; "
            f"none at all means the shared rule is no longer being called.")
    return out


# --------------------------------------------------------------------------
# SEMANTIC -- the one that actually holds
# --------------------------------------------------------------------------
def semantic_violations() -> list:
    """Drive the real consumer with a sentinel-returning resolve_ref."""
    sys.path.insert(0, str(TODO_GRAPH))
    sys.path.insert(0, str(CONSUMER.parent))
    try:
        import ref_resolution as _rr
        import snapshot_protocol as _protocol
        import check_stub_behind_stamp as _consumer
    except Exception as exc:  # noqa: BLE001 -- any import failure is infra
        raise DelegationError(f"cannot import the consumer or the shared "
                              f"resolver: {exc}") from exc

    real = _rr.resolve_ref   # restored in each pass's finally
    real_stub = getattr(_consumer, "_is_stub_cached", None)
    if real_stub is None:
        raise DelegationError("the consumer no longer exposes `_is_stub_cached`; "
                              "the resolved-coordinate assertion cannot run")
    out = []

    # ONE ISOLATED PASS PER VERDICT CLASS, not one rotating pass.
    #
    # The rotating version checked each class's CARDINALITY, and under a
    # round-robin every class receives an equal share -- so a consumer that
    # symmetrically SWAPPED two classes' coverage passed with zero violations
    # (Codex adversarial, section 18 round 2, reproduced against the live
    # 1,626-ref cache). Cardinality is simply the wrong invariant: it cannot
    # distinguish "these verdicts propagated" from "these verdicts were
    # permuted". Forcing EVERY occurrence to ONE class makes the expected
    # coverage exact -- that class holds all of them and every other class is
    # empty -- so a swap has nowhere to hide.
    for cls in list(_protocol.ALL_BUCKETS) + [None]:
        calls = {"n": 0}
        coords = []

        def sentinel_resolve_ref(ref, scope, root, _cls=cls):
            calls["n"] += 1
            # CONSTRUCT the verdict; do NOT call the real resolver first. Its
            # answer was discarded by the `_replace` below, but computing it
            # repeated the whole resolution, source indexing and section
            # pairing for every ref on every one of the seven passes: measured
            # 6.02s here versus 0.18s constructing directly, on a lint check
            # that runs on every clean-tree invocation (Codex perf, section 18
            # review). The seven isolated passes are unchanged -- only the
            # discarded work is gone.
            result = _rr.RefResult(rel_path=None, provenance="delegation-probe",
                                   bucket=None, def_abs=None, def_rel=None,
                                   line_start=None, line_end=None)
            if _cls is None:
                # A RESOLVED verdict with DISTINCTIVE coordinates. The consumer
                # passes these straight to `_is_stub_cached`, so capturing them
                # there is what proves it propagated the location rather than
                # re-deriving or ignoring it -- a count of resolved verdicts
                # cannot see a rewritten def_abs or line range.
                return result._replace(
                    bucket=None, def_abs=str(CONSUMER),
                    def_rel="scripts/lint/check_stub_behind_stamp.py",
                    line_start=41, line_end=43)
            # A DISTINCTIVE rel_path for the POST-resolution classes. The
            # consumer publishes `result.rel_path` as the coverage sample for
            # those buckets specifically -- reporting the path actually opened
            # rather than the authored token, which for a repaired ref names
            # nothing. Checking only the list LENGTH left that second output
            # path unverified: a mutation back to the authored `file_rel`
            # preserved every count and passed all seven walks while sending a
            # reader to a file that may not exist (Codex adversarial, section
            # 18 round 4).
            return result._replace(bucket=_cls, def_abs=None, def_rel=None,
                                   line_start=None, line_end=None,
                                   rel_path=_SENTINEL_REL)

        def capture_stub(def_abs, line_start, line_end, _c=coords):
            _c.append((def_abs, line_start, line_end))
            # STUB-POSITIVE, not None. Returning None kept the walk on the
            # "not a stub" path, so the reporting branch that actually
            # PUBLISHES a finding -- and carries `result.def_rel` into it --
            # was never executed by any of the seven passes (Codex adversarial,
            # section 18 round 3). A consumer could rewrite or suppress the
            # reported location after inspection and stay green.
            return ("STATUS_NOT_IMPLEMENTED", 42)

        try:
            _rr.resolve_ref = sentinel_resolve_ref
            if cls is None:
                _consumer._is_stub_cached = capture_stub
            # Capture the consumer's published findings; they are its real
            # output channel, and they must not leak into this check's own.
            _buf = io.StringIO()
            with contextlib.redirect_stdout(_buf):
                occurrences, coverage, findings = _drive_consumer(
                    _rr, _consumer)
            published = _buf.getvalue()
            if occurrences is None:
                raise DelegationError(
                    "could not count symbol occurrences from the cache -- run "
                    "`bash scripts/todo-graph/build-and-validate.sh "
                    "--keep-cache` first")
        finally:
            _rr.resolve_ref = real
            _consumer._is_stub_cached = real_stub

        label = "RESOLVED" if cls is None else repr(cls)

        # (a) DELEGATION: one shared call per occurrence, no more, no fewer.
        if calls["n"] != occurrences:
            out.append(
                f"[{label}] the consumer called the shared resolve_ref "
                f"{calls['n']} time(s) for {occurrences} symbol occurrence(s). "
                f"Every occurrence must reach the shared rule exactly once; a "
                f"shortfall means the consumer resolved some of them itself.")

        # (b) PROPAGATION: with every verdict forced to ONE class, the expected
        # coverage is exact -- that class holds all of them, all others empty.
        if cls is None:
            if (coverage.get("resolved") or 0) != occurrences:
                out.append(
                    f"[{label}] handed the consumer {occurrences} resolved "
                    f"verdict(s) but it reported "
                    f"{coverage.get('resolved') or 0}.")
            # (b2a) THE PUBLISHED FINDINGS. Every occurrence was forced to a
            # stub-positive resolved verdict, so the consumer must report one
            # finding per occurrence, each naming the def_rel the shared rule
            # returned -- not the authored path, and not a re-derived one.
            # The consumer PRINTS each finding, so the published text is the
            # output under test. Every line must name the def_rel the shared
            # rule returned and the body-open line it was handed back -- not
            # the authored path, and not a re-derived location.
            want = "scripts/lint/check_stub_behind_stamp.py:42:"
            lines = [ln for ln in published.splitlines() if ln.strip()]
            bad_findings = [ln for ln in lines if not ln.startswith(want)]
            if findings != occurrences or bad_findings:
                out.append(
                    f"[{label}] the consumer published {findings} finding(s) "
                    f"for {occurrences} stub-positive resolved verdict(s)"
                    + (f", and {len(bad_findings)} did not report the location "
                       f"the shared rule returned (e.g. {bad_findings[0]!r})"
                       if bad_findings else "")
                    + ". A resolved verdict's reported location must come from "
                      "the shared rule, not from the consumer.")

            # (b2) THE COORDINATES, not merely the count.
            bad = [c for c in coords
                   if c != (str(CONSUMER), 41, 43)]
            if len(coords) != occurrences or bad:
                out.append(
                    f"[{label}] the consumer inspected {len(coords)} location(s) "
                    f"for {occurrences} resolved verdict(s)"
                    + (f", and {len(bad)} did not match the coordinates the "
                       f"shared rule returned (e.g. {bad[0]!r})" if bad else "")
                    + ". A resolved verdict's def path and line range must "
                      "propagate verbatim into the body inspection.")
        else:
            entries = coverage.get(cls) or []
            got = len(entries)
            if cls in _protocol.POST_RESOLUTION_BUCKETS:
                bad_paths = [e for e in entries
                             if not str(e).startswith(_SENTINEL_REL + ":")]
                if bad_paths:
                    out.append(
                        f"[{label}] {len(bad_paths)} coverage entry/entries do "
                        f"not carry the rel_path the shared rule returned "
                        f"(e.g. {bad_paths[0]!r}). A post-resolution bucket "
                        f"must report the path actually opened, and that path "
                        f"must come from the shared rule.")
            if got != occurrences:
                out.append(
                    f"[{label}] handed the consumer {occurrences} verdict(s) of "
                    f"this class but only {got} landed in it. A per-class "
                    f"shortfall is the bucket-keyed fallback this check exists "
                    f"to catch.")
            if coverage.get("resolved"):
                out.append(
                    f"[{label}] the consumer reported "
                    f"{coverage['resolved']} RESOLVED occurrence(s) while every "
                    f"shared verdict was an unresolved bucket -- it reached a "
                    f"resolution the shared rule did not give it.")

        # (c) NOTHING INVENTED: every OTHER class must be empty this pass. This
        # is what makes a symmetric swap impossible rather than merely unlikely.
        for other in _protocol.ALL_BUCKETS:
            if other == cls:
                continue
            if coverage.get(other):
                out.append(
                    f"[{label}] the consumer reported "
                    f"{len(coverage[other])} {other!r} verdict(s) this pass, "
                    f"but the shared rule returned only {label}. Those "
                    f"verdicts were reached by the consumer itself.")
    return out





def _drive_consumer(_rr, _consumer):
    """Run the consumer's walk. Returns (expected_occurrences, coverage, findings)."""
    cache = os.environ.get("STUB_LINT_CACHE") or str(
        REPO_ROOT / "build/todo-cache.json")
    if not Path(cache).is_file():
        return None, {}, []
    walk = getattr(_consumer, "_walk", None)
    if walk is None:
        raise DelegationError("the consumer no longer exposes `_walk`; this "
                              "check's drive point is gone")
    # ROUTED THROUGH THE SHARED CACHE-SCHEMA VALIDATOR. This used a raw
    # `json.loads` guarded only for OSError/ValueError, so it got none of the
    # protections section 17 built: no size ceiling before allocation, no
    # generation binding (this check reads the SAME file `build.py` rewrites),
    # and no shape validation. It drives the consumer's `stamped_items` walk, so
    # it takes the DEFAULT profile -- the same contract the consumer itself uses,
    # which is the point: two views of one cache must not disagree about whether
    # it is usable.
    sys.path.insert(0, str(TODO_GRAPH))
    try:
        import cache_schema as _cs
    except ImportError as exc:
        raise DelegationError(
            f"cannot import the shared cache-schema validator: {exc}") from exc
    # FRESHNESS IS DELIBERATELY OFF HERE, and it is the one part of the shared
    # rule this caller does not want.
    #
    # This check asks ONE question: does the consumer route every symbol
    # occurrence through the shared resolver? Whether the cache is current is
    # the CONSUMER's gate (`check_stub_behind_stamp` maps STALE to its own
    # documented code) and is covered by its own fixtures. Taking the check here
    # too would not add coverage -- it would add a second, redundant way for
    # this prober to fail.
    #
    # It would also be unsatisfiable as invoked: `test_build.sh:6463-6473` copies
    # this script into a scratch tree and points STUB_LINT_CACHE at the REAL
    # cache, so `REPO_ROOT` resolves to a directory with no `todo/` at all. With
    # freshness on, all eight delegation fixtures refused at rc 3 against a
    # perfectly good cache. The remaining protections -- size ceiling before
    # allocation, single-descriptor read, MemoryError normalisation and full
    # shape validation -- all still apply, which is what routing was for.
    repo_arg = os.environ.get("STUB_LINT_REPO_ROOT")
    repo_root = Path(repo_arg).resolve() if repo_arg else REPO_ROOT
    try:
        nodes, _info = _cs.load_and_validate(
            Path(cache), repo_root / "todo", check_stale=False)
    except _cs.CacheSchemaError as exc:
        # Kept as a DelegationError so this check's own documented exit code is
        # preserved -- sharing the RULE must not collapse the CODES.
        raise DelegationError(
            f"cannot read the cache {cache} [{exc.reason}]: {exc}") from exc
    # Counted from the CACHE, independently of anything the consumer reports.
    # Deriving the expectation from the consumer's own output would make the
    # comparison circular: a consumer that skipped occurrences would lower both
    # sides of it and pass.
    #
    # The `or []` fallbacks are gone: the validator guarantees `stamped_items`
    # (when present) and `refs` are lists, so a wrong-typed one now REFUSES
    # rather than silently counting zero -- which would have lowered the
    # expectation and passed the comparison it exists to make.
    expected = sum(
        1
        for node in nodes
        for it in node.get("stamped_items", ())
        for ref in it["refs"]
        if ref["kind"] == "symbol")
    with _rr.walk_scope():
        findings, coverage = walk(nodes, REPO_ROOT)
    return expected, coverage, findings


def main(argv) -> int:
    if argv:
        sys.stderr.write("usage: check_consumer_delegation.py\n")
        return 2
    if not CONSUMER.is_file():
        sys.stderr.write(f"[consumer-delegation] the consumer is missing: "
                         f"{CONSUMER}\n")
        return 3
    try:
        sys.path.insert(0, str(TODO_GRAPH))
        import snapshot_protocol as _protocol
        violations = structural_violations(CONSUMER, set(_protocol.ALL_BUCKETS))  # noqa: E501
        violations += semantic_violations()
    except DelegationError as exc:
        sys.stderr.write(f"[consumer-delegation] {exc}\n")
        return 3
    except Exception as exc:  # noqa: BLE001
        sys.stderr.write(f"[consumer-delegation] the check could not run: "
                         f"{exc}\n")
        return 3

    if violations:
        for v in violations:
            print(f"  VIOLATION {v}")
        print(f"\nFAIL: {len(violations)} delegation violation(s). The lint "
              f"consumer and the resolution snapshot must reach every symbol "
              f"verdict through one shared rule; the identity gate proves "
              f"nothing about a consumer that resolves on its own.")
        return 1
    print("OK: the lint consumer routes every symbol occurrence through the "
          "shared resolve_ref exactly once, and every verdict it returns "
          "propagates verbatim.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
