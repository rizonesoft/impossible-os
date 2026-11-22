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
        open({str(ready)!r}, "w").close()
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
