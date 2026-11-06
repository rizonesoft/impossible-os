#!/usr/bin/env python3
"""--watch must not wait out its arm timeout for an inotifywait that has exited.

On a tree with no todo/ directory it exits at once, the sentinel never comes, and the
5 s wait outlived a 3 s caller, so the watcher was killed before it answered. Only
machines with inotify-tools take this path, so it failed on CI and passed locally.
Control: a live process that reports the sentinel must still return True, and one that
stays silent must still wait.
"""
import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("query", REPO / "scripts/todo-graph/query.py")
Q = importlib.util.module_from_spec(spec)
sys.modules["query"] = Q
spec.loader.exec_module(Q)


def _wait(proc, text=b""):
    with tempfile.TemporaryFile() as errf:
        errf.write(text)
        errf.flush()
        t0 = time.monotonic()
        got = Q._await_watches_established(errf, proc)
    return got, time.monotonic() - t0


def test_dead_process_returns_at_once():
    p = subprocess.Popen(["false"])
    p.wait()
    got, took = _wait(p)
    assert got is False and took < 1, (got, took)


def test_sentinel_is_still_recognised():
    p = subprocess.Popen(["sleep", "5"])
    try:
        got, took = _wait(p, Q.WATCH_ARM_SENTINEL.encode())
        assert got is True and took < 1, (got, took)
    finally:
        p.kill()
        p.wait()


def test_a_live_silent_process_still_waits():
    p = subprocess.Popen(["sleep", "5"])
    old = Q.WATCH_ARM_TIMEOUT_SECS
    Q.WATCH_ARM_TIMEOUT_SECS = 0.3
    try:
        got, took = _wait(p)
        assert got is False and took >= 0.3, (got, took)
    finally:
        Q.WATCH_ARM_TIMEOUT_SECS = old
        p.kill()
        p.wait()


if __name__ == "__main__":
    test_dead_process_returns_at_once()
    test_sentinel_is_still_recognised()
    test_a_live_silent_process_still_waits()
    print("PASS test_watch_arm_wait")
