#!/usr/bin/env python3
"""Stop every process of this user that carries one exact environment entry.

    reap_env.py KEY=VALUE

The review dispatcher gives each pass its own CLAUDE_PLUGIN_DATA, so every process the Codex companion starts for
that pass (its broker, which detaches into its own session, and the app-server under it) inherits the entry, and
nothing else does. A process tree or a recorded pid would miss the broker: it is detached, and it is recorded only
once it is ready. The scan repeats until none is running, so a child started during the reap is found too. Each
match gets SIGTERM, then SIGKILL once the grace has passed, through a pidfd so a reused pid is never hit. This
process and its ancestors are never matched.

Exit 0 once every match has stopped or was already gone, 1 when one survived SIGKILL, 2 on a bad argument.
"""
import os
import signal
import sys
import time

GRACE_S = 2.0
DEADLINE_S = 6.0


def _ancestors():
    pids, pid = set(), os.getpid()
    while pid > 1:
        pids.add(pid)
        try:
            with open(f"/proc/{pid}/stat") as fh:
                pid = int(fh.read().rsplit(") ", 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            break
    return pids


def matches(entry):
    """The pids of this user's processes whose environment holds `entry` exactly, outside this process's line."""
    want, uid, skip, found = entry.encode(), os.getuid(), _ancestors(), []
    for name in os.listdir("/proc"):
        if not name.isdigit() or int(name) in skip:
            continue
        try:
            if os.stat(f"/proc/{name}").st_uid != uid:
                continue
            with open(f"/proc/{name}/environ", "rb") as fh:
                if want in fh.read().split(b"\0"):
                    found.append(int(name))
        except OSError:  # fail-direction: a process that exited, or one we may not read, is not ours to stop
            continue
    return found


def _running(pid):
    try:
        with open(f"/proc/{pid}/stat") as fh:
            return fh.read().rsplit(") ", 1)[1][0] != "Z"
    except (OSError, IndexError):
        return False


def _signal(pid, sig):
    """Signal `pid` through a pidfd, so a pid reused between the scan and the signal is never hit."""
    try:
        fd = os.pidfd_open(pid)
    except OSError:  # fail-direction: the process already exited, which is the goal
        return
    try:
        signal.pidfd_send_signal(fd, sig)
    except OSError:  # fail-direction: it exited between the open and the signal
        pass
    finally:
        os.close(fd)


def reap(entry):
    """Signal every marked process, then scan again, until a scan finds none running: a marked process can start
    a child while it handles SIGTERM, and one snapshot would miss it. SIGTERM first; a process still running after
    GRACE_S, or any found after that, gets SIGKILL. Gives up after DEADLINE_S."""
    started = time.monotonic()
    termed = set()
    while time.monotonic() - started < DEADLINE_S:
        live = [p for p in matches(entry) if _running(p)]
        if not live:
            return 0
        late = time.monotonic() - started > GRACE_S
        for pid in live:
            if late:
                _signal(pid, signal.SIGKILL)
            elif pid not in termed:
                _signal(pid, signal.SIGTERM)
                termed.add(pid)
        time.sleep(0.05)
    return 0 if not [p for p in matches(entry) if _running(p)] else 1


def main(argv):
    if len(argv) != 2 or "=" not in argv[1] or argv[1].startswith("="):
        print("usage: reap_env.py KEY=VALUE", file=sys.stderr)
        return 2
    return reap(argv[1])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
