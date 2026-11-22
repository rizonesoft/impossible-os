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


def _finish(log, rc=0):
    """What a dispatcher does after the review process exits."""
    pathlib.Path(str(log) + ".done").write_text(f"{rc}\n")


def test_sentinel_present_returns_done():
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.log"
        log.write_text("review output...\nTurn completed (rc=0)\n")
        _finish(log)
        r = _run(log)
        assert r.returncode == 0, r.stderr
        assert "DONE:" in r.stdout and "Turn completed (rc=0)" in r.stdout
        assert "NEXT:" in r.stdout and "receiving-code-review" in r.stdout, r.stdout


def test_companion_progress_line_is_not_the_terminal_sentinel():
    # The companion prints "[codex] Turn completed." mid-log, before its verdict;
    # only the dispatcher's <log>.done, written after the review exits, ends a wait.
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.log"
        log.write_text("[codex] Running command: ...\n[codex] Turn completed.\n")
        r = _run(log, maxw=1)
        assert r.returncode == 3, (r.returncode, r.stdout, r.stderr)
        assert "STILL RUNNING" in r.stdout


def test_quoted_terminal_line_inside_a_live_log_is_not_done():
    # A live review whose latest line quotes a terminal line is still running:
    # only the dispatcher's <log>.done ends a wait, never log text.
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.log"
        log.write_text("review quoting an earlier run:\nTurn completed (rc=0)\n")
        r = _run(log, maxw=1)
        assert r.returncode == 3, (r.returncode, r.stdout, r.stderr)


def test_no_sentinel_returns_still_running():
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.log"
        log.write_text("still going...\n")
        r = _run(log, maxw=1)
        assert r.returncode == 3, (r.returncode, r.stdout, r.stderr)
        assert "STILL RUNNING" in r.stdout
        assert "NEXT:" not in r.stdout, r.stdout   # no receive before the wave is done


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
        _finish(a)
        b.write_text("still going...\n")             # b not done -> STILL RUNNING
        r = _run(a, b, maxw=1)
        assert r.returncode == 3 and str(b) in r.stdout, r.stdout
        b.write_text("Turn completed (rc=0)\n")       # now both done -> DONE
        _finish(b)
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
        r1 = _run(log, maxw=1, stale=5, cwd=d)       # first sighting -> running
        assert r1.returncode == 3, (r1.returncode, r1.stdout)
        time.sleep(2)                                 # exceed stale window, no growth
        r2 = _run(log, maxw=1, stale=1, cwd=d)
        assert r2.returncode == 4, (r2.returncode, r2.stdout)
        assert "STALE" in r2.stdout and "no growth" in r2.stdout


def test_a_trailing_option_without_a_value_is_refused_at_once():
    for opt in ("--max", "--stale-secs"):
        r = subprocess.run(["bash", str(TOOL), "x.log", opt], capture_output=True, text=True, timeout=10)
        assert r.returncode == 2 and "needs a value" in r.stderr, (opt, r.returncode, r.stderr)


def test_a_missing_log_whose_sighting_cannot_be_kept_is_missing_at_once():
    import os
    with tempfile.TemporaryDirectory() as d:
        state = pathlib.Path(d) / ".claude/state"
        state.mkdir(parents=True)
        (state / "waiter-first-seen").write_text("")     # a file where the directory goes
        r = _run(pathlib.Path(d) / "never.out", maxw=1, stale=300, cwd=d)
        assert r.returncode == 5, (r.returncode, r.stdout)


def test_an_empty_sighting_left_by_an_interrupted_write_reads_as_missing():
    import hashlib
    with tempfile.TemporaryDirectory() as d:
        log = str(pathlib.Path(d) / "never.out")
        side = pathlib.Path(d) / ".claude/state/waiter-first-seen" / hashlib.md5(log.encode()).hexdigest()[:16]
        side.parent.mkdir(parents=True)
        side.write_text("")
        r = _run(pathlib.Path(log), maxw=1, stale=300, cwd=d)
        assert r.returncode == 5, (r.returncode, r.stdout, r.stderr)


def test_a_log_silent_from_the_start_is_stale_in_the_same_call():
    # B2: the first sighting is sampled when the wait starts, not when it ends.
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.log"
        log.write_text("streaming...\n")
        r = _run(log, maxw=2, stale=1, cwd=d)
        assert r.returncode == 4, (r.returncode, r.stdout)


def test_growth_resets_stall_clock():
    # B2: a log that GREW between checks is NOT stale (slow-but-alive, keep waiting).
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.log"
        log.write_text("chunk1\n")
        r1 = _run(log, maxw=1, stale=5, cwd=d)
        assert r1.returncode == 3
        time.sleep(4)                                  # silent past stale=3 ...
        log.write_text("chunk1\nchunk2 more streamed output\n")   # ... then GREW
        r2 = _run(log, maxw=1, stale=3, cwd=d)
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
        _finish(log)
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


def test_no_stale_keeps_waiting_on_a_log_that_is_already_silent(tmp_path):
    import os
    import time
    log = tmp_path / "r-round.out"
    log.write_text("working\n")
    old = time.time() - 4000
    os.utime(log, (old, old))
    t = time.monotonic()
    r = subprocess.run(["bash", str(TOOL), "--max", "3", "--stale-secs", "60", "--no-stale", str(log)],
                       capture_output=True, text=True, timeout=30)
    assert r.returncode == 3, (r.returncode, r.stdout)
    assert time.monotonic() - t >= 2.5, "a second wait on a silent log must sleep, not return at once"
    missing = subprocess.run(["bash", str(TOOL), "--max", "2", "--stale-secs", "1", "--no-stale",
                              str(tmp_path / "nope.out")], capture_output=True, text=True, timeout=30)
    assert missing.returncode == 5, missing.stdout


def test_a_result_whose_owner_is_gone_is_recovered_by_the_waiter():
    """A log whose owner lock is free (the owner died) and has no status: the waiter recovers it as 1 at once,
    rather than waiting on a review nobody will ever finish."""
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.out"
        log.write_text("partial\n")
        pathlib.Path(str(log) + ".owner").write_text("")
        r = _run(log, maxw=5)
        assert r.returncode == 0 and "rc=1" in r.stdout, r
        assert "owner ended without publishing" in log.read_text()


def test_a_stalled_recovery_never_holds_the_waiter_past_its_deadline():
    """Recovery stalls (a filesystem that does not answer): it runs detached, so the waiter still returns
    within --max, reporting the review pending; the time it spent counts against one deadline."""
    import os
    import signal
    with tempfile.TemporaryDirectory() as d:
        bindir = pathlib.Path(d) / "bin"
        bindir.mkdir()
        pidfile = pathlib.Path(d) / "stalled.pid"
        fake = bindir / "python3"
        fake.write_text("#!/bin/bash\n"
                        f'if [ "$2" = recover ]; then echo $$ > {pidfile}; exec sleep 8; fi\n'
                        'exec /usr/bin/python3 "$@"\n')
        fake.chmod(0o755)
        log = pathlib.Path(d) / "r.out"
        log.write_text("partial\n")
        pathlib.Path(str(log) + ".owner").write_text("")
        began = time.monotonic()
        try:
            r = subprocess.run(["bash", str(TOOL), "--max", "3", str(log)], capture_output=True, text=True,
                               cwd=d, timeout=30, env={**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}"})
            took = time.monotonic() - began
            assert r.returncode in (3, 4) and took < 6, (r, took)
            assert pidfile.exists(), "recovery was never started"
        finally:
            t = time.monotonic()
            while time.monotonic() - t < 5 and not (pidfile.exists() and pidfile.read_text().strip()):
                time.sleep(0.05)
            if pidfile.exists() and pidfile.read_text().strip():
                try:
                    os.kill(int(pidfile.read_text()), signal.SIGKILL)
                except ProcessLookupError:
                    pass


def test_a_recovery_that_cannot_publish_is_reported_as_ended_not_running():
    """The owner is gone and its recovery cannot publish (a full filesystem): the review has ended and will
    never publish, so the waiter reports it UNPUBLISHED (exit 4, re-dispatch it), even with --no-stale."""
    import os
    with tempfile.TemporaryDirectory() as d:
        bindir = pathlib.Path(d) / "bin"
        bindir.mkdir()
        (bindir / "python3").write_text('#!/bin/bash\nif [ "$2" = recover ]; then exit 1; fi\n'
                                        'exec /usr/bin/python3 "$@"\n')
        (bindir / "python3").chmod(0o755)
        log = pathlib.Path(d) / "r.out"
        log.write_text("partial\n")
        pathlib.Path(str(log) + ".owner").write_text("")
        r = subprocess.run(["bash", str(TOOL), "--max", "15", "--no-stale", str(log)], capture_output=True,
                           text=True, cwd=d, timeout=60, env={**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}"})
        assert r.returncode == 4 and "UNPUBLISHED" in r.stdout, r


def test_the_sleep_is_what_is_left_after_the_silence_scan():
    """A slow silence scan (here each stat takes two seconds) is counted before the sleep, so the waiter
    still returns close to --max instead of sleeping the time the scan already spent."""
    import os
    with tempfile.TemporaryDirectory() as d:
        bindir = pathlib.Path(d) / "bin"
        bindir.mkdir()
        (bindir / "stat").write_text('#!/bin/sh\nsleep 2\nexec /usr/bin/stat "$@"\n')
        (bindir / "stat").chmod(0o755)
        log = pathlib.Path(d) / "r.out"
        log.write_text("partial\n")
        began = time.monotonic()
        r = subprocess.run(["bash", str(TOOL), "--max", "3", "--stale-secs", "999", str(log)], capture_output=True,
                           text=True, cwd=d, timeout=60, env={**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}"})
        took = time.monotonic() - began
        assert r.returncode == 3 and took < 6, (r.returncode, took)


def test_a_stalled_owner_probe_never_holds_the_waiter_past_its_deadline():
    """Opening the owner lock stalls (here flock takes thirty seconds): the probe runs in the observer, so the
    waiter still answers within --max from what it has observed."""
    import os
    with tempfile.TemporaryDirectory() as d:
        bindir = pathlib.Path(d) / "bin"
        bindir.mkdir()
        (bindir / "flock").write_text("#!/bin/sh\nexec sleep 30\n")
        (bindir / "flock").chmod(0o755)
        log = pathlib.Path(d) / "r.out"
        log.write_text("partial\n")
        pathlib.Path(str(log) + ".owner").write_text("")
        began = time.monotonic()
        r = subprocess.run(["bash", str(TOOL), "--max", "3", "--no-stale", str(log)], capture_output=True,
                           text=True, cwd=d, timeout=60, env={**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}"})
        took = time.monotonic() - began
        assert r.returncode == 3 and took < 3 + 1.5, (r, took)


def test_a_damaged_status_beside_a_released_owner_is_recovered_not_read_as_done():
    """An empty .done (a write the host cut short) is no status: the waiter never reports it as a finished
    review, and the recovery replaces it with 1 once the owner is gone."""
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.out"
        log.write_text("partial\n")
        pathlib.Path(str(log) + ".owner").write_text("")
        pathlib.Path(str(log) + ".done").write_text("")
        r = _run(log, maxw=8)
        assert r.returncode == 0 and "rc=1)" in r.stdout, r
        assert pathlib.Path(str(log) + ".done").read_text() == "1\n"


def test_slow_metadata_reads_after_the_deadline_never_delay_the_answer():
    """Three logs whose every stat takes two seconds: the answer comes at --max from the observations made so
    far, never after a fresh read of each log."""
    import os
    with tempfile.TemporaryDirectory() as d:
        bindir = pathlib.Path(d) / "bin"
        bindir.mkdir()
        (bindir / "stat").write_text('#!/bin/sh\nsleep 2\nexec /usr/bin/stat "$@"\n')
        (bindir / "stat").chmod(0o755)
        logs = []
        for n in range(3):
            log = pathlib.Path(d) / f"r{n}.out"
            log.write_text("partial\n")
            logs.append(str(log))
        began = time.monotonic()
        r = subprocess.run(["bash", str(TOOL), "--max", "1", "--no-stale", *logs], capture_output=True, text=True,
                           cwd=d, timeout=60, env={**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}"})
        took = time.monotonic() - began
        assert r.returncode == 3 and took < 1 + 1.5, (r, took)


def test_a_full_stdout_pipe_never_holds_the_waiter_past_its_deadline():
    """The caller waits for the exit before it drains stdout, and the pipe is already full: the report's one
    write is bounded, so the waiter still exits by --max (plus the writer's second) with its verdict code."""
    import os
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.out"
        log.write_text("review\nTurn completed (rc=0)\n")
        pathlib.Path(str(log) + ".done").write_text("0\n")
        rd, wr = os.pipe()
        os.set_blocking(wr, False)
        try:
            while True:
                os.write(wr, b"x" * 65536)
        except BlockingIOError:
            pass                                       # the pipe is full, as the test needs
        os.set_blocking(wr, True)
        began = time.monotonic()
        try:
            r = subprocess.run(["bash", str(TOOL), "--max", "1", str(log)], stdout=wr, stderr=subprocess.DEVNULL,
                               cwd=d, timeout=30)
        finally:
            os.close(wr)
            os.close(rd)
        assert r.returncode == 0 and time.monotonic() - began < 1 + 3, (r.returncode, time.monotonic() - began)


def test_a_stalled_tail_never_hides_a_finished_wave():
    """Every status is published and only the transcript's tail stalls: the answer is DONE (exit 0) at --max,
    with a note that the tail was not read, never STILL RUNNING."""
    import os
    with tempfile.TemporaryDirectory() as d:
        bindir = pathlib.Path(d) / "bin"
        bindir.mkdir()
        import shutil
        real = shutil.which("python3")
        (bindir / "python3").write_text(f'#!/bin/sh\ncase " $* " in *" tail "*) sleep 5;; esac\nexec {real} "$@"\n')
        (bindir / "python3").chmod(0o755)
        log = pathlib.Path(d) / "r.out"
        log.write_text("review\nTurn completed (rc=0)\n")
        pathlib.Path(str(log) + ".done").write_text("0\n")
        began = time.monotonic()
        r = subprocess.run(["bash", str(TOOL), "--max", "1", str(log)], capture_output=True, text=True, cwd=d,
                           timeout=60, env={**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}"})
        assert r.returncode == 0 and "DONE:" in r.stdout and "could not be read" in r.stdout, r
        assert time.monotonic() - began < 1 + 3


def test_setup_runs_no_command_that_could_hold_the_clock():
    """Every command the setup once ran (a grep per argument, dirname) now takes three seconds: the setup
    runs none of them, so the answer still comes within --max of the call's start."""
    import os
    with tempfile.TemporaryDirectory() as d:
        bindir = pathlib.Path(d) / "bin"
        bindir.mkdir()
        for name in ("grep", "dirname"):
            (bindir / name).write_text("#!/bin/sh\nexec sleep 3\n")
            (bindir / name).chmod(0o755)
        log = pathlib.Path(d) / "r.out"
        log.write_text("partial\n")
        began = time.monotonic()
        r = subprocess.run(["bash", str(TOOL), "--max", "1", "--no-stale", str(log), str(log) + "2"],
                           capture_output=True, text=True, cwd=d, timeout=60,
                           env={**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}"})
        assert r.returncode in (3, 5) and time.monotonic() - began < 1 + 1.5, (r, time.monotonic() - began)


def test_a_transcript_replaced_by_a_fifo_answers_done_and_leaves_nothing_behind():
    """The status is published and the transcript has been replaced by a FIFO with no writer: the answer is
    DONE at once, and no process of this call is left holding the FIFO open."""
    import os
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.out"
        os.mkfifo(log)
        pathlib.Path(str(log) + ".done").write_text("0\n")
        began = time.monotonic()
        r = subprocess.run(["bash", str(TOOL), "--max", "5", str(log)], capture_output=True, text=True, cwd=d,
                           timeout=60)
        assert r.returncode == 0 and "DONE:" in r.stdout and time.monotonic() - began < 3, r
        time.sleep(0.5)
        holders = []
        for pid in filter(str.isdigit, os.listdir("/proc")):
            try:
                if any(os.readlink(f"/proc/{pid}/fd/{n}") == str(log) for n in os.listdir(f"/proc/{pid}/fd")):
                    holders.append(pid)
            except OSError:
                continue
        assert not holders, holders


def test_a_setup_warning_never_holds_the_call_on_a_full_stderr():
    """The bare-number warning is written to a stderr pipe that is full and never drained: the call still
    answers within --max of its start."""
    import os
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.out"
        log.write_text("partial\n")
        rd, wr = os.pipe()
        os.set_blocking(wr, False)
        try:
            while True:
                os.write(wr, b"x" * 65536)
        except BlockingIOError:
            pass
        os.set_blocking(wr, True)
        try:
            began = time.monotonic()
            p = subprocess.Popen(["bash", str(TOOL), "--max", "1", "--no-stale", str(log), "300"], cwd=d,
                                 stdout=subprocess.DEVNULL, stderr=wr)
            os.close(wr)
            wr = -1
            p.wait(timeout=10)
            assert p.returncode == 3 and time.monotonic() - began < 1 + 2.5, (p.returncode, time.monotonic() - began)
        finally:
            if wr >= 0:
                os.close(wr)
            os.close(rd)


def test_a_warning_and_a_report_on_full_pipes_share_one_budget():
    """The wave is done, a setup warning is queued, and both stdout and stderr are one full pipe nobody
    drains: the call still returns within one reporting budget, not two."""
    import os
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.out"
        log.write_text("review\n")
        pathlib.Path(str(log) + ".done").write_text("0\n")
        rd, wr = os.pipe()
        os.set_blocking(wr, False)
        try:
            while True:
                os.write(wr, b"x" * 65536)
        except BlockingIOError:
            pass
        os.set_blocking(wr, True)
        try:
            began = time.monotonic()
            p = subprocess.Popen(["bash", str(TOOL), "--max", "3", str(log), "300"], cwd=d, stdout=wr, stderr=wr)
            os.close(wr)
            wr = -1
            p.wait(timeout=20)
            assert p.returncode == 0 and time.monotonic() - began < 3 + 0.8, (p.returncode, time.monotonic() - began)
        finally:
            if wr >= 0:
                os.close(wr)
            os.close(rd)
