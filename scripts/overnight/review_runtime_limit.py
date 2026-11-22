#!/usr/bin/env python3
"""review_runtime_limit.py [OPTIONS] SECONDS COMMAND | [OPTIONS] --until-cs UPTIME_CS COMMAND | --no-deadline COMMAND

Runs COMMAND (a bash command string) and ends the whole attempt within the limit: TERM to every process of
the attempt a grace before the end, KILL a margin before it, everything reaped by it. Exits with COMMAND's
status, 124 when the limit ended it (or it was seen ending only after the limit), or 128 + the signal when
this supervisor was cancelled (TERM, INT, HUP), which ends the attempt the same way.
scripts/overnight/review-runtime-limit.sh is the entry point.

THE ATTEMPT IS EVERY PROCESS DESCENDED FROM IT, and this supervisor is their child subreaper
(PR_SET_CHILD_SUBREAPER): a process whose parent exits is re-parented here instead of to init, so no step
can leave the attempt by starting its own session (git under the candidate reader does), by double-forking,
or by being started from a TERM handler that then exits. Whatever way COMMAND ends, every remaining
descendant is killed and reaped by the deadline, so a caller that publishes a status afterwards knows no
process of the attempt can still write to its log (one the kernel holds in uninterruptible I/O after KILL is
reported: it can finish only the call it is in, and a process with KILL pending cannot fork). Every signal
goes through a pidfd held for a process proved a member, never to a bare number a finished process may have
given up. Only a process handed to another supervisor (a service manager) leaves the tree.

--until-cs takes the caller's absolute deadline in hundredths of a second of /proc/uptime (CLOCK_BOOTTIME,
which a wall-clock step does not move), so time the caller spent before this started is not given back; a
deadline already passed, or a cancellation that came first, runs nothing.

THE REVIEW BROKER'S SUPERVISOR (--broker LOG PROMPT, with --publish-by FINAL_CS). The broker runs its whole
life under this, so one deadline and one cancellation cover every step and every moment between steps, and
this is the one place a result the broker owns is published. COMMAND tells it, through the inherited pipe
REVIEW_BROKER_HANDOFF_FD, which step it is in ("step <what>"), the answer it will give ("response <json>"),
and a job it hands over for a host with no service manager ("job <command>"). When COMMAND fails, is ended
by the limit or is cancelled after it said it owns the result ("owns", sent before it registers the attempt),
the result is published here (claimed, so a unit that already owns it keeps it) by review_result.py in a
child waited on only until FINAL_CS; before that only its prompt is removed. A job handed over is started here
only after the attempt's teardown, outside the tree, so the teardown cannot end it. Once COMMAND said
"answered", the caller has its answer and the result is the launched job's.

THE DETACHED JOB'S SUPERVISOR (--job LOG PROMPT [--prompt-sha DIGEST]). One supervisor for the whole job,
never one nested in another (whose grace would end the inner one before it could answer): it claims the
result (holding LOG.owner locked for its life, then creating LOG exclusively; a result another owner holds is
left alone and nothing runs), reads PROMPT once and checks it against DIGEST (passed to COMMAND as
REVIEW_PROMPT_FD), runs COMMAND (the review, its output appended to LOG) until the steps' deadline, and
publishes COMMAND's status, or 124 when the limit ended it, as soon as the filesystem allows: no caller
waits on this detached process. A publication that fails is this supervisor's exit 1. Every attempt runs
under review_warden.py, which ends its group when this supervisor ends, so a supervisor that dies leaves
nothing running; the verdict waiter then publishes its result as 1 (review_result.py recover).
"""
import ctypes
import os
import re
import select
import signal
import sys
import time

PR_SET_CHILD_SUBREAPER = 36
POLL = 0.1              # the limit is kept to a tenth of a second
NO_DEADLINE = 10 ** 12  # centiseconds: no caller limit (a supervisor still owns cancellation and the result)
HERE = os.path.dirname(os.path.abspath(__file__))
WARDEN = os.path.join(HERE, "review_warden.py")
_LIFELINES: list[int] = []     # write ends of the attempts' lifelines, open until this process ends


PIPE_BUF = 4096        # a write of at most this many bytes to a pipe that polls writable does not block


class Bounded:
    """A stream on fd 1 or 2 whose writes wait for room only until `until` (a second after a cancellation):
    a caller that stops reading a full pipe cannot hold this supervisor past its deadline. What could not
    be written by then is dropped, and so is everything after it."""

    def __init__(self, fd, until, cancel=None):
        self.fd, self.until, self.cancel, self.dead = fd, until, cancel, False

    def write(self, text):
        if not self.dead and not emit(self.fd, text.encode("utf-8", "replace"), self.until, self.cancel):
            self.dead = True
        return len(text)

    def flush(self):
        pass

    def isatty(self):
        return False


def _same_terminal(fd):
    """Whether reopening terminal `fd` through /proc reaches the endpoint it is (a slave, a console), not a
    new one (a PTY master's multiplexer)."""
    try:
        return os.path.basename(os.readlink(f"/proc/self/fd/{fd}")) != "ptmx"
    except OSError:
        return False


def _writer(fd):
    """A write function for `fd` that never blocks, and leaves the inherited descriptor's flags alone
    (another process shares them): a pipe or a terminal is reopened as a private nonblocking descriptor
    through /proc, and a socket is sent to with MSG_DONTWAIT. Returns (write, poll fd, close), or None for a
    descriptor no write to can be made nonblocking (a regular file, a device), which emit hands to a
    writer process instead."""
    import stat
    mode = os.fstat(fd).st_mode
    # A terminal is reopened only when the reopen names the same endpoint: the multiplexer behind a PTY
    # master (/dev/ptmx) would hand out a new master instead, so a master goes to the writer process.
    if stat.S_ISFIFO(mode) or (stat.S_ISCHR(mode) and os.isatty(fd) and _same_terminal(fd)):
        own = os.open(f"/proc/self/fd/{fd}", os.O_WRONLY | os.O_NONBLOCK | os.O_CLOEXEC)
        return (lambda view: os.write(own, view)), own, (lambda: os.close(own))
    if stat.S_ISSOCK(mode):
        import socket
        sock = socket.socket(fileno=os.dup(fd))
        return (lambda view: sock.send(view, socket.MSG_DONTWAIT)), fd, sock.close
    return None


def _emit_through_writer(fd, data, until, cancel, late=True):
    """`data` written to `fd` by a `cat` whose input is a pipe from here: what is waited on is that process,
    only until `until` (a second after a cancellation). A filesystem that stalls the write holds the writer,
    never this supervisor; the destination is not a pipe, so the writer holding it delays no reader's EOF."""
    rd, wr = os.pipe()
    try:
        # LIFECYCLE-WAIVER: writes one answer or diagnostic for a REVIEW attempt; it does not relaunch or watch the session.
        pid = os.posix_spawnp("cat", ["cat"], _child_env(), file_actions=[
            (os.POSIX_SPAWN_DUP2, rd, 0), (os.POSIX_SPAWN_DUP2, fd, 1),
            (os.POSIX_SPAWN_OPEN, 2, os.devnull, os.O_WRONLY, 0)])
    except OSError:
        os.close(rd)
        os.close(wr)
        return False
    os.close(rd)
    os.set_blocking(wr, False)
    sent = _emit(memoryview(data), lambda view: os.write(wr, view), wr, until, cancel, late)
    os.close(wr)
    while sent:
        if cancel is not None and cancel.sig:
            until = min(until, cancel.at + 100)
        done, raw = os.waitpid(pid, os.WNOHANG)
        if done:
            return os.waitstatus_to_exitcode(raw) == 0
        if now_cs() >= until:
            break
        time.sleep(0.01)
    # Past the deadline (or the input was not all taken): the writer is this process's own child, ended
    # here so an answer that is too late is never delivered later and no writer outlives the call. Reaped
    # if it has already gone; one held in an uninterruptible write is reaped by init once it ends.
    try:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, os.WNOHANG)
    except OSError:
        pass                                   # fail-direction: it has already been reaped
    return False


def emit(fd, data, until, cancel=None, late=True):
    """Write `data` to `fd`, waiting for room only until `until` (a second after a cancellation; past it, what
    fits at once is still written). True when all of it was written. No write blocks: another writer taking
    the room a poll reported only sends the loop back to wait for more."""
    view = memoryview(data)
    try:
        own = _writer(fd)
    except OSError:
        return False
    if own is None:
        return _emit_through_writer(fd, data, until, cancel, late)
    write, polled, close = own
    try:
        return _emit(view, write, polled, until, cancel, late)
    finally:
        close()


def _emit(view, write, polled, until, cancel, late=True):
    """`late`: past the deadline, what fits at once is still written (a diagnostic). Without it nothing is
    written once the deadline has passed (an answer, whose success must mean it arrived in time)."""
    poller = select.poll()
    poller.register(polled, select.POLLOUT)
    while view:
        if cancel is not None and cancel.sig:
            until = min(until, cancel.at + 100)
        left = until - now_cs()
        if left <= 0 and not late:
            return False
        try:
            events = poller.poll(max(0, min(left, 100)) * 10)   # past the deadline: one try, no wait
        except InterruptedError:
            continue
        if not events:
            if left <= 0:
                return False
            continue
        if events[0][1] & (select.POLLERR | select.POLLHUP | select.POLLNVAL):
            return False
        if not late and now_cs() >= until:
            return False                      # the poll itself took the time that was left
        try:
            view = view[write(view[:PIPE_BUF]):]
        except BlockingIOError:
            if left <= 0:
                return False                  # the room another writer took: no more waiting for it
            continue
        except OSError:
            return False
    return True


def _load_result():
    """review_result.py, the one publisher, loaded from beside this file."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("review_result", os.path.join(HERE, "review_result.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_result = _load_result()


MAX_PROMPT = 1 << 17   # the longest prompt the reviewer takes as one argument (the kernel's limit)
TEARDOWN_CS = 200      # the teardown's own budget once the command has ended or been ended


def now_cs() -> int:
    return int(time.clock_gettime(time.CLOCK_BOOTTIME) * 100)


def usage():
    print("usage: review-runtime-limit.sh [--broker LOG PROMPT | --job LOG PROMPT [--prompt-sha DIGEST]] "
          "[--publish-by UPTIME_CS] "
          "SECONDS COMMAND | --until-cs UPTIME_CS COMMAND | --no-deadline COMMAND", file=sys.stderr)
    sys.exit(2)


def parse(argv):
    opts = {"end": None, "final": None, "broker": None, "job": None, "prompt_sha": "", "broker_prompt": None,
            "prompt_len": None}
    args = list(argv)
    while args and args[0].startswith("--"):
        flag = args.pop(0)
        if flag in ("--until-cs", "--publish-by") and args and re.fullmatch(r"[0-9]+", args[0]):
            opts["end" if flag == "--until-cs" else "final"] = int(args.pop(0))
        elif flag == "--prompt-sha" and args and re.fullmatch(r"[0-9a-f]{64}", args[0]):
            opts["prompt_sha"] = args.pop(0)
        elif flag == "--prompt-len" and args and re.fullmatch(r"[0-9]{1,12}", args[0]):
            opts["prompt_len"] = int(args.pop(0))
        elif flag == "--broker-prompt" and args:
            opts["broker_prompt"] = args.pop(0)            # the raw prompt, handed to the broker in memory
        elif flag == "--no-deadline":
            opts["end"] = now_cs() + NO_DEADLINE
        elif flag in ("--broker", "--job") and len(args) >= 2 and args[0] and args[1]:
            opts[flag[2:]] = (args.pop(0), args.pop(0))
        else:
            usage()
    if opts["end"] is None:
        if len(args) != 2 or not re.fullmatch(r"[1-9][0-9]*", args[0]):
            usage()
        opts["end"] = now_cs() + int(args.pop(0)) * 100
    if len(args) != 1 or not args[0] or (opts["broker"] and opts["job"]):
        usage()
    opts["final"] = max(opts["final"] or opts["end"], opts["end"])
    return opts, args[0]


def _stat(pid):
    """(state, parent) of a process, from its whole stat record: the name (in parentheses) can hold spaces,
    ')' and newlines, so the fields are read after the LAST ')'. None when it is gone or unreadable."""
    try:
        with open(f"/proc/{pid}/stat", "rb") as fh:
            fields = fh.read().rpartition(b")")[2].split()
        return fields[0], int(fields[1])
    except (OSError, IndexError, ValueError):
        return None


def _exited(fd):
    """Whether the process a pidfd holds has exited: the whole thread group (a leader thread that ended while
    others run shows Z in its stat record, but its pidfd is not ready, so it is still a member)."""
    p = select.poll()
    p.register(fd, select.POLLIN)
    return bool(p.poll(0))


def signal_all(sig, sent=None):
    """`sig` to every member of the attempt, each through a pidfd: a process counts only when its stat
    record, read while its pidfd showed it alive, names as parent a member whose own pidfd still shows it
    alive after that read, so a number freed and given to an unrelated process is never taken for a member.
    Each member is signalled once its children are proved and its pidfd closed, so the fds held at once are
    one path down the tree and its pending siblings. Returns (signalled, unheld): unheld counts members whose
    pidfd could not be had for a reason other than their exit (descriptor pressure), retried by the caller.
    `sent` collects the numbers signalled."""
    kids = {}
    for name in os.listdir("/proc"):
        if name.isdigit():
            st = _stat(name)
            if st is not None:
                kids.setdefault(st[1], []).append(int(name))
    signalled = unheld = 0
    stack = [(os.getpid(), None)]
    try:
        while stack:
            parent, parent_fd = stack.pop()
            for pid in kids.get(parent, ()):
                try:
                    fd = os.pidfd_open(pid)
                except ProcessLookupError:   # fail-direction: gone before it was held; its orphans come here
                    continue
                except OSError:
                    unheld += 1
                    continue
                st = _stat(pid)
                if (st is None or st[1] != parent or _exited(fd)
                        or (parent_fd is not None and _exited(parent_fd))):
                    os.close(fd)
                    continue
                stack.append((pid, fd))
            if parent_fd is not None:
                try:
                    signal.pidfd_send_signal(parent_fd, sig)
                    signalled += 1
                    if sent is not None:
                        sent.add(parent)
                except ProcessLookupError:   # fail-direction: it exited after it was proved a member
                    pass
                os.close(parent_fd)
    finally:
        for _pid, fd in stack:
            if fd is not None:
                os.close(fd)
    return signalled, unheld


class Result:
    """The result an attempt publishes: its log and prompt, whether it owns the result, and whether that
    ownership is already claimed."""

    def __init__(self, log, prompt, owns=False, claimed=False):
        self.log, self.prompt, self.owns, self.claimed = log, prompt, owns, claimed


class Broker(Result):
    """What the review broker tells its supervisor, read from the hand-off pipe once its writers are gone."""

    def __init__(self, log, prompt):
        super().__init__(log, prompt)
        self.read_end, self.write_end = os.pipe()
        os.set_inheritable(self.write_end, True)
        os.environ["REVIEW_BROKER_HANDOFF_FD"] = str(self.write_end)
        self.step, self.response, self.job = "", "", ""
        self.buf = b""
        os.set_blocking(self.read_end, False)

    def started(self):
        os.close(self.write_end)

    def drain(self):
        """What the broker has written so far, read without blocking. Called while it runs, so a long
        record (the fallback job's command) never leaves it blocked on a full pipe."""
        while self.read_end is not None:
            try:
                chunk = os.read(self.read_end, 1 << 16)
            except BlockingIOError:          # a writer the kernel still holds: what it wrote is here
                return
            if not chunk:
                return
            self.buf += chunk

    def read(self):
        self.drain()
        os.close(self.read_end)
        self.read_end = None
        for line in self.buf.decode("utf-8", "replace").split("\n"):
            kind, _, value = line.partition(" ")
            if kind == "step":
                self.step = value
            elif kind == "response":
                self.response = value
            elif kind == "job":
                self.job = value
            elif kind == "owns":
                self.owns = True
            elif kind == "kind" and re.fullmatch(r"[a-z][a-z-]{0,40}", value):
                self.log, self.prompt = (_named(self.log, value), _named(self.prompt, value))


def _result_step(args, until, cancel=None):
    """review_result.py `args` in a child (which claims and publishes in one process), waited on until
    `until`, or a second after a cancellation: its exit status, or None when it was not back by then. A
    stalled filesystem leaves it to finish on its own, outside this supervisor's answer, so it has streams of
    its own: its diagnostics come through a pipe read here, and a caller capturing this supervisor's output
    sees EOF when the supervisor answers, not when the helper does."""
    rd, wr = os.pipe()
    try:
        # LIFECYCLE-WAIVER: publishes one REVIEW attempt's result; it does not relaunch or watch the session.
        pid = os.posix_spawn(sys.executable, [sys.executable, os.path.join(HERE, "review_result.py"), *args],
                             _child_env(), file_actions=[(os.POSIX_SPAWN_OPEN, 0, os.devnull, os.O_RDONLY, 0),
                                                         (os.POSIX_SPAWN_OPEN, 1, os.devnull, os.O_WRONLY, 0),
                                                         (os.POSIX_SPAWN_DUP2, wr, 2)])
    finally:
        os.close(wr)
    os.set_blocking(rd, False)
    said = b""

    def relay():
        nonlocal said
        while True:
            try:
                chunk = os.read(rd, 1 << 16)
            except BlockingIOError:
                return
            if not chunk:
                return
            said += chunk
    try:
        while True:
            relay()
            done, raw = os.waitpid(pid, os.WNOHANG)
            if done:
                relay()
                return os.waitstatus_to_exitcode(raw)
            if now_cs() >= until:
                return None
            if cancel is not None and cancel.sig:
                until = min(until, cancel.at + 100)    # the grace is counted from the signal itself
                cancel = None
            time.sleep(0.01)
    finally:
        os.close(rd)
        if said:
            sys.stderr.write(said.decode("utf-8", "replace"))
            sys.stderr.flush()


def _named(path, kind):
    """`path` with its last `{kind}` placeholder replaced by the kind the broker named."""
    head, mark, tail = path.rpartition("{kind}")
    return head + kind + tail if mark else path


def _publish(result, status, final, cancel=None):
    """The result published by `final`. True when it was published or another owner holds it; for an attempt
    never registered, only its prompt is removed, and an attempt that never named its kind wrote nothing."""
    if "{kind}" in result.prompt and not result.owns:
        return True
    if result.owns:
        args = ["publish", result.log, str(status), "--prompt", result.prompt] + ([] if result.claimed else ["--claim"])
    else:
        args = ["discard", result.prompt]
    rc = _result_step(args, final, cancel)
    if rc is None:
        print(f"[review-broker] the result ({status}) was still being published at the deadline; "
              f"it is written to {result.log}.done when the filesystem allows", file=sys.stderr)
    return rc == 0


def _child_env():
    env = dict(os.environ)
    for name in ("REVIEW_BROKER_HANDOFF_FD", "REVIEW_BROKER_INNER", "REVIEW_BROKER_PROMPT_FD"):
        env.pop(name, None)
    return env


def _launch(job):
    """Starts a job the broker handed over in a session of its own, its streams on /dev/null; it is this
    supervisor's child only until this exits, and is never ended by it."""
    # LIFECYCLE-WAIVER: detaches a REVIEW on a host with no service manager; it does not relaunch or watch the session.
    os.posix_spawnp("bash", ["bash", "-c", job], _child_env(), setsid=True, file_actions=[
        (os.POSIX_SPAWN_OPEN, 0, os.devnull, os.O_RDONLY, 0),
        (os.POSIX_SPAWN_OPEN, 1, os.devnull, os.O_WRONLY, 0),
        (os.POSIX_SPAWN_OPEN, 2, os.devnull, os.O_WRONLY, 0)])


class Cancel:
    """The first cancellation (TERM, INT or HUP) this supervisor received, as 128 + the signal, or 0."""
    WATCHED = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)

    def __init__(self):
        self.sig, self.at = 0, 0
        for sig in self.WATCHED:
            signal.signal(sig, self._handle)

    def _handle(self, signum, _frame):
        if not self.sig:
            self.sig, self.at = 128 + signum, now_cs()


def main(argv) -> int:
    opts, cmd = parse(argv)
    end, final = opts["end"], opts["final"]
    # Watched before anything starts, so a cancellation during the job's claim is seen, never the default
    # action that would leave a claimed result unpublished.
    cancel = Cancel()
    sys.stderr = Bounded(2, final, cancel)
    if end - now_cs() <= 0 and not opts["job"]:
        print("review-runtime-limit: the deadline had passed before the command started; nothing was run",
              file=sys.stderr)
        return 124
    if opts["job"]:
        # The job claims its result before anything can refuse, so a refusal is published (as 1).
        return _job(opts["job"], (opts["prompt_sha"], opts["prompt_len"]), cmd, end, cancel)
    why = _setup()
    if why:
        print(f"review-runtime-limit: {why}; nothing was run", file=sys.stderr)
        return 2
    broker = Broker(*opts["broker"]) if opts["broker"] else None
    if broker is not None and opts["broker_prompt"] is not None:
        try:
            os.environ["REVIEW_BROKER_PROMPT_FD"] = str(_prompt_fd(opts["broker_prompt"]))
        except (OSError, UnicodeEncodeError) as exc:
            print(f"review-runtime-limit: the prompt could not be handed to the broker ({exc}); nothing was run",
                  file=sys.stderr)
            return 2
    rc = _attempt(cmd, end, broker, cancel)
    if broker is None:
        return rc
    cancelled, limited, rc = rc
    return finish_broker(broker, rc, cancelled, limited, final, cancel)


def _setup():
    """What this supervisor needs before it starts an attempt: to be the subreaper its orphans return to,
    and pidfds. "" when it has them, else why not."""
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0:
        return f"cannot supervise the attempt's processes (prctl: {os.strerror(ctypes.get_errno())})"
    try:
        os.close(os.pidfd_open(os.getpid()))
    except OSError as exc:
        return f"cannot hold the attempt's processes (pidfd_open: {exc.strerror})"
    return ""


def _prompt_fd(text):
    """The checked prompt in an anonymous in-memory file the attempt inherits (REVIEW_PROMPT_FD): no exec
    string carries it until the reviewer's own argument, so the hand-off never shrinks the longest prompt
    a reviewer can be given."""
    fd = os.memfd_create("review-prompt", 0)
    data = memoryview(text.encode("utf-8"))
    while data:
        data = data[os.write(fd, data):]
    os.lseek(fd, 0, os.SEEK_SET)
    os.set_inheritable(fd, True)
    return fd


def _read_prompt(prompt, digest, length=None):
    """The prompt's text, read whole and checked against the digest (and byte length) the broker
    registered, or None (with the reason) when it cannot be read whole or is not the registered prompt. A
    file of another size is refused before any of it is read, and no more than its registered length and
    one byte is ever read, so a replaced file cannot make this owner hold more than the prompt."""
    import hashlib
    import stat
    try:
        # Opened nonblocking and refused unless it is a regular file: a path replaced by a FIFO or a device
        # would otherwise hold this owner (and its lock) in open or read for ever.
        fd = os.open(prompt, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW | os.O_CLOEXEC)
    except OSError as exc:
        return None, f"the prompt could not be read ({exc.strerror})"
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode):
            return None, "the prompt is not a regular file"
        if length is not None and info.st_size != length:
            return None, "the prompt read is not the one registered (its size differs)"
        cap = (length if length is not None else MAX_PROMPT) + 1
        chunks, got = [], 0
        while got < cap:
            chunk = os.read(fd, min(1 << 16, cap - got))
            if not chunk:
                break
            chunks.append(chunk)
            got += len(chunk)
        data = b"".join(chunks)
        if len(data) >= cap:
            return None, "the prompt read is not the one registered (it is longer)"
    except OSError as exc:
        return None, f"the prompt could not be read ({exc.strerror})"
    finally:
        os.close(fd)
    if digest and hashlib.sha256(data).hexdigest() != digest:
        return None, "the prompt read is not the one registered (its digest differs)"
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return None, "the prompt is not UTF-8 text"
    if "\0" in text:
        return None, "the prompt holds a NUL byte"
    return text, ""


def _note(log, text):
    """A line in the job's log, for a reason the review was not run; best effort."""
    try:
        fd = os.open(log, os.O_WRONLY | os.O_APPEND | os.O_CLOEXEC)
        try:
            os.write(fd, f"review-runtime-limit: {text}\n".encode())
        finally:
            os.close(fd)
    except OSError:
        pass                                           # fail-direction: the status is still published


def _job(job, digest, cmd, end, cancel):
    """The detached job: claim, run, publish, all in this one process. The claim and the publication are
    calls made here, never steps handed to a helper whose wait could time out, so whoever creates the log
    is the process that publishes its status: nothing can claim a result and leave it unpublished. A
    filesystem that stalls the claim or the publication delays this detached process, which no caller
    waits on; the review itself is still held to the steps' deadline. A claim won after that deadline, or
    after a cancellation, runs nothing and publishes 124 or the cancellation's status."""
    log, prompt = job
    digest, length = digest
    owned = _result.claim(log)
    if owned == 4:
        return 0                                       # another owner holds the result and publishes it
    if owned != 0:
        print("review-runtime-limit: the job's result could not be claimed; nothing was run", file=sys.stderr)
        return 1
    why = _setup()
    if why:
        _note(log, why + "; nothing was run")
        status = 1
    elif cancel.sig or now_cs() >= end:
        status = cancel.sig or 124
    else:
        text, why = _read_prompt(prompt, digest, length)
        if text is None:
            _note(log, why + "; nothing was run")
            status = 1
        else:
            try:
                fd = _prompt_fd(text)
            except OSError as exc:
                _note(log, f"the prompt could not be handed to the review ({exc.strerror}); nothing was run")
                status = 1
            else:
                os.environ["REVIEW_PROMPT_FD"] = str(fd)
                status = _attempt(cmd, end, None, cancel)
                os.close(fd)
    return status if _result.publish(log, str(status), prompt) == 0 else 1


def _attempt(cmd, end, broker, cancel):
    """COMMAND run under the limit and torn down: its exit status (for the broker, (cancelled, limited,
    status))."""
    watched = set(Cancel.WATCHED)
    left = end - now_cs()
    # TERM a grace before the end (30 s, or a quarter of a limit up to 120 s, at least a tenth of a second),
    # KILL a margin before it (half a second at most), so what is left is reaped by the end, not after it.
    grace = max(10, left // 4 if left <= 12000 else 3000)
    term_at, kill_at = end - grace, end - min(50, max(5, grace // 2))
    # The last look before the start, with the cancellations held: one already delivered, or one pending in
    # the kernel behind the mask, runs nothing; one that lands after this is delivered once the command
    # exists, and ends it.
    signal.pthread_sigmask(signal.SIG_BLOCK, watched)
    try:
        pending = signal.sigpending() & watched
        if cancel.sig or pending:
            rc = cancel.sig or 128 + min(pending)
            return (True, False, rc) if broker is not None else rc
        if now_cs() >= kill_at:
            print("review-runtime-limit: the deadline passed before the command started; nothing was run",
                  file=sys.stderr)
            return (False, True, 124) if broker is not None else 124
        # LIFECYCLE-WAIVER: groups one REVIEW attempt so it can be ended whole; it does not detach, relaunch or
        # watch the session (this supervisor waits for it and reaps everything it leaves).
        # The group's leader is the warden (review_warden.py): it holds the read end of a pipe whose only write
        # end this supervisor keeps for its life, and ends the whole group when that pipe closes, so nothing
        # in the group outlives this supervisor, however it ends.
        life_rd, life_wr = os.pipe()
        _LIFELINES.append(life_wr)
        said_rd, said_wr = os.pipe()
        os.set_blocking(said_rd, False)
        try:
            os.set_inheritable(life_rd, True)
            os.set_inheritable(said_wr, True)
            leader = os.posix_spawnp(sys.executable, [sys.executable, WARDEN, str(life_rd), str(said_wr), cmd],
                                     os.environ, setsid=True, setsigmask=())
        except OSError as exc:
            # Not started (a process limit, a missing shell): a failed attempt, published by its owner.
            print(f"review-runtime-limit: the command could not be started ({exc.strerror})", file=sys.stderr)
            return (False, False, 1) if broker is not None else 1
        finally:
            os.close(life_rd)
            os.close(said_wr)
    finally:
        signal.pthread_sigmask(signal.SIG_UNBLOCK, watched)
    if broker is not None:
        broker.started()
    status, limited, termed = None, False, False

    def reap():
        """Reap what has exited; the leader's status is kept. False once no child is left."""
        nonlocal status
        while True:
            try:
                pid, raw = os.waitpid(-1, os.WNOHANG)
            except ChildProcessError:
                return False
            if pid == 0:
                return True
            if pid == leader:
                # The command's own status, which the warden reports before it ends the group; a warden that
                # reported nothing was ended itself (the limit's KILL) and its wait status stands.
                try:
                    said = os.read(said_rd, 32).strip()
                except BlockingIOError:
                    said = b""
                status = int(said) if said.isdigit() else os.waitstatus_to_exitcode(raw)

    while status is None:
        reap()
        if broker is not None:
            broker.drain()
        if status is not None or cancel.sig:
            break
        now = now_cs()
        if now >= kill_at:
            limited = True
            break
        if now >= term_at and not termed:
            limited = termed = True
            signal_all(signal.SIGTERM)
        time.sleep(POLL)
    # One teardown for every way out: what is left of the attempt is killed and reaped, walked again until
    # nothing is (a process can fork between a walk and its signal, and its orphans come back here). No
    # child left means no descendant left, so a command that left nothing costs no walk at all. It has a
    # budget of its own (two seconds, one after a cancellation) inside the deadline: a KILLed process is
    # reaped at once unless the kernel holds it in uninterruptible I/O, and then waiting longer (with no
    # deadline, for ever) only delays the result while each pass walks /proc.
    drain_end = max(min(end, now_cs() + (100 if cancel.sig else TEARDOWN_CS)), now_cs() + 5)
    while reap():
        if broker is not None:
            broker.drain()
        signal_all(signal.SIGKILL)
        if not reap():
            break
        if now_cs() >= drain_end:
            # Every member still present is sent KILL once more, walked until a pass finds none it had not
            # signalled (a KILLed process cannot fork), so what is reported was KILLed, not merely seen.
            sent, unheld = set(), 0
            for _ in range(8):
                before = len(sent)
                _n, unheld = signal_all(signal.SIGKILL, sent)
                if len(sent) == before and not unheld:
                    break
            print(f"review-runtime-limit: {len(sent)} process(es) of the attempt were KILLed and still ending at "
                  f"the deadline (uninterruptible I/O)"
                  + (f"; {unheld} could not be held to be signalled" if unheld else ""), file=sys.stderr)
            break
        time.sleep(0.01)
    # Seen ending only after the deadline (this supervisor was not scheduled in between): the limit's.
    if now_cs() >= end:
        limited = True
    rc = cancel.sig or (124 if limited or status is None else (status if status >= 0 else 128 - status))
    return rc if broker is None else (bool(cancel.sig), limited and not cancel.sig, rc)


def _deliver(broker):
    """The broker's answer (the job to poll) written to the caller and checked: True when it was."""
    if emit(1, (broker.response + "\n").encode(), broker.final, broker.cancel, late=False):
        return True
    print(f"[review-broker] the answer could not be delivered by the deadline; the review was launched "
          f"and publishes its result to {broker.log}.done", file=sys.stderr)
    return False


def finish_broker(broker, rc, cancelled, limited, final, cancel=None):
    """The broker's answer once its attempt is torn down: the result it owns published, a job it handed
    over started, the answer delivered here (checked), and the caller told which step the limit or a
    cancellation ended."""
    broker.read()
    broker.final, broker.cancel = final, cancel
    step = broker.step or "the dispatch"
    if step == "answered" and not cancelled and broker.response:
        # The submission succeeded: the result is the launched job's, whatever happens to the answer.
        if broker.job:
            try:
                _launch(broker.job)
            except OSError as exc:
                print(f"[review-broker] the job could not be started ({exc.strerror})", file=sys.stderr)
                _publish(broker, 1, final, cancel)
                return 1
        return 0 if _deliver(broker) else 1
    if rc == 0:
        rc = 1                            # ended without the answer it owes: a failed dispatch
    published = _publish(broker, rc, final, cancel)
    if cancelled:
        print(f"[review-broker] cancelled during {step} (exit {rc}); its result is recorded as {rc}",
              file=sys.stderr)
        return rc
    if not limited:
        return rc                         # the broker's own refusal: it said why
    if step == "the submission" and broker.response:
        # Whether the unit started is unknown; its result is recorded by its one owner, so the caller gets the
        # job to poll, as for any launch.
        return 0 if _deliver(broker) and published else 1
    print(f"[review-broker] BLOCK -- {step} took the whole CODEX_REVIEW_MAX_SECS; nothing was launched",
          file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
