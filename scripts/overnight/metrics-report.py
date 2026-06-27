#!/usr/bin/env python3
"""Render overnight per-section metrics; optionally A/B two run files.

Usage: metrics-report.py RUN.jsonl [BASELINE.jsonl]
Stdlib only; ASCII output.
"""
from __future__ import annotations

import json
import sys

TOKEN_FIELDS = (
    "input_tokens", "output_tokens",
    "cache_read_input_tokens", "cache_creation_input_tokens",
)


def load(path: str) -> list[dict]:
    recs = []
    with open(path, encoding="ascii") as fh:
        for line in fh:
            line = line.strip()
            if line:
                recs.append(json.loads(line))
    return recs


def totals(recs: list[dict]) -> dict:
    agg = {k: 0 for k in TOKEN_FIELDS}
    agg.update(turns=0, agent_dispatches=0, grep_calls=0, lsp_calls=0)
    for r in recs:
        for k in agg:
            v = r.get(k)
            if isinstance(v, int):
                agg[k] += v
    return agg


def fmt_row(label: str, r: dict) -> str:
    lsp = r.get("lsp_calls", 0)
    grep = r.get("grep_calls", 0)
    ratio = f"{lsp}/{grep}"
    return (f"{label:<10} out={r.get('output_tokens', 0):>9} "
            f"in={r.get('input_tokens', 0):>9} "
            f"cache_r={r.get('cache_read_input_tokens', 0):>9} "
            f"agents={r.get('agent_dispatches', 0):>3} "
            f"lsp/grep={ratio:>7}")


def main(argv: list[str]) -> int:
    if not argv:
        print("usage: metrics-report.py RUN.jsonl [BASELINE.jsonl]", file=sys.stderr)
        return 2
    try:
        recs = load(argv[0])
    except FileNotFoundError:
        print(f"no such metrics file: {argv[0]}", file=sys.stderr)
        return 2
    for r in recs:
        print(fmt_row(f"sec {r.get('section_index', '?')}", r))
    tot = totals(recs)
    print(fmt_row("TOTAL", tot))
    if len(argv) > 1:
        try:
            base = totals(load(argv[1]))
        except FileNotFoundError:
            print(f"no such baseline file: {argv[1]}", file=sys.stderr)
            return 2
        delta = base["output_tokens"] - tot["output_tokens"]
        sign = "-" if delta >= 0 else "+"
        print(f"A/B output-token delta vs baseline: {sign}{abs(delta)} "
              f"(baseline {base['output_tokens']} -> run {tot['output_tokens']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
