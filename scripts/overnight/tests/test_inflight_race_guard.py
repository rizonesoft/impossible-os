#!/usr/bin/env python3
"""D1: inflight_race_guard warns when the main session greps/reads the SAME
files an in-flight exploratory Agent was dispatched to map -- and stays silent
for non-overlapping work (the correct parallel pattern) and non-exploratory
agents. Warning-only; the state file is the only side effect."""
import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
HOOK = REPO / ".claude/hooks/inflight_race_guard.py"


def _run(payload, state_dir):
    # Redirect the hook's state file into a temp dir by running from there is not
    # possible (path is __file__-relative), so tests share the real state path but
    # each test clears it first. Run in-process-isolated via subprocess.
    return subprocess.run([sys.executable, str(HOOK)],
                          input=json.dumps(payload), text=True, capture_output=True)


STATE = REPO / ".claude/state/inflight-dispatch.json"


def _clear():
    try:
        STATE.unlink()
    except FileNotFoundError:
        pass


def _dispatch(atype, prompt):
    return _run({"tool_name": "Task",
                 "tool_input": {"subagent_type": atype, "prompt": prompt}}, None)


def _grep(pattern, path=""):
    return _run({"tool_name": "Grep",
                 "tool_input": {"pattern": pattern, "path": path}}, None)


def _read(fp):
    return _run({"tool_name": "Read", "tool_input": {"file_path": fp}}, None)


def test_overlap_warns_once_then_silent():
    _clear()
    try:
        _dispatch("kernel-explorer", "map src/kernel/task.c and mutex.c surface")
        r1 = _read("src/kernel/task.c")
        assert "racing in-flight agent" in r1.stdout, r1.stdout
        r2 = _grep("thread_create", "src/kernel/task.c")
        assert r2.stdout.strip() == "", "must warn ONCE per dispatch: " + r2.stdout
    finally:
        _clear()


def test_non_overlapping_work_is_silent():
    _clear()
    try:
        _dispatch("kernel-explorer", "map src/kernel/task.c")
        r = _grep("foo", "src/kernel/vmm.c")   # different file -> parallel work, OK
        assert r.stdout.strip() == "", "non-overlapping work must not warn: " + r.stdout
    finally:
        _clear()


def test_non_exploratory_agent_records_nothing():
    _clear()
    try:
        _dispatch("code-reviewer", "review src/kernel/task.c")
        assert not STATE.exists(), "a non-exploratory dispatch must not arm the guard"
        r = _read("src/kernel/task.c")
        assert r.stdout.strip() == ""
    finally:
        _clear()


def test_subagentstop_clears_window():
    _clear()
    try:
        _dispatch("kernel-explorer", "map src/kernel/task.c")
        assert STATE.exists()
        _run({"agent_transcript_path": "/x/leaf.jsonl"}, None)  # SubagentStop: no tool_name
        assert not STATE.exists(), "SubagentStop must close the in-flight window"
    finally:
        _clear()


if __name__ == "__main__":
    test_overlap_warns_once_then_silent()
    test_non_overlapping_work_is_silent()
    test_non_exploratory_agent_records_nothing()
    test_subagentstop_clears_window()
    print("PASS: inflight-race-guard (D1)")
