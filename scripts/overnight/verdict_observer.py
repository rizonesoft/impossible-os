#!/usr/bin/env python3
"""The verdict waiter's observer: one process that makes every filesystem read for wait-for-codex-verdict.sh.

The waiter's shell owns the deadline and touches no file. This process probes each log, recovers a result
whose owner is gone, and writes what it saw to stdout, one line per observation:

    done RC N               log N has published status RC
    unreadable N            log N's status is there but could not be read (unknown: never recovered or judged)
    unpublished N           log N's owner ended and its recovery could not publish (it never will)
    seen EXISTS SIZE SILENT N
                            log N's metadata: a regular file (1) or not (0), its size, seconds since it grew
                            (since it was first looked for, when it does not exist)
    unknown N               log N's metadata could not be read (a storage fault): nothing is known of it
    cycle                   a round of probes is complete
    statuses                every log has a status; the last log's tail follows
    tail TEXT               a line of that tail
    alldone                 the end of the tail; this process exits

A log is named by N, its position among the arguments (from 0), never by its path: a path may hold a
newline, and every line here is one record.

Every probe runs here, in this one process, so the waiter ends all of them by ending this process: a read
stalled on storage never outlives the call as a process of its own. A recovery is the one process started
here. It runs in a session of its own, so ending this process leaves it to publish the result, and it is
known as a recovery from the moment it is started.

Each round, a log's latest observation is its state; completion is decided from every log's status after
the round, never from a flag set during it. A recovery's exit status is kept until a round reads "no status"
conclusively and checks the owner: only then is a failed recovery reported unpublished.
"""
import argparse
import errno
import fcntl
import hashlib
import os
import stat as stat_mod
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import review_result  # noqa: E402  (the sibling module, found through the path set just above)

RESULT_TOOL = str(HERE / "review_result.py")
CLOEXEC = getattr(os, "O_CLOEXEC", 0)


def now() -> float:
    """The elapsed-time clock (CLOCK_BOOTTIME, as the waiter's /proc/uptime), which a suspend advances."""
    return time.clock_gettime(getattr(time, "CLOCK_BOOTTIME", time.CLOCK_MONOTONIC))


def probe_status(log: str):
    """(status, error) of LOG, through the one status parser every reader uses."""
    return review_result.read_status(log)


ABSENT = frozenset({errno.ENOENT, errno.ENOTDIR})   # the lstat errors that show nothing is at the path


def probe_meta(log: str):
    """(exists, size, mtime) from one lstat: metadata only, so a FIFO that took the path never holds it.
    None when the lstat failed for a reason that does not show the path is empty (EIO, EACCES, ...): such a
    log is not known to be missing, and never starts or advances the missing-log clock."""
    try:
        st = os.lstat(log)
    except OSError as exc:
        if exc.errno not in ABSENT:
            return None
        return 0, 0, None
    if not stat_mod.S_ISREG(st.st_mode):
        return 0, 0, None
    return 1, st.st_size, int(st.st_mtime)


def owner_state(log: str):
    """"free" when LOG's owner lock exists and nobody holds it (the owner has gone), "held" when its owner
    holds it, None when that could not be read (an open or lock error). Never waits."""
    try:
        fd = os.open(log + ".owner", os.O_RDONLY | CLOEXEC)
    except OSError:
        return None
    try:
        fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
        return "free"
    except BlockingIOError:
        return "held"
    except OSError:
        return None
    finally:
        os.close(fd)


def owner_free(log: str) -> bool:
    """True only when the owner is known to have gone."""
    return owner_state(log) == "free"


def first_seen(log: str, act_dir: str) -> int:
    """When LOG was first looked for (seconds since the epoch), kept in a sidecar that survives calls. A
    sighting that cannot be kept, or a sidecar left empty or damaged, reads as the epoch, so a missing log
    is reported missing at once rather than looking newly launched forever."""
    side = os.path.join(act_dir, hashlib.md5(log.encode()).hexdigest()[:16])
    try:
        os.makedirs(act_dir, exist_ok=True)
        if not os.path.isfile(side):
            tmp = f"{side}.tmp.{os.getpid()}"
            with open(tmp, "w") as fh:
                fh.write(f"{int(time.time())}\n")
            os.replace(tmp, side)
        with open(side) as fh:
            text = fh.readline().strip()
    except OSError:
        return 0
    return int(text) if text.isdigit() else 0


def read_tail(log: str) -> list:
    """The last lines of LOG's transcript (review_result's bounded tail), or [] when it cannot be read."""
    try:
        fd = review_result.open_regular(log, os.O_RDONLY)
    except OSError:
        return []
    try:
        size = os.fstat(fd).st_size
        data = os.pread(fd, review_result.TAIL_BYTES, max(0, size - review_result.TAIL_BYTES))
    except OSError:
        return []
    finally:
        os.close(fd)
    return data.decode("utf-8", "replace").splitlines()[-review_result.TAIL_LINES:]


def start_recovery(log: str):
    """A recovery of LOG's result, in a session of its own: ending the observer leaves it to finish."""
    return subprocess.Popen(["python3", RESULT_TOOL, "recover", log], stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            # LIFECYCLE-WAIVER: publishes one REVIEW attempt's result; it does not relaunch or watch the session.
                            start_new_session=True)


class _NotStarted:
    """A recovery that could not be started: judged like one that ended without publishing."""

    @staticmethod
    def poll() -> int:
        return 1


_NOT_STARTED = _NotStarted()


def say(line: str) -> None:
    sys.stdout.write(line + "\n")
    sys.stdout.flush()


def observe(logs: list, secs: float, poll: int, act_dir: str) -> int:
    end = now() + secs
    status: dict = {}
    recovery: dict = {}            # log -> Popen, until its exit status has been judged
    tried: set = set()             # logs recovered once in this call (at most once each)
    unpublished: set = set()
    while True:
        soon = False
        for n, f in enumerate(logs):
            if f in status:
                continue
            found, error = probe_status(f)
            if found is not None:
                status[f] = found
                say(f"done {found} {n}")
                continue
            if error is not None:
                # What the status holds is unknown: never recovered, judged stale or reported unpublished.
                say(f"unreadable {n}")
                continue
            if f in unpublished:
                say(f"unpublished {n}")
                continue
            proc = recovery.get(f)
            if proc is not None:
                rc = proc.poll()
                if rc is None:
                    soon = True
                else:
                    # This round read "no status" conclusively: the recovery's result is judged now, once its
                    # owner's state is known too (an owner check that fails keeps it for the next round).
                    owner = owner_state(f) if rc != 0 else "held"
                    if owner == "free":
                        del recovery[f]
                        unpublished.add(f)
                        say(f"unpublished {n}")
                        continue
                    if owner is None:
                        soon = True
                    else:
                        del recovery[f]
            elif f not in tried and os.path.lexists(f) and owner_free(f):
                tried.add(f)
                try:
                    recovery[f] = start_recovery(f)
                    soon = True
                except OSError:
                    # The recovery could not be started (no descriptors, no memory): it is judged as a recovery
                    # that failed, the next time "no status" is read conclusively (a read error first is
                    # reported unreadable, as always), so nothing is judged on this round's reading.
                    recovery[f] = _NOT_STARTED
                    soon = True
                    say(f"unknown {n}")
                    continue
            meta = probe_meta(f)
            if meta is None:
                say(f"unknown {n}")
                continue
            exists, size, mtime = meta
            wall = int(time.time())
            silent = wall - mtime if exists else wall - first_seen(f, act_dir)
            say(f"seen {exists} {size} {silent} {n}")
        if all(f in status for f in logs):
            say("statuses")
            for line in read_tail(logs[-1]):
                say(f"tail {line}")
            say("alldone")
            return 0
        say("cycle")
        # The next round comes after POLL, or a second after a recovery started or is running (its status is
        # then seen at once), and in any case a second before the deadline.
        left = end - now() - 1
        time.sleep(max(1.0, min(1 if soon else poll, left)))


def main(argv) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--secs", type=float, required=True)
    ap.add_argument("--poll", type=int, default=10)
    ap.add_argument("--act-dir", default=".claude/state/waiter-first-seen")
    ap.add_argument("logs", nargs="+")
    args = ap.parse_args(argv)
    try:
        return observe(args.logs, args.secs, args.poll, args.act_dir)
    except BrokenPipeError:
        return 0                   # the waiter has gone; nothing is left to tell


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
