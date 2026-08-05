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
    orig = BASELINE.read_text(encoding="utf-8")
    data = json.loads(orig)
    try:
        data["resolved"] = data["resolved"] + 10_000
        BASELINE.write_text(json.dumps(data), encoding="utf-8")
        r = _run()
        assert r.returncode == 7, (r.returncode, r.stdout[:300], r.stderr[:300])
        assert "COVERAGE REGRESSION" in r.stderr, r.stderr[:400]
        assert "blinder" in r.stderr, r.stderr[:400]
    finally:
        BASELINE.write_text(orig, encoding="utf-8")


def test_an_improvement_is_reported_so_the_floor_gets_raised():
    orig = BASELINE.read_text(encoding="utf-8")
    data = json.loads(orig)
    try:
        data["resolved"] = 1
        BASELINE.write_text(json.dumps(data), encoding="utf-8")
        r = _run()
        assert r.returncode == 0, r.returncode
        assert "coverage improved" in r.stderr, r.stderr[:400]
    finally:
        BASELINE.write_text(orig, encoding="utf-8")


def test_a_missing_baseline_degrades_to_warn_not_block():
    """Fail OPEN on the baseline itself. A lint that refuses every commit because
    its own bookkeeping file is missing is worse than the gap it guards."""
    orig = BASELINE.read_text(encoding="utf-8")
    try:
        BASELINE.write_text("{ not json", encoding="utf-8")
        r = _run()
        assert r.returncode == 0, (r.returncode, r.stderr[:300])
        assert "stub-behind-stamp:coverage" in r.stderr
    finally:
        BASELINE.write_text(orig, encoding="utf-8")


if __name__ == "__main__":
    test_coverage_is_reported_not_discarded()
    test_coverage_never_lands_on_STDOUT()
    test_unresolved_refs_are_named_not_just_counted()
    test_the_baseline_is_a_real_floor_and_it_holds_today()
    test_a_coverage_DROP_is_an_error()
    test_an_improvement_is_reported_so_the_floor_gets_raised()
    test_a_missing_baseline_degrades_to_warn_not_block()
    print("PASS: Check 7 coverage counting + regression floor")
