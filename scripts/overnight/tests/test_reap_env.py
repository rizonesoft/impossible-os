#!/usr/bin/env python3
"""scripts/overnight/reap_env.py stops every process that carries one exact environment entry, and no other."""
import os
import pathlib
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parents[3]
REAP = REPO / "scripts/overnight/reap_env.py"


def _alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    # A reaped child of this test is a zombie until waited for: not running.
    try:
        return pathlib.Path(f"/proc/{pid}/stat").read_text().split(") ")[1][0] != "Z"
    except (OSError, IndexError):
        return False


def _detached(env_extra, tmp_path, name):
    """A detached sleeper in its own session, as the companion's broker is, carrying `env_extra`."""
    pid_file = tmp_path / f"{name}.pid"
    subprocess.Popen(["bash", "-c", f"sleep 300 & echo $! > {pid_file}; wait"], start_new_session=True,
                     env={**os.environ, **env_extra}, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if pid_file.exists() and pid_file.read_text().strip():
            return int(pid_file.read_text())
        time.sleep(0.05)
    raise AssertionError("the sleeper never started")


def test_reaps_the_marked_processes_and_only_those(tmp_path):
    marker = f"CLAUDE_PLUGIN_DATA={tmp_path}/pass"
    mine = _detached({"CLAUDE_PLUGIN_DATA": f"{tmp_path}/pass"}, tmp_path, "mine")
    other = _detached({"CLAUDE_PLUGIN_DATA": f"{tmp_path}/pass-other"}, tmp_path, "other")
    try:
        done = subprocess.run([sys.executable, str(REAP), marker], capture_output=True, text=True, timeout=30)
        assert done.returncode == 0, done.stderr
        for _ in range(50):
            if not _alive(mine):
                break
            time.sleep(0.05)
        assert not _alive(mine), "the marked process outlived the reap"
        assert _alive(other), "a process with a different value was reaped"
    finally:
        for pid in (mine, other):
            try:
                os.kill(pid, 9)
            except ProcessLookupError:
                pass


def test_refuses_a_marker_that_is_not_key_equals_value():
    done = subprocess.run([sys.executable, str(REAP), "CLAUDE_PLUGIN_DATA"], capture_output=True, text=True,
                          timeout=30)
    assert done.returncode == 2
