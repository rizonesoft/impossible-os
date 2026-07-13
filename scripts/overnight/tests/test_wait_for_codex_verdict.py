#!/usr/bin/env python3
"""B1: wait-for-codex-verdict.sh bounds each invocation UNDER the Bash tool's
~120s default so a bare poll is never killed, and returns a clear status the
caller re-invokes on. Verifies: sentinel -> exit 0 + DONE; no sentinel -> exit 3
+ STILL RUNNING; missing log -> still running; and the bounded wait never
overshoots max (so the default 100s stays under the 120s ceiling)."""
import pathlib
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
TOOL = REPO / "scripts/overnight/wait-for-codex-verdict.sh"


def _run(*logs, maxw=5):
    return subprocess.run(
        ["bash", str(TOOL), "--max", str(maxw), *[str(x) for x in logs]],
        capture_output=True, text=True)


def test_sentinel_present_returns_done():
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.log"
        log.write_text("review output...\nTurn completed (rc=0)\n")
        r = _run(log)
        assert r.returncode == 0, r.stderr
        assert "DONE:" in r.stdout and "Turn completed (rc=0)" in r.stdout


def test_no_sentinel_returns_still_running():
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.log"
        log.write_text("still going...\n")
        r = _run(log, maxw=1)
        assert r.returncode == 3, (r.returncode, r.stdout, r.stderr)
        assert "STILL RUNNING" in r.stdout


def test_missing_log_returns_still_running():
    with tempfile.TemporaryDirectory() as d:
        r = _run(pathlib.Path(d) / "absent.log", maxw=1)
        assert r.returncode == 3, (r.returncode, r.stdout)
        assert "STILL RUNNING" in r.stdout


def test_multi_log_waits_for_all():
    # Multi-kind round: DONE only when EVERY log completed; otherwise pending.
    with tempfile.TemporaryDirectory() as d:
        a = pathlib.Path(d) / "a.log"
        b = pathlib.Path(d) / "b.log"
        a.write_text("Turn completed (rc=0)\n")
        b.write_text("still going...\n")             # b not done -> STILL RUNNING
        r = _run(a, b, maxw=1)
        assert r.returncode == 3 and str(b) in r.stdout, r.stdout
        b.write_text("Turn completed (rc=0)\n")       # now both done -> DONE
        r = _run(a, b)
        assert r.returncode == 0 and r.stdout.count("DONE:") == 2, r.stdout


def test_bounded_wait_does_not_overshoot_max():
    # max=2 must finish in ~2s (never a full poll interval), proving the default
    # 100s bound stays well under the 120s Bash-tool ceiling.
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.log"
        log.write_text("still going...\n")
        t0 = time.monotonic()
        r = _run(log, maxw=2)
        elapsed = time.monotonic() - t0
        assert r.returncode == 3
        assert elapsed < 6, f"bounded wait overshot: {elapsed:.1f}s for max=2"


def test_usage_error_without_logfile():
    r = subprocess.run(["bash", str(TOOL)], capture_output=True, text=True)
    assert r.returncode == 2


if __name__ == "__main__":
    test_sentinel_present_returns_done()
    test_no_sentinel_returns_still_running()
    test_missing_log_returns_still_running()
    test_multi_log_waits_for_all()
    test_bounded_wait_does_not_overshoot_max()
    test_usage_error_without_logfile()
    print("PASS: wait-for-codex-verdict (B1)")
