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

THE DETACHED JOB'S SUPERVISOR (--job LOG PROMPT, with --publish-by FINAL_CS). One supervisor for the whole
job, never one nested in another (whose grace would end the inner one before it could answer): it claims
the result (LOG created exclusively; a result another owner holds is left alone and nothing runs), runs
COMMAND (the review, its output appended to LOG) until the steps' deadline, and publishes COMMAND's status,
or 124 when the limit ended it, by the final one. A publication that fails is this supervisor's exit 1.
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


def now_cs() -> int:
    return int(time.clock_gettime(time.CLOCK_BOOTTIME) * 100)


def usage():
    print("usage: review-runtime-limit.sh [--broker LOG PROMPT] [--publish-by UPTIME_CS] "
          "SECONDS COMMAND | --until-cs UPTIME_CS COMMAND | --no-deadline COMMAND", file=sys.stderr)
    sys.exit(2)


def parse(argv):
    opts = {"end": None, "final": None, "broker": None, "job": None}
    args = list(argv)
    while args and args[0].startswith("--"):
        flag = args.pop(0)
        if flag in ("--until-cs", "--publish-by") and args and re.fullmatch(r"[0-9]+", args[0]):
            opts["end" if flag == "--until-cs" else "final"] = int(args.pop(0))
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

    def started(self):
        os.close(self.write_end)

    def read(self):
        os.set_blocking(self.read_end, False)
        data = b""
        while True:
            try:
                chunk = os.read(self.read_end, 1 << 16)
            except BlockingIOError:          # a writer the kernel still holds: what it wrote is here
                break
            if not chunk:
                break
            data += chunk
        os.close(self.read_end)
        for line in data.decode("utf-8", "replace").split("\n"):
            kind, _, value = line.partition(" ")
            if kind == "step":
                self.step = value
            elif kind == "response":
                self.response = value
            elif kind == "job":
                self.job = value
            elif kind == "owns":
                self.owns = True


def _result_step(args, until):
    """review_result.py `args` in a child, waited on until `until` (a stalled filesystem leaves it to finish
    on its own, outside this supervisor's answer): its exit status, or None at the deadline."""
    # LIFECYCLE-WAIVER: publishes one REVIEW attempt's result; it does not relaunch or watch the session.
    pid = os.posix_spawn(sys.executable, [sys.executable, os.path.join(HERE, "review_result.py"), *args],
                         _child_env())
    while True:
        done, raw = os.waitpid(pid, os.WNOHANG)
        if done:
            return os.waitstatus_to_exitcode(raw)
        if now_cs() >= until:
            return None
        time.sleep(0.01)


def _publish(result, status, final):
    """The result published by `final`. True when it was published or another owner holds it; for an attempt
    never registered, only its prompt is removed."""
    if result.owns:
        args = ["publish", result.log, str(status), "--prompt", result.prompt] + ([] if result.claimed else ["--claim"])
    else:
        args = ["discard", result.prompt]
    rc = _result_step(args, final)
    if rc is None:
        print(f"[review-broker] the result ({status}) was still being published at the deadline; "
              f"it is written to {result.log}.done when the filesystem allows", file=sys.stderr)
    return rc == 0


def _child_env():
    env = dict(os.environ)
    for name in ("REVIEW_BROKER_HANDOFF_FD", "REVIEW_BROKER_INNER"):
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


def main(argv) -> int:
    opts, cmd = parse(argv)
    end, final = opts["end"], opts["final"]
    if end - now_cs() <= 0:
        print("review-runtime-limit: the deadline had passed before the command started; nothing was run",
              file=sys.stderr)
        return 124
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0:
        print(f"review-runtime-limit: cannot supervise the attempt's processes "
              f"(prctl: {os.strerror(ctypes.get_errno())}); nothing was run", file=sys.stderr)
        return 2
    try:
        os.close(os.pidfd_open(os.getpid()))
    except OSError as exc:
        print(f"review-runtime-limit: cannot hold the attempt's processes (pidfd_open: {exc.strerror}); "
              f"nothing was run", file=sys.stderr)
        return 2
    broker = Broker(*opts["broker"]) if opts["broker"] else None
    job = None
    if opts["job"]:
        # The claim, before anything runs: a result another owner holds is left alone.
        claimed = _result_step(["claim", opts["job"][0]], end)
        if claimed == 4:
            return 0
        if claimed != 0:
            print("review-runtime-limit: the job's result could not be claimed"
                  + (" by the deadline" if claimed is None else "") + "; nothing was run", file=sys.stderr)
            return 124 if claimed is None else 1
        job = Result(*opts["job"], owns=True, claimed=True)
    rc = _attempt(cmd, end, broker)
    if job is not None:
        return rc if _publish(job, rc, final) else 1
    if broker is None:
        return rc
    cancelled, limited, rc = rc
    return finish_broker(broker, rc, cancelled, limited, final)


def _attempt(cmd, end, broker):
    """COMMAND run under the limit and torn down: its exit status (for the broker, (cancelled, limited,
    status))."""
    cancelled = 0
    watched = {signal.SIGTERM, signal.SIGINT, signal.SIGHUP}

    def cancel(signum, _frame):
        nonlocal cancelled
        cancelled = cancelled or 128 + signum
    for sig in watched:
        signal.signal(sig, cancel)
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
        if cancelled or pending:
            rc = cancelled or 128 + min(pending)
            return (True, False, rc) if broker is not None else rc
        if now_cs() >= kill_at:
            print("review-runtime-limit: the deadline passed before the command started; nothing was run",
                  file=sys.stderr)
            return (False, True, 124) if broker is not None else 124
        # LIFECYCLE-WAIVER: groups one REVIEW attempt so it can be ended whole; it does not detach, relaunch or
        # watch the session (this supervisor waits for it and reaps everything it leaves).
        leader = os.posix_spawnp("bash", ["bash", "-c", cmd], os.environ, setsid=True, setsigmask=(),
                                 setsigdef=(signal.SIGPIPE, signal.SIGXFSZ))
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
                status = os.waitstatus_to_exitcode(raw)

    while status is None:
        reap()
        if status is not None or cancelled:
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
    # child left means no descendant left, so a command that left nothing costs no walk at all. It is held
    # to the deadline (a second at most after a cancellation).
    drain_end = max(min(end, now_cs() + 100) if cancelled else end, now_cs() + 5)
    while reap():
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
    rc = cancelled or (124 if limited or status is None else (status if status >= 0 else 128 - status))
    return rc if broker is None else (bool(cancelled), limited and not cancelled, rc)


def finish_broker(broker, rc, cancelled, limited, final):
    """The broker's answer once its attempt is torn down: the result it owns published, a job it handed
    over started, and the caller told which step the limit or a cancellation ended."""
    broker.read()
    step = broker.step or "the dispatch"
    if step == "answered" and not cancelled:
        rc = 0                            # the caller has its answer: the result is the launched job's
    if rc == 0 and broker.job:
        try:
            _launch(broker.job)
            return 0
        except OSError as exc:
            print(f"[review-broker] the job could not be started ({exc.strerror})", file=sys.stderr)
            _publish(broker, 1, final)
            return 1
    if rc == 0:
        return 0
    published = _publish(broker, rc, final)
    if cancelled:
        print(f"[review-broker] cancelled during {step} (exit {rc}); its result is recorded as {rc}",
              file=sys.stderr)
        return rc
    if not limited:
        return rc                         # the broker's own refusal: it said why
    if step == "the submission" and broker.response:
        # Whether the unit started is unknown; its result is recorded by its one owner, so the caller gets the
        # job to poll, as for any launch.
        print(broker.response, flush=True)
        return 0 if published else 1
    print(f"[review-broker] BLOCK -- {step} took the whole CODEX_REVIEW_MAX_SECS; nothing was launched",
          file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
