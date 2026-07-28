#!/usr/bin/env python3
"""One cost table per run, in the units that actually drive the bill (T4-1).

WHY THIS EXISTS. Every prior cost effort in this repo optimized OUTPUT tokens,
because output is what a run report showed. Measured 2026-07-27: output is ~10%
of spend and CACHE-READ is ~81%. The backlog spent its attention on the small
slice for want of a number. This prints the numbers that would have redirected
it, in the same shape every run, so the next decision is made against measured
reality instead of an estimate.

SCOPING IS LOAD-BEARING AND EXPLICIT. Two of the four data sources are
append-only across runs:

  .claude/overnight/metrics/<run>.jsonl   PER-RUN     (tokens, turns, agents)
  .claude/state/tool-history.jsonl        cross-run   (tool calls)
  .claude/state/offload-events.jsonl      cross-run   (hook fires, cache hits)

The cross-run logs carry timestamps but no run id, so they are filtered to this
run's WINDOW -- start parsed from the metrics filename (`run-YYYYMMDD-HHMMSS`),
end from its mtime. Any figure that could not be scoped is labelled, never
silently reported as if it were this run's. A report that quietly mixes an
all-time count into a per-run table is worse than no report, because it reads as
authoritative -- that exact double-count is why two backlog items carried
inflated savings.

DOLLARS ARE RELATIVE SIZING, NOT A BILL. List-rate arithmetic at Opus prices, to
show which BUCKET dominates. Sidechain is priced at Opus as an upper bound even
though the fleet is Sonnet by doctrine; the conclusion (cache-read dominates)
only strengthens at Sonnet rates.

Usage: cost-summary.py METRICS.jsonl [--project DIR]
Stdlib only. Fail-open: a missing source degrades one row, never the report.
"""
from __future__ import annotations

import json
import os
import re
import sys
import time
from pathlib import Path

# Opus list rates, USD per million tokens. Relative sizing only -- see header.
RATE_IN, RATE_OUT, RATE_CACHE_W, RATE_CACHE_R = 15.0, 75.0, 18.75, 1.50
_RUN_TS_RE = re.compile(r"run-(\d{8})-(\d{6})")


def _run_window(metrics: Path):
    """(start_epoch, end_epoch) for this run, or (None, None) if unparseable."""
    m = _RUN_TS_RE.search(metrics.name)
    if not m:
        return (None, None)
    try:
        start = time.mktime(time.strptime(m.group(1) + m.group(2), "%Y%m%d%H%M%S"))
        return (start, metrics.stat().st_mtime)
    except Exception:
        return (None, None)


def _segments(metrics: Path) -> list:
    out = []
    try:
        for line in metrics.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                out.append(json.loads(line))
            except ValueError:
                continue
    except OSError:
        pass
    return out


def _in_window(ts, start, end) -> bool:
    if start is None or ts is None:
        return False
    return start <= ts <= end + 60          # small tail for the final writes


def _tool_history(root: Path, start, end):
    """(reads_total, reads_distinct, skill_invocations{name:count}) in window."""
    reads, distinct, skills = 0, set(), {}
    p = root / ".claude/state/tool-history.jsonl"
    try:
        for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
            try:
                d = json.loads(line)
            except ValueError:
                continue
            ts = d.get("ts_ns")
            if not isinstance(ts, int) or not _in_window(ts / 1e9, start, end):
                continue
            name, tgt = d.get("tool_name"), d.get("target") or ""
            if name == "Read":
                reads += 1
                distinct.add(tgt)
            elif name == "Skill":
                skills[tgt] = skills.get(tgt, 0) + 1
    except OSError:
        return (0, 0, {})
    return (reads, len(distinct), skills)


def _skill_bytes(root: Path, skills: dict):
    """(bytes_injected, resolved_count, unresolved_count).

    Only repo skills can be sized from disk; plugin skills live outside the
    tree, so they are COUNTED as unresolved rather than guessed at."""
    total, hit, miss = 0, 0, 0
    for name, n in skills.items():
        f = root / ".claude/skills" / str(name).split(":")[-1] / "SKILL.md"
        try:
            total += f.stat().st_size * n
            hit += n
        except OSError:
            miss += n
    return (total, hit, miss)


def _offload(root: Path, start, end):
    """(hook_fires, cache_hits, cache_stores, converged, redispatch) in window."""
    fires = hits = stores = conv = redis = 0
    p = root / ".claude/state/offload-events.jsonl"
    try:
        for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
            try:
                d = json.loads(line)
            except ValueError:
                continue
            if not _in_window(d.get("ts"), start, end):
                continue
            k = d.get("kind")
            fires += k == "fire"
            hits += k == "cache-hit"
            stores += k == "cache-store"
            conv += k == "converged"
            redis += k == "redispatch"
    except OSError:
        pass
    return (fires, hits, stores, conv, redis)


def _fmt_usd(v: float) -> str:
    return f"${v:,.2f}"


def main(argv: list) -> int:
    if not argv:
        print("usage: cost-summary.py METRICS.jsonl [--project DIR]", file=sys.stderr)
        return 2
    metrics = Path(argv[0])
    root = Path(argv[argv.index("--project") + 1]) if "--project" in argv else Path.cwd()
    segs = _segments(metrics)
    if not segs:
        print("cost summary: no metrics segments (nothing to report)")
        return 0

    turns = sum(s.get("turns") or 0 for s in segs)
    cr = sum(s.get("cache_read_input_tokens") or 0 for s in segs)
    cw = sum(s.get("cache_creation_input_tokens") or 0 for s in segs)
    out = sum(s.get("output_tokens") or 0 for s in segs)
    inp = sum(s.get("input_tokens") or 0 for s in segs)
    s_cr = sum(s.get("sidechain_cache_read_input_tokens") or 0 for s in segs)
    s_cw = sum(s.get("sidechain_cache_creation_input_tokens") or 0 for s in segs)
    s_out = sum(s.get("sidechain_output_tokens") or 0 for s in segs)
    agents = sum(s.get("agent_dispatches") or 0 for s in segs)
    per_turn = [((s.get("cache_read_input_tokens") or 0) // (s.get("turns") or 1))
                for s in segs if (s.get("turns") or 0) > 0]

    cost = {
        "main cache-read": cr * RATE_CACHE_R / 1e6,
        "main output": out * RATE_OUT / 1e6,
        "main cache-write": cw * RATE_CACHE_W / 1e6,
        "sidechain cache-read": s_cr * RATE_CACHE_R / 1e6,
        "sidechain cache-write": s_cw * RATE_CACHE_W / 1e6,
        "sidechain output": s_out * RATE_OUT / 1e6,
        "main input": inp * RATE_IN / 1e6,
    }
    total = sum(cost.values()) or 1.0

    start, end = _run_window(metrics)
    scoped = start is not None
    reads, distinct, skills = _tool_history(root, start, end) if scoped else (0, 0, {})
    sk_bytes, sk_hit, sk_miss = _skill_bytes(root, skills)
    fires, hits, stores, conv, redis = _offload(root, start, end) if scoped else (0,)*5

    print("=== cost summary (T4-1) ===")
    print(f"run: {metrics.name}   segments: {len(segs)}   turns: {turns:,}")
    print()
    print("  bucket                    tokens          cost     share")
    for k, v in sorted(cost.items(), key=lambda kv: -kv[1]):
        toks = {"main cache-read": cr, "main output": out, "main cache-write": cw,
                "sidechain cache-read": s_cr, "sidechain cache-write": s_cw,
                "sidechain output": s_out, "main input": inp}[k]
        print(f"  {k:<22} {toks:>12,}  {_fmt_usd(v):>11}  {100*v/total:>7.1f}%")
    print(f"  {'TOTAL':<22} {'':>12}  {_fmt_usd(total):>11}")
    print()
    avg_ctx = cr // max(turns, 1)
    end_ctx = max(per_turn or [0])
    print(f"  context/turn  avg {avg_ctx:,} over {turns:,} turns   "
          f"end-of-segment {end_ctx:,}")
    # LENGTH CONFOUND (measured 2026-07-28, three canary segments). Context
    # grows monotonically inside a segment and is never trimmed, so
    # avg ~= (start + end)/2 -- a LINEAR FUNCTION OF SEGMENT LENGTH. Reading a
    # rising avg as "rollover is not resetting context" is exactly the wrong
    # conclusion and cost this repo a backlog item: length-matched at 423
    # assistant messages the same three segments sat at 244,332 / 243,832 /
    # 254,769 (4.5% spread) against the 40% spread their raw averages showed,
    # and each one started at ~57.5K, i.e. rollover reset perfectly every time.
    # Report the length-invariant figures next to it so the metric cannot be
    # misread the same way twice.
    print("    avg SCALES WITH SEGMENT LENGTH (~= (start+end)/2): it measures "
          "how long the")
    print("    section ran, NOT whether rollover reset. Compare end-of-segment "
          "and tok/turn.")
    if end_ctx > avg_ctx:
        # Linear model over the segment: start = 2*avg - end (model-consistent,
        # and conservative -- the true start measured lower, which makes the
        # split saving LARGER than quoted, never smaller).
        start_model = max(0, 2 * avg_ctx - end_ctx)
        rate = (end_ctx - start_model) // max(turns, 1)
        print(f"    accumulation ~{rate:,} tok/turn, never trimmed -> "
              f"cache-read is QUADRATIC in segment length")
        for k in (2, 3):
            split_avg = start_model + (end_ctx - start_model) / (2 * k)
            share = split_avg / avg_ctx if avg_ctx else 1.0
            print(f"    same work split across {k} sections: ~{100*share:.0f}% "
                  f"of this cache-read cost ({_fmt_usd(cr * share * RATE_CACHE_R / 1e6)})")
    print(f"  agent dispatches: {agents}")
    if not scoped:
        print("  (per-run tool/hook figures unavailable: metrics filename carries "
              "no run-YYYYMMDD-HHMMSS stamp, so the append-only logs could not be "
              "scoped to this run -- omitted rather than reported as all-time)")
    else:
        ratio = (100 * (reads - distinct) // reads) if reads else 0
        print(f"  re-reads: {reads - distinct}/{reads} ({ratio}%) over {distinct} distinct files")
        sk_note = f", {sk_miss} plugin-skill invocations unsized" if sk_miss else ""
        print(f"  skill injection: {sk_bytes:,} bytes from {sk_hit} repo-skill "
              f"invocations{sk_note}")
        print(f"  hook fires: {fires}")
        ch = f"{100*hits//(hits+stores)}%" if (hits + stores) else "n/a"
        print(f"  agent cache: {hits} hits / {stores} stores ({ch})")
        if conv or redis:
            print(f"  review convergence: {conv}/{conv+redis} rounds suppressed")
    print()
    print("  Dollars are list-rate arithmetic for RELATIVE sizing, not a bill;")
    print("  sidechain priced at Opus as an upper bound (the fleet is Sonnet).")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except Exception as exc:                       # never break a run report
        print(f"cost summary unavailable ({exc})")
        sys.exit(0)
