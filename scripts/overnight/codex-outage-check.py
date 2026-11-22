#!/usr/bin/env python3
"""Is the Codex BACKEND down? Checked by the review broker before every dispatch.

Why: on 2026-09-03 (run-20260903-160406.log:423-436) a design review hit a Codex
backend outage (`unexpected status 404`) and the run dispatched and waited five
times, 10 calls and ~4m20s, before it concluded "outage" and deferred. Each leg
already SAYS it failed at the transport level; nothing read that before sending
the next one.

Rule: walk the broker manifest newest-first over COMPLETED legs dispatched in the
last WINDOW seconds. If the newest THRESHOLD of them all failed with a backend
transport signature, the backend is treated as down: exit 75 (EX_TEMPFAIL) and
print why, so the broker refuses and the run defers the review instead of
retrying. A successful leg, or the window passing, clears it.

What counts as an outage (measured over 3,422 legs on 2026-09-28): an HTTP
status from the backend, `stream disconnected before completion`, and `Selected
model is at capacity`. NOT counted: `app-server exited unexpectedly` (a one-leg
crash whose documented answer is re-dispatching that leg), a local `ENOENT` in
the companion's temp-file cleanup, content flags, and review-side errors such as
`Not a valid object name`. Those are not the backend being down.

Usage: codex-outage-check.py [--manifest PATH] [--window S] [--threshold N] [--now EPOCH]
Exit: 0 dispatch allowed, 75 backend down (reason on stdout).
"""
from __future__ import annotations

import argparse
import errno
import json
import os
import re
import sys
import time
from pathlib import Path

OUTAGE_RE = re.compile(
    r"Codex error: (unexpected status \d{3}|stream disconnected before completion|Selected model is at capacity)")


_RESULT = None


def _review_result():
    """review_result.py, beside this file: the one status parser and regular-file opener."""
    global _RESULT
    if _RESULT is None:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "review_result_for_outage", os.path.join(os.path.dirname(os.path.abspath(__file__)), "review_result.py"))
        _RESULT = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(_RESULT)
    return _RESULT


def _read_sized(path) -> bytes:
    """A regular file's bytes, read to its size at open; OSError when it cannot be, or shrank while read."""
    rr = _review_result()
    fd = rr.open_regular(str(path), os.O_RDONLY)
    try:
        left, parts = os.fstat(fd).st_size, []
        while left > 0:
            chunk = os.read(fd, min(1 << 20, left))
            if not chunk:
                raise OSError(errno.ENODATA, f"{path} shrank while it was read")
            parts.append(chunk)
            left -= len(chunk)
    finally:
        os.close(fd)
    return b"".join(parts)


def leg_state(log: Path):
    """('ok'|'outage'|'other'|'running', signature) for one leg's artifact."""
    # The dispatcher's <log>.done holds the exit status once the review has exited, read by the one parser
    # every reader uses; until then the leg is running, whatever its log says. A damaged status is no exit
    # status: never an outage, and a status that cannot be read is not known to have ended.
    done = str(log) + ".done"
    rc, error = _review_result().read_status_file(done)
    if rc is None:
        return ("other", "") if error is None and os.path.lexists(done) else ("running", "")
    if rc == "0":
        return "ok", ""
    try:
        text = _read_sized(log).decode("utf-8", "replace")
    except OSError:
        return "other", ""
    o = OUTAGE_RE.findall(text)
    return ("outage", o[-1]) if o else ("other", "")


def check(manifest: Path, window: int, threshold: int, now: float):
    try:
        lines = _read_sized(manifest).decode("utf-8", "replace").splitlines()
    except OSError:
        return True, ""
    # Completed attempts launched within the window, newest FINISH first:
    # reviews overlap, so the one that finished last says how the backend is
    # now, whichever was launched first.
    attempts = []
    for line in reversed(lines[-200:]):
        try:
            rec = json.loads(line)
        except ValueError:
            continue
        ts = rec.get("ts")
        # Rows are appended at finish, keyed by launch: an old launch can sit
        # after newer ones, so skip it and keep scanning the bounded tail.
        if not isinstance(ts, (int, float)) or now - ts > window:
            continue
        # A receipt the broker's own job wrote (parent = that job) is the same
        # attempt as the broker's row: count each attempt once.
        if rec.get("parent"):
            continue
        log = Path(rec.get("logFile", ""))
        state, sig = leg_state(log)
        if state == "running":
            continue
        try:
            finished = os.stat(str(log) + ".done").st_mtime
        except OSError:  # fail-direction: an unreadable time orders the attempt by its launch
            finished = float(ts)
        attempts.append((finished, float(ts), state, sig, rec.get("kind", "?")))
    attempts.sort(reverse=True)
    streak, sigs = 0, []
    for _finished, _ts, state, sig, kind in attempts:
        if state != "outage":
            break
        streak += 1
        sigs.append(f"{kind}: {sig}")
        if streak >= threshold:
            return False, (f"the last {streak} completed Codex legs (within {window // 60} min) failed at the "
                           f"backend: " + "; ".join(sigs))
    return True, ""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", type=Path, default=Path(".claude/overnight/reviews/manifest.jsonl"))
    ap.add_argument("--window", type=int, default=1200)
    ap.add_argument("--threshold", type=int, default=2)
    ap.add_argument("--now", type=float, default=None)
    a = ap.parse_args()
    ok, why = check(a.manifest, a.window, a.threshold, a.now if a.now is not None else time.time())
    if ok:
        return 0
    print(why)
    return 75


if __name__ == "__main__":
    sys.exit(main())
