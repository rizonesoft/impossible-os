#!/usr/bin/env python3
"""Deterministic TODO-hygiene enumerator (completeness-owning, zero false-positive).

Covers one rule-checkable class the todo-graph validator + triage oracle do NOT:
  placeholder  -- an UNAMBIGUOUS unfilled placeholder token in a TODO file

Tokens are deliberately limited to forms with no legitimate descriptive use, so a
hit is always a genuine unfilled placeholder. `<hash>` is intentionally excluded:
it appears in real format descriptions (e.g. the SID form S-1-5-80-<hash>), so it
is not deterministically a placeholder -- that judgment is left to the fuzzy
todo-hygiene-auditor. Blocker-ownership ("is this BLOCKED item owned?") is likewise
a judgment call (XREF vs domain-shorthand vs issue link) and lives in the auditor,
not here.

Does NOT duplicate file-level XREF / section-drift (validate.py) or section-level
stamp classification (sequencer_triage.py). Stdlib only; ASCII output.

Usage: todo-hygiene.py [PATH ...]   (default: every todo markdown file)
Prints: class<TAB>path:line<TAB>message ; exit 1 if any finding else 0.
"""
from __future__ import annotations

import os
import sys

PLACEHOLDER_TOKENS = (
    "<commit>", "<commit-hash>", "<TBD>", "<DATE>", "<X commits>",
)


def _iter_todo_files(args: list[str]) -> list[str]:
    if args:
        return args
    out = []
    for dirpath, _dirs, files in os.walk("todo"):
        for fn in files:
            if fn.endswith(".md"):
                out.append(os.path.join(dirpath, fn))
    return sorted(out)


def scan_file(path: str) -> list[tuple[str, int, str]]:
    findings = []
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return findings
    for i, ln in enumerate(lines, 1):
        for tok in PLACEHOLDER_TOKENS:
            if tok in ln:
                findings.append(("placeholder", i, f"unfilled {tok}"))
    return findings


def main(argv: list[str]) -> int:
    total = 0
    for path in _iter_todo_files(argv):
        for cls, ln, msg in scan_file(path):
            print(f"{cls}\t{path}:{ln}\t{msg}")
            total += 1
    return 1 if total else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
