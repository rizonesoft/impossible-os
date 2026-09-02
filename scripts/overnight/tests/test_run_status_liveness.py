#!/usr/bin/env python3
"""run-status.py reports LIVENESS beside the armed marker (v18 close-out,
2026-09-03).

A host restart on 2026-08-28 killed the transient systemd units mid-gate and
left `.claude/state/sequencer-armed` in place; `run-status.md` then said
`Armed: True` about a run that no longer existed and cost the next session its
first ten minutes. The marker is a file a dead run cannot remove, so the page
now consults `run-liveness.sh` (units, launcher flock, report freshness) and
names a stale marker outright.

THREE directions are pinned, because the first version of this line had only
two and was WRONG on the third: a live run must not be called stale; a dead
run with a marker and no timer must be; and an ARMED run whose timer has not
fired yet must NOT be, because it is healthy. That last case was observed 90
seconds after a real arm on 2026-09-03 -- `run-liveness.sh` says DEAD while
the timer waits, which is the correct answer to its own question and the wrong
input to a staleness verdict on its own.
"""
import importlib.util
import os
import pathlib
import stat
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
STATUS = HERE.parent / "run-status.py"


def _mod():
    spec = importlib.util.spec_from_file_location("run_status", STATUS)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _project(d, liveness_rc, armed, timer=False):
    root = pathlib.Path(d)
    (root / "scripts" / "overnight").mkdir(parents=True)
    (root / ".claude" / "state").mkdir(parents=True)
    stub = root / "scripts" / "overnight" / "run-liveness.sh"
    stub.write_text(f"#!/bin/sh\nexit {liveness_rc}\n")
    stub.chmod(stub.stat().st_mode | stat.S_IXUSR)
    if armed:
        (root / ".claude" / "state" / "sequencer-armed").write_text("x\n")
    return root


def _with_timer(mod, scheduled):
    """Pin the timer probe. The real one shells out to `systemctl --user
    is-active <repo-basename>.timer`, which cannot be staged from a tempdir."""
    mod._timer_scheduled = lambda _p, _s=scheduled: _s
    return mod


def test_dead_run_with_marker_and_no_timer_is_named_stale():
    m = _with_timer(_mod(), False)
    with tempfile.TemporaryDirectory() as d:
        out = m.render(_project(d, 1, armed=True), "t")
        assert "Armed: True   Live: DEAD" in out, out
        assert "STALE MARKER" in out, out


def test_armed_and_waiting_for_its_timer_is_not_called_stale():
    """The regression: armed, timer scheduled, nothing executing yet."""
    m = _with_timer(_mod(), True)
    with tempfile.TemporaryDirectory() as d:
        out = m.render(_project(d, 1, armed=True), "t")
        assert "Armed: True   Live: DEAD" in out, out
        assert "STALE MARKER" not in out, out
        assert "ARMED and WAITING" in out, out


def test_live_run_is_not_called_stale():
    m = _with_timer(_mod(), False)
    with tempfile.TemporaryDirectory() as d:
        out = m.render(_project(d, 0, armed=True), "t")
        assert "Armed: True   Live: LIVE" in out, out
        assert "STALE MARKER" not in out, out


def test_disarmed_dead_tree_is_plain():
    m = _with_timer(_mod(), False)
    with tempfile.TemporaryDirectory() as d:
        out = m.render(_project(d, 1, armed=False), "t")
        assert "Armed: False   Live: DEAD" in out, out
        assert "STALE MARKER" not in out, out


def test_missing_liveness_script_is_unknown_not_a_crash():
    m = _with_timer(_mod(), False)
    with tempfile.TemporaryDirectory() as d:
        root = _project(d, 1, armed=True)
        (root / "scripts" / "overnight" / "run-liveness.sh").unlink()
        out = m.render(root, "t")
        assert "Live: UNKNOWN" in out, out
        assert "STALE MARKER" not in out, out


if __name__ == "__main__":
    test_dead_run_with_marker_and_no_timer_is_named_stale()
    test_armed_and_waiting_for_its_timer_is_not_called_stale()
    test_live_run_is_not_called_stale()
    test_disarmed_dead_tree_is_plain()
    test_missing_liveness_script_is_unknown_not_a_crash()
    print("PASS: run-status liveness")
