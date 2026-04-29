#!/usr/bin/env bash
# heuristic-misses-report.sh -- summarize .claude/state/heuristic-misses.jsonl
# per implement-todo-section step (TODO-08 partial-enforcement heuristics).
#
# For each step that has emitted at least one miss, print:
#   - total miss count
#   - false-positive ratio (user_flagged / total)
#   - the most-recent 3 misses with TODO path + section
#
# Read-only; runs in <1s on logs up to several MiB. Used to feed the
# WARN -> ERROR promotion decision documented in
# docs/infrastructure/ai-system.md "Hook Promotion Pipeline".

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG="${HEURISTIC_MISSES_LOG:-$REPO_ROOT/.claude/state/heuristic-misses.jsonl}"

if [ ! -f "$LOG" ]; then
    echo "[heuristic-misses-report] no log at $LOG -- no misses recorded yet"
    exit 0
fi

if [ ! -s "$LOG" ]; then
    echo "[heuristic-misses-report] log empty -- no misses recorded yet"
    exit 0
fi

python3 - "$LOG" <<'PY'
import json
import sys
from collections import defaultdict

log_path = sys.argv[1]
by_step = defaultdict(list)
parse_errors = 0

with open(log_path, "r", encoding="utf-8") as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
        except Exception:
            parse_errors += 1
            continue
        if not isinstance(rec, dict):
            parse_errors += 1
            continue
        step = rec.get("step")
        if not isinstance(step, int):
            parse_errors += 1
            continue
        by_step[step].append(rec)

if not by_step:
    print("[heuristic-misses-report] no parseable miss records")
    if parse_errors:
        print(f"[heuristic-misses-report]   {parse_errors} unparseable line(s)")
    sys.exit(0)

print("=" * 72)
print("Heuristic miss summary -- implement-todo-section partial-enforcement")
print("=" * 72)
total = sum(len(rs) for rs in by_step.values())
print(f"Total misses across all steps: {total}")
if parse_errors:
    print(f"Unparseable lines: {parse_errors}")
print()

for step in sorted(by_step.keys()):
    rs = by_step[step]
    n = len(rs)
    fp = sum(1 for r in rs if r.get("false_positive_user_flagged") is True)
    ratio = (fp / n) if n else 0.0
    print(f"Step {step:>2} -- {n} miss(es), {fp} flagged FP ({ratio*100:.1f}%)")
    # Show the most-recent 3
    recent = sorted(rs, key=lambda r: r.get("ts_ns", 0), reverse=True)[:3]
    for r in recent:
        signal = r.get("signal", "")
        todo_path = r.get("todo_path", "") or "(no path)"
        section = r.get("section")
        sec_str = f"section-{section}" if isinstance(section, int) else "section-?"
        detail = r.get("detail", "")[:80]
        flag = " [FP]" if r.get("false_positive_user_flagged") is True else ""
        print(f"    - {signal}{flag} | {todo_path} {sec_str}")
        if detail:
            print(f"        {detail}")
    print()

print("Promotion criteria (per docs/infrastructure/ai-system.md):")
print("  - Step 1/2: <5% FP over 20 sections")
print("  - Step 3:   <10% FP over 30 sections")
print("  - Step 9/15/17/18: <10% FP over 20 sections")
print()
print("Mark a miss as false-positive:")
print("  scripts/heuristic-mark-false-positive.sh <step> [<n>]")
PY
