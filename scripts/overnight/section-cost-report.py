#!/usr/bin/env python3
"""Normalized per-section cost report + soft-SLO flags.

Joins the per-section metrics sidecar (stream-report JSONL) with the commits
the run produced, so cost comparisons are normalized by work size (changed
LOC, files) instead of raw tokens -- a hard VMM section must not make an
efficient configuration look worse than an easy docs section.

Usage: section-cost-report.py METRICS.jsonl [--project DIR] [--since ISO]

Per section: main-loop turns/output tokens, cache-read tokens, sidechain
share, agents dispatched, and (per commit in the section window when git data
is available) changed LOC + files. Soft-SLO flags (advisory, never
quality-cutting): high turn count with zero agent dispatches (offload miss),
output tokens per changed LOC outliers, cache-read collapse (possible prompt
instability).
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


def load(path: str) -> list:
    recs = []
    for ln in Path(path).read_text(encoding="utf-8").splitlines():
        ln = ln.strip()
        if ln:
            try:
                recs.append(json.loads(ln))
            except ValueError:
                pass
    return recs


def commit_stats(project: Path, since: str) -> list:
    try:
        out = subprocess.check_output(
            ["git", "-C", str(project), "log", "--since", since,
             "--pretty=%H %ct %s", "--shortstat"],
            text=True, timeout=30)
    except Exception:
        return []
    commits, cur = [], None
    for ln in out.splitlines():
        if not ln.strip():
            continue
        if not ln.startswith(" "):
            parts = ln.split(" ", 2)
            cur = {"sha": parts[0][:10], "epoch": int(parts[1]),
                   "subject": parts[2] if len(parts) > 2 else "",
                   "files": 0, "loc": 0}
            commits.append(cur)
        elif cur is not None:
            for tok in ln.split(","):
                tok = tok.strip()
                if tok.endswith(("changed",)) and "file" in tok:
                    cur["files"] = int(tok.split()[0])
                elif "insertion" in tok or "deletion" in tok:
                    cur["loc"] += int(tok.split()[0])
    return commits


def main(argv) -> int:
    if not argv:
        print("usage: section-cost-report.py METRICS.jsonl [--project DIR] "
              "[--since ISO]", file=sys.stderr)
        return 2
    recs = load(argv[0])
    project = Path(argv[argv.index("--project") + 1]) if "--project" in argv \
        else Path(".")
    since = argv[argv.index("--since") + 1] if "--since" in argv else "24 hours ago"
    commits = commit_stats(project, since)
    total_loc = sum(c["loc"] for c in commits) or 1
    total_files = sum(c["files"] for c in commits)

    print(f"{'sec':>4} {'turns':>6} {'out_tok':>9} {'cache_r':>11} "
          f"{'side%':>6} {'agents':>6}  flags")
    tot_out = tot_turns = 0
    for r in recs:
        out_t = r.get("output_tokens", 0)
        side = r.get("sidechain_output_tokens", 0)
        both = out_t + side or 1
        flags = []
        if r.get("turns", 0) > 150 and r.get("agent_dispatches", 0) == 0:
            flags.append("OFFLOAD-MISS")
        if r.get("cache_read_input_tokens", 0) < r.get("turns", 0) * 1000 \
                and r.get("turns", 0) > 20:
            flags.append("CACHE-COLD?")
        tot_out += out_t
        tot_turns += r.get("turns", 0)
        print(f"{r.get('section_index', '?'):>4} {r.get('turns', 0):>6} "
              f"{out_t:>9} {r.get('cache_read_input_tokens', 0):>11} "
              f"{100 * side // both:>5}% {r.get('agent_dispatches', 0):>6}  "
              f"{','.join(flags)}")
    print(f"\ncommits since '{since}': {len(commits)}, changed LOC {total_loc}, "
          f"files {total_files}")
    print(f"normalized: {tot_out / total_loc:.1f} main output tokens / changed "
          f"LOC; {tot_turns} main turns total")
    if commits:
        per = tot_out / max(1, len(commits))
        print(f"           {per:,.0f} main output tokens / shipped commit")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
