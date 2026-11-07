#!/usr/bin/env python3
"""Deferred stamps whose stated blocker can be checked, and no longer holds.

A Deferred stamp reads DONE to the triage oracle, which never re-tests its reason,
and `stranded_deferrals.py` only sweeps `[/]` items whose cross-TODO owner shipped.
The 2026-11-07 run found what that leaves behind by accident: parks on a Codex outage
two months after it ended, and 22 sections parked on 15-95 bytes of `.text` with
153,647 free.

Reports two classes. Each names the park to re-judge; none reopens anything, since a
park can cite one blocker and hold on another:
  outage   cites a Codex outage while the outage check says the backend is up
  ceiling  cites a byte count below today's smallest per-section image budget

Deliberately NOT classes, both measured 2026-11-07 against the tree from before the run:
  self-XREF  ranged from 1 of 6 to 7 of 9 hidden work by sample, too mixed to report.
  XREF cycle a walk over Deferred XREF edges found 0 cycles, including the one it was
             proposed for (`TODO-15` s4 -> `TODO-12` s29 -> `TODO-15` s5): the edge back
             to s4 is prose, not an XREF. It would be a check that can never fire.

ADVISORY, per the control-plane rule that a new check warns first: `--check` exits 0
and prints a summary line. Promote it to a fixpoint refusal only with precision
measured over a cycle.

Usage: stale_parks.py [--json] [--check] [--selftest]
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "scripts"))
import todo_fence as _fence  # noqa: E402  (the shared heading and fence classifier)
_SIGN = "§"

_STAMP_RE = re.compile(r"^>\s*\*\*Deferred:\*\*")
_OUTAGE_RE = re.compile(
    r"Codex (?:outage|review unavailable|backend (?:is )?down)"
    r"|external Codex backend|BLOCKED, same outage", re.I)
_PARK_ITEM_RE = re.compile(r"^\s*- \[/\]")
_CEILING_RE = re.compile(r"(?:ceiling|headroom|\.text|\.bss|image budget)", re.I)
_BYTES_RE = re.compile(r"\b(\d{1,6}) bytes\b")


def stamps(text: str):
    """Yield (line_number, section_number, stamp_text) outside code fences."""
    lines = text.splitlines()
    fenced = _fence.fence_scan(lines).mask
    section = None
    for n, line in enumerate(lines, 1):
        if fenced[n - 1]:
            continue
        h = _fence.classify_heading(line)
        if h.kind == "ok":
            section = h.n
        elif _STAMP_RE.match(line) or _PARK_ITEM_RE.match(line):
            yield n, section, line


def smallest_budget(root: Path):
    try:
        r = subprocess.run([sys.executable, "scripts/overnight/bss-headroom.py"], cwd=root,
                           capture_output=True, text=True, timeout=60)
        m = re.search(r"per-section budgets: (.*?)\)", json.loads(r.stdout)["advice"])
        return min(int(v) for v in re.findall(r"\s(\d+)", " " + m.group(1)))
    except Exception:
        return None  # fail-direction: no budget reading reports no ceiling parks


def outage_active(root: Path) -> bool:
    try:
        r = subprocess.run(
            [sys.executable, "scripts/overnight/codex-outage-check.py", "--manifest",
             ".claude/overnight/reviews/manifest.jsonl"],
            cwd=root, capture_output=True, text=True, timeout=30)
        return r.returncode == 75
    except Exception:
        return True  # fail-direction: unknown state must not call an outage over


def audit(files: dict, budget, outage: bool) -> dict:
    out = {"outage": [], "ceiling": []}
    for path, text in files.items():
        for line, section, stamp in stamps(text):
            where = {"file": path, "line": line, "section": section}
            if not outage and _OUTAGE_RE.search(stamp):
                out["outage"].append(where)
            if budget is not None and _CEILING_RE.search(stamp):
                cited = [int(b) for b in _BYTES_RE.findall(stamp)]
                if cited and max(cited) < budget:
                    out["ceiling"].append({**where, "cited": max(cited), "budget": budget})
    return out


def load(root: Path) -> dict:
    return {str(p.relative_to(root / "todo")): p.read_text(encoding="utf-8")
            for p in sorted((root / "todo").rglob("TODO-*.md"))
            if "overnight-runner-improvements" not in str(p) and "token-saver" not in str(p)}


def _selftest() -> int:
    a = "x/TODO-01-a.md"
    files = {a: ("## 4. Four\n> **Deferred:** parked on the Codex outage\n"
                 "## 5. Five\n> **Deferred:** only 95 bytes of .text left\n"
                 "## 6. Six\n> **Deferred:** needs 900000 bytes of .text\n"
                 "## 7. Seven\n- [/] item -- BLOCKED, same outage -> gotcha\n"
                 "- [ ] open text naming the Codex outage is not a park\n"
                 "```\n> **Deferred:** the Codex outage in a fence\n```\n")}
    got = audit(files, 1000, outage=False)
    fails = []
    if [o["section"] for o in got["outage"]] != [4, 7]:
        fails.append(f"outage {got['outage']}")
    if [o["section"] for o in got["ceiling"]] != [5]:
        fails.append(f"ceiling {got['ceiling']}")
    # REFUSAL'S MIRROR: a live outage or an unreadable budget must report nothing.
    quiet = audit(files, None, outage=True)
    if quiet["outage"] or quiet["ceiling"]:
        fails.append(f"quiet {quiet}")
    print(f"stale_parks selftest: {fails or 'OK'}")
    return 1 if fails else 0


def main(argv: list) -> int:
    if "--selftest" in argv:
        return _selftest()
    res = audit(load(REPO), smallest_budget(REPO), outage_active(REPO))
    if "--json" in argv:
        print(json.dumps(res, indent=1))
    elif "--check" in argv:
        n = {k: len(v) for k, v in res.items()}
        if any(n.values()):
            print(f"WARNING: stale Deferred parks, re-judge them: {n} (scripts/overnight/stale_parks.py)")
    else:
        for kind, rows in res.items():
            for row in rows:
                print(kind, row)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
