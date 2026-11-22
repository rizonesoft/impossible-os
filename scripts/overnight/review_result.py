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
LOG. `recover` (which the verdict waiter runs on a log with no status) acts only once LOG exists, so it
never competes with a claim in progress, and then takes that lock: a lock it gets with LOG present and no
LOG.done means the owner ended without publishing (out of memory, a crash), and it publishes 1. A live owner
holds the lock, so recovery never touches its result. Recovery signals no process: the attempt's group
leader (review_warden.py) ends its own group when the owner ends. `publish` appends "Turn completed (rc=S)"
to LOG and then publishes S in LOG.done (written beside it and renamed over it, so a reader sees all or
nothing); `--claim` claims first and leaves a result another owner holds alone (exit 0). `discard` removes
the prompt of an attempt that was never registered, so no result is owed for it.

Every required step is checked: a failed append, write or rename exits 1 (the status is not published), and
the removal of FILE that follows is cleanup, reported but never able to turn a failed publication into a
success or a published one into a failure.
"""
import errno
import fcntl
import os
import re
import stat
import sys


CLOEXEC = getattr(os, "O_CLOEXEC", 0)
_HELD: dict[str, int] = {}     # LOG -> its owner lock, held (close-on-exec) until this process ends


def open_regular(path: str, flags: int, mode: int = 0o644) -> int:
    """`path` opened as a regular file, or OSError: opened without blocking, so a FIFO or device that replaced
    it fails (or is refused) at once instead of holding the open, then made blocking again for its writes."""
    fd = os.open(path, flags | os.O_NONBLOCK | CLOEXEC, mode)
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise OSError(errno.EINVAL, "not a regular file")
        os.set_blocking(fd, True)
    except BaseException:
        os.close(fd)
        raise
    return fd


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


NOTE = b"review_result: the result's owner ended without publishing it; recorded as 1\nTurn completed (rc=1)\n"


def _note_once(owner_fd: int, log: str) -> None:
    """NOTE written to `log` at the offset recorded in the owner file, recording the end of the log there
    first when no offset is recorded yet; a note already complete at that offset is left alone."""
    # Only a whole record is trusted: `note-at N` and its newline, nothing else. A record cut short (a write
    # that stopped part way) was written before the log was touched, so the note is placed afresh at the end.
    found = re.fullmatch(rb"note-at ([0-9]{1,20})\n", os.pread(owner_fd, 64, 0))
    out = open_regular(log, os.O_RDWR)
    try:
        if found:
            at = int(found.group(1))
        else:
            at = os.fstat(out).st_size
            os.ftruncate(owner_fd, 0)
            _pwrite_all(owner_fd, f"note-at {at}\n".encode(), 0)
        if os.pread(out, len(NOTE), at) == NOTE:
            return
        _pwrite_all(out, NOTE, at)
    finally:
        os.close(out)


def _pwrite_all(fd: int, data: bytes, at: int) -> None:
    """All of `data` at offset `at`, or OSError: a write that makes no progress raises."""
    view = memoryview(data)
    while view:
        n = os.pwrite(fd, view, at + len(data) - len(view))
        if n <= 0:
            raise OSError(28, "no progress writing")
        view = view[n:]


def read_status(log: str) -> tuple:
    """(status, error) for `log`: read_status_file on <log>.done."""
    return read_status_file(log + ".done")


def read_status_file(path: str) -> tuple:
    """THE status parser every reader uses (the commit gate and envelope through review_pin, the waiter,
    the recovery): (status, error) for the .done file `path`: the status it publishes when it is a small regular file holding one
    status (1 to 3 digits and a newline), read without blocking (a FIFO or device fails at once); None when
    there is none or the file is damaged (empty, cut short, not a status, not a regular file). `error` is
    the OSError met opening or reading a file that is there: what it holds is unknown, so it is never taken
    for a missing or damaged status (a recovery that replaced it could overwrite a published success)."""
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | CLOEXEC)
    except FileNotFoundError:
        return None, None
    except OSError as exc:
        return None, exc
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            return None, None
        data = os.read(fd, 33)
    except OSError as exc:
        return None, exc
    finally:
        os.close(fd)
    found = re.fullmatch(rb"([0-9]{1,3})\n?", data)
    return (found.group(1).decode() if found else None), None


def status(log: str) -> str | None:
    """THE status `log` publishes (read_status), or None when there is none, it is damaged, or it could not
    be read. The verdict waiter and the envelope read it here, so a damaged file is never a finished review."""
    return read_status(log)[0]


def _print_status(log: str) -> int:
    """0 and the status; 1 when there is none or it is damaged; 3 when it could not be read (its content is
    unknown: never a reason to recover or to dispatch the review again)."""
    found, error = read_status(log)
    if error is not None:
        print(f"review_result: {log}.done could not be read ({error.strerror})", file=sys.stderr)
        return 3
    if found is None:
        return 1
    print(found)
    return 0


def recover(log: str) -> int:
    """Publish 1 for a result whose owner ended without publishing it. 0 when there was nothing to do (a
    status is published, nothing is claimed yet, the owner is alive, or no owner was recorded) or the result
    was recovered; 1 on an error. A damaged status (an empty or unreadable .done) is no status: once the owner
    is gone it is replaced by 1, as a missing one is."""
    found, error = read_status(log)
    if error is not None:
        print(f"review_result: {log}.done could not be read ({error.strerror}); it is left as it is", file=sys.stderr)
        return 1
    if found is not None or not os.path.lexists(log):
        return 0                                   # published, or not claimed yet: a claim may be in progress
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
        found, error = read_status(log)
        if error is not None:
            print(f"review_result: {log}.done could not be read ({error.strerror}); it is left as it is",
                  file=sys.stderr)
            return 1                               # never replace a status that may be a published success
        if found is not None:
            return 0                               # published meanwhile
        # The note is written once, at one place in the log: that place is recorded in the owner file (held
        # locked) BEFORE the note is written, so a retry, even after a recovery that ended between the two,
        # writes the same bytes at the same offset. The log's mtime, which the waiter reads for activity,
        # then moves only while the note is still incomplete, and the stall is reported.
        try:
            _note_once(fd, log)
        except OSError:
            pass                                   # fail-direction: the status below is still written
        return publish(log, "1", log_line=False)
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


def publish(log: str, status: str, prompt: str = "", *, claim_first: bool = False, log_line: bool = True) -> int:
    if not re.fullmatch(r"[0-9]{1,3}", status):
        print(f"review_result: {status!r} is not a status", file=sys.stderr)
        return 2
    if claim_first:
        owned = claim(log)
        if owned == 4:
            return 0                       # another owner holds the result and publishes it
        if owned != 0:
            return 1
    # The status is written to a file created here, under a name no other publisher uses, so nothing that
    # took a shared temporary name (a FIFO, a stale file) can hold or corrupt the publication.
    tmp = f"{log}.done.{os.getpid()}-{os.urandom(4).hex()}.tmp"
    try:
        if log_line:
            fd = open_regular(log, os.O_WRONLY | os.O_APPEND)
            try:
                _write_all(fd, f"Turn completed (rc={status})\n".encode())
            finally:
                os.close(fd)
        fd = open_regular(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL)
        try:
            _write_all(fd, f"{status}\n".encode())
        finally:
            os.close(fd)
        os.replace(tmp, f"{log}.done")
    except OSError as exc:
        try:
            os.unlink(tmp)
        except OSError:
            pass                           # fail-direction: never created, or already renamed
        print(f"review_result: the result ({status}) could not be published to {log}.done ({exc.strerror})",
              file=sys.stderr)
        return 1
    if prompt:
        discard(prompt)                    # cleanup: reported, never the publication's status
    return 0


TAIL_BYTES = 16384
TAIL_LINES = 25


def tail(log: str) -> int:
    """The last lines of the transcript `log` (at most TAIL_LINES of its last TAIL_BYTES), printed; read only
    from a regular file opened without blocking, so a FIFO or device that replaced it fails at once (1)."""
    try:
        fd = open_regular(log, os.O_RDONLY)
    except OSError:
        return 1
    try:
        size = os.fstat(fd).st_size
        data = os.pread(fd, TAIL_BYTES, max(0, size - TAIL_BYTES))
    except OSError:
        return 1
    finally:
        os.close(fd)
    lines = data.decode("utf-8", "replace").splitlines()[-TAIL_LINES:]
    sys.stdout.write("".join(line + "\n" for line in lines))
    return 0


def main(argv) -> int:
    if len(argv) == 2 and argv[0] in ("claim", "discard", "recover", "status", "tail"):
        return {"claim": claim, "discard": discard, "recover": recover, "status": _print_status,
                "tail": tail}[argv[0]](argv[1])
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
