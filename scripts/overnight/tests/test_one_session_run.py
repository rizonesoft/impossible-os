#!/usr/bin/env python3
"""One session, its stop guard and its heartbeat. tmux is a shim that logs its calls."""
import os
import pathlib
import shutil
import stat
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parents[3]
HOOKS = REPO / ".claude/hooks"
sys.path.insert(0, str(HOOKS))


def _fixture(d, alive=True, idle=0):
    root = pathlib.Path(d) / "repo"
    (root / "scripts/overnight").mkdir(parents=True)
    (root / ".claude/state").mkdir(parents=True)
    for rel in ("scripts/overnight/heartbeat.sh", "scripts/overnight/run-common.sh"):
        shutil.copy(REPO / rel, root / rel)
    (root / "scripts/overnight/supervisor.py").write_text(
        'print(\'{"repair_needed": %s}\' % ("true" if __import__("os").environ.get("STALLED") else "false"))\n')
    (root / "scripts/overnight/notify.sh").write_text("echo notified >> \"$LOG\"\n")
    (root / ".claude/state/sequencer-armed").write_text("x\n")
    bindir = pathlib.Path(d) / "bin"
    bindir.mkdir()
    log = pathlib.Path(d) / "calls.log"
    tmux = bindir / "tmux"
    tmux.write_text('#!/bin/bash\necho "tmux $*" >> "$LOG"\n'
                    '[ "$1" = has-session ] && exit "${SESSION_RC:-0}"\nexit 0\n')
    tmux.chmod(tmux.stat().st_mode | stat.S_IEXEC)
    tdir = pathlib.Path(d) / "transcripts"
    tdir.mkdir()
    (root / ".claude/state/run-session-id").write_text("abc\n")
    # A newer transcript from an attended session must not read as this run working.
    t = tdir / "abc.jsonl"
    (tdir / "attended.jsonl").write_text("{}\n")
    t.write_text("{}\n")
    os.utime(t, (time.time() - idle, time.time() - idle))
    env = {**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}", "LOG": str(log),
           "SESSION_RC": "0" if alive else "1", "HEARTBEAT_TRANSCRIPTS": str(tdir),
           "RUN_NOTIFY_ENV": "/nonexistent"}
    return root, log, env


def _once(root, env):
    r = subprocess.run(["bash", "scripts/overnight/heartbeat.sh", "--once"], cwd=root, env=env,
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r
    return r


def test_a_working_session_is_left_alone():
    with tempfile.TemporaryDirectory() as d:
        root, log, env = _fixture(d, idle=60)
        _once(root, env)
        calls = log.read_text()
        assert "send-keys" not in calls and "new-session" not in calls, calls


def test_a_dead_session_restarts_with_continue_and_alerts():
    with tempfile.TemporaryDirectory() as d:
        root, log, env = _fixture(d, alive=False)
        _once(root, env)
        calls = log.read_text()
        assert "new-session" in calls and "--resume abc" in calls and "notified" in calls, calls
        assert "--continue" not in calls, "--continue could resume an attended session"
        assert "OVERNIGHT_SEQUENCER_RUN=1" in calls and "CLAUDE_CODE_AUTO_COMPACT_WINDOW=400000" in calls


def test_refusal_a_session_that_keeps_dying_backs_off():
    with tempfile.TemporaryDirectory() as d:
        root, log, env = _fixture(d, alive=False)
        _once(root, env)
        _once(root, env)
        assert log.read_text().count("new-session") == 1, log.read_text()


def test_an_idle_session_is_nudged():
    with tempfile.TemporaryDirectory() as d:
        root, log, env = _fixture(d, idle=4000)
        _once(root, env)
        assert "Continue the run." in log.read_text()


def test_a_stalled_idle_session_is_told_to_diagnose():
    with tempfile.TemporaryDirectory() as d:
        root, log, env = _fixture(d, idle=4000)
        _once(root, {**env, "STALLED": "1"})
        calls = log.read_text()
        assert "find out why" in calls and "Continue the run." not in calls, calls


def test_refusal_a_stopped_run_is_not_restarted():
    with tempfile.TemporaryDirectory() as d:
        root, log, env = _fixture(d, alive=False)
        (root / ".claude/state/sequencer-armed").unlink()
        _once(root, env)
        assert not log.exists() or "new-session" not in log.read_text()
    with tempfile.TemporaryDirectory() as d:
        root, log, env = _fixture(d, alive=False)
        (root / ".claude/state/sequencer-fixpoint").write_text("done\n")
        _once(root, env)
        assert not log.exists() or "new-session" not in log.read_text()


def _stop(armed=True, fixpoint=False, oracle="NEEDS_WORK", rollover=False):
    import run_phase_guard as g
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        g.ARMED_MARKER = root / "armed"
        g.FIXPOINT_SENTINEL = root / "fix"
        g.STATE_PATH = root / "state.json"
        if armed:
            g.ARMED_MARKER.write_text("x")
        if fixpoint:
            g.FIXPOINT_SENTINEL.write_text("x")
        g._live_oracle_status = lambda: oracle
        g.save_state({"active": True, "rollover": {"pending": rollover, "epoch": time.time()}})
        os.environ["OVERNIGHT_SEQUENCER_RUN"] = "1"
        try:
            return g.handle_stop()
        finally:
            del os.environ["OVERNIGHT_SEQUENCER_RUN"]


def test_stop_guard_holds_the_session_until_the_work_is_done():
    assert _stop() == 2
    # A verified rollover used to end the session for a relaunch. Nothing relaunches now.
    assert _stop(rollover=True) == 2
    assert _stop(fixpoint=True) == 0
    assert _stop(armed=False) == 0
    assert _stop(oracle="DONE") == 0
    assert _stop(oracle="BLOCKED") == 0


def test_run_script_shape():
    text = (REPO / "scripts/run.sh").read_text()
    for verb in ("start)", "attach)", "status)", "stop)"):
        assert verb in text, verb
    subprocess.run(["bash", "-n", str(REPO / "scripts/run.sh")], check=True)


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("OK")
