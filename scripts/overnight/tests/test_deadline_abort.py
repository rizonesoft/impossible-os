#!/usr/bin/env python3
"""deadline-check.sh: bound a run in time, and abort a run that produces nothing.

The runner had no time bound at all before 2026-07-31 -- it ran to fixpoint or
until a human disarmed it -- so "arm it for 24 hours and judge the evidence"
could not be expressed and every bounded experiment became a manual vigil.

The property that matters most is WHERE the stop happens. This check runs at
SPAWN time, so a run past its deadline simply does not start another segment
and the last one already ended at a verified rollover: clean tree, pushed,
receipts valid. A systemd timer firing --disarm would land mid-section as often
as not. These tests pin the verdicts, and pin that an unreadable bound never
silently means "run forever".
"""
from __future__ import annotations

import pathlib
import subprocess
import tempfile
import time

CHECK = pathlib.Path(__file__).resolve().parents[1] / "deadline-check.sh"

CONTINUE, STOP = 0, 10


def _project(td, deadline=None, noship=None):
    root = pathlib.Path(td)
    (root / ".claude/state").mkdir(parents=True, exist_ok=True)
    (root / ".claude/overnight").mkdir(parents=True, exist_ok=True)
    if deadline is not None:
        (root / ".claude/state/run-deadline").write_text(f"{deadline}\n")
    if noship is not None:
        (root / ".claude/overnight/noship-streak").write_text(f"{noship}\n")
    return root


def _check(root, env=None):
    import os
    e = dict(os.environ)
    if env:
        e.update(env)
    r = subprocess.run(["bash", str(CHECK), str(root)],
                       capture_output=True, text=True, timeout=60, env=e)
    return r.returncode, r.stdout.strip()


def test_no_deadline_runs_to_fixpoint():
    with tempfile.TemporaryDirectory() as td:
        rc, out = _check(_project(td))
        assert rc == CONTINUE, (rc, out)
        assert "no deadline set" in out, out


def test_future_deadline_continues_and_reports_remaining():
    with tempfile.TemporaryDirectory() as td:
        rc, out = _check(_project(td, deadline=int(time.time()) + 3600))
        assert rc == CONTINUE, (rc, out)
        assert "min remaining" in out, out


def test_past_deadline_stops():
    with tempfile.TemporaryDirectory() as td:
        rc, out = _check(_project(td, deadline=int(time.time()) - 5))
        assert rc == STOP, (rc, out)
        assert "deadline reached" in out, out
        assert "clean stop" in out, out


def test_unreadable_deadline_does_not_silently_mean_forever():
    """A corrupt bound must be VISIBLE. It also must not stop a healthy run --
    that is an operator problem, not a reason to halt."""
    with tempfile.TemporaryDirectory() as td:
        root = _project(td)
        (root / ".claude/state/run-deadline").write_text("not-a-number\n")
        rc, out = _check(root)
        assert rc == CONTINUE, (rc, out)
        assert "unreadable" in out, out


def test_noship_streak_aborts_at_the_limit():
    with tempfile.TemporaryDirectory() as td:
        rc, out = _check(_project(td, noship=2))
        assert rc == STOP, (rc, out)
        assert "shipped no section" in out, out


def test_noship_below_the_limit_continues():
    with tempfile.TemporaryDirectory() as td:
        rc, out = _check(_project(td, noship=1))
        assert rc == CONTINUE, (rc, out)


def test_noship_limit_is_configurable():
    with tempfile.TemporaryDirectory() as td:
        rc, _ = _check(_project(td, noship=3), env={"OVERNIGHT_NOSHIP_LIMIT": "9"})
        assert rc == CONTINUE, rc
        rc, _ = _check(_project(td, noship=3), env={"OVERNIGHT_NOSHIP_LIMIT": "3"})
        assert rc == STOP, rc


def test_deadline_takes_precedence_over_a_healthy_streak():
    with tempfile.TemporaryDirectory() as td:
        rc, out = _check(_project(td, deadline=int(time.time()) - 1, noship=0))
        assert rc == STOP, (rc, out)
        assert "deadline reached" in out, out


if __name__ == "__main__":
    test_no_deadline_runs_to_fixpoint()
    test_future_deadline_continues_and_reports_remaining()
    test_past_deadline_stops()
    test_unreadable_deadline_does_not_silently_mean_forever()
    test_noship_streak_aborts_at_the_limit()
    test_noship_below_the_limit_continues()
    test_noship_limit_is_configurable()
    test_deadline_takes_precedence_over_a_healthy_streak()
    print("PASS: deadline + abort criteria")
