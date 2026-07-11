#!/usr/bin/env python3
"""Classify a finished overnight run and trip the unproductive-run circuit breaker.

Lesson source (2026-07-04, retired Codex overnight runner): the watchdog
relaunched every ~10 minutes into the same environment blocker nine times in
~100 minutes (~32M input tokens, zero ships). The launcher's oracle-BLOCKED
backoff never fired because the queue genuinely had NEEDS_WORK; the failure
was each RUN dying unproductively, which nothing counted. This helper counts
it, so a persistent environment failure backs the watchdog off instead of
burning tokens at full cadence all night.

Outcome model (fail-open: git errors -> "unknown", no state change):
  productive   -- HEAD moved during the run (work landed), or the agent exited
                  0 after a substantive session (>= --min-secs, default 900s).
  checkpoint   -- the session ended in a verified ROLLOVER (guard state
                  carries `rollover.pending`): a planned context rotation,
                  short and commit-free BY DESIGN. Counting it unproductive
                  would trip the breaker after 3 rollovers. Streak is left
                  UNTOUCHED -- not reset -- so a genuinely dead loop
                  interleaved with rollovers still trips on its own runs.
  unproductive -- the agent exited nonzero, or exited 0 quickly with no HEAD
                  move (classic dead-on-arrival relaunch).
  snoozed      -- a usage-limit snooze was recorded for this run; the snooze
                  file already governs cadence, streak untouched.

Breaker: at --threshold (default 3) consecutive unproductive runs, write the
same watchdog-backoff-until file the launcher pre-flight honors (escalating
30 min per extra failure, capped at 60 min). The run stays ARMED -- doctrine
says only an oracle-verified fixpoint or the human operator disarms; the
launcher surfaces the trip via notify.sh + NEEDS-OPERATOR.md.

Usage:
  run-outcome.py PROJECT_DIR --exit N --start-head SHA --run-secs S
                 [--snoozed] [--end-head SHA] [--min-secs N] [--threshold N]

Prints a one-line JSON decision to stdout. Stdlib only.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

BACKOFF_CAP_SECS = 3600
BACKOFF_STEP_SECS = 1800


def _git_head(project_dir: Path) -> str:
    try:
        r = subprocess.run(
            ["git", "-C", str(project_dir), "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=15,
        )
        return r.stdout.strip() if r.returncode == 0 else ""
    except Exception:
        return ""


def classify(exit_code: int, start_head: str, end_head: str,
             run_secs: int, min_secs: int) -> str:
    if not start_head or not end_head:
        return "unknown"
    if end_head != start_head:
        return "productive"
    if exit_code == 0 and run_secs >= min_secs:
        return "productive"
    return "unproductive"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("project_dir")
    ap.add_argument("--exit", dest="exit_code", type=int, required=True)
    ap.add_argument("--start-head", required=True)
    ap.add_argument("--run-secs", type=int, required=True)
    ap.add_argument("--end-head", default=None)
    ap.add_argument("--snoozed", action="store_true")
    ap.add_argument("--min-secs", type=int, default=900)
    ap.add_argument("--threshold", type=int, default=3)
    ap.add_argument("--now", type=int, default=None, help="epoch override (tests)")
    args = ap.parse_args()

    project = Path(args.project_dir)
    state_dir = project / ".claude" / "overnight" / "state"
    streak_file = state_dir / "unproductive-streak"
    backoff_file = project / ".claude" / "overnight" / "watchdog-backoff-until"
    now = args.now if args.now is not None else int(time.time())

    decision = {"outcome": "unknown", "streak": 0, "breaker": False,
                "backoff_until": None}

    if args.snoozed:
        decision["outcome"] = "snoozed"
        print(json.dumps(decision))
        return 0

    # A planned VERIFIED-ROLLOVER exit is not unproductive: the guard state
    # says the session ended ON PURPOSE (context rotation) and the watchdog
    # relaunches. Streak untouched (see outcome model).
    try:
        guard = json.loads(
            (project / ".claude/state/sequencer-run.json").read_text())
        rolling = bool((guard.get("rollover") or {}).get("pending"))
        if rolling:
            decision["outcome"] = "checkpoint"
            decision["checkpoint_kind"] = "rollover"
            print(json.dumps(decision))
            return 0
    except Exception:
        pass  # fail toward normal classification

    end_head = args.end_head if args.end_head is not None else _git_head(project)
    outcome = classify(args.exit_code, args.start_head, end_head,
                       args.run_secs, args.min_secs)
    decision["outcome"] = outcome

    if outcome == "productive":
        try:
            streak_file.unlink()
        except FileNotFoundError:
            pass
        except OSError:
            pass
        print(json.dumps(decision))
        return 0
    if outcome == "unknown":
        print(json.dumps(decision))
        return 0

    count = 0
    try:
        count = int(json.loads(streak_file.read_text()).get("count", 0))
    except Exception:
        count = 0
    count += 1
    decision["streak"] = count
    try:
        state_dir.mkdir(parents=True, exist_ok=True)
        streak_file.write_text(json.dumps(
            {"count": count, "last": now,
             "reason": f"exit={args.exit_code} run_secs={args.run_secs} head_moved=false"}))
    except OSError:
        pass

    if count >= args.threshold:
        delay = min(BACKOFF_STEP_SECS * (count - args.threshold + 1), BACKOFF_CAP_SECS)
        until = now + delay
        decision["breaker"] = True
        decision["backoff_until"] = until
        try:
            # Same "epoch count" shape the launcher's BLOCKED path writes/reads.
            backoff_file.write_text(f"{until} {count}\n")
        except OSError:
            pass

    print(json.dumps(decision))
    return 0


if __name__ == "__main__":
    sys.exit(main())
