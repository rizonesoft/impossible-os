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
import json
import re
import sys
import time
from pathlib import Path

OUTAGE_RE = re.compile(
    r"Codex error: (unexpected status \d{3}|stream disconnected before completion|Selected model is at capacity)")
DONE_RE = re.compile(r"^Turn completed \(rc=(\d+)\)\s*$", re.M)


def leg_state(log: Path):
    """('ok'|'outage'|'other'|'running', signature) for one leg's artifact."""
    try:
        text = log.read_text(errors="replace")
    except OSError:
        return "other", ""
    m = DONE_RE.findall(text)
    if not m:
        return "running", ""
    if m[-1] == "0":
        return "ok", ""
    o = OUTAGE_RE.findall(text)
    return ("outage", o[-1]) if o else ("other", "")


def check(manifest: Path, window: int, threshold: int, now: float):
    try:
        lines = manifest.read_text(errors="replace").splitlines()
    except OSError:
        return True, ""
    streak, sigs = 0, []
    for line in reversed(lines[-200:]):
        try:
            rec = json.loads(line)
        except ValueError:
            continue
        ts = rec.get("ts")
        if not isinstance(ts, (int, float)) or now - ts > window:
            break
        state, sig = leg_state(Path(rec.get("logFile", "")))
        if state == "running":
            continue
        if state != "outage":
            break
        streak += 1
        sigs.append(f"{rec.get('kind', '?')}: {sig}")
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
