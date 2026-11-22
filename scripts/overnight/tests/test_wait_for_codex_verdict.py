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


def test_a_report_writer_slow_to_start_never_holds_the_waiter():
    """Every program the report could be written through takes three seconds to start: the report is
    written by the shell itself, so the wait still answers within its budget."""
    import os
    with tempfile.TemporaryDirectory() as d:
        bindir = pathlib.Path(d) / "bin"
        bindir.mkdir()
        for name in ("timeout", "cat"):
            (bindir / name).write_text(f"#!/bin/sh\nsleep 3\nexec /usr/bin/{name} \"$@\"\n")
            (bindir / name).chmod(0o755)
        log = pathlib.Path(d) / "r.out"
        log.write_text("review\n")
        pathlib.Path(str(log) + ".done").write_text("0\n")
        began = time.monotonic()
        r = subprocess.run(["bash", str(TOOL), "--max", "1", str(log)], capture_output=True, text=True, cwd=d,
                           timeout=60, env={**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}"})
        assert r.returncode == 0 and "DONE:" in r.stdout and time.monotonic() - began < 2.5, (r, time.monotonic() - began)


def test_the_report_is_written_inside_max_even_on_a_full_pipe():
    """A pending review, --max 1, and a stdout pipe nobody drains: the report's time is reserved inside
    --max, so the call ends by its deadline, not a second after it."""
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
            p = subprocess.Popen(["bash", str(TOOL), "--max", "1", "--no-stale", str(log)], cwd=d, stdout=wr,
                                 stderr=subprocess.DEVNULL)
            os.close(wr)
            wr = -1
            p.wait(timeout=20)
            took = time.monotonic() - began
            assert p.returncode == 3 and took < 1.2, (p.returncode, took)
        finally:
            if wr >= 0:
                os.close(wr)
            os.close(rd)


def test_a_usage_error_returns_inside_its_deadline():
    """A usage error with a timeout that is slow to start: the message is written by the shell's own bounded
    writer, so the call ends within --max instead of waiting on the slow command."""
    import os
    with tempfile.TemporaryDirectory() as d:
        bindir = pathlib.Path(d) / "bin"
        bindir.mkdir()
        (bindir / "timeout").write_text("#!/bin/bash\nsleep 3\nexec /usr/bin/timeout \"$@\"\n")
        (bindir / "timeout").chmod(0o755)
        began = time.monotonic()
        r = subprocess.run(["bash", str(TOOL), "--max", "1"], capture_output=True, text=True, cwd=d, timeout=30,
                           env={**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}"})
        assert r.returncode == 2 and time.monotonic() - began < 2.0, (r, time.monotonic() - began)


def _faults(bindir, patch, recover=""):
    """A python3 on PATH that runs the waiter's observer with `patch` applied to it first (the module is `o`
    in the patch), and handles a recovery with the shell snippet `recover` before running it as usual."""
    import shutil
    real = shutil.which("python3")
    (bindir / "faults.py").write_text(patch)
    boot = ("import os, sys; p = sys.argv[1]; sys.path.insert(0, os.path.dirname(p)); import verdict_observer as o; "
            "exec(open(sys.argv[2]).read(), {'o': o}); sys.exit(o.main(sys.argv[3:]))")
    (bindir / "python3").write_text(
        "#!/bin/bash\n"
        f'case "$1" in */verdict_observer.py) exec {real} -c "{boot}" "$1" "{bindir / "faults.py"}" "${{@:2}}";; esac\n'
        f'if [ "$2" = recover ]; then :; {recover}\nfi\n'
        f'exec {real} "$@"\n')
    (bindir / "python3").chmod(0o755)


def _statuses(codes):
    """A patch whose status reads answer `codes` in turn (the last repeats): 0 a status, 1 none, 3 an error."""
    return ("import errno\n"
            f"_seq = {list(codes)!r}\n"
            "def probe_status(log):\n"
            "    c = _seq.pop(0) if len(_seq) > 1 else _seq[0]\n"
            "    if c == 0:\n"
            "        return '0', None\n"
            "    if c == 1:\n"
            "        return None, None\n"
            "    return None, OSError(errno.EIO, 'Input/output error')\n"
            "o.probe_status = probe_status\n")


def _wait(d, bindir, *args, timeout=60):
    import os
    return subprocess.run(["bash", str(TOOL), *[str(a) for a in args]], capture_output=True, text=True, cwd=d,
                          timeout=timeout, env={**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}"})


def _setup(d):
    bindir = pathlib.Path(d) / "bin"
    bindir.mkdir()
    return bindir


def test_nothing_the_wait_starts_outlives_it_but_a_recovery_that_still_publishes():
    """The class: a probe stalls for good (metadata held by storage) and a recovery is slow to start (its
    interpreter held before exec). When the wait returns, no process of the call is left except that
    recovery, which still publishes the result."""
    import os
    with tempfile.TemporaryDirectory() as d:
        bindir = _setup(d)
        marks = pathlib.Path(d) / "probe.pid"
        _faults(bindir, "import os, time\n"
                        "def probe_meta(log):\n"
                        f"    open({str(marks)!r}, 'a').write(f'{{os.getpid()}}\\n')\n"
                        "    time.sleep(60)\n"
                        "o.probe_meta = probe_meta\n",
                recover="sleep 2")
        log = pathlib.Path(d) / "r.out"
        log.write_text("partial\n")
        pathlib.Path(str(log) + ".owner").write_text("")
        # A second log, with no recovery in hand, is the one whose metadata probe stalls: a log being
        # recovered is never age-judged, so its metadata is not probed.
        other = pathlib.Path(d) / "q.out"
        other.write_text("partial\n")
        began = time.monotonic()
        r = _wait(d, bindir, "--max", "1", "--no-stale", log, other)
        assert r.returncode == 3 and time.monotonic() - began < 1.5, (r, time.monotonic() - began)
        pids = [int(p) for p in marks.read_text().split()]
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline and any(os.path.exists(f"/proc/{p}") for p in pids):
            time.sleep(0.05)
        left = [p for p in pids if os.path.exists(f"/proc/{p}")]
        for p in left:
            os.kill(p, 9)
        assert not left, left
        deadline = time.monotonic() + 8
        done = pathlib.Path(str(log) + ".done")
        while time.monotonic() < deadline and not done.exists():
            time.sleep(0.05)
        assert done.exists() and done.read_text() == "1\n", "the recovery did not publish"


def test_a_stalled_recovery_never_holds_the_waiter_past_its_deadline():
    """Recovery stalls (a filesystem that does not answer): it runs in its own session, so the waiter still
    returns within --max, reporting the review pending."""
    import os
    import signal
    with tempfile.TemporaryDirectory() as d:
        bindir = _setup(d)
        pidfile = pathlib.Path(d) / "stalled.pid"
        _faults(bindir, "", recover=f"echo $$ > {pidfile}; exec sleep 8")
        log = pathlib.Path(d) / "r.out"
        log.write_text("partial\n")
        pathlib.Path(str(log) + ".owner").write_text("")
        began = time.monotonic()
        try:
            r = _wait(d, bindir, "--max", "3", log)
            assert r.returncode in (3, 4) and time.monotonic() - began < 4, (r, time.monotonic() - began)
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


def test_slow_metadata_reads_never_delay_the_answer():
    """Every metadata read takes two seconds, for one log and for three: the answer comes at --max from the
    observations made so far, never after a fresh read of each log."""
    with tempfile.TemporaryDirectory() as d:
        bindir = _setup(d)
        _faults(bindir, "import time\n_real = o.probe_meta\n"
                        "o.probe_meta = lambda log: (time.sleep(2), _real(log))[1]\n")
        logs = []
        for n in range(3):
            log = pathlib.Path(d) / f"r{n}.out"
            log.write_text("partial\n")
            logs.append(log)
        for maxw, which in ((3, logs[:1]), (1, logs)):
            began = time.monotonic()
            r = _wait(d, bindir, "--max", maxw, "--no-stale", *which)
            took = time.monotonic() - began
            assert r.returncode == 3 and took < maxw + 0.5, (r, took)


def test_a_stalled_owner_probe_never_holds_the_waiter_past_its_deadline():
    """The owner lock's probe stalls: the waiter still answers within --max from what it has observed."""
    with tempfile.TemporaryDirectory() as d:
        bindir = _setup(d)
        _faults(bindir, "import time\no.owner_free = lambda log: time.sleep(30)\n")
        log = pathlib.Path(d) / "r.out"
        log.write_text("partial\n")
        pathlib.Path(str(log) + ".owner").write_text("")
        began = time.monotonic()
        r = _wait(d, bindir, "--max", "3", "--no-stale", log)
        assert r.returncode == 3 and time.monotonic() - began < 3.5, (r, time.monotonic() - began)


def test_a_stalled_tail_never_hides_a_finished_wave():
    """Every status is published and only the transcript's tail stalls: the answer is DONE (exit 0) at --max,
    with a note that the tail was not read, never STILL RUNNING."""
    with tempfile.TemporaryDirectory() as d:
        bindir = _setup(d)
        _faults(bindir, "import time\no.read_tail = lambda log: time.sleep(5) or []\n")
        log = pathlib.Path(d) / "r.out"
        log.write_text("review\nTurn completed (rc=0)\n")
        pathlib.Path(str(log) + ".done").write_text("0\n")
        began = time.monotonic()
        r = _wait(d, bindir, "--max", "1", log)
        assert r.returncode == 0 and "DONE:" in r.stdout and "could not be read" in r.stdout, r
        assert time.monotonic() - began < 1.5


def test_slow_bookkeeping_never_hides_a_published_verdict():
    """The first-sighting bookkeeping takes a second and a half: a published status is read first, so the
    wait still answers DONE within --max."""
    with tempfile.TemporaryDirectory() as d:
        bindir = _setup(d)
        _faults(bindir, "import time\n_real = o.first_seen\n"
                        "o.first_seen = lambda log, a: (time.sleep(1.5), _real(log, a))[1]\n")
        log = pathlib.Path(d) / "r.out"
        log.write_text("review\n")
        pathlib.Path(str(log) + ".done").write_text("0\n")
        r = _wait(d, bindir, "--max", "1", log)
        assert r.returncode == 0 and "DONE:" in r.stdout, r


def test_a_pending_log_replaced_by_a_fifo_never_stops_the_other_probes():
    """The first log is a FIFO that took the path (opening it would block); the second is done: the probe of
    the first reads only metadata, so the second is still observed."""
    import os
    with tempfile.TemporaryDirectory() as d:
        first, second = pathlib.Path(d) / "a.out", pathlib.Path(d) / "b.out"
        os.mkfifo(first)
        second.write_text("review\n")
        pathlib.Path(str(second) + ".done").write_text("0\n")
        r = _run(first, second, maxw=2, cwd=d)
        assert r.returncode == 3 and f"running: {second}" not in r.stdout, r
        assert f"running: {first} (" in r.stdout and "no observation" not in r.stdout, r


def test_an_unreadable_status_is_never_reported_unpublished_or_stale():
    """The owner has gone and the status cannot be read (an I/O error), with an old transcript: the status
    is unknown, so the wait reports it UNREADABLE and still running, never UNPUBLISHED or STALE."""
    import os
    with tempfile.TemporaryDirectory() as d:
        bindir = _setup(d)
        _faults(bindir, _statuses([3]))
        log = pathlib.Path(d) / "r.out"
        log.write_text("review\n")
        pathlib.Path(str(log) + ".owner").write_text("")
        os.utime(log, (1, 1))
        for args in (("--no-stale",), ("--stale-secs", "1")):
            r = _wait(d, bindir, "--max", "2", *args, log)
            assert r.returncode == 3 and "UNREADABLE" in r.stdout, r
            assert "UNPUBLISHED" not in r.stdout and "STALE" not in r.stdout, r


def test_a_transient_unreadable_status_never_hides_a_later_stale_log():
    """The first status read fails, the next finds none and the transcript is old: the latest observation
    decides, so the wait reports STALE (exit 4), not UNREADABLE."""
    import os
    with tempfile.TemporaryDirectory() as d:
        bindir = _setup(d)
        _faults(bindir, _statuses([3, 1]))
        log = pathlib.Path(d) / "r.out"
        log.write_text("review\n")
        os.utime(log, (1, 1))
        r = _wait(d, bindir, "--max", "20", "--stale-secs", "1", log)
        assert r.returncode == 4 and "STALE:" in r.stdout and "UNREADABLE" not in r.stdout, r


def test_a_failed_recovery_is_unpublished_even_after_a_transient_read_error():
    """No status, the owner gone, a recovery that cannot publish, and a status read that fails once on either
    side of it: the recovery's result is kept until a read finds no status, so the wait reports UNPUBLISHED
    (exit 4), never a review still running."""
    with tempfile.TemporaryDirectory() as d:
        for codes in ([3, 1], [1, 3, 1]):
            sub = pathlib.Path(d) / "x".join(map(str, codes))
            sub.mkdir()
            bindir = _setup(sub)
            _faults(bindir, _statuses(codes), recover="exit 1")
            log = sub / "r.out"
            log.write_text("review\n")
            pathlib.Path(str(log) + ".owner").write_text("")
            r = _wait(sub, bindir, "--max", "20", "--no-stale", log)
            assert r.returncode == 4 and f"UNPUBLISHED: {log}" in r.stdout and "UNREADABLE" not in r.stdout, (codes, r)


def test_a_status_published_after_a_recovery_ends_the_wait_done():
    """No status, a recovery that fails, then a status is there (another publisher raced it): completion is
    decided from the statuses after the round, so a short wait answers DONE (exit 0), not STILL RUNNING."""
    with tempfile.TemporaryDirectory() as d:
        bindir = _setup(d)
        _faults(bindir, _statuses([1, 0, 3]), recover="exit 1")
        log = pathlib.Path(d) / "r.out"
        log.write_text("review\n")
        pathlib.Path(str(log) + ".owner").write_text("")
        r = _wait(d, bindir, "--max", "3", "--no-stale", log)
        assert r.returncode == 0 and "DONE:" in r.stdout and "UNREADABLE" not in r.stdout, r




def test_a_cancelled_wait_ends_its_observer_even_while_a_probe_is_held():
    """SIGTERM to the waiter while its observer is held in a metadata probe: the waiter exits 143 and the
    observer is gone with it, never left behind one per cancelled call."""
    import os
    import signal
    with tempfile.TemporaryDirectory() as d:
        bindir = _setup(d)
        marks = pathlib.Path(d) / "probe.pid"
        _faults(bindir, "import os, time\n"
                        "def probe_meta(log):\n"
                        f"    open({str(marks)!r}, 'a').write(f'{{os.getpid()}}\\n')\n"
                        "    time.sleep(60)\n"
                        "o.probe_meta = probe_meta\n")
        log = pathlib.Path(d) / "r.out"
        log.write_text("partial\n")
        p = subprocess.Popen(["bash", str(TOOL), "--max", "30", "--no-stale", str(log)], cwd=d,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             env={**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}"})
        end = time.monotonic() + 10
        while not (marks.exists() and marks.read_text().strip()) and time.monotonic() < end:
            time.sleep(0.05)
        pid = int(marks.read_text().split()[0])
        p.send_signal(signal.SIGTERM)
        assert p.wait(timeout=5) == 143
        end = time.monotonic() + 3
        while time.monotonic() < end:
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.05)
        else:
            raise AssertionError(f"observer {pid} outlived the cancelled wait")


def test_a_log_whose_metadata_cannot_be_read_is_never_reported_missing():
    """A live log with no status whose lstat fails with EIO, and a first-sighting store that cannot be
    written: past --stale-secs the waiter still reports it running (exit 3), never MISSING (exit 5)."""
    with tempfile.TemporaryDirectory() as d:
        bindir = _setup(d)
        _faults(bindir, "import errno, os\n"
                        "_real = os.lstat\n"
                        "def lstat(p, *a, **k):\n"
                        "    if str(p).endswith('r.out'):\n"
                        "        raise OSError(errno.EIO, 'Input/output error')\n"
                        "    return _real(p, *a, **k)\n"
                        "o.os.lstat = lstat\n"
                        "o.first_seen = lambda log, act: 0\n")
        log = pathlib.Path(d) / "r.out"
        log.write_text("partial\n")
        r = _wait(d, bindir, "--max", "4", "--stale-secs", "1", log)
        assert r.returncode == 3 and "MISSING" not in r.stdout, (r.returncode, r.stdout)


def test_a_log_path_holding_a_newline_keeps_its_published_status():
    """A review directory whose name holds a newline: the published 124 is reported for that log."""
    with tempfile.TemporaryDirectory() as d:
        rev = pathlib.Path(d) / "a\nb"
        rev.mkdir()
        log = rev / "r.out"
        log.write_text("x\nTurn completed (rc=124)\n")
        pathlib.Path(str(log) + ".done").write_text("124\n")
        r = subprocess.run(["bash", str(TOOL), "--max", "10", str(log)], capture_output=True, text=True, cwd=d,
                           timeout=30)
        assert r.returncode == 0 and "-- Turn completed (rc=124)" in r.stdout, (r.returncode, r.stdout)


def test_a_recovery_that_cannot_be_started_never_hides_the_wave():
    """The first log's recovery cannot start (EMFILE); the second has a published status: the first is
    reported UNPUBLISHED and the second DONE, never the whole wave as still running."""
    with tempfile.TemporaryDirectory() as d:
        bindir = _setup(d)
        _faults(bindir, "import errno\n"
                        "def start_recovery(log):\n"
                        "    raise OSError(errno.EMFILE, 'Too many open files')\n"
                        "o.start_recovery = start_recovery\n")
        a = pathlib.Path(d) / "a.out"
        a.write_text("partial\n")
        pathlib.Path(str(a) + ".owner").write_text("")
        b = pathlib.Path(d) / "b.out"
        b.write_text("x\nTurn completed (rc=0)\n")
        pathlib.Path(str(b) + ".done").write_text("0\n")
        r = _wait(d, bindir, "--max", "5", a, b)
        assert "UNPUBLISHED" in r.stdout and r.returncode != 3, (r.returncode, r.stdout)



def test_a_cancelled_wait_ends_a_report_writer_blocked_on_a_full_pipe():
    """stdout is a full pipe whose reader stays open: the report writer blocks, the waiter is sent TERM, and
    it exits 143 with every child it started gone, the blocked writer included."""
    import os
    import signal
    with tempfile.TemporaryDirectory() as d:
        log = pathlib.Path(d) / "r.out"
        log.write_text("x\nTurn completed (rc=0)\n")
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
            p = subprocess.Popen(["bash", str(TOOL), "--max", "30", str(log)], cwd=d, stdout=wr,
                                 stderr=subprocess.DEVNULL)
            children = set()
            end = time.monotonic() + 10
            while time.monotonic() < end and not children:
                time.sleep(0.3)
                for task in pathlib.Path(f"/proc/{p.pid}/task").iterdir():
                    try:
                        children |= {int(c) for c in (task / "children").read_text().split()}
                    except OSError:
                        pass
            assert children, "the waiter never started its report writer"
            time.sleep(0.3)
            p.send_signal(signal.SIGTERM)
            assert p.wait(timeout=5) == 143
            end = time.monotonic() + 3
            left = set(children)
            while left and time.monotonic() < end:
                for pid in list(left):
                    try:
                        os.kill(pid, 0)
                    except ProcessLookupError:
                        left.discard(pid)
                time.sleep(0.05)
            assert not left, f"children {left} outlived the cancelled wait"
        finally:
            os.close(wr)
            os.close(rd)


def test_a_recovery_that_cannot_start_then_an_unreadable_status_is_never_stale():
    """The recovery cannot start (EMFILE), the next status read fails (EIO), then "no status" is read
    conclusively: the log is never judged stale on the transcript, and ends UNPUBLISHED."""
    with tempfile.TemporaryDirectory() as d:
        bindir = _setup(d)
        _faults(bindir, _statuses([1, 3, 1]) +
                "def start_recovery(log):\n"
                "    raise OSError(errno.EMFILE, 'Too many open files')\n"
                "o.start_recovery = start_recovery\n")
        log = pathlib.Path(d) / "r.out"
        log.write_text("partial\n")
        import os
        os.utime(log, (0, 0))                              # an old transcript: stale by its age alone
        pathlib.Path(str(log) + ".owner").write_text("")
        r = _wait(d, bindir, "--max", "6", "--stale-secs", "1", log)
        assert "UNPUBLISHED" in r.stdout and "STALE" not in r.stdout, (r.returncode, r.stdout)


def test_every_pipe_wait_is_a_slice_checked_against_the_elapsed_time_deadline():
    """A relative read -t does not count a host's suspend, so the waiter waits only through _read_slice (at
    most a second, then the elapsed-time deadline again): the one read -t in the script is that helper's."""
    import re
    text = TOOL.read_text()
    waits = [m.start() for m in re.finditer(r"\bread\b[^\n]*-t\b", text)]
    body = text[text.index("_read_slice() {"):text.index("\n}\n", text.index("_read_slice() {"))]
    assert len(waits) == 1 and "read -r -t" in body, waits


def test_a_record_split_across_wait_slices_is_read_whole():
    """The observer writes a status record in two parts 1.5 s apart: the slices keep the first part, and the
    record is read whole (DONE with its status)."""
    with tempfile.TemporaryDirectory() as d:
        bindir = _setup(d)
        _faults(bindir, "import sys, time\n"
                        "_real = o.say\n"
                        "def say(line):\n"
                        "    if line.startswith('done '):\n"
                        "        sys.stdout.write(line[:3]); sys.stdout.flush(); time.sleep(1.5)\n"
                        "        sys.stdout.write(line[3:] + '\\n'); sys.stdout.flush()\n"
                        "    else:\n"
                        "        _real(line)\n"
                        "o.say = say\n")
        log = pathlib.Path(d) / "r.out"
        log.write_text("x\nTurn completed (rc=0)\n")
        pathlib.Path(str(log) + ".done").write_text("0\n")
        r = _wait(d, bindir, "--max", "10", log)
        assert r.returncode == 0 and "-- Turn completed (rc=0)" in r.stdout, (r.returncode, r.stdout)


def test_an_owner_check_that_fails_once_keeps_the_failed_recovery_for_the_next_round():
    """The recovery cannot start, and the first owner-lock check fails (EIO): the result is kept, and the
    next round, whose check reads the lock free, ends the log UNPUBLISHED in the same call. (The first check is
    the one before the recovery; the second judges it.)"""
    with tempfile.TemporaryDirectory() as d:
        bindir = _setup(d)
        _faults(bindir, _statuses([1]) +
                "def start_recovery(log):\n"
                "    raise OSError(errno.EMFILE, 'Too many open files')\n"
                "o.start_recovery = start_recovery\n"
                "_real_owner, _calls = o.owner_state, [0]\n"
                "def owner_state(log):\n"
                "    _calls[0] += 1\n"
                "    return None if _calls[0] == 2 else _real_owner(log)\n"
                "o.owner_state = owner_state\n")
        log = pathlib.Path(d) / "r.out"
        log.write_text("partial\n")
        pathlib.Path(str(log) + ".owner").write_text("")
        r = _wait(d, bindir, "--max", "6", "--no-stale", log)
        assert "UNPUBLISHED" in r.stdout, (r.returncode, r.stdout)


def test_a_fifo_in_place_of_an_owner_lock_never_hides_the_rest_of_the_wave():
    """The first log's owner lock is a FIFO with no writer; the second log has published its status. The
    observer opens the lock only as a regular file, so it goes on to the second: only the first is pending."""
    import os
    with tempfile.TemporaryDirectory() as d:
        first, second = pathlib.Path(d) / "a.out", pathlib.Path(d) / "b.out"
        first.write_text("partial\n")
        os.mkfifo(str(first) + ".owner")
        second.write_text("x\nTurn completed (rc=0)\n")
        pathlib.Path(str(second) + ".done").write_text("0\n")
        r = subprocess.run(["bash", str(TOOL), "--max", "6", "--no-stale", str(first), str(second)],
                           capture_output=True, text=True, cwd=d, timeout=60)
        pending = [ln for ln in r.stdout.splitlines() if "pending:" in ln]
        assert pending and "a.out" in pending[0] and "b.out" not in r.stdout, (r.returncode, r.stdout)


def test_an_aged_transcript_is_never_judged_stale_while_a_failed_recovery_awaits_its_owner_check():
    """As the owner-check test above, with an old transcript and stale detection on: the wait ends
    UNPUBLISHED, never STALE before the failed recovery is judged."""
    import os
    with tempfile.TemporaryDirectory() as d:
        bindir = _setup(d)
        _faults(bindir, _statuses([1]) +
                "def start_recovery(log):\n"
                "    raise OSError(errno.EMFILE, 'Too many open files')\n"
                "o.start_recovery = start_recovery\n"
                "_real_owner, _calls = o.owner_state, [0]\n"
                "def owner_state(log):\n"
                "    _calls[0] += 1\n"
                "    return None if _calls[0] == 2 else _real_owner(log)\n"
                "o.owner_state = owner_state\n")
        log = pathlib.Path(d) / "r.out"
        log.write_text("partial\n")
        os.utime(log, (0, 0))
        pathlib.Path(str(log) + ".owner").write_text("")
        r = _wait(d, bindir, "--max", "6", "--stale-secs", "1", log)
        assert "UNPUBLISHED" in r.stdout and "STALE" not in r.stdout, (r.returncode, r.stdout)



def test_an_aged_transcript_is_never_judged_stale_in_the_round_that_starts_its_recovery():
    """The owner has gone and the transcript is long past --stale-secs: the recovery that starts this round
    publishes on a later one, and the wait reports that status, never STALE before it."""
    import os
    with tempfile.TemporaryDirectory() as d:
        bindir = _setup(d)
        log = pathlib.Path(d) / "r.out"
        _faults(bindir, "import subprocess\n"
                        "def start_recovery(f):\n"
                        "    return subprocess.Popen(['bash', '-c', 'sleep 2; printf \"0\\\\n\" > \"$0.done\"', f])\n"
                        "o.start_recovery = start_recovery\n")
        log.write_text("x\nTurn completed (rc=0)\n")
        os.utime(log, (0, 0))
        pathlib.Path(str(log) + ".owner").write_text("")
        r = _wait(d, bindir, "--max", "8", "--stale-secs", "1", log)
        assert r.returncode == 0 and "STALE" not in r.stdout, (r.returncode, r.stdout)


def test_a_damaged_first_seen_record_never_hides_the_rest_of_the_wave():
    """The first log is missing and its first-seen record holds bytes that are not a timestamp; the second
    has published. The observer reads the damage as the epoch and goes on: the second is not pending."""
    import hashlib
    with tempfile.TemporaryDirectory() as d:
        first, second = pathlib.Path(d) / "a.out", pathlib.Path(d) / "b.out"
        act = pathlib.Path(d) / ".claude/state/waiter-first-seen"
        act.mkdir(parents=True)
        (act / hashlib.md5(str(first).encode()).hexdigest()[:16]).write_bytes(b"\xff\xfe12\n")
        second.write_text("x\nTurn completed (rc=0)\n")
        pathlib.Path(str(second) + ".done").write_text("0\n")
        r = subprocess.run(["bash", str(TOOL), "--max", "6", str(first), str(second)],
                           capture_output=True, text=True, cwd=d, timeout=60)
        assert "b.out" not in r.stdout and "a.out" in r.stdout, (r.returncode, r.stdout)
