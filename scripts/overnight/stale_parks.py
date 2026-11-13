#!/usr/bin/env python3
"""Deferred stamps whose stated blocker can be checked, and no longer holds.

A Deferred stamp reads DONE to the triage oracle, which never re-tests its reason,
and `stranded_deferrals.py` only sweeps `[/]` items whose cross-TODO owner shipped.
The 2026-11-07 run found what that leaves behind by accident: parks on a Codex outage
two months after it ended, and 22 sections parked on 15-95 bytes of `.text` with
153,647 free.

Reports three classes. Each names the park to re-judge; none reopens anything, since a
park can cite one blocker and hold on another:
  outage   cites a Codex outage while the outage check says the backend is up
  ceiling  cites a byte count below today's smallest per-section image budget
  closed   waits on a person or a machine in a section with no `**Needs:**` line
           (the never-park rule, 2026-11-11): reopen it with a recorded decision or
           an emulated stand-in, or keep it and name the Need

Deliberately NOT classes, both measured 2026-11-07 against the tree from before the run:
  self-XREF  ranged from 1 of 6 to 7 of 9 hidden work by sample, too mixed to report.
  XREF cycle a walk over Deferred XREF edges found 0 cycles, including the one it was
             proposed for (`TODO-15` s4 -> `TODO-12` s29 -> `TODO-15` s5): the edge back
             to s4 is prose, not an XREF. It would be a check that can never fire.

`outage` and `ceiling` are ADVISORY: `--check` exits 0 and prints a summary line.
`closed` is a fixpoint refusal: `stranded_deferrals.py --gate` (which the fixpoint CLI
runs) refuses while `closed_parks()` lists anything, and `--gate` here exits 1 on it.
Its person words are the staged check's own (`todo-staged-check.py`), so a park that
check would refuse when added is the park this lists when it already exists.

Usage: stale_parks.py [--json] [--check] [--gate] [--selftest]
"""
from __future__ import annotations

import importlib.util
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "scripts"))
import todo_fence as _fence  # noqa: E402  (the shared heading and fence classifier)

# The triage oracle's own section walk (`sequencer_triage._scan_sections`), loaded,
# never copied: a stamp or a `**Needs:**` line belongs to the section the oracle
# says it does, so a nested `- ## N.` heading opens a section and an unnumbered
# closing heading such as `## Verification` ends one.
_triage_spec = importlib.util.spec_from_file_location(
    "_triage_for_stale_parks",
    str(Path(__file__).resolve().parents[2] / ".claude" / "hooks" / "sequencer_triage.py"))
_triage = importlib.util.module_from_spec(_triage_spec)
_triage_spec.loader.exec_module(_triage)


def _section_of_lines(text: str):
    """(physical lines, fence mask, per-line section number or None), by the oracle's walk."""
    scan, sections = _triage._scan_sections(text)
    owner = [None] * len(scan.lines)
    for num, _ln, start, end in sections:
        for i in range(start, end):
            owner[i] = num
    return scan.lines, scan.mask, owner
_SIGN = "§"

_STAMP_RE = re.compile(r"^>\s*\*\*Deferred:\*\*")
_OUTAGE_RE = re.compile(
    r"Codex (?:outage|review unavailable|backend (?:is )?down)"
    r"|external Codex backend|BLOCKED, same outage", re.I)
_PARK_ITEM_RE = re.compile(r"^\s*- \[/\]")
_CEILING_RE = re.compile(r"(?:ceiling|headroom|\.text|\.bss|image budget)", re.I)
_BYTES_RE = re.compile(r"\b(\d{1,6}) bytes\b")

_spec = importlib.util.spec_from_file_location("todo_staged_check", REPO / "scripts" / "todo-staged-check.py")
_staged = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_staged)
_WAITS_ON_PERSON_RE = _staged._WAITS_ON_PERSON_RE
_NEEDS_SOMETHING_RE = _staged._NEEDS_SOMETHING_RE
# A machine the run cannot boot (the `bare-metal` and `whpx-host` Needs), named as what the
# park waits on: "bare-metal" as an adjective ("a bare-metal-safe brk", "a hazard on bare
# metal") is not a wait, and measured 2026-11-13 six of 15 such matches were that.
_WAITS_ON_MACHINE_RE = re.compile(
    r"bare[- ]metal\b(?![- ]safe)[^.;]{0,30}?\b(?:run|proof|verification|validation|row|timing|artifact"
    r"|acceptance|test)\b"
    r"|(?:verif\w*|validat\w*|revisit|requires?|needs?)\s+(?:on\s+|with\s+)?(?:AMD \S+ |Intel \S+ )?"
    r"(?:bare[- ]metal|real (?:firmware|hardware|silicon)|\S+ hardware)\b"
    r"|hardware availability|test (?:laptop|machine|rig)|\bsilicon\b|VirtualBox|requires Intel|real firmware",
    re.I)

# A blocker that no longer exists: `receipt_surface_guard.py`, which kept the unattended
# run out of the Makefile, the ABI generators and `tools/boot-info-manifest/`, was
# deleted with the process gates on 2026-11-09 (`71f25965a`). A park on it is reopened.
_RETIRED_GUARD_RE = re.compile(
    r"receipt[ _-]surface|operator-only|unattended run(?:ner)? may not|may not (?:edit|touch)"
    r"|cannot be added unattended|forbidden to (?:edit|touch)|control[- ]plane code the unattended", re.I)


def stamps(text: str):
    """Yield (line_number, section_number, stamp_text) outside code fences, the
    section being the one the triage oracle attributes the line to (None outside
    any numbered section)."""
    lines, fenced, owner = _section_of_lines(text)
    for i, line in enumerate(lines):
        if not fenced[i] and (_STAMP_RE.match(line) or _PARK_ITEM_RE.match(line)):
            yield i + 1, owner[i], line


def closed_parks(files: dict) -> list:
    """Deferred stamps that wait on a person or a machine in a section with no
    `**Needs:**` line naming it, and every stamp that waits on the retired guard. An
    `awaiting-` stamp is already a park the oracle revisits (BLOCKED, or open work once
    its question is answered), so it is not listed."""
    out = []
    for path, text in files.items():
        lines, fenced, owner = _section_of_lines(text)
        needs = {owner[i] for i, line in enumerate(lines)
                 if not fenced[i] and owner[i] is not None and _NEEDS_SOMETHING_RE.match(line)}
        for line, sec, stamp in stamps(text):
            if not _STAMP_RE.match(stamp) or "awaiting-" in stamp:
                continue
            kind = ("retired-guard" if _RETIRED_GUARD_RE.search(stamp)
                    else "person" if _WAITS_ON_PERSON_RE.search(stamp)
                    else "machine" if _WAITS_ON_MACHINE_RE.search(stamp) else None)
            # A Need names what a person or machine park waits on. It never covers a
            # park on the retired guard: that blocker is gone whatever the section needs.
            if kind and (kind == "retired-guard" or sec not in needs):
                out.append({"file": path, "line": line, "section": sec, "waits_on": kind})
    return out


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
    out = {"outage": [], "ceiling": [], "closed": closed_parks(files)}
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
    b = "y/TODO-02-b.md"
    closed = closed_parks({b: (
        "## 1. One\n> **Deferred:** [M] operator-gated ABI decision on the wire encoding\n"
        "## 2. Two\n> **Deferred:** [L] VirtualBox + bare-metal verification (hardware availability)\n"
        "## 3. Three\n**Needs:** bare-metal\n\n> **Deferred:** [L] verify on bare metal\n"
        "## 9. Nine\n> **Deferred:** [M] a bare-metal-safe brk needs the VMM work first\n"
        "## 10. Ten\n**Needs:** bare-metal\n\n> **Deferred:** [M] the Makefile is receipt surface\n"
        "## 11. Eleven\n**Needs:** bare-metal\n\n- ## 12. Nested\n\n> **Deferred:** [M] operator-gated choice\n"
        "## 13. Thirteen\n\n> **Deferred:** [M] owner decision pending\n\n## Verification\n\n**Needs:** operator\n"
        "## 14. Fourteen\n> **Deferred:** [M] receipt-surface Makefiles stay attended\n"
        "## 4. Four\n> **Deferred:** [M] awaiting-operator design decision\n"
        "## 5. Five\n> **Deferred:** [H] blocked on the loader wiring another section owns\n"
        "## 8. Eight\n> **Deferred:** [M] fix edits the Makefile, receipt surface an unattended run may not touch\n"
        "## 6. Six\n- [/] verify on bare metal\n"
        "```\n## 7. Seven\n> **Deferred:** operator decision in a fence\n```\n")})
    if [(c["section"], c["waits_on"]) for c in closed] != [(1, "person"), (2, "machine"), (10, "retired-guard"), (12, "person"), (13, "person"), (14, "retired-guard"), (8, "retired-guard")]:
        fails.append(f"closed {closed}")
    print(f"stale_parks selftest: {fails or 'OK'}")
    return 1 if fails else 0


def main(argv: list) -> int:
    if "--selftest" in argv:
        return _selftest()
    res = audit(load(REPO), smallest_budget(REPO), outage_active(REPO))
    if "--json" in argv:
        print(json.dumps(res, indent=1))
    elif "--gate" in argv:
        for row in res["closed"]:
            print(f"closed {row['file']}:{row['line']} section {row['section']} waits on a {row['waits_on']}")
        return 1 if res["closed"] else 0
    elif "--check" in argv:
        n = {k: len(v) for k, v in res.items()}
        for row in res["closed"]:
            print(f"closed {row['file']}:{row['line']} section {row['section']} waits on a {row['waits_on']}")
        if any(n.values()):
            print(f"WARNING: stale Deferred parks, re-judge them: {n} (scripts/overnight/stale_parks.py)")
    else:
        for kind, rows in res.items():
            for row in rows:
                print(kind, row)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
