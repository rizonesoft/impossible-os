"""scripts/bounded_run.py: a bounded command never outlives the call, its stderr tail is exact, and a
cancellation at any moment reaches the caller's own handler."""
import os
import signal
import subprocess
import sys
import textwrap
import threading
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "scripts"))
import bounded_run


def _gone(pid, within=5.0):
    deadline = time.monotonic() + within
    while time.monotonic() < deadline:
        try:
            state = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0]
        except (FileNotFoundError, ProcessLookupError):      # gone before the open, or during the read (ESRCH)
            return True
        if state in ("Z", "X"):
            return True
        time.sleep(0.05)
    return False


def _reap(pidfile):
    try:
        os.kill(int(pidfile.read_text()), signal.SIGKILL)
    except (FileNotFoundError, ValueError, ProcessLookupError):   # never started, or already gone
        pass


def _helper(pidfile):
    """A shell line that starts a helper holding the inherited pipes, recorded whole or not at all."""
    return f"sh -c 'echo $$ > \"{pidfile}.part\" && mv \"{pidfile}.part\" \"{pidfile}\"; exec sleep 61' &"


def test_the_stderr_tail_is_exactly_the_last_bytes_written(monkeypatch):
    monkeypatch.setattr(bounded_run, "STDERR_KEEP", 100)
    one = 'import sys; sys.stderr.buffer.write(b"a" * 1000 + b"END"); sys.stderr.flush()'
    rc, _out, err = bounded_run.run([sys.executable, "-c", one], "one write", timeout=30, keep_stderr=True)
    assert rc == 0 and len(err) == 100 and err.endswith(b"END") and set(err[:-3]) == {ord("a")}, (len(err), err[:20])
    many = ('import sys\nfor i in range(300):\n    sys.stderr.buffer.write(b"%03d|" % i); sys.stderr.flush()\n')
    rc, _out, err = bounded_run.run([sys.executable, "-c", many], "many writes", timeout=30, keep_stderr=True)
    want = b"".join(b"%03d|" % i for i in range(300))[-100:]
    assert rc == 0 and err == want, err


def test_a_cancellation_before_the_first_reader_starts_reaches_the_caller(monkeypatch):
    class Boom(Exception):
        """The caller's own SIGTERM handler ran."""

    def caller(_signum, _frame):
        raise Boom()
    real_start, calls = threading.Thread.start, []

    def start(self):
        calls.append(self)
        if len(calls) == 1:
            signal.raise_signal(signal.SIGTERM)           # lands before the thread is started
        return real_start(self)
    monkeypatch.setattr(threading.Thread, "start", start)
    before = signal.signal(signal.SIGTERM, caller)
    try:
        with pytest.raises(Boom):
            bounded_run.run(["sleep", "30"], "a sleeper", timeout=30, keep_stderr=True)
        assert signal.getsignal(signal.SIGTERM) is caller
    finally:
        signal.signal(signal.SIGTERM, before)


_STREAM = textwrap.dedent("""
    import os, signal, sys, time
    sys.path.insert(0, {scripts!r})
    import staged_dash_check as m
    m.TIMEOUT = {timeout}
    reader = m.BlobReader()
    open({ready!r}, "w").close()
    began = time.monotonic()
    try:
        reader.count("a" * 40)
    except ValueError as exc:
        print("refused", round(time.monotonic() - began, 1), exc)
        reader.abort()
""")


def _fake_cat_file(tmp_path, pidfile):
    """git whose cat-file never answers and leaves a helper holding its stdout."""
    import shutil
    bindir = tmp_path / "bin"
    bindir.mkdir()
    real = shutil.which("git")
    (bindir / "git").write_text(f'#!/bin/sh\ncase " $* " in *" cat-file "*) {_helper(pidfile)}\n wait; exit 0 ;; esac\n'
                                f'exec "{real}" "$@"\n')
    (bindir / "git").chmod(0o755)
    return {**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}"}


def test_the_blob_reader_ends_a_helper_holding_its_output_at_the_deadline(tmp_path):
    pidfile, ready = tmp_path / "helper.pid", tmp_path / "ready"
    env = _fake_cat_file(tmp_path, pidfile)
    script = _STREAM.format(scripts=str(REPO / "scripts"), timeout=1, ready=str(ready))
    try:
        done = subprocess.run([sys.executable, "-c", script], capture_output=True, text=True, timeout=20, env=env)
        assert done.returncode == 0 and done.stdout.startswith("refused"), done
        assert float(done.stdout.split()[1]) < 4, done.stdout
        assert pidfile.exists() and _gone(int(pidfile.read_text())), "the helper outlived the reader"
    finally:
        _reap(pidfile)


def test_a_signal_while_the_blob_reader_waits_ends_its_helper_too(tmp_path):
    pidfile, ready = tmp_path / "helper.pid", tmp_path / "ready"
    env = _fake_cat_file(tmp_path, pidfile)
    script = _STREAM.format(scripts=str(REPO / "scripts"), timeout=60, ready=str(ready))
    proc = subprocess.Popen([sys.executable, "-c", script], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    try:
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and not (ready.exists() and pidfile.exists()):
            time.sleep(0.05)
        assert ready.exists() and pidfile.exists(), "the reader never started"
        proc.send_signal(signal.SIGTERM)
        proc.wait(timeout=10)
        assert proc.returncode == -signal.SIGTERM, proc.returncode   # the caller's own (default) handler ran
        assert _gone(int(pidfile.read_text())), "the helper outlived the reader"
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        _reap(pidfile)


def test_a_failed_command_leaves_no_helper_of_its_own(tmp_path):
    pidfile = tmp_path / "helper.pid"
    cmd = ["sh", "-c", f"{_helper(pidfile)[:-1]} </dev/null >/dev/null 2>&1 &\n"
                       f"while [ ! -e \"{pidfile}\" ]; do :; done; exit 1"]
    try:
        rc, _out, _err = bounded_run.run(cmd, "a failing command", timeout=30, keep_stderr=True)
        assert rc == 1 and _gone(int(pidfile.read_text())), "the failed command's helper outlived it"
    finally:
        _reap(pidfile)


def test_a_successful_command_leaves_a_background_helper_with_its_own_streams(tmp_path):
    """Decided: after success a helper git leaves running with its own streams (the credential cache) is git's."""
    pidfile = tmp_path / "helper.pid"
    cmd = ["sh", "-c", f"{_helper(pidfile)[:-1]} </dev/null >/dev/null 2>&1 &\n"
                       f"while [ ! -e \"{pidfile}\" ]; do :; done; exit 0"]
    try:
        rc, _out, _err = bounded_run.run(cmd, "a succeeding command", timeout=30)
        assert rc == 0 and not _gone(int(pidfile.read_text()), within=0.3)
    finally:
        _reap(pidfile)


def test_the_wait_takes_the_time_already_spent_from_the_deadline(monkeypatch):
    real_start, calls = threading.Thread.start, []

    def start(self):
        calls.append(self)
        if len(calls) == 1:
            time.sleep(1.0)                               # a slow start eats into the one budget
        return real_start(self)
    monkeypatch.setattr(threading.Thread, "start", start)
    began = time.monotonic()
    with pytest.raises(ValueError, match="did not answer within 1.5 s"):
        bounded_run.run(["sleep", "30"], "a sleeper", timeout=1.5)
    assert time.monotonic() - began < 2.3


def test_closing_a_stream_waits_for_a_helper_holding_its_output(tmp_path):
    pidfile = tmp_path / "helper.pid"
    stream = bounded_run.Stream(["sh", "-c", f"{_helper(pidfile)}\nwhile [ ! -e \"{pidfile}\" ]; do :; done; exit 0"],
                                "a stream", timeout=1)
    try:
        began = time.monotonic()
        with pytest.raises(ValueError, match="did not finish within 1 s"):
            stream.close()
        assert time.monotonic() - began < 3 and _gone(int(pidfile.read_text()))
    finally:
        _reap(pidfile)


def test_a_signal_while_a_stream_closes_reaches_the_caller_at_once(tmp_path):
    ready = tmp_path / "ready"
    script = textwrap.dedent(f"""
        import sys
        sys.path.insert(0, {str(REPO / "scripts")!r})
        import bounded_run
        stream = bounded_run.Stream(["sleep", "61"], "a sleeper", timeout=60)
        open({str(ready)!r}, "w").write(str(stream.proc.pid))
        stream.close()
    """)
    proc = subprocess.Popen([sys.executable, "-c", script])
    try:
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and not ready.exists():
            time.sleep(0.05)
        time.sleep(0.5)                                   # inside close() by now
        began = time.monotonic()
        proc.send_signal(signal.SIGTERM)
        proc.wait(timeout=10)
        assert proc.returncode == -signal.SIGTERM and time.monotonic() - began < 3
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        if ready.exists() and ready.read_text():              # the sleeper's own group, whatever happened
            try:
                os.killpg(int(ready.read_text()), signal.SIGKILL)
            except ProcessLookupError:
                pass


def test_a_stream_never_signals_its_group_after_the_leader_is_reaped(monkeypatch):
    """A timer callback already running at teardown, and a second abort, both come before the reap or not at all."""
    late = []
    real_kill = bounded_run._kill_group

    def spy(proc):
        late.append(proc.returncode is not None)             # True: the id was already released
        real_kill(proc)
    monkeypatch.setattr(bounded_run, "_kill_group", spy)
    gate = threading.Event()
    real_expire = bounded_run.Stream._expire

    def paused(self):
        gate.wait(10)                                         # the callback has started but not yet signalled
        real_expire(self)
    monkeypatch.setattr(bounded_run.Stream, "_expire", paused)
    stream = bounded_run.Stream(["sleep", "30"], "a sleeper", timeout=0.2)
    time.sleep(0.5)                                           # the timer has fired and is waiting on the gate
    threading.Timer(0.5, gate.set).start()
    stream.abort()
    stream.abort()                                            # Ctrl-C through the dash check aborts a second time
    assert late and not any(late), late


def test_the_exit_wait_does_not_spend_a_slow_start_twice(monkeypatch):
    real_start = threading.Thread.start

    def start(self):
        if getattr(self, "_target", None) is not None and self._target.__name__ == "wait":
            time.sleep(1.0)                                   # the exit waiter itself starts late
        return real_start(self)
    monkeypatch.setattr(threading.Thread, "start", start)
    began = time.monotonic()
    with pytest.raises(ValueError, match="did not answer within 1.5 s"):
        bounded_run.run(["sleep", "30"], "a sleeper", timeout=1.5)
    assert time.monotonic() - began < 2.2


def _slow_popen(monkeypatch, delay):
    real = subprocess.Popen

    def popen(*a, **k):
        time.sleep(delay)
        return real(*a, **k)
    monkeypatch.setattr(bounded_run.subprocess, "Popen", popen)


def test_a_slow_start_is_spent_from_the_deadline(monkeypatch):
    _slow_popen(monkeypatch, 0.3)
    with pytest.raises(ValueError, match="did not answer within 0.1 s"):
        bounded_run.run(["true"], "a quick command", timeout=0.1)


def test_a_slow_start_is_spent_from_a_streams_deadline(monkeypatch):
    _slow_popen(monkeypatch, 0.4)
    with pytest.raises(ValueError, match="did not start within 0.2 s"):
        bounded_run.Stream(["cat"], "a stream", timeout=0.2)


def test_an_exit_seen_only_after_the_deadline_is_late(monkeypatch):
    real = bounded_run._exit_status

    def late(proc, deadline):
        rc = real(proc, deadline)
        time.sleep(0.3)                                    # the caller is descheduled after the exit
        return rc
    monkeypatch.setattr(bounded_run, "_exit_status", late)
    with pytest.raises(ValueError, match="did not answer"):
        bounded_run.run(["sleep", "0.05"], "a short sleep", timeout=0.15)


def test_a_watchdog_started_late_still_ends_a_stream_at_its_deadline(monkeypatch):
    def delayed(run):
        def go(self):
            time.sleep(0.3)
            return run(self)
        return go
    monkeypatch.setattr(threading.Timer, "run", delayed(threading.Timer.run))
    if hasattr(bounded_run, "_AtDeadline"):
        monkeypatch.setattr(bounded_run._AtDeadline, "run", delayed(bounded_run._AtDeadline.run))
    began = time.monotonic()
    stream = bounded_run.Stream(["sleep", "30"], "a sleeper", timeout=0.2)
    try:
        with pytest.raises(ValueError, match="did not answer"):
            stream.read(1)                                 # ended at the deadline, not answered
        took = time.monotonic() - began
    finally:
        stream.abort()
    assert took < 0.45, took


def test_a_stream_closed_after_its_deadline_is_late_before_its_watchdog_runs(monkeypatch):
    run = bounded_run._AtDeadline.run

    def late(self):
        time.sleep(0.5)
        return run(self)
    monkeypatch.setattr(bounded_run._AtDeadline, "run", late)
    stream = bounded_run.Stream(["true"], "a quick command", timeout=0.1)
    time.sleep(0.2)                                        # the command is done; the caller resumes late
    with pytest.raises(ValueError, match="did not finish"):
        stream.close()


def test_a_leader_that_does_not_go_after_its_kill_is_reported_and_never_signalled_again(monkeypatch):
    monkeypatch.setattr(bounded_run, "REAP_LIMIT", 0.2)
    real_wait = subprocess.Popen.wait

    def stuck(self, timeout=None):                         # as a leader held in uninterruptible I/O
        if timeout is not None:
            raise subprocess.TimeoutExpired(self.args, timeout)
        return real_wait(self)
    monkeypatch.setattr(subprocess.Popen, "wait", stuck)
    signalled, real_killpg = [], os.killpg
    monkeypatch.setattr(bounded_run.os, "killpg", lambda pgid, sig: (signalled.append(pgid), real_killpg(pgid, sig)))
    began = time.monotonic()
    with pytest.raises(ValueError, match="did not end within"):
        bounded_run.run(["sleep", "30"], "a stuck command", timeout=0.2)
    assert time.monotonic() - began < 2
    calls = len(signalled)
    proc = type("P", (), {"pid": 1, "abandoned": True})()
    bounded_run._kill_group(proc)
    assert len(signalled) == calls


def _escaped_cat_file(tmp_path, pidfile):
    """git whose cat-file never answers and leaves a helper in a session of its own holding its stdout, out
    of reach of the group's end."""
    import shutil
    bindir = tmp_path / "bin"
    bindir.mkdir()
    real = shutil.which("git")
    (bindir / "git").write_text(f'#!/bin/sh\ncase " $* " in *" cat-file "*) setsid {_helper(pidfile)[:-1]} 2>/dev/null &\n'
                                f' while [ ! -e "{pidfile}" ]; do :; done; exit 0 ;; esac\nexec "{real}" "$@"\n')
    (bindir / "git").chmod(0o755)
    return {**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}"}


def test_a_read_held_open_outside_the_group_ends_by_the_deadline(tmp_path):
    pidfile, ready = tmp_path / "helper.pid", tmp_path / "ready"
    env = _escaped_cat_file(tmp_path, pidfile)
    script = _STREAM.format(scripts=str(REPO / "scripts"), timeout=1, ready=str(ready))
    try:
        done = subprocess.run([sys.executable, "-c", script], capture_output=True, text=True, timeout=20, env=env)
        assert done.returncode == 0 and done.stdout.startswith("refused"), done
        assert float(done.stdout.split()[1]) < 2.5, done.stdout
    finally:
        _reap(pidfile)


def test_closing_a_stream_held_open_outside_the_group_ends_by_the_deadline(tmp_path):
    pidfile = tmp_path / "helper.pid"
    script = textwrap.dedent(f"""
        import sys, time
        sys.path.insert(0, {str(REPO / "scripts")!r})
        import bounded_run
        stream = bounded_run.Stream(["sh", "-c", {f"setsid {_helper(pidfile)[:-1]} 2>/dev/null & while [ ! -e '{pidfile}' ]; do :; done; exit 0"!r}],
                                    "a stream", timeout=1)
        began = time.monotonic()
        try:
            stream.close()
        except ValueError as exc:
            print("refused", round(time.monotonic() - began, 1), exc)
    """)
    try:
        done = subprocess.run([sys.executable, "-c", script], capture_output=True, text=True, timeout=20)
        assert done.stdout.startswith("refused") and float(done.stdout.split()[1]) < 2.5, done
    finally:
        _reap(pidfile)


def test_a_hangup_ends_the_command_group_and_reaches_the_caller(tmp_path):
    pidfile = tmp_path / "sleeper.pid"
    script = textwrap.dedent(f"""
        import sys
        sys.path.insert(0, {str(REPO / "scripts")!r})
        import bounded_run
        bounded_run.run(["sh", "-c", {f"echo $$ > '{pidfile}.part' && mv '{pidfile}.part' '{pidfile}'; exec sleep 61"!r}],
                        "a sleeper", timeout=60)
    """)
    proc = subprocess.Popen([sys.executable, "-c", script], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and not pidfile.exists():
            time.sleep(0.05)
        assert pidfile.exists(), "the command never started"
        proc.send_signal(signal.SIGHUP)
        proc.wait(timeout=10)
        assert proc.returncode == -signal.SIGHUP, proc.returncode
        assert _gone(int(pidfile.read_text())), "the command outlived its caller's hangup"
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        _reap(pidfile)


def _stalled_start(monkeypatch, tmp_path, before_fork):
    """A start that stalls for a second, before its fork or after it (an exec held by failing storage). The
    command started through the gate is replaced by one whose first act is to create the marker file, so
    the marker records whether the command ever ran."""
    real = subprocess.Popen
    marker = tmp_path / "ran"

    def popen(argv, *a, **k):
        argv = [*argv[:5], "sh", "-c", f": > {marker}"]  # the gate and its descriptor, then the command
        if before_fork:
            time.sleep(1)
        proc = real(argv, *a, **k)
        time.sleep(0.5 if before_fork else 1)          # an ungated command has acted by the time it returns
        return proc
    monkeypatch.setattr(bounded_run.subprocess, "Popen", popen)
    return marker


@pytest.mark.parametrize("before_fork", [True, False])
@pytest.mark.parametrize("kind", ["run", "stream"])
def test_a_start_that_stalls_returns_by_the_deadline_and_never_runs_late(monkeypatch, tmp_path, kind, before_fork):
    """The pre-push dash check starts git through these: a start held before its fork or in its exec returns
    control by the deadline, and the command never runs once the call has given up on it."""
    marker = _stalled_start(monkeypatch, tmp_path, before_fork)
    began = time.monotonic()
    with pytest.raises(ValueError, match="within 0.3 s"):
        if kind == "run":
            bounded_run.run(["git", "status"], "git status", timeout=0.3)
        else:
            bounded_run.Stream(["git", "cat-file", "--batch"], "git cat-file", timeout=0.3)
    assert time.monotonic() - began < 0.3 + 0.5
    time.sleep(1.5)
    assert not marker.exists(), "the stalled start's command ran after the call had returned"


def test_a_cancellation_during_a_stalled_start_ends_it_at_once(monkeypatch, tmp_path):
    """SIGTERM while the start is held: the caller's own handler gets it promptly, and nothing runs."""
    marker = _stalled_start(monkeypatch, tmp_path, before_fork=False)
    got = []
    old = signal.signal(signal.SIGTERM, lambda s, f: got.append(s))
    threading.Timer(0.2, os.kill, (os.getpid(), signal.SIGTERM)).start()
    began = time.monotonic()
    try:
        with pytest.raises(SystemExit):
            bounded_run.run(["git", "status"], "git status", timeout=20)
    finally:
        signal.signal(signal.SIGTERM, old)
    assert got == [signal.SIGTERM] and time.monotonic() - began < 1
    time.sleep(1.5)
    assert not marker.exists()


@pytest.mark.parametrize("why", ["deadline", "stop"])
def test_a_start_that_returned_while_its_owner_slept_is_given_up_on(why):
    """The launch thread finished, then the owner resumed past its deadline (or after a stop): the result is
    not accepted, so its gate stays closed, and it is handed to the abandon path exactly once."""
    abandoned = []
    began = time.monotonic()
    real_join = threading.Thread.join

    def late_join(self, timeout=None):
        real_join(self)                                # the launch thread has returned its result
        time.sleep(0.3)                                # ... and the owner is descheduled past its deadline
    threading.Thread.join = late_join
    try:
        got = bounded_run.start_supervised(lambda: 42, began + (0.1 if why == "deadline" else 30),
                                           (lambda: why == "stop"), abandoned.append)
    finally:
        threading.Thread.join = real_join
    assert got == (False, None) and abandoned == [42], (got, abandoned)


def test_a_helper_holding_both_pipes_never_delays_the_timeout():
    """A command times out while a helper in a session of its own holds stdout and stderr for 2 s: run()
    reports the timeout within a fraction of a second of its deadline, not after the helper lets go."""
    import bounded_run
    began = time.monotonic()
    with pytest.raises(ValueError):
        bounded_run.run(["bash", "-c", "setsid sleep 2 & sleep 10"], "the helper test", timeout=0.25,
                        keep_stderr=True)
    assert time.monotonic() - began < 1.0, time.monotonic() - began
