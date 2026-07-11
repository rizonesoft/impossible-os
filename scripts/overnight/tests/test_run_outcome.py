#!/usr/bin/env python3
import json, pathlib, subprocess, sys, tempfile

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE.parent / "run-outcome.py"

NOW = 1_800_000_000


def _run(root, *extra):
    args = [sys.executable, str(SCRIPT), str(root), "--now", str(NOW), *extra]
    r = subprocess.run(args, text=True, capture_output=True)
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)


def _mkrepo(d):
    root = pathlib.Path(d)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "-c", "user.email=t@t", "-c",
                    "user.name=t", "commit", "-q", "--allow-empty", "-m", "seed"],
                   check=True)
    return root


def _head(root):
    return subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                          text=True, capture_output=True).stdout.strip()


def _streak(root):
    p = root / ".claude" / "overnight" / "state" / "unproductive-streak"
    return json.loads(p.read_text())["count"] if p.exists() else 0


def _backoff(root):
    p = root / ".claude" / "overnight" / "watchdog-backoff-until"
    return p.read_text().split() if p.exists() else None


def test_head_moved_is_productive_and_resets_streak():
    with tempfile.TemporaryDirectory() as d:
        root = _mkrepo(d)
        # seed a streak, then a commit lands during the "run"
        (root / ".claude" / "overnight" / "state").mkdir(parents=True)
        (root / ".claude" / "overnight" / "state" / "unproductive-streak").write_text(
            json.dumps({"count": 2, "last": NOW - 600}))
        out = _run(root, "--exit", "1", "--start-head", "deadbeef", "--run-secs", "60")
        assert out["outcome"] == "productive", out
        assert _streak(root) == 0
        assert out["breaker"] is False


def test_long_clean_exit_is_productive():
    with tempfile.TemporaryDirectory() as d:
        root = _mkrepo(d)
        out = _run(root, "--exit", "0", "--start-head", _head(root),
                   "--run-secs", "1200")
        assert out["outcome"] == "productive", out


def test_fast_dead_run_increments_streak_no_breaker_below_threshold():
    with tempfile.TemporaryDirectory() as d:
        root = _mkrepo(d)
        out = _run(root, "--exit", "1", "--start-head", _head(root),
                   "--run-secs", "120")
        assert out["outcome"] == "unproductive"
        assert out["streak"] == 1 and out["breaker"] is False
        assert _backoff(root) is None


def test_breaker_trips_at_threshold_and_escalates():
    with tempfile.TemporaryDirectory() as d:
        root = _mkrepo(d)
        head = _head(root)
        for i in range(1, 4):
            out = _run(root, "--exit", "1", "--start-head", head,
                       "--run-secs", "120")
        assert out["streak"] == 3 and out["breaker"] is True
        until, count = _backoff(root)
        assert int(until) == NOW + 1800 and count == "3"
        # 4th failure escalates to the 3600s cap
        out = _run(root, "--exit", "1", "--start-head", head,
                   "--run-secs", "120")
        until, count = _backoff(root)
        assert int(until) == NOW + 3600 and count == "4"
        # 5th failure stays at the cap
        out = _run(root, "--exit", "1", "--start-head", head,
                   "--run-secs", "120")
        until, _ = _backoff(root)
        assert int(until) == NOW + 3600


def test_snoozed_run_leaves_streak_untouched():
    with tempfile.TemporaryDirectory() as d:
        root = _mkrepo(d)
        _run(root, "--exit", "1", "--start-head", _head(root), "--run-secs", "60")
        out = _run(root, "--exit", "1", "--start-head", _head(root),
                   "--run-secs", "60", "--snoozed")
        assert out["outcome"] == "snoozed"
        assert _streak(root) == 1  # unchanged


def test_rollover_checkpoint_leaves_streak_untouched():
    with tempfile.TemporaryDirectory() as d:
        root = _mkrepo(d)
        head = _head(root)
        state = root / ".claude/state"
        state.mkdir(parents=True)
        # existing streak of 2 (one shy of the breaker)
        (root / ".claude/overnight/state").mkdir(parents=True)
        (root / ".claude/overnight/state/unproductive-streak").write_text(
            json.dumps({"count": 2, "last": NOW}))
        # a verified rollover pending in the guard state
        (state / "sequencer-run.json").write_text(json.dumps(
            {"active": True, "rollover": {"pending": True, "epoch": NOW}}))
        d1 = _run(root, "--exit", "0", "--start-head", head,
                  "--run-secs", "120")
        assert d1["outcome"] == "checkpoint", d1
        assert d1["checkpoint_kind"] == "rollover"
        # streak untouched (still 2), breaker NOT tripped, no backoff written
        assert _streak(root) == 2
        assert _backoff(root) is None


def test_rollover_checkpoint_classified():
    with tempfile.TemporaryDirectory() as d:
        root = _mkrepo(d)
        head = _head(root)
        state = root / ".claude/state"
        state.mkdir(parents=True)
        (state / "sequencer-run.json").write_text(json.dumps(
            {"active": True, "rollover": {"pending": True, "epoch": NOW}}))
        d1 = _run(root, "--exit", "0", "--start-head", head,
                  "--run-secs", "60")
        assert d1["outcome"] == "checkpoint" and \
            d1["checkpoint_kind"] == "rollover", d1
        assert _streak(root) == 0


def test_git_unavailable_fails_open():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)  # not a git repo
        out = _run(root, "--exit", "1", "--start-head", "", "--run-secs", "60")
        assert out["outcome"] == "unknown"
        assert _streak(root) == 0


if __name__ == "__main__":
    test_head_moved_is_productive_and_resets_streak()
    test_long_clean_exit_is_productive()
    test_fast_dead_run_increments_streak_no_breaker_below_threshold()
    test_breaker_trips_at_threshold_and_escalates()
    test_snoozed_run_leaves_streak_untouched()
    test_rollover_checkpoint_leaves_streak_untouched()
    test_rollover_checkpoint_classified()
    test_git_unavailable_fails_open()
    print("PASS: run_outcome")
