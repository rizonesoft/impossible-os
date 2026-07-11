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
    dispatch_ts = [e["ts"] for e in events if e.get("kind") == "dispatch"]

    stats = defaultdict(lambda: {"fires": 0, "followed": 0, "unfollowed": []})
    for e in events:
        if e.get("kind") != "fire":
            continue
        s = stats[e.get("hook", "?")]
        s["fires"] += 1
        if any(e["ts"] <= t <= e["ts"] + window_s for t in dispatch_ts):
            s["followed"] += 1
        else:
            s["unfollowed"].append(f"ts={e['ts']} {e.get('detail', '')}")

    total_dispatches = len(dispatch_ts)
    print(f"offload events: {len(events)} ({total_dispatches} dispatches), "
          f"follow window {window_s}s")
    for hook, s in sorted(stats.items()):
        rate = s["followed"] / s["fires"] if s["fires"] else 0.0
        print(f"  {hook:<28} fires={s['fires']:>4} followed={s['followed']:>4} "
              f"rate={rate:.0%}")
        for u in s["unfollowed"][-3:]:
            print(f"      unfollowed: {u}")
    if not stats:
        print("  (no reminder fires recorded)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
