#!/usr/bin/env python3
"""The leader of one review attempt's process group: the attempt dies with its supervisor.

    review_warden.py LIFELINE_FD STATUS_FD COMMAND

Started by review_runtime_limit.py in a session of its own, so this process is the group leader and the group's
id cannot be reused while it lives. It runs COMMAND (bash -c) in the same group. The group ends whole, with
SIGKILL from this process (itself included), at the first of two moments:

- COMMAND exits: its status (128 + N for signal N) is written to STATUS_FD first, a pipe the supervisor reads,
  so a helper COMMAND left behind in the group never outlives it, whether or not the supervisor is still there
  to tear down;
- LIFELINE_FD reaches end of file: it is the read end of a pipe whose only write end the supervisor holds,
  so it closes when the supervisor ends, however it ends (an out-of-memory termination, a crash).

Signalling its own group is the one signal that needs no identity check, because the group is the one it
leads. Nothing that stays in the group outlives the command or the supervisor, and a recovery that publishes
a dead owner's result never has to find or signal a process. A process that leaves the group (a session of
its own, as scripts/bounded_run.py starts git) is not ended here: on a host with a service manager the job's
unit cgroup ends it, and on the non-systemd fallback it is accepted scope.

The cancellations the supervisor sends to the group (TERM, INT, HUP) are ignored here, so COMMAND's own
answer to them is the status reported; the supervisor's KILL ends this process like any other member.
"""
import os
import select
import signal
import sys


def main(argv) -> int:
    if len(argv) != 3 or not argv[0].isdigit() or not argv[1].isdigit():
        print("usage: review_warden.py LIFELINE_FD STATUS_FD COMMAND", file=sys.stderr)
        return 2
    lifeline, status = int(argv[0]), int(argv[1])
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, signal.SIG_IGN)
    os.set_inheritable(lifeline, False)
    os.set_inheritable(status, False)
    # The command gets the dispositions a shell started directly would (Python ignores SIGPIPE and
    # SIGXFSZ; the cancellations ignored here are restored too).
    child = os.posix_spawnp("bash", ["bash", "-c", argv[2]], os.environ, setsigmask=(),
                            setsigdef=(signal.SIGPIPE, signal.SIGXFSZ, signal.SIGTERM, signal.SIGINT,
                                       signal.SIGHUP))
    exited = os.pidfd_open(child)
    poller = select.poll()
    poller.register(exited, select.POLLIN)
    poller.register(lifeline, select.POLLIN | select.POLLHUP)
    while True:
        try:
            ready = {fd for fd, _ in poller.poll()}
        except InterruptedError:
            continue
        if exited in ready:
            _, raw = os.waitpid(child, 0)
            code = os.waitstatus_to_exitcode(raw)
            code = code if code >= 0 else 128 - code
            try:
                os.write(status, f"{code}\n".encode())   # a few bytes to an empty pipe: never blocks
            except OSError:
                pass                            # fail-direction: the supervisor is gone; the group still ends
            os.killpg(0, signal.SIGKILL)        # the command is done: nothing it left in the group outlives it
        if lifeline in ready:
            os.killpg(0, signal.SIGKILL)        # the supervisor is gone: the whole group ends with it


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))       # reached only for a usage error: every other way out is the group KILL
