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

# Codex review C-M2 fix 2026-04-29: doctrine in
# docs/infrastructure/ai-system.md "Hook Promotion Pipeline" specifies
# the FP ratio is computed over the last N entries (20 for steps
# 1/2/9/15/17/18; 30 for step 3). Lifetime ratio is shown separately
# for context. Promotion decision uses the windowed value.
WINDOW_BY_STEP = {1: 20, 2: 20, 3: 30, 4: 30, 9: 20, 15: 20, 17: 20, 18: 20}

# Per-section dedup for steps where the same section can fire the gate
# multiple times (Phase 1 evidence-gate retry loop). Collapse by
# (todo_path, section), keeping the most-recent entry per group, so the
# windowed FP ratio counts sections, not attempts. Steps 1/2/3/9/15/17/18
# are first-edit / first-codex gates that fire once per section by
# construction, so their per-attempt sampling already equals per-section.
SECTION_SCOPED_STEPS = {4}

def _section_collapse(records):
    """Collapse records by (todo_path, section), keeping the most-recent
    per group. Records with missing metadata (empty todo_path or None
    section) stay UNIQUE -- keyed on ts_ns so each remains a distinct
    observation. This prevents the prior-round failure mode where every
    under-attributed BLOCK collapsed into one ('', None) bucket and
    hid the FP signal the report is supposed to surface.

    FP flags are aggregated (logical OR) across the group: if ANY
    record in the section was flagged false-positive, the collapsed
    record carries the flag. Without this, an earlier-flagged FP
    followed by a later unflagged retry would silently un-flag the
    section and understate the FP rate the threshold-tuning data
    relies on.
    """
    by_key = {}
    fp_seen = {}
    for r in records:
        todo_path = (r.get("todo_path", "") or "").strip()
        section = r.get("section")
        if todo_path and isinstance(section, int):
            key = (todo_path, section)
        else:
            # Non-collapsible: each record gets a unique key so it
            # survives as its own observation in the windowed FP ratio.
            key = ("__unique__", r.get("ts_ns", 0), id(r))
        if r.get("false_positive_user_flagged") is True:
            fp_seen[key] = True
        prior = by_key.get(key)
        if prior is None or r.get("ts_ns", 0) > prior.get("ts_ns", 0):
            by_key[key] = r
    out = []
    for key, r in by_key.items():
        if fp_seen.get(key) and r.get("false_positive_user_flagged") is not True:
            # Keep the latest record for display; lift the FP flag from
            # the group so the windowed ratio counts this section as FP.
            r = dict(r)
            r["false_positive_user_flagged"] = True
        out.append(r)
    return out

for step in sorted(by_step.keys()):
    rs = by_step[step]
    if step in SECTION_SCOPED_STEPS:
        rs = _section_collapse(rs)
    n_total = len(rs)
    fp_total = sum(1 for r in rs if r.get("false_positive_user_flagged") is True)
    ratio_total = (fp_total / n_total) if n_total else 0.0
    window = WINDOW_BY_STEP.get(step, 20)
    sorted_recent = sorted(rs, key=lambda r: r.get("ts_ns", 0), reverse=True)
    windowed = sorted_recent[:window]
    n_w = len(windowed)
    fp_w = sum(1 for r in windowed if r.get("false_positive_user_flagged") is True)
    ratio_w = (fp_w / n_w) if n_w else 0.0
    print(f"Step {step:>2} -- last-{window} window: {n_w} miss(es), {fp_w} FP "
          f"({ratio_w*100:.1f}%) | lifetime: {n_total} miss(es), {fp_total} FP "
          f"({ratio_total*100:.1f}%)")
    if n_w < window:
        remaining = window - n_w
        print(f"    (need {remaining} more observation(s) before promotion criterion applies)")
    # Show the most-recent 3
    recent = sorted_recent[:3]
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
print("  - Step 4:   <5% FP over 30 sections (Phase 1 evidence-gate; tighter")
print("              tolerance because the gate is foundational; section-27 doctrine)")
print("  - Step 9/15/17/18: <10% FP over 20 sections")
print()
print("Mark a miss as false-positive:")
print("  scripts/heuristic-mark-false-positive.sh <step> [<n>]")
PY
