#!/usr/bin/env python3
"""review_runtime_limit.py SECONDS COMMAND | --until-cs UPTIME_CENTISECONDS COMMAND

Runs COMMAND (a bash command string) and ends the whole attempt within the limit: TERM to every process of
the attempt a grace before the end, KILL at it. Exits with COMMAND's status, 124 when the limit ended it (or
it was seen ending only after the limit), or 128 + the signal when this supervisor was cancelled (TERM, INT,
HUP), which ends the attempt the same way. scripts/overnight/review-runtime-limit.sh is the entry point.

THE ATTEMPT IS EVERY PROCESS DESCENDED FROM IT, and this supervisor is their child subreaper
(PR_SET_CHILD_SUBREAPER): a process whose parent exits is re-parented here instead of to init, so no step
can leave the attempt by starting its own session (git under the candidate reader does), by double-forking,
or by being started from a TERM handler that then exits. Whatever way COMMAND ends, every remaining
descendant is killed and reaped before this exits, so a caller that publishes a status afterwards (the review
broker's job) knows no process of the attempt can still write to its log. Only a process handed to another
supervisor (a service manager) leaves the tree, and the review steps start none.

GNU timeout is not enough for that: once its direct child exits on TERM it stops supervising, and a
descendant that ignores TERM keeps running. --until-cs takes the caller's absolute deadline in hundredths of
a second of /proc/uptime (CLOCK_BOOTTIME, which a wall-clock step does not move), so time the caller spent
before this started is not given back; a deadline already passed runs nothing.
"""
import ctypes
import os
import re
import signal
import sys
import time

PR_SET_CHILD_SUBREAPER = 36
POLL = 0.1              # the limit is kept to a tenth of a second
DRAIN_LIMIT = 10.0      # how long a KILLed process may take to go (one in uninterruptible I/O) before it is reported


def now_cs() -> int:
    return int(time.clock_gettime(time.CLOCK_BOOTTIME) * 100)


def usage():
    print("usage: review-runtime-limit.sh SECONDS COMMAND | --until-cs UPTIME_CENTISECONDS COMMAND", file=sys.stderr)
    sys.exit(2)


def parse(argv):
    if argv[:1] == ["--until-cs"]:
        if len(argv) != 3 or not re.fullmatch(r"[0-9]+", argv[1]) or not argv[2]:
            usage()
        end, cmd = int(argv[1]), argv[2]
        if end - now_cs() <= 0:
            print("review-runtime-limit: the deadline had passed before the command started; nothing was run",
                  file=sys.stderr)
            sys.exit(124)
        return end, cmd
    if len(argv) != 2 or not re.fullmatch(r"[1-9][0-9]*", argv[0]) or not argv[1]:
        usage()
    return now_cs() + int(argv[0]) * 100, argv[1]


def _stat(pid):
    """(state, parent) of a process, from its whole stat record: the name (in parentheses) can hold spaces,
    ')' and newlines, so the fields are read after the LAST ')'. None when it is gone or unreadable."""
    try:
        with open(f"/proc/{pid}/stat", "rb") as fh:
            fields = fh.read().rpartition(b")")[2].split()
        return fields[0], int(fields[1])
    except (OSError, IndexError, ValueError):
        return None


def descendants():
    """Every live (not zombie) process descended from this one, read from /proc in one pass."""
    kids = {}
    for name in os.listdir("/proc"):
        if name.isdigit():
            st = _stat(name)
            if st is not None:
                kids.setdefault(st[1], []).append((int(name), st[0]))
    out, queue, i = [], [os.getpid()], 0
    while i < len(queue):
        for pid, state in kids.get(queue[i], ()):
            queue.append(pid)
            if state != b"Z":
                out.append(pid)
        i += 1
    return out


def signal_all(sig):
    for pid in descendants():
        try:
            os.kill(pid, sig)
        except ProcessLookupError:   # fail-direction: gone between the read and the signal
            pass


def main(argv) -> int:
    end, cmd = parse(argv)
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0:
        print(f"review-runtime-limit: cannot supervise the attempt's processes "
              f"(prctl: {os.strerror(ctypes.get_errno())}); nothing was run", file=sys.stderr)
        return 2
    cancelled = 0

    def cancel(signum, _frame):
        nonlocal cancelled
        cancelled = cancelled or 128 + signum
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, cancel)
    left = end - now_cs()
    # TERM a grace before the end (30 s, or a quarter of a limit up to 120 s, at least a tenth of a second).
    grace = max(10, left // 4 if left <= 12000 else 3000)
    term_at = end - grace
    # LIFECYCLE-WAIVER: groups one REVIEW attempt so it can be ended whole; it does not detach, relaunch or
    # watch the session (this supervisor waits for it and reaps everything it leaves).
    leader = os.posix_spawnp("bash", ["bash", "-c", cmd], os.environ, setsid=True,
                             setsigdef=(signal.SIGPIPE, signal.SIGXFSZ))
    status, limited, termed = None, False, False

    def reap(block):
        """Reap what has exited; the leader's status is kept. False once no child is left."""
        nonlocal status
        while True:
            try:
                pid, raw = os.waitpid(-1, 0 if block else os.WNOHANG)
            except ChildProcessError:
                return False
            if pid == 0:
                return True
            if pid == leader:
                status = os.waitstatus_to_exitcode(raw)
            if block:
                return True

    while status is None:
        reap(block=False)
        if status is not None:
            break
        if cancelled:
            break
        now = now_cs()
        if now >= end:
            limited = True
            break
        if now >= term_at and not termed:
            limited = termed = True
            signal_all(signal.SIGTERM)
        time.sleep(POLL)
    # One teardown for every way out: whatever is left of the attempt is killed and reaped, re-read until
    # nothing is, since a process can fork between a read and the signal and its orphans come back here.
    drain_end = time.monotonic() + DRAIN_LIMIT
    while True:
        alive = descendants()
        for pid in alive:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:   # fail-direction: gone between the read and the signal
                pass
        if not reap(block=False) and not alive:
            break
        if time.monotonic() >= drain_end:
            print(f"review-runtime-limit: {len(alive)} process(es) of the attempt did not end after KILL "
                  f"(uninterruptible I/O?): {' '.join(map(str, alive))}", file=sys.stderr)
            return 124 if not cancelled else cancelled
        time.sleep(0.01)
    # Seen ending only after the deadline (this supervisor was not scheduled in between): the limit's.
    if now_cs() >= end:
        limited = True
    if cancelled:
        return cancelled
    if limited or status is None:
        return 124
    return status if status >= 0 else 128 - status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
