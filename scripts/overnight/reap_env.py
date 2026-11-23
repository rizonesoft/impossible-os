#!/usr/bin/env python3
"""Stop every process of this user that carries one exact environment entry.

    reap_env.py KEY=VALUE

The review dispatcher gives each pass its own CLAUDE_PLUGIN_DATA, so every process the Codex companion starts for
that pass (its broker, which detaches into its own session, and the app-server under it) inherits the entry, and
nothing else does. A process tree or a recorded pid would miss the broker: it is detached, and it is recorded only
once it is ready. Each match gets SIGTERM, then SIGKILL after a short grace. This process and its ancestors are
never matched (the dispatcher that calls this carries the entry too, until it unsets it).

Exit 0 once every match has stopped or was already gone, 1 when one survived SIGKILL, 2 on a bad argument.
"""
import os
import signal
import sys
import time

GRACE_S = 2.0


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


def reap(entry):
    pids = matches(entry)
    for sig in (signal.SIGTERM, signal.SIGKILL):
        for pid in pids:
            try:
                os.kill(pid, sig)
            except ProcessLookupError:  # fail-direction: already gone is the goal
                pass
        until = time.monotonic() + GRACE_S
        while any(_running(p) for p in pids) and time.monotonic() < until:
            time.sleep(0.05)
        pids = [p for p in pids if _running(p)]
        if not pids:
            return 0
    return 1


def main(argv):
    if len(argv) != 2 or "=" not in argv[1] or argv[1].startswith("="):
        print("usage: reap_env.py KEY=VALUE", file=sys.stderr)
        return 2
    return reap(argv[1])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
