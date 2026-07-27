#!/usr/bin/env python3
"""Offload-reminder follow-rate report (reminder->gate promotion evidence).

Reads .claude/state/offload-events.jsonl (written by the reminder hooks and
the agent-dispatch recorder via _offload_log) and pairs every reminder 'fire'
with whether an Agent dispatch followed within the window (default 10 min).

  follow rate ~ 1.0  -> the reminder was consistently right; promoting it to
                        a hard gate blocks almost nothing legitimate.
  follow rate << 1.0 -> the un-followed fires are either model non-compliance
                        (gate helps) or false positives (gate hurts) --
                        sample the 'unfollowed' lines before promoting.

Usage: offload-report.py [PROJECT_DIR] [--window-s N]
"""
from __future__ import annotations

import json
import sys
from collections import defaultdict
from pathlib import Path


def main(argv) -> int:
    project = Path(argv[0]) if argv and not argv[0].startswith("-") else Path(".")
    window_s = 600
    if "--window-s" in argv:
        window_s = int(argv[argv.index("--window-s") + 1])
    path = project / ".claude/state/offload-events.jsonl"
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        print("no offload events recorded yet", file=sys.stderr)
        return 1
    events = []
    for ln in lines:
        try:
            e = json.loads(ln)
            if isinstance(e.get("ts"), int):
                events.append(e)
        except ValueError:
            continue
    events.sort(key=lambda e: e["ts"])
    # T3-3: the convergence gate is not a reminder -- it has no "follow", it
    # either suppressed a Codex round or it did not. Report it as a ratio so
    # the saving is COUNTABLE. A suppression count alone cannot say whether the
    # gate works or is simply never consulted, which is how the agent cache sat
    # at 155 stores / 0 hits unnoticed.
    conv = sum(1 for e in events if e.get("kind") == "converged")
    redis = sum(1 for e in events if e.get("kind") == "redispatch")
    if conv or redis:
        tot = conv + redis
        print(f"review convergence: {conv}/{tot} rounds suppressed "
              f"({100 * conv // max(tot, 1)}%)")
        print(f"  each suppressed round removes a Codex verdict body, its "
              f"finding triage, and a ~6.2 KB receiving-code-review body "
              f"from the main context.")
        if conv == 0 and redis:
            print("  NOTE: 0 suppressed. Either inputs genuinely moved every "
                  "round, or the gate is not being consulted -- check that the "
                  "review skills call `should-redispatch` before each kind.")
        print()

    dispatch_ts = [e["ts"] for e in events if e.get("kind") == "dispatch"]
    # R2 (2026-07-19): non-Agent compliance. A hook whose sanctioned follow-up
    # is a rerouted COMMAND (build_offload_reminder -> run-artifact.sh) logs
    # kind="follow"; counting only Agent dispatches misread the working P3.4
    # block as a 9% follow rate. Follows are per-hook, dispatches stay global.
    follow_ts = defaultdict(list)
    for e in events:
        if e.get("kind") == "follow":
            follow_ts[e.get("hook", "?")].append(e["ts"])

    stats = defaultdict(lambda: {"fires": 0, "followed": 0, "unfollowed": []})
    for e in events:
        if e.get("kind") != "fire":
            continue
        hook = e.get("hook", "?")
        s = stats[hook]
        s["fires"] += 1
        if (any(e["ts"] <= t <= e["ts"] + window_s for t in dispatch_ts)
                or any(e["ts"] <= t <= e["ts"] + window_s
                       for t in follow_ts[hook])):
            s["followed"] += 1
        else:
            s["unfollowed"].append(f"ts={e['ts']} {e.get('detail', '')}")

    total_dispatches = len(dispatch_ts)
    total_follows = sum(len(v) for v in follow_ts.values())
    print(f"offload events: {len(events)} ({total_dispatches} dispatches, "
          f"{total_follows} follows), follow window {window_s}s")
    # A hook can be compliant on every attempt (wrapped from the start ->
    # follow events, zero fires); surface it instead of looking unused.
    for hook in follow_ts:
        stats[hook]  # materialize the row
    for hook, s in sorted(stats.items()):
        rate = s["followed"] / s["fires"] if s["fires"] else 0.0
        print(f"  {hook:<28} fires={s['fires']:>4} followed={s['followed']:>4} "
              f"rate={rate:.0%} follows={len(follow_ts[hook]):>4}")
        for u in s["unfollowed"][-3:]:
            print(f"      unfollowed: {u}")
    if not stats:
        print("  (no reminder fires recorded)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
