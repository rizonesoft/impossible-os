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
descendant is killed and reaped by the deadline, so a caller that publishes a status afterwards (the review
broker's job) knows no process of the attempt can still write to its log (one the kernel holds in
uninterruptible I/O after KILL is reported: it can finish only the call it is in). Every signal goes through a
pidfd held for a process proved a member, never to a bare number a finished process may have given up. Only a process handed to another
supervisor (a service manager) leaves the tree, and the review steps start none.

GNU timeout is not enough for that: once its direct child exits on TERM it stops supervising, and a
descendant that ignores TERM keeps running. --until-cs takes the caller's absolute deadline in hundredths of
a second of /proc/uptime (CLOCK_BOOTTIME, which a wall-clock step does not move), so time the caller spent
before this started is not given back; a deadline already passed runs nothing.
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


def _exited(fd):
    """Whether the process a pidfd holds has exited (a zombie included)."""
    p = select.poll()
    p.register(fd, select.POLLIN)
    return bool(p.poll(0))


def held_tree():
    """The attempt's live processes, each held by a pidfd and proved a member top-down: a process counts only
    when its stat record, read while its pidfd showed it alive, names as parent a member whose own pidfd still
    shows it alive after that read. A number freed and given to an unrelated process is therefore never taken
    for a member, however the host schedules this walk (a pidfd never signals a process it does not hold).
    Returns [(pid, fd)]; the caller closes the fds."""
    kids = {}
    for name in os.listdir("/proc"):
        if name.isdigit():
            st = _stat(name)
            if st is not None and st[0] != b"Z":       # a zombie has no children and nothing to signal
                kids.setdefault(st[1], []).append(int(name))
    held, queue = [], [(os.getpid(), None)]
    while queue:
        parent, parent_fd = queue.pop()
        for pid in kids.get(parent, ()):
            try:
                fd = os.pidfd_open(pid)
            except OSError:              # fail-direction: gone before it was held; a later pass sees its orphans
                continue
            st = _stat(pid)
            if (st is None or st[1] != parent or _exited(fd)
                    or (parent_fd is not None and _exited(parent_fd))):
                os.close(fd)
                continue
            held.append((pid, fd))
            queue.append((pid, fd))
    return held


def signal_all(sig):
    """`sig` to every member of the attempt through its pidfd; the number signalled."""
    held = held_tree()
    n = 0
    try:
        for _pid, fd in held:
            try:
                signal.pidfd_send_signal(fd, sig)
                n += 1
            except ProcessLookupError:   # fail-direction: it exited after it was proved a member
                pass
    finally:
        for _pid, fd in held:
            os.close(fd)
    return n


def main(argv) -> int:
    end, cmd = parse(argv)
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
    # The last look before the start, with the cancellations held: one that lands after it is delivered
    # once the command exists, and ends it.
    signal.pthread_sigmask(signal.SIG_BLOCK, watched)
    try:
        if cancelled:
            return cancelled
        if now_cs() >= kill_at:
            print("review-runtime-limit: the deadline passed before the command started; nothing was run",
                  file=sys.stderr)
            return 124
        # LIFECYCLE-WAIVER: groups one REVIEW attempt so it can be ended whole; it does not detach, relaunch or
        # watch the session (this supervisor waits for it and reaps everything it leaves).
        leader = os.posix_spawnp("bash", ["bash", "-c", cmd], os.environ, setsid=True, setsigmask=(),
                                 setsigdef=(signal.SIGPIPE, signal.SIGXFSZ))
    finally:
        signal.pthread_sigmask(signal.SIG_UNBLOCK, watched)
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
    # to the deadline (a second at most after a cancellation): a process the kernel keeps in uninterruptible
    # I/O after KILL cannot run another instruction of its own, only finish the call it is in.
    drain_end = max(min(end, now_cs() + 100) if cancelled else end, now_cs() + 5)
    while reap():
        signal_all(signal.SIGKILL)
        if not reap():
            break
        if now_cs() >= drain_end:
            print(f"review-runtime-limit: {len(held_tree())} process(es) of the attempt were still ending at the "
                  f"deadline after KILL (uninterruptible I/O); none can run again", file=sys.stderr)
            break
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
