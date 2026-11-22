#!/usr/bin/env python3
"""review_result.py: the one publisher of a review attempt's result.

    review_result.py claim LOG
    review_result.py recover LOG
    review_result.py publish LOG STATUS [--prompt FILE] [--claim]
    review_result.py discard FILE

A result has ONE OWNER: whoever first creates LOG exclusively (the detached job at its start, or the broker's
supervisor once it ended an attempt it had not handed off). `claim` creates it: exit 0 when this caller owns
it, 4 when another already does, 1 when it could not be created. The claim and the publication of one result are
always made by one process (the detached job's supervisor in-process, or one `publish --claim` call), so
no hand-off between processes can leave a claimed result unpublished.

THE OWNER IS KNOWN TO BE ALIVE. A claim first takes an exclusive lock on LOG.owner and holds it for the
owner's whole life (the kernel releases it when the process ends, however it ends), and only then creates
LOG. The owner of a detached job records its review's process group there. `recover` (which the verdict
waiter runs on a log with no status) takes that lock: a lock it gets with no LOG.done means the owner ended
without publishing (an OOM kill, a crash), so it ends what is left of that process group and publishes 1.
A live owner holds the lock, so recovery never touches its result. `publish` appends "Turn completed (rc=S)"
to LOG and then publishes S in LOG.done (written beside it and renamed over it, so a reader sees all or
nothing); `--claim` claims first and leaves a result another owner holds alone (exit 0). `discard` removes
the prompt of an attempt that was never registered, so no result is owed for it.

Every required step is checked: a failed append, write or rename exits 1 (the status is not published), and
the removal of FILE that follows is cleanup, reported but never able to turn a failed publication into a
success or a published one into a failure.
"""
import fcntl
import os
import re
import signal
import sys


CLOEXEC = getattr(os, "O_CLOEXEC", 0)
_HELD: dict[str, int] = {}     # LOG -> its owner lock, held (close-on-exec) until this process ends


def _owner(log: str) -> str:
    return log + ".owner"


def claim(log: str) -> int:
    """0 when this process now owns LOG's result (and holds its owner lock until it ends), 4 when another
    owner has it (alive, or its result already created), 1 on an error."""
    try:
        fd = os.open(_owner(log), os.O_RDWR | os.O_CREAT | CLOEXEC, 0o644)
    except OSError as exc:
        print(f"review_result: {_owner(log)} could not be opened ({exc.strerror})", file=sys.stderr)
        return 1
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        os.close(os.open(log, os.O_WRONLY | os.O_CREAT | os.O_EXCL | CLOEXEC, 0o644))
    except (BlockingIOError, FileExistsError):
        os.close(fd)
        return 4
    except OSError as exc:
        os.close(fd)
        print(f"review_result: {log} could not be claimed ({exc.strerror})", file=sys.stderr)
        return 1
    _HELD[log] = fd
    return 0


def _stat(pid: int):
    """(process group, start time) of a process from /proc, or None when it is gone."""
    try:
        with open(f"/proc/{pid}/stat", "rb") as f:
            fields = f.read().rsplit(b") ", 1)[1].split()
    except (OSError, IndexError):
        return None
    return int(fields[2]), int(fields[19])


def record_attempt(log: str, leader: int) -> None:
    """Record the review's process group (its leader's id and start time) in the held owner file, so a
    recovery after this owner's death can end what is left of it. A record that cannot be written is
    reported; recovery then still publishes the result."""
    fd, st = _HELD.get(log), _stat(leader)
    if fd is None or st is None:
        return
    try:
        os.ftruncate(fd, 0)
        os.pwrite(fd, f"{leader} {st[1]}\n".encode(), 0)
    except OSError as exc:
        print(f"review_result: the review's process group could not be recorded ({exc.strerror})",
              file=sys.stderr)


def _end_group(pgid: int, start: int) -> None:
    """KILL every member of the recorded process group. The group is the recorded one: its leader, while it
    exists, has the recorded start time, and a group id is never given to a new group while a member of the
    old one is alive. Each member is signalled through a pidfd taken and checked first."""
    lead = _stat(pgid)
    if lead is not None and lead[1] != start:
        return                                     # the id now names another process
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        pid = int(name)
        st = _stat(pid)
        if st is None or st[0] != pgid:
            continue
        try:
            pidfd = os.pidfd_open(pid)
        except OSError:
            continue
        try:
            st = _stat(pid)
            if st is not None and st[0] == pgid:
                signal.pidfd_send_signal(pidfd, signal.SIGKILL)
        except OSError:
            pass                                   # fail-direction: it ended meanwhile
        finally:
            os.close(pidfd)


def recover(log: str) -> int:
    """Publish 1 for a result whose owner ended without publishing it, ending what is left of its review.
    0 when there was nothing to do (a status is published, the owner is alive, or no owner was recorded)
    or the result was recovered; 1 on an error."""
    if os.path.lexists(log + ".done"):
        return 0
    try:
        fd = os.open(_owner(log), os.O_RDWR | CLOEXEC)
    except FileNotFoundError:
        return 0                                   # fail-direction: a result from before the owner lock
    except OSError as exc:
        print(f"review_result: {_owner(log)} could not be opened ({exc.strerror})", file=sys.stderr)
        return 1
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return 0                               # its owner is alive and publishes it
        if os.path.lexists(log + ".done") or not os.path.lexists(log):
            return 0                               # published meanwhile, or the owner died before creating it
        record = os.pread(fd, 64, 0).split()
        if len(record) == 2 and record[0].isdigit() and record[1].isdigit():
            _end_group(int(record[0]), int(record[1]))
        try:
            out = os.open(log, os.O_WRONLY | os.O_APPEND | CLOEXEC)
            try:
                _write_all(out, b"review_result: the result's owner ended without publishing it; recorded as 1\n")
            finally:
                os.close(out)
        except OSError:
            pass                                   # fail-direction: publish below reports a log it cannot write
        return publish(log, "1")
    finally:
        os.close(fd)


def _write_all(fd: int, data: bytes) -> None:
    view = memoryview(data)
    while view:
        view = view[os.write(fd, view):]


def discard(prompt: str) -> int:
    try:
        os.unlink(prompt)
    except FileNotFoundError:
        return 0                           # fail-direction: the prompt was never written
    except OSError as exc:
        print(f"review_result: {prompt} could not be removed ({exc.strerror})", file=sys.stderr)
        return 1
    return 0


def publish(log: str, status: str, prompt: str = "", *, claim_first: bool = False) -> int:
    if not re.fullmatch(r"[0-9]{1,3}", status):
        print(f"review_result: {status!r} is not a status", file=sys.stderr)
        return 2
    if claim_first:
        owned = claim(log)
        if owned == 4:
            return 0                       # another owner holds the result and publishes it
        if owned != 0:
            return 1
    tmp = f"{log}.done.tmp"
    try:
        fd = os.open(log, os.O_WRONLY | os.O_APPEND | getattr(os, "O_CLOEXEC", 0))
        try:
            _write_all(fd, f"Turn completed (rc={status})\n".encode())
        finally:
            os.close(fd)
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | getattr(os, "O_CLOEXEC", 0), 0o644)
        try:
            _write_all(fd, f"{status}\n".encode())
        finally:
            os.close(fd)
        os.replace(tmp, f"{log}.done")
    except OSError as exc:
        print(f"review_result: the result ({status}) could not be published to {log}.done ({exc.strerror})",
              file=sys.stderr)
        return 1
    if prompt:
        discard(prompt)                    # cleanup: reported, never the publication's status
    return 0


def main(argv) -> int:
    if len(argv) == 2 and argv[0] in ("claim", "discard", "recover"):
        return {"claim": claim, "discard": discard, "recover": recover}[argv[0]](argv[1])
    if len(argv) >= 3 and argv[0] == "publish":
        log, status, rest = argv[1], argv[2], argv[3:]
        prompt, claim_first = "", False
        while rest:
            flag = rest.pop(0)
            if flag == "--prompt" and rest:
                prompt = rest.pop(0)
            elif flag == "--claim":
                claim_first = True
            else:
                break
        else:
            return publish(log, status, prompt, claim_first=claim_first)
    print("usage: review_result.py claim LOG | recover LOG | publish LOG STATUS [--prompt FILE] [--claim] | discard FILE",
          file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
