#!/usr/bin/env python3
"""Run a command bounded in time, as one process group that never outlives the call.

Every git command the review candidate reader (scripts/review_candidate.py) and the dash check
(scripts/staged_dash_check.py) start goes through `run`, or `Stream` for the one command the dash check talks
to as it goes (git cat-file --batch). git starts helpers of its own (a clean filter, a filesystem monitor
hook, ssh, a credential helper), and a helper inherits git's pipes: ending git alone on a timeout leaves the
helper holding them, and a reader waiting for their end never returns. So the command runs in its own process
group, and on a timeout, on Ctrl-C and on SIGTERM the whole group is ended before the pipes are waited on
again. `run` reads the output in threads it waits on against one deadline.

The signals are caught before the group starts and held until it is registered, so no moment leaves it
running unowned; a cancellation then ends the group and is delivered again to the caller's own handler. A
timeout is a ValueError naming `what`, never the command, since a command line can carry a URL with a token.

The leader is waited on without being reaped (waitid with WNOWAIT), so its id still names the group when the
group is ended: a timeout, a cancellation and a failed exit all end every member, and no id can have been
reused in between.

Out of scope by decision: a helper that leaves the group on its own (setsid) is outside what a process group
can end; and after a successful exit the call returns once the command's pipes have closed, so a helper git
leaves running in the background with its own streams (the credential cache daemon, by design) is git's, and
is not ended.
"""
import collections
import fcntl
import os
import selectors
import signal
import subprocess
import threading
import time

CHUNK = 1 << 20
STDERR_KEEP = 1 << 16        # the end of a long stderr is what names the failure
GRACE = 5                    # seconds to wait for the pipes to close once the group is ended
REAP_LIMIT = 5               # seconds a KILLed leader may take to exit (uninterruptible I/O) before it is reported


class Cancelled(BaseException):
    """Ctrl-C or SIGTERM reached the caller while a bounded command ran."""


class _Watch:
    """The caller's SIGTERM (and SIGINT, unless the caller ignores it) replaced for one command's life. While
    `hold` is set a signal is recorded and delivered later; otherwise `live(signum)` is called."""

    def __init__(self, live):
        self.hold, self.pending, self.live, self.done = True, 0, live, False
        self.watched = [signal.SIGTERM]
        if signal.getsignal(signal.SIGINT) is signal.default_int_handler:
            self.watched.append(signal.SIGINT)             # an ignored SIGINT (a background caller) stays ignored
        if signal.getsignal(signal.SIGHUP) is not signal.SIG_IGN:
            self.watched.append(signal.SIGHUP)             # the terminal went away: as a cancellation, unless ignored
        self.previous = {sig: signal.signal(sig, self._on) for sig in self.watched}

    def _on(self, signum, _frame):
        if self.hold:
            self.pending = self.pending or signum          # not registered, or being ended: deliver later
            return
        self.live(signum)

    def finish(self, signum=0):
        """Put the caller's handlers back with the watched signals blocked, so none that arrives meanwhile is
        recorded here and lost; one held by the block reaches the caller's handler as the mask lifts, and one
        recorded earlier (or this cancellation) is delivered again. Only the first call acts."""
        if self.done:
            return
        self.done = True
        old = signal.pthread_sigmask(signal.SIG_BLOCK, self.watched)
        try:
            for sig, handler in self.previous.items():
                signal.signal(sig, handler)
            signum = signum or self.pending
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, old)
        if signum:
            signal.raise_signal(signum)
            raise SystemExit(128 + signum)                 # the caller's handler returned: still a cancellation


def _kill_group(proc):
    if getattr(proc, "abandoned", False):                  # its id may be released and reused: never again
        return
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:                             # the group has already exited: nothing to end
        pass


def _end_group(proc, what="the command"):
    _kill_group(proc)
    _reap(proc, what)


def _reap(proc, what):
    """Reap the leader within REAP_LIMIT. One the kernel keeps in uninterruptible I/O past it is reported
    and abandoned: never signalled again (subprocess reaps it later, and its id is then free for reuse)."""
    try:
        proc.wait(timeout=REAP_LIMIT)
    except subprocess.TimeoutExpired:
        proc.abandoned = True
        raise ValueError(f"{what} did not end within {REAP_LIMIT} s of being killed "
                         f"(uninterruptible I/O?)") from None


def _exit_status(proc, deadline):
    """The leader's exit status once it exits, read without reaping it, or None at `deadline` (a monotonic
    time). The wait runs in a thread so the caller's join stays open to signals; the time left is read after
    that thread has started, so a slow start is not spent twice."""
    box = []

    def wait():
        try:
            info = os.waitid(os.P_PID, proc.pid, os.WEXITED | os.WNOWAIT)
        except ChildProcessError:                          # reaped already: the group was ended meanwhile
            return
        box.append(info.si_status if info.si_code == os.CLD_EXITED else -info.si_status)
    t = threading.Thread(target=wait, daemon=True)
    t.start()
    t.join(max(0.0, deadline - time.monotonic()))
    return box[0] if box else None


_NOTHING = object()


def start_supervised(spawn, deadline, stopped, abandon=None):
    """(True, `spawn()`) once the start returns within `deadline` (a monotonic time) and before `stopped()`,
    else (False, None) at once. A start can stall: a spawn waits until its child has executed the program,
    and loading it from failing storage holds that wait. So it runs in a launch thread the caller never
    waits on past the deadline. What the caller relies on is the gate every user of this keeps: the started
    process cannot act until its owner, still within the deadline, releases it (a pipe it reads first, or its
    input), so a start that is given up on runs nothing. A start that returns after it was given up on is
    handed to `abandon` (by whichever of the two threads sees it last); a start that fails after that is
    dropped, its caller has gone."""
    box, lock = {}, threading.Lock()

    def go():
        try:
            result = spawn()
        except BaseException as exc:                       # noqa: BLE001 -- handed to the caller's thread
            with lock:
                box["exc"] = exc
            return
        with lock:
            late = box.get("late", False)
            if not late:
                box["result"] = result
        if late and abandon is not None:
            abandon(result)
    t = threading.Thread(target=go, daemon=True)
    t.start()
    while True:
        t.join(min(0.05, max(0.0, deadline - time.monotonic())))
        if not t.is_alive() or stopped() or time.monotonic() >= deadline:
            break
    # The decision is made here, under the lock, against the clock and the stop as they are now: a start that
    # returned while this caller was descheduled past its deadline, or after a stop, is given up on like one
    # still held, so its gate is never released; it is handed to `abandon` here, once.
    with lock:
        authorised = time.monotonic() < deadline and not stopped()
        if "result" in box and authorised:
            return True, box["result"]
        if "exc" in box and authorised:
            raise box["exc"]
        box["late"] = True
        returned = box.get("result", _NOTHING)
    if returned is not _NOTHING and abandon is not None:
        abandon(returned)
    return False, None


# The gate a command passes before it runs (bash, since dash takes only one-digit descriptors in a redirection): one line, "go", on the pipe whose number is the first argument,
# then that pipe is closed and the command executed in this process. End of file or anything else runs
# nothing (exit 125).
_GATE = 'g=$1; shift; IFS= read -r go <&"$g" || exit 125; [ "$go" = go ] || exit 125; eval "exec $g<&-"; exec "$@"'


def _launch(cmd, popen, deadline, watch, what, timeout):
    """`cmd` started with `popen(argv, pass_fds)` through the gate, supervised by start_supervised: the gate
    is released only once the start returned in time and is this call's own, so a start that stalled (before
    its fork or in its exec) never runs the command after the call has given up on it. A start that returns
    late is ended and reaped in the launch thread."""
    low, gate_wr = os.pipe()
    # Above the standard streams: with one of them closed in this process the pipe can take its number, and
    # the child's own stream would then be duplicated over the gate.
    gate_rd = fcntl.fcntl(low, fcntl.F_DUPFD_CLOEXEC, 3)
    os.close(low)

    def spawn():
        try:
            return popen(["/bin/bash", "-c", _GATE, "gate", str(gate_rd), *cmd], (gate_rd,))
        finally:
            os.close(gate_rd)                              # the child has its copy; a failed start, none

    def abandon(proc):
        _kill_group(proc)
        try:
            proc.wait(timeout=REAP_LIMIT)
        except subprocess.TimeoutExpired:
            pass                                           # fail-direction: subprocess reaps it when it exits
    try:
        started, proc = start_supervised(spawn, deadline, lambda: bool(watch.pending), abandon)
        if not started:
            if watch.pending:
                raise Cancelled(watch.pending)
            raise subprocess.TimeoutExpired(what, timeout)
        try:
            os.write(gate_wr, b"go\n")                    # a few bytes to an empty pipe: never blocks
        except OSError:
            pass                                           # fail-direction: the gate exited; its status says so
        return proc
    finally:
        os.close(gate_wr)


def _pump(stream, put, errors):
    try:
        while chunk := stream.read1(CHUNK):
            put(chunk)
    except BaseException as exc:                           # handed to the caller, never lost in the thread
        errors.append(exc)
    finally:
        stream.close()


def _feed(stream, data, errors):
    try:
        stream.write(data)
    except BrokenPipeError:                                # the command stopped reading: its status says why
        pass
    except BaseException as exc:
        errors.append(exc)
    finally:
        try:
            stream.close()
        except BrokenPipeError:                            # the unflushed rest had no reader: as above
            pass


class _Tail:
    """The last STDERR_KEEP bytes written, exactly, kept in the chunks they arrived in."""

    def __init__(self):
        self.chunks, self.size = collections.deque(), 0

    def put(self, chunk):
        self.chunks.append(chunk)
        self.size += len(chunk)
        while self.size > STDERR_KEEP:
            excess = self.size - STDERR_KEEP
            if len(self.chunks[0]) <= excess:
                self.size -= len(self.chunks.popleft())
            else:
                self.chunks[0] = self.chunks[0][excess:]
                self.size -= excess

    def value(self):
        return b"".join(self.chunks)


def run(cmd, what, *, timeout, env=None, stdin=None, sink=None, keep_stderr=False):
    """(exit status, stdout, stderr) of `cmd`. stdout is handed to `sink` chunk by chunk when one is given
    (and returned empty), else returned whole; stderr is kept (its last STDERR_KEEP bytes) only when
    `keep_stderr`, else discarded. `stdin` is bytes to write, or None for no input."""

    def cancel(signum):
        raise Cancelled(signum)

    watch = _Watch(cancel)
    proc, out, err, errors, threads = None, [], _Tail(), [], []
    deadline = time.monotonic() + timeout                  # before the start: a slow start is spent from it
    try:
        proc = _launch(cmd, lambda argv, fds: subprocess.Popen(
            argv, stdin=subprocess.DEVNULL if stdin is None else subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE if keep_stderr else subprocess.DEVNULL, env=env, pass_fds=fds,
            start_new_session=True),  # LIFECYCLE-WAIVER: waited on here; on timeout or cancellation its whole group is ended
            deadline, watch, what, timeout)
        watch.hold = False
        if watch.pending:
            raise Cancelled(watch.pending)
        if time.monotonic() >= deadline:
            raise subprocess.TimeoutExpired(what, timeout)
        threads.append(threading.Thread(target=_pump, args=(proc.stdout, sink or out.append, errors), daemon=True))
        if keep_stderr:
            threads.append(threading.Thread(target=_pump, args=(proc.stderr, err.put, errors), daemon=True))
        if stdin is not None:
            threads.append(threading.Thread(target=_feed, args=(proc.stdin, stdin, errors), daemon=True))
        for t in threads:
            t.start()
        rc = _exit_status(proc, deadline)
        if rc is None:
            raise subprocess.TimeoutExpired(what, timeout)
        if rc != 0:
            _kill_group(proc)                              # a failure leaves no helper of its own behind
        for t in threads:
            t.join(max(0.0, deadline - time.monotonic()))
            if t.is_alive():                               # a helper still holds a pipe at the deadline
                raise subprocess.TimeoutExpired(what, timeout)
        if time.monotonic() >= deadline:                   # seen only after it (this caller was descheduled)
            raise subprocess.TimeoutExpired(what, timeout)
        watch.hold = True
        proc.wait()                                        # reaped only now: the id named the group until here
    except BaseException as exc:
        watch.hold = True
        try:
            if proc is not None:
                _end_group(proc, what)
                for t in threads:
                    if t.ident is not None:                # started: a cancellation can land before a start
                        t.join(GRACE)                      # the pipes close with the group
        finally:
            watch.finish(exc.args[0] if isinstance(exc, Cancelled) else 0)
        if isinstance(exc, subprocess.TimeoutExpired):
            raise ValueError(f"{what} did not answer within {timeout:g} s") from None
        raise
    watch.finish()
    if errors:
        raise errors[0]
    return rc, b"".join(out), err.value()


class _AtDeadline(threading.Thread):
    """Calls `fn` at an absolute monotonic deadline unless cancelled first. The wait is measured when the
    thread runs, so a watchdog the host starts late spends no extra time (a relative timer would)."""

    def __init__(self, deadline, fn):
        super().__init__(daemon=True)
        self.deadline, self.fn, self._cancelled = deadline, fn, threading.Event()

    def cancel(self):
        self._cancelled.set()

    def run(self):
        while not self._cancelled.is_set():
            left = self.deadline - time.monotonic()
            if left <= 0:
                self.fn()
                return
            self._cancelled.wait(left)


class Stream:
    """A command the caller writes to (`write`) and reads from (`read`, `readline`) as it goes, in its own
    process group. Every read and write waits on its pipe only until the deadline, so one no process will
    answer (a process outside the group holding the pipe, or one the kernel keeps in uninterruptible I/O
    after it was ended) is an error by it; a timer ends the whole group at the deadline. Ctrl-C, SIGTERM and
    SIGHUP end the group at once and then reach the caller's own handler, whenever they arrive, `close`
    included. `close` ends input, waits for the exit and for the output's end within the same deadline;
    `abort` ends the group.

    Every way out goes through one teardown, which acts once: the timer is cancelled and joined while the
    leader is still unreaped, so neither it nor a second abort can signal a group id after its release."""

    def __init__(self, cmd, what, *, timeout, env=None):
        self.what, self.timeout, self.expired = what, timeout, False
        self.proc, self._timer, self._done = None, None, False
        self._watch = _Watch(self._cancel)
        self._deadline = time.monotonic() + timeout        # before the start: a slow start is spent from it
        try:
            self.proc = _launch(cmd, lambda argv, fds: subprocess.Popen(
                argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env, pass_fds=fds,
                start_new_session=True),  # LIFECYCLE-WAIVER: ended by the timer, close() or abort(), group and all
                self._deadline, self._watch, what, timeout)
            self._in, self._out, self._buf = self.proc.stdin.fileno(), self.proc.stdout.fileno(), bytearray()
            os.set_blocking(self._in, False)
            os.set_blocking(self._out, False)
            self._timer = _AtDeadline(self._deadline, self._expire)
            self._timer.start()
            self._watch.hold = False
            pending = self._watch.pending
        except BaseException as exc:
            if self.proc is not None:
                self._teardown(kill=True)
            self._watch.finish(exc.args[0] if isinstance(exc, Cancelled) else 0)
            if isinstance(exc, subprocess.TimeoutExpired):
                raise ValueError(f"{what} did not start within {timeout:g} s") from None
            raise
        if pending:
            self._cancel(pending)

    def _ready(self, fd, events):
        """Wait until `fd` is ready or the deadline; at the deadline the group is ended and this is an error."""
        with selectors.DefaultSelector() as sel:
            sel.register(fd, events)
            left = self._deadline - time.monotonic()
            if left > 0 and sel.select(left):
                return
        self.expired = True
        self.abort()
        raise ValueError(f"{self.what} did not answer within {self.timeout:g} s")

    def _fill(self) -> bool:
        """More output into the buffer; False at its end."""
        while True:
            try:
                chunk = os.read(self._out, CHUNK)
            except BlockingIOError:
                self._ready(self._out, selectors.EVENT_READ)
                continue
            if not chunk:
                return False
            self._buf += chunk
            return True

    def read(self, n: int) -> bytes:
        """Up to `n` bytes of output, fewer only at its end."""
        while len(self._buf) < n and self._fill():
            pass
        out = bytes(self._buf[:n])
        del self._buf[:n]
        return out

    def readline(self) -> bytes:
        """The next line of output with its newline, or what is left at its end."""
        start = 0
        while (i := self._buf.find(b"\n", start)) < 0:
            start = len(self._buf)
            if not self._fill():
                i = len(self._buf) - 1
                break
        out = bytes(self._buf[:i + 1])
        del self._buf[:i + 1]
        return out

    def write(self, data: bytes) -> None:
        """All of `data` to the command's input; a command that stopped reading is an error."""
        view = memoryview(data)
        while view:
            try:
                view = view[os.write(self._in, view):]
            except BlockingIOError:
                self._ready(self._in, selectors.EVENT_WRITE)
            except BrokenPipeError:
                raise ValueError(f"{self.what} stopped reading its input") from None

    def _expire(self):
        self.expired = True
        _kill_group(self.proc)                             # the leader is unreaped: teardown joins this first

    def _teardown(self, kill):
        """Disarm the timer and wait for a callback already running, end the group when `kill`, then reap the
        leader. Only the first call acts."""
        if self._done:
            return
        self._done = True
        if self._timer is not None:
            self._timer.cancel()
            if self._timer.ident is not None:
                self._timer.join()
        if kill:
            _kill_group(self.proc)
        _reap(self.proc, self.what)

    def _stop(self, signum=0):
        """End the group, reap it and give the caller its handlers back (delivering `signum`)."""
        self._watch.hold = True
        try:
            self._teardown(kill=True)
        finally:
            self._watch.finish(signum)

    def _cancel(self, signum):
        self._stop(signum)

    def abort(self) -> None:
        """End the group at once: after a failed read its output is never consumed, so a graceful close would
        wait for a producer that cannot finish."""
        self._stop()

    def close(self) -> int:
        try:
            self.proc.stdin.close()
        except OSError:
            pass                                           # fail-direction: the exit status below reports it
        rc = _exit_status(self.proc, self._deadline)
        if rc is None:
            self._stop()
            raise ValueError(f"{self.what} did not finish within {self.timeout:g} s")
        if rc != 0:
            _kill_group(self.proc)                         # a failure leaves no helper of its own behind
        try:
            while self._fill():                            # the end of output: every holder gone, or the deadline
                del self._buf[:]
        except ValueError:
            raise ValueError(f"{self.what} did not finish within {self.timeout:g} s") from None
        # Judged here, not only by the watchdog: one the host runs late is cancelled below before it can say.
        late = self.expired or time.monotonic() >= self._deadline
        self._watch.hold = True
        try:
            self._teardown(kill=late)                      # reaped only now, as in run()
        finally:
            self._watch.finish()
        if late:
            raise ValueError(f"{self.what} did not finish within {self.timeout:g} s")
        return rc
