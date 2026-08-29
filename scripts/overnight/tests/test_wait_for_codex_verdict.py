#!/usr/bin/env python3
"""B1: wait-for-codex-verdict.sh bounds each invocation UNDER the Bash tool's
~120s default so a bare poll is never killed, and returns a clear status the
caller re-invokes on. Verifies: sentinel -> exit 0 + DONE; no sentinel -> exit 3
+ STILL RUNNING; missing log -> still running; and the bounded wait never
overshoots max (so the default 100s stays under the 120s ceiling).

B2: on a still-running return the waiter reports per-log byte size + how long
since the log last GREW, and flags a log that has not grown for --stale-secs as
STALE (exit 4) so a genuinely-hung review is re-dispatched while a slow-but-alive
one keeps being waited on."""
import pathlib
import subprocess
import time
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
TOOL = REPO / "scripts/overnight/wait-for-codex-verdict.sh"


def _run(*logs, maxw=5, stale=None, cwd=None):
    args = ["bash", str(TOOL), "--max", str(maxw)]
    if stale is not None:
        args += ["--stale-secs", str(stale)]
    args += [str(x) for x in logs]
    # Default the CWD to the first log's dir so the B2 activity sidecar
    # (.claude/state/waiter-activity/) lands in the test's tempdir, not the repo.
    if cwd is None and logs:
        cwd = str(pathlib.Path(logs[0]).parent)
    return subprocess.run(args, capture_output=True, text=True, cwd=cwd)


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


def test_stale_flagged_after_no_growth():
    # B2: a log that does not grow across re-invocations past --stale-secs -> exit 4.
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.log"
        log.write_text("streaming...\n")             # content, no sentinel
        r1 = _run(log, maxw=1, stale=1, cwd=d)       # first sighting -> running
        assert r1.returncode == 3, (r1.returncode, r1.stdout)
        time.sleep(2)                                 # exceed stale window, no growth
        r2 = _run(log, maxw=1, stale=1, cwd=d)
        assert r2.returncode == 4, (r2.returncode, r2.stdout)
        assert "STALE" in r2.stdout and "no growth" in r2.stdout


def test_growth_resets_stall_clock():
    # B2: a log that GREW between checks is NOT stale (slow-but-alive, keep waiting).
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.log"
        log.write_text("chunk1\n")
        r1 = _run(log, maxw=1, stale=1, cwd=d)
        assert r1.returncode == 3
        time.sleep(2)
        log.write_text("chunk1\nchunk2 more streamed output\n")   # GREW
        r2 = _run(log, maxw=1, stale=1, cwd=d)
        assert r2.returncode == 3, (r2.returncode, r2.stdout)     # not stale
        assert "running:" in r2.stdout


def test_still_running_reports_activity():
    # B2: the still-running return reports bytes + last-growth age for the operator.
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.log"
        log.write_text("x" * 50)
        r = _run(log, maxw=1, stale=100, cwd=d)
        assert r.returncode == 3
        assert "bytes" in r.stdout and "last growth" in r.stdout


def test_bare_number_arg_ignored_not_phantom_log():
    # J2a: `... <log> 300` (intending --max 300) must NOT add a phantom "300"
    # log; the DONE log alone should still complete.
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.log"
        log.write_text("Turn completed (rc=0)\n")
        r = subprocess.run(["bash", str(TOOL), "--max", "5", str(log), "300"],
                           capture_output=True, text=True, cwd=d)
        assert r.returncode == 0, (r.returncode, r.stdout, r.stderr)
        assert "ignoring bare number '300'" in r.stderr, r.stderr


def test_missing_path_is_reported_as_missing_not_hung():
    """v17 close-out (2026-08-29): a path reconstructed from the jobId instead of
    the broker's logFile was reported as `0 bytes ... likely hung; re-dispatch`,
    and the re-dispatch cost a duplicate Codex run. A nonexistent path is now
    MISSING (exit 5), never STALE (exit 4); an existing stale log beside it
    still wins as STALE (refusal direction unchanged)."""
    with tempfile.TemporaryDirectory() as d:
        missing = pathlib.Path(d) / "reconstructed-from-jobid.out"
        _run(missing, maxw=1, stale=1)           # first sight starts the clock
        time.sleep(2)
        r = _run(missing, maxw=1, stale=1)
        assert r.returncode == 5, (r.returncode, r.stdout, r.stderr)
        assert "MISSING:" in r.stdout and "likely hung" not in r.stdout, r.stdout
        real = pathlib.Path(d) / "real.out"
        real.write_text("x")
        _run(missing, real, maxw=1, stale=1)
        time.sleep(2)
        r = _run(missing, real, maxw=1, stale=1)
        assert r.returncode == 4, (r.returncode, r.stdout)
        assert "STALE:" in r.stdout and "MISSING:" in r.stdout, r.stdout


if __name__ == "__main__":
    test_sentinel_present_returns_done()
    test_no_sentinel_returns_still_running()
    test_missing_log_returns_still_running()
    test_multi_log_waits_for_all()
    test_bounded_wait_does_not_overshoot_max()
    test_usage_error_without_logfile()
    test_stale_flagged_after_no_growth()
    test_growth_resets_stall_clock()
    test_still_running_reports_activity()
    test_bare_number_arg_ignored_not_phantom_log()
    test_missing_path_is_reported_as_missing_not_hung()
    print("PASS: wait-for-codex-verdict (B1 + B2 + J2a)")
