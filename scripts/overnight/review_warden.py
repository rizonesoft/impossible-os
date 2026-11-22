#!/usr/bin/env python3
"""The leader of one review attempt's process group: the attempt dies with its supervisor.

    review_warden.py LIFELINE_FD COMMAND

Started by review_runtime_limit.py in a session of its own, so this process is the group leader and the group's
id cannot be reused while it lives. It runs COMMAND (bash -c) in the same group and exits with its status
(128 + N for signal N). LIFELINE_FD is the read end of a pipe whose only write end the supervisor holds: it
reaches end of file when the supervisor ends, however it ends (an out-of-memory termination, a crash). Then
this process ends its own group with SIGKILL, itself included. Signalling its own group is the one signal
that needs no identity check, because the group is the one it leads. Nothing that stays in the group outlives
the supervisor, and a recovery that publishes the dead owner's result never has to find or signal a process.

The cancellations the supervisor sends to the group (TERM, INT, HUP) are ignored here, so COMMAND's own
answer to them is the status reported; the supervisor's KILL ends this process like any other member.
"""
import os
import select
import signal
import sys


def main(argv) -> int:
    if len(argv) != 2 or not argv[0].isdigit():
        print("usage: review_warden.py LIFELINE_FD COMMAND", file=sys.stderr)
        return 2
    lifeline = int(argv[0])
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, signal.SIG_IGN)
    os.set_inheritable(lifeline, False)
    # The command gets the dispositions a shell started directly would (Python ignores SIGPIPE and
    # SIGXFSZ; the cancellations ignored here are restored too).
    child = os.posix_spawnp("bash", ["bash", "-c", argv[1]], os.environ, setsigmask=(),
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
            return code if code >= 0 else 128 - code
        if lifeline in ready:
            os.killpg(0, signal.SIGKILL)        # the supervisor is gone: the whole group ends with it


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
