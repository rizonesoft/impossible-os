#!/usr/bin/env python3
"""review_result.py: the one publisher of a review attempt's result.

    review_result.py claim LOG
    review_result.py publish LOG STATUS [--prompt FILE] [--claim]
    review_result.py discard FILE

A result has ONE OWNER: whoever first creates LOG exclusively (the detached job at its start, or the broker's
supervisor once it ended an attempt it had not handed off). `claim` creates it: exit 0 when this caller owns
it, 4 when another already does, 1 when it could not be created. The claim and the publication of one result are
always made by one process (the detached job's supervisor in-process, or one `publish --claim` call), so
no hand-off between processes can leave a claimed result unpublished. `publish` appends "Turn completed (rc=S)"
to LOG and then publishes S in LOG.done (written beside it and renamed over it, so a reader sees all or
nothing); `--claim` claims first and leaves a result another owner holds alone (exit 0). `discard` removes
the prompt of an attempt that was never registered, so no result is owed for it.

Every required step is checked: a failed append, write or rename exits 1 (the status is not published), and
the removal of FILE that follows is cleanup, reported but never able to turn a failed publication into a
success or a published one into a failure.
"""
import os
import re
import sys


def claim(log: str) -> int:
    try:
        os.close(os.open(log, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0), 0o644))
    except FileExistsError:
        return 4
    except OSError as exc:
        print(f"review_result: {log} could not be created ({exc.strerror})", file=sys.stderr)
        return 1
    return 0


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
    if len(argv) == 2 and argv[0] in ("claim", "discard"):
        return (claim if argv[0] == "claim" else discard)(argv[1])
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
    print("usage: review_result.py claim LOG | publish LOG STATUS [--prompt FILE] [--claim] | discard FILE",
          file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
