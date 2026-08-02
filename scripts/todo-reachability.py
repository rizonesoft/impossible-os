#!/usr/bin/env python3
"""Can a future pass still SEE this item? One question, one detector.

WHY THIS EXISTS RATHER THAN A FOURTH PARTIAL CHECK. Three checks already guard
pieces of this, each correct and each blind to the others:

  * `todo-orphan-check.py`  -- `- [ ]` in a section stamped BOTH Verified and
                               Quality-reviewed, exempting Deferred sections
  * `validate.py`           -- dangling sections / orphan IO rows
  * `sequencer_triage.py`   -- the classifier those checks are policing

Between them, three shapes of invisible work were found on 2026-08-02, all in
one morning, all real:

  1. A `## N.` body with NO Implementation Order row. PROVEN against the real
     completed `00-infrastructure/TODO-05`: adding a section body with an open
     item left the file classified DONE and the work unreachable; the SAME
     section with an IO row flipped it to NEEDS_WORK. The oracle classifies
     from the row and never reads the body.
  2. A bare `- [ ]` in a stamped, DONE section -- the shape orphan-check owns.
  3. Open `- [ ]` items inside DEFERRED-stamped sections. orphan-check exempts
     these BY DESIGN ("recorded blockers with a named owner"), and the exemption
     is right -- but 109 such items exist, and an item parked without naming an
     owner is indistinguishable from one that names one. It is work in the
     wrong SHAPE: the doctrine's repair is `- [/]` with the blocker named.

The question a reader actually has is not "which of three rules did this break"
but "will anyone ever come back to this?". That is what this answers.

REACHABLE means: some future pass will re-read this item. Concretely, the item
lives in a section the triage oracle does not classify DONE, OR in a Deferred
section whose parked item names an owner the owner-side sweep can act on.

Usage:
  todo-reachability.py [--json] [paths...]    exit 1 if unreachable work exists
  todo-reachability.py --selftest
Stdlib only; reads the todo-graph cache when present for the oracle's verdict.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

SECTION_RE = re.compile(r"^## (\d+)\.")
OPEN_ITEM_RE = re.compile(r"^\s*- \[ \]")
PARKED_ITEM_RE = re.compile(r"^\s*- \[/\]")
VERIFIED_RE = re.compile(r"^> \*\*Verified:")
DEFERRED_RE = re.compile(r"^> \*\*Deferred:")
IO_ROW_RE = re.compile(r"^\|[^|]*\|\s*(\d+)\s*\|")

# An owner is anything a later pass can act on: an XREF, a named TODO, an
# explicit awaiting-* token, or a file path. Deliberately generous -- the point
# is to separate "parked WITH a handle" from "parked into nothing", not to
# grade prose.
OWNER_RE = re.compile(
    r"XREF|awaiting-[a-z]+|TODO-\d+|todo/[0-9]{2}-[a-z-]+/|`[a-z_]+\.(c|h|py|sh)`",
    re.IGNORECASE)


def _sections(lines):
    starts = [(i, int(m.group(1))) for i, l in enumerate(lines)
              if (m := SECTION_RE.match(l))]
    for idx, (ln, num) in enumerate(starts):
        end = starts[idx + 1][0] if idx + 1 < len(starts) else len(lines)
        yield num, ln, lines[ln:end]


def _io_rows(lines):
    """Section numbers listed in the `## Implementation Order` table.

    SCOPED to that table, and tolerant of BOTH row shapes found in the corpus
    (2026-08-02) -- the number sits in column 2 behind a status glyph in most
    files (`| 💎 | 1 | Deliverable | ... |`) and in column 1 in others
    (`| 1 | Section | Tag | Dep |`). Scanning every table in the file instead
    produced 304 false "no IO row" findings, because an Inputs table's rows
    never carry a section number.
    """
    rows = set()
    inside = False
    for l in lines:
        if l.startswith("## "):
            inside = l[3:].strip().lower().startswith("implementation order")
            continue
        if not inside or not l.startswith("|"):
            continue
        cells = [c.strip() for c in l.strip().strip("|").split("|")]
        for cell in cells[:2]:
            if cell.isdigit():
                rows.add(int(cell))
                break
    return rows


def _oracle_done(path, root):
    """Sections the triage oracle calls DONE, or None when unavailable."""
    tri = Path(root) / ".claude/hooks/sequencer_triage.py"
    if not tri.exists():
        return None
    try:
        r = subprocess.run([sys.executable, str(tri), "--classify", str(path)],
                           capture_output=True, text=True, timeout=60, cwd=root)
        d = json.loads(r.stdout)
    except Exception:
        return None
    return {s.get("n") for s in d.get("sections", []) if s.get("class") == "DONE"}


def audit(path, root="."):
    """[(kind, section, detail)] for unreachable work in one file."""
    try:
        lines = Path(path).read_text(encoding="utf-8").split("\n")
    except OSError:
        return []
    rows = _io_rows(lines)
    done = _oracle_done(path, root)
    out = []
    for num, ln, body in _sections(lines):
        opens = [b.strip()[:90] for b in body if OPEN_ITEM_RE.match(b)]
        parked = [b.strip() for b in body if PARKED_ITEM_RE.match(b)]
        deferred = any(DEFERRED_RE.match(b) for b in body)
        stamped = any(VERIFIED_RE.match(b) for b in body)

        # 1. body with no Implementation Order row -- invisible to the oracle
        if num not in rows and (opens or parked):
            out.append(("no-io-row", num,
                        f"section {num} has {len(opens)+len(parked)} item(s) but no "
                        f"Implementation Order row; the oracle classifies from the "
                        f"row and never reads the body"))
            continue

        is_done = (num in done) if done is not None else stamped

        # 2. open items in a DONE section that is not a recorded deferral
        if is_done and opens and not deferred:
            out.append(("open-in-done", num,
                        f"{len(opens)} open item(s) in a DONE section: {opens[0]}"))

        # 3. open items parked in a Deferred section -- wrong shape
        if is_done and opens and deferred:
            out.append(("open-in-deferred", num,
                        f"{len(opens)} open `- [ ]` item(s) inside a Deferred "
                        f"section (should be `- [/]` naming the blocker): {opens[0]}"))

        # NO "parked-ownerless" RULE. It was tried on 2026-08-02 and REMOVED:
        # `[/]` means IN PROGRESS in this repo, not "parked awaiting an owner"
        # (implement-todo-section: "`[x]` fully done or `[/]` in progress"), so
        # requiring every `[/]` to name a blocker misread 329 ordinary
        # progress markers -- e.g. `- [/] GetEnvironmentVariableA(...)` -- as
        # defects. A detector that cries wolf 329 times is worse than none.
    return out


def _targets(argv):
    paths = [a for a in argv if not a.startswith("-")]
    if paths:
        return paths
    return sorted(str(p) for p in Path("todo").rglob("TODO-*.md"))


def main(argv) -> int:
    as_json = "--json" in argv
    findings = {}
    for path in _targets(argv):
        hits = audit(path)
        if hits:
            findings[path] = hits
    if as_json:
        print(json.dumps(findings, indent=2))
    else:
        total = sum(len(v) for v in findings.values())
        by_kind = {}
        for hits in findings.values():
            for kind, _, _ in hits:
                by_kind[kind] = by_kind.get(kind, 0) + 1
        for path, hits in sorted(findings.items()):
            print(f"\n{path}")
            for kind, num, detail in hits:
                print(f"  [{kind}] {detail}")
        print(f"\n{total} unreachable finding(s) across {len(findings)} file(s)")
        for k, v in sorted(by_kind.items(), key=lambda kv: -kv[1]):
            print(f"  {v:4d}  {k}")
    return 1 if findings else 0


def _selftest() -> int:
    import tempfile
    ok = True
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "TODO-99-x.md"
        p.write_text(
            "# X\n\n## Implementation Order\n\n| 💎 | 1 | a | -- | [x] |\n\n"
            "## 1. Stamped and done\n\n- [x] done\n- [ ] sneaked in\n"
            "> **Verified:** 2026-01-01 | commit `x`\n\n"
            "## 2. No IO row at all\n\n- [ ] invisible work\n")
        hits = {k for k, _, _ in audit(str(p), root=td)}
        for want in ("open-in-done", "no-io-row"):
            if want not in hits:
                print(f"FAIL: {want} not detected ({hits})"); ok = False
    print("todo-reachability selftest", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(_selftest() if "--selftest" in sys.argv else main(sys.argv[1:]))
