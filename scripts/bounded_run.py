#!/usr/bin/env python3
"""Run a command bounded in time, as one process group that never outlives the call.

Every git command the review candidate reader (scripts/review_candidate.py) and the dash check
(scripts/staged_dash_check.py) start goes through `run`. git starts helpers of its own (a clean filter, a
filesystem monitor hook, ssh, a credential helper), and a helper inherits git's pipes: ending git alone on a
timeout leaves the helper holding them, and a reader waiting for their end never returns. So the command runs
in its own process group, its output is read by threads the call waits on against one deadline, and on a
timeout, on Ctrl-C and on SIGTERM the whole group is ended before the pipes are waited on again.

The signals are caught before the group starts and held until it is registered, so no moment leaves it
running unowned; a cancellation then ends the group and is delivered again to the caller's own handler. A
timeout is a ValueError naming `what`, never the command, since a command line can carry a URL with a token.
A helper that leaves the group on its own (setsid) is outside what a process group can end.
"""
import os
import signal
import subprocess
import threading
import time

CHUNK = 1 << 20
STDERR_KEEP = 1 << 16        # the end of a long stderr is what names the failure
GRACE = 5                    # seconds to wait for the pipes to close once the group is ended


class Cancelled(BaseException):
    """Ctrl-C or SIGTERM reached the caller while a bounded command ran."""


def _end_group(proc):
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:                             # the group has already exited: nothing to end
        pass
    proc.wait()


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


def run(cmd, what, *, timeout, env=None, stdin=None, sink=None, keep_stderr=False):
    """(exit status, stdout, stderr) of `cmd`. stdout is handed to `sink` chunk by chunk when one is given
    (and returned empty), else returned whole; stderr is kept (its last STDERR_KEEP bytes) only when
    `keep_stderr`, else discarded. `stdin` is bytes to write, or None for no input."""
    st = {"proc": None, "hold": True, "pending": 0}

    def on_signal(signum, _frame):
        if st["hold"]:
            st["pending"] = st["pending"] or signum        # not registered, or being ended: deliver later
            return
        raise Cancelled(signum)

    watched = [signal.SIGTERM]
    if signal.getsignal(signal.SIGINT) is signal.default_int_handler:
        watched.append(signal.SIGINT)                      # an ignored SIGINT (a background caller) stays ignored
    previous = {sig: signal.signal(sig, on_signal) for sig in watched}

    def finish(signum=0):
        """Put the caller's handlers back with the watched signals blocked, so none that arrives meanwhile is
        recorded here and lost; one held by the block reaches the caller's handler as the mask lifts, and one
        recorded earlier (or this cancellation) is delivered again."""
        old = signal.pthread_sigmask(signal.SIG_BLOCK, watched)
        try:
            for sig, handler in previous.items():
                signal.signal(sig, handler)
            signum = signum or st["pending"]
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, old)
        if signum:
            signal.raise_signal(signum)
            raise SystemExit(128 + signum)                 # the caller's handler returned: still a cancellation

    out, err, errors, threads = [], [], [], []

    def keep(chunk):
        err.append(chunk)
        while len(err) > 1 and sum(map(len, err)) - len(err[0]) >= STDERR_KEEP:
            err.pop(0)

    try:
        st["proc"] = proc = subprocess.Popen(
            cmd, stdin=subprocess.DEVNULL if stdin is None else subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE if keep_stderr else subprocess.DEVNULL, env=env,
            start_new_session=True)  # LIFECYCLE-WAIVER: waited on here; on timeout or cancellation its whole group is ended
        st["hold"] = False
        if st["pending"]:
            raise Cancelled(st["pending"])
        deadline = time.monotonic() + timeout
        threads.append(threading.Thread(target=_pump, args=(proc.stdout, sink or out.append, errors), daemon=True))
        if keep_stderr:
            threads.append(threading.Thread(target=_pump, args=(proc.stderr, keep, errors), daemon=True))
        if stdin is not None:
            threads.append(threading.Thread(target=_feed, args=(proc.stdin, stdin, errors), daemon=True))
        for t in threads:
            t.start()
        rc = proc.wait(timeout=timeout)
        for t in threads:
            t.join(max(0.0, deadline - time.monotonic()))
            if t.is_alive():                               # a helper still holds a pipe at the deadline
                raise subprocess.TimeoutExpired(what, timeout)
        st["hold"] = True
    except BaseException as exc:
        st["hold"] = True
        if st["proc"] is not None:
            _end_group(st["proc"])
            for t in threads:
                t.join(GRACE)                              # the pipes close with the group
        finish(exc.args[0] if isinstance(exc, Cancelled) else 0)
        if isinstance(exc, subprocess.TimeoutExpired):
            raise ValueError(f"{what} did not answer within {timeout} s") from None
        raise
    finish()
    if errors:
        raise errors[0]
    return rc, b"".join(out), b"".join(err)
