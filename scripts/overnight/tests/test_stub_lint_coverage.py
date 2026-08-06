#!/usr/bin/env python3
"""Check 7 must COUNT what it cannot resolve, and must not go blind silently.

MEASURED 2026-08-05: of 189 stamped symbol refs, only 55 resolve. The other 134
were `continue`d -- so a ref the lint never examined was indistinguishable from
one it examined and cleared. The lint printed the same nothing for both, and was
~71% blind while reporting clean.

The regression gate is the load-bearing half. A resolver rewrite took coverage
from 55 to 1 while the full 115/115 suite stayed green and its own new fixtures
passed, because an unresolved symbol yields no finding -- losing resolution looks
EXACTLY like passing. Only a recorded floor can see that.
"""
from __future__ import annotations

import contextlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
CHECK = REPO / "scripts/lint/check_stub_behind_stamp.py"
BASELINE = REPO / "scripts/lint/stub-lint-baseline.json"


def _run(env_extra=None):
    env = dict(os.environ)
    env.update(env_extra or {})
    return subprocess.run([sys.executable, str(CHECK)], cwd=str(REPO),
                          capture_output=True, text=True, env=env, timeout=300)


@contextlib.contextmanager
def _baseline(content):
    """Point the floor at a TEMPORARY baseline carrying `content`.

    These fixtures used to overwrite the TRACKED scripts/lint/
    stub-lint-baseline.json in place and restore it in a `finally`. A `finally`
    does not run on SIGKILL, on an external timeout that kills the runner, or
    on a crash mid-write, and two concurrent runs interleave -- any of which
    leaves the repo's real floor truncated, artificially high (blocking every
    later commit) or artificially low (silently weakening the very gate these
    tests protect). A regression test must not be able to wedge production
    state, so it writes its own file and overrides the path instead.
    """
    with tempfile.TemporaryDirectory() as td:
        p = pathlib.Path(td) / "stub-lint-baseline.json"
        p.write_text(content, encoding="utf-8")
        yield {"STUB_LINT_BASELINE": str(p)}


def test_coverage_is_reported_not_discarded():
    r = _run()
    assert "stub-behind-stamp:coverage" in r.stderr, r.stderr[:400]
    assert "unresolved=" in r.stderr and "resolved=" in r.stderr, r.stderr[:400]


def test_coverage_never_lands_on_STDOUT():
    """stdout is this check's FINDINGS channel: lint.sh routes every line of it
    through error() on rc 0. Putting the informational coverage line there turned
    into 6 phantom lint errors -- done for real on 2026-08-05. stdout must carry
    findings and nothing else."""
    r = _run()
    assert "stub-behind-stamp:coverage" not in r.stdout, r.stdout[:400]
    for line in r.stdout.splitlines():
        if line.strip():
            assert "stub-behind-stamp:" in line, f"non-finding on stdout: {line!r}"


def test_unresolved_refs_are_named_not_just_counted():
    """A bare count cannot be acted on. The first few must be nameable."""
    r = _run()
    if "unresolved=0" not in r.stderr:
        assert "unresolved (NOT checked):" in r.stderr, r.stderr[:600]


def test_the_baseline_is_a_real_floor_and_it_holds_today():
    data = json.loads(BASELINE.read_text(encoding="utf-8"))
    assert isinstance(data.get("resolved"), int), data
    r = _run()
    assert r.returncode == 0, (r.returncode, r.stderr[:400])


def test_a_coverage_DROP_is_an_error():
    """The failure a green test suite cannot see. Raise the recorded floor above
    the live number and the check must refuse (rc 7), naming the regression."""
    data = json.loads(BASELINE.read_text(encoding="utf-8"))
    data["resolved"] = data["resolved"] + 10_000
    with _baseline(json.dumps(data)) as env:
        r = _run(env)
        assert r.returncode == 7, (r.returncode, r.stdout[:300], r.stderr[:300])
        assert "COVERAGE REGRESSION" in r.stderr, r.stderr[:400]
        assert "blinder" in r.stderr, r.stderr[:400]


def test_an_improvement_is_reported_so_the_floor_gets_raised():
    with _baseline('{"resolved": 1}') as env:
        r = _run(env)
        assert r.returncode == 0, r.returncode
        assert "coverage improved" in r.stderr, r.stderr[:400]


def test_a_corrupt_baseline_FAILS_CLOSED():
    """REVERSED 2026-08-06 (was: degrades to warn-not-block).

    The original reasoning was that a lint refusing every commit over its own
    bookkeeping file is worse than the gap it guards. That protects a bootstrap
    case which CANNOT OCCUR: stub-lint-baseline.json is tracked repo state, so
    a fresh clone always has it. What the warn-only path actually bought was a
    silent, unrecorded bypass -- delete the file and the only coverage floor
    stops applying while the check still exits 0.
    """
    with _baseline("{ not json") as env:
        r = _run(env)
        assert r.returncode == 8, (r.returncode, r.stderr[:300])
        assert "BASELINE INVALID" in r.stderr, r.stderr[:300]


def test_a_non_integer_baseline_FAILS_CLOSED():
    """Well-formed JSON whose `resolved` is not a usable count must not be
    silently treated as 'no floor' either -- that is the same bypass wearing
    valid syntax. `true` is the sharp one: bool is an int subclass in python, so
    an isinstance(x, int) check alone accepts it."""
    for bad in ('{"resolved": null}', '{"resolved": "57"}',
                '{"resolved": true}', '{"resolved": -1}', '{}',
                '[1]', '"just a string"'):
        with _baseline(bad) as env:
            r = _run(env)
            assert r.returncode == 8, (bad, r.returncode, r.stderr[:200])


def test_an_invalid_UTF8_baseline_is_rc8_not_a_traceback():
    """UnicodeDecodeError is a ValueError, NOT an OSError. Catching only
    (OSError, JSONDecodeError) let invalid UTF-8 escape as an uncaught
    traceback and exit 1 -- an undocumented code that contradicts the rc 8
    contract and gives the caller no actionable diagnostic."""
    with tempfile.TemporaryDirectory() as td:
        p = pathlib.Path(td) / "stub-lint-baseline.json"
        p.write_bytes(b'{"resolved": 57, "x": "\xff\xfe not utf8"}')
        r = _run({"STUB_LINT_BASELINE": str(p)})
        assert r.returncode == 8, (r.returncode, r.stderr[:300])
        assert "BASELINE INVALID" in r.stderr, r.stderr[:300]


def test_the_baseline_bypass_is_explicit_and_reported():
    """A bypass must exist (so a genuine emergency is not wedged) but it must be
    opt-in AND announce itself, which is what the swallowed-exception path was
    not."""
    with _baseline("{ not json") as env:
        env = dict(env, STUB_LINT_ALLOW_NO_BASELINE="1")
        r = _run(env)
        assert r.returncode == 0, (r.returncode, r.stderr[:300])
        assert "baseline floor SKIPPED" in r.stderr, r.stderr[:300]


def test_the_fixtures_never_mutate_the_TRACKED_baseline():
    """The guard on the guard. These tests previously rewrote the real
    stub-lint-baseline.json and relied on `finally` to put it back; a SIGKILL,
    an external timeout or a crash mid-write would leave the repo floor
    corrupt (blocking every later commit) or artificially low (silently
    weakening the gate). Prove the tracked file is byte-identical after the
    whole mutation battery has run."""
    before = BASELINE.read_bytes()
    test_a_coverage_DROP_is_an_error()
    test_a_corrupt_baseline_FAILS_CLOSED()
    test_a_non_integer_baseline_FAILS_CLOSED()
    test_the_baseline_bypass_is_explicit_and_reported()
    assert BASELINE.read_bytes() == before, "tracked baseline was modified"


def test_every_occurrence_lands_in_exactly_one_bucket():
    """THE denominator invariant. MEASURED 2026-08-06: 503 paired symbol refs
    existed but only 193 reached resolution -- the rest were `continue`d before
    any counting, so retargeting a ref at a nonexistent path made the published
    ratio look BETTER. Buckets must sum to the population or the number is a
    fiction; a mismatch is rc 6, not a plausible-looking ratio."""
    r = _run()
    assert r.returncode in (0, 7), (r.returncode, r.stderr[:300])
    cov = [l for l in r.stderr.splitlines() if "stub-behind-stamp:coverage" in l][0]
    buck = [l for l in r.stderr.splitlines() if l.strip().startswith("buckets:")][0]
    resolved, total = cov.split("resolved=")[1].split()[0].split("/")
    got = {k: int(v) for k, v in
           (p.split("=") for p in buck.split("buckets:")[1].split())}
    assert int(resolved) + sum(got.values()) == int(total), (cov, buck)


def test_unpaired_refs_are_counted_so_the_POPULATION_cannot_shrink():
    """The invariant is only worth as much as the population it runs over.

    A first cut of the bucket work counted `occurrences` AFTER the file/symbol
    pairing check, which put the invariant downstream of a drop it could not
    see: an extractor regression that stopped emitting `file` would move refs
    out of a counted bucket into nothing at all, every bucket would still sum,
    the unchanged resolved floor would still pass, and the ratio would IMPROVE
    because its denominator shrank. MEASURED 2026-08-06: 1,625 kind=symbol refs
    exist and 1,122 of them carry a symbol with no file.
    """
    # Derive the expected population INDEPENDENTLY from the cache. Asserting
    # only that an `unpaired-ref=` key exists is not enough -- reintroducing the
    # bug leaves the key present at 0, so a weaker form of this test passed the
    # mutation that removes the fix.
    cache = json.loads((REPO / "build/todo-cache.json").read_text(encoding="utf-8"))
    expected = sum(1
                   for node in cache
                   for it in (node.get("stamped_items") or [])
                   for ref in it.get("refs", [])
                   if ref.get("kind") == "symbol")

    r = _run()
    buck = [l for l in r.stderr.splitlines() if l.strip().startswith("buckets:")][0]
    assert "unpaired-ref=" in buck, buck
    cov = [l for l in r.stderr.splitlines() if "stub-behind-stamp:coverage" in l][0]
    total = int(cov.split("resolved=")[1].split()[0].split("/")[1])
    assert total == expected, (
        f"denominator {total} != {expected} kind=symbol refs in the cache -- "
        f"refs are being dropped before they are counted")


def test_missing_file_refs_are_counted_not_dropped():
    """The largest single class, and the one the old shape hid entirely."""
    r = _run()
    assert "missing-file=" in r.stderr, r.stderr[:400]
    n = int(r.stderr.split("missing-file=")[1].split()[0])
    if n:
        assert "missing file (ref points nowhere):" in r.stderr, r.stderr[:600]


def test_lint_sh_surfaces_the_coverage_line_to_the_CALLER():
    """INTEGRATION, not helper-only. The helper writes coverage to stderr
    because stdout is its findings channel -- and lint.sh's rc-0 branch read
    stdout only, so a normal lint run published no ratio at all and the whole
    point of counting the blind spot was invisible where people actually look.
    Testing the helper's own stderr would not have caught that.
    """
    r = subprocess.run(["bash", str(REPO / "scripts/lint.sh")], cwd=str(REPO),
                       capture_output=True, text=True, timeout=900)
    line = [l for l in r.stdout.splitlines()
            if "Check 7" in l and "resolved=" in l]
    assert line, r.stdout[-800:]
    # SEVERITY, not just presence. The roadmap chose "WARN on the standing
    # gap"; shipping it as an informational line made the implemented severity
    # weaker than the committed decision, and a presence-only assertion could
    # not tell the difference.
    assert "warn" in line[0], line[0]


def test_the_classifier_is_cached_PER_FILE_not_per_symbol():
    """PERF GUARD, structural rather than wall-clock.

    Classifying each unresolved ref with its own full-file scan took Check 7
    from 147ms to 829ms -- 5.62x on a check that runs in the PRE-COMMIT HOOK
    for every contributor on every commit. The stripped text does not depend on
    the symbol, so the fix was to cache it per FILE; the guard is that the
    number of expensive strips stays bounded by distinct files rather than by
    distinct (file, symbol) pairs. A timing assertion would be flaky on a
    loaded host; this states the invariant that actually matters.
    """
    sys.path.insert(0, str(REPO / "scripts/lint"))
    sys.path.insert(0, str(REPO / "scripts/todo-graph"))
    import importlib
    m = importlib.import_module("check_stub_behind_stamp")
    m._code_text.cache_clear()
    m._raw_text.cache_clear()
    m._resolve_cached.cache_clear()
    m._is_stub_cached.cache_clear()
    nodes = json.loads((REPO / "build/todo-cache.json").read_text(encoding="utf-8"))
    m._walk(nodes, REPO)
    ci = m._code_text.cache_info()
    assert ci.misses <= ci.hits + ci.misses, ci
    # Distinct files that needed stripping must be well under the number of
    # classification calls; equality would mean the per-file cache is doing
    # nothing and every ref pays its own scan again.
    assert ci.misses < 100, f"too many full-file strips: {ci}"


def test_bucket_names_do_not_claim_a_CAUSE():
    """A call site, a prototype and a macro invocation all carry `symbol(` while
    the real definition lives elsewhere -- src/kernel/test/test_alpc.c only
    CALLS kmalloc_fail_next, defined at src/kernel/mm/heap.c:499. So a bucket
    named 'resolver gap' or 'bookkeeping error' would be wrong for a large share
    of its own contents. The published names must stay evidence-shaped."""
    r = _run()
    buck = [l for l in r.stderr.splitlines() if l.strip().startswith("buckets:")][0]
    for causal in ("bookkeeping", "resolver-gap", "stale-ref", "absent="):
        assert causal not in buck, (causal, buck)
    assert "calllike-unresolved=" in buck and "no-calllike-token=" in buck, buck


if __name__ == "__main__":
    test_coverage_is_reported_not_discarded()
    test_coverage_never_lands_on_STDOUT()
    test_unresolved_refs_are_named_not_just_counted()
    test_the_baseline_is_a_real_floor_and_it_holds_today()
    test_a_coverage_DROP_is_an_error()
    test_an_improvement_is_reported_so_the_floor_gets_raised()
    test_a_corrupt_baseline_FAILS_CLOSED()
    test_a_non_integer_baseline_FAILS_CLOSED()
    test_an_invalid_UTF8_baseline_is_rc8_not_a_traceback()
    test_the_baseline_bypass_is_explicit_and_reported()
    test_the_fixtures_never_mutate_the_TRACKED_baseline()
    test_every_occurrence_lands_in_exactly_one_bucket()
    test_unpaired_refs_are_counted_so_the_POPULATION_cannot_shrink()
    test_lint_sh_surfaces_the_coverage_line_to_the_CALLER()
    test_the_classifier_is_cached_PER_FILE_not_per_symbol()
    test_missing_file_refs_are_counted_not_dropped()
    test_bucket_names_do_not_claim_a_CAUSE()
    print("PASS: Check 7 coverage counting + regression floor + bucket invariant")
