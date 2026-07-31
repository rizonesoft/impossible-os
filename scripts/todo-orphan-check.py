#!/usr/bin/env python3
"""Detect open `- [ ]` items ORPHANED behind a DONE-classified TODO section.

THE DEFECT THIS CLOSES (2026-07-31, measured live). The triage oracle
(`sequencer_triage.classify_section`) derives a section's class from its
Implementation Order status plus its stamps -- the checklist items inside the
body are never consulted. That is correct for a Deferred-parked section and
WRONG for an item appended after the stamp: the runner never routes back to a
DONE section, so the item is invisible to every future pass, cannot hold
fixpoint open, and the run eventually reports the repo complete over it. The
overnight run was actively growing this pool by following CLAUDE.md's
completion-first rule ("file adjacent work in the owning TODO section")
literally into sections that were already closed.

WHAT COUNTS AS AN ORPHAN -- the precision rule matters more than the check.
The naive scan ("any `[ ]` behind a DONE section") over-matched 14.6x on the
live tree: 1,712 hits of which 1,595 were deliberately PARKED items in
sections carrying a `> **Deferred:**` stamp -- recorded blockers with a
re-open path (the owner-side `stranded_deferrals.py` sweep). This is the same
over-match class stranded_deferrals' own header documents paying for once.
An orphan is therefore:

    `- [ ]` present
    AND the section's Implementation Order status is [x] or [/]
    AND the section carries BOTH **Verified:** and **Quality reviewed:** stamps
    AND the section carries NO **Deferred:** stamp

Sections DONE *via* a Deferred stamp are exempt by construction; so is
anything in a NEEDS_WORK/BLOCKED section (still reachable).

The stamp/heading regexes are IMPORTED from sequencer_triage.py, not re-typed,
so this check cannot drift from the classifier it polices. The IO-table status
is parsed from the table itself (the classifier reads it via the todo-graph
cache; this check must work cache-free in pre-commit).

  todo-orphan-check.py [--repo DIR] [paths...]   list orphans; exit 1 if any
  todo-orphan-check.py --selftest

Repair guidance printed with each hit:
  * item names an owner/XREF/blocker  -> flip to `- [/]` (parked shape)
  * genuinely open work               -> move to a NEW section (next free
    number, body LAST) with a reciprocal XREF, or to a NEEDS_WORK section
  * operator-only                     -> flip to `- [/]` + "Operator-only" tag
"""
from __future__ import annotations

import glob
import importlib.util
import re
import sys
from pathlib import Path


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def _triage(root: Path):
    spec = importlib.util.spec_from_file_location(
        "sequencer_triage", root / ".claude/hooks/sequencer_triage.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


OPEN_ITEM_RE = re.compile(r"^\s*- \[ \]")
# An Implementation Order row: `| <marker> | <order> | ... | [x] |`. The status
# is the LAST cell; the order number is the first all-digit cell.
IO_ROW_RE = re.compile(r"^\|.*\|\s*\[([ x/])\]\s*\|\s*$")


def _io_statuses(lines):
    """{section_number: status_char} parsed from the Implementation Order table."""
    out = {}
    for ln in lines:
        m = IO_ROW_RE.match(ln)
        if not m:
            continue
        cells = [c.strip() for c in ln.strip().strip("|").split("|")]
        num = next((c for c in cells if c.isdigit()), None)
        if num is not None:
            out[int(num)] = m.group(1)
    return out


def scan_file(path: Path, tri) -> list:
    """[(section, heading, [item lines])] orphans in one TODO file."""
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return []
    lines = text.split("\n")
    statuses = _io_statuses(lines)
    if not statuses:
        return []
    # Walk sections once, collecting stamps and open items per section.
    cur = None
    stamps: dict = {}
    items: dict = {}
    heads: dict = {}
    for ln in lines:
        m = tri.SECTION_HEADING_RE.match(ln)
        if m:
            cur = int(m.group(1))
            heads[cur] = ln.strip()
            continue
        # ANY non-numbered `## ` heading ends the current section. Without this
        # the file's closing matter (## OS Comparison / Unit Tests /
        # Verification / History) attributes to the LAST numbered section --
        # the exact terminal-section over-capture that section_block() paid for
        # in 2026-07 (21 phantom items on TODO-10, 95 on TODO-25) and that this
        # scanner reproduced on its first run: 291 phantom items vs the real
        # 117, every extra one a file-level Verification checklist line.
        if ln.startswith("## "):
            cur = None
            continue
        if cur is None:
            continue
        ks = stamps.setdefault(cur, set())
        if tri.VERIFIED_RE.match(ln):
            ks.add("V")
        elif tri.QUALITY_RE.match(ln):
            ks.add("Q")
        elif tri.DEFERRED_RE.match(ln):
            ks.add("D")
        elif OPEN_ITEM_RE.match(ln):
            items.setdefault(cur, []).append(ln.strip())
    out = []
    for n, its in sorted(items.items()):
        if statuses.get(n) not in ("x", "/"):
            continue                     # still NEEDS_WORK -> reachable
        ks = stamps.get(n, set())
        if "D" in ks:
            continue                     # Deferred-parked -> has a re-open path
        if "V" in ks and "Q" in ks:
            out.append((n, heads.get(n, f"## {n}."), its))
    return out


def main(argv) -> int:
    root = _repo_root()
    if "--repo" in argv:
        root = Path(argv[argv.index("--repo") + 1]).resolve()
    tri = _triage(_repo_root())          # regex source is always the live hook
    paths = [Path(a) for a in argv if not a.startswith("-")
             and a != str(root)] or None
    if paths is None:
        paths = [Path(p) for p in sorted(
            glob.glob(str(root / "todo/**/*.md"), recursive=True))]
    rc = 0
    for p in paths:
        for n, head, its in scan_file(p, tri):
            rc = 1
            rel = p.relative_to(root) if p.is_absolute() else p
            print(f"{rel} sec {n}: {len(its)} orphaned [ ] behind a stamped "
                  f"section ({head[:60]})")
            for it in its:
                print(f"    {it[:160]}")
    return rc


def _selftest() -> int:
    import tempfile
    fails = []

    def check(name, cond):
        if not cond:
            fails.append(name)

    tri = _triage(_repo_root())
    doc = "\n".join([
        "# T", "",
        "| S | Order | D | Dep | Status |",
        "| - | :-: | - | - | :-: |",
        "| x |  1  | a | -- |  [x]   |",
        "| x |  2  | b | -- |  [x]   |",
        "| x |  3  | c | -- |  [/]   |",
        "| x |  4  | d | -- |  [ ]   |",
        "",
        "## 1. Stamped with an orphan",
        "- [x] done thing",
        "- [ ] follow-up appended after the stamp",
        "> **Verified:** 2026-07-31 | commit `x` | 1/1",
        "> **Quality reviewed:** 2026-07-31 | Codex",
        "",
        "## 2. Deferred-parked (EXEMPT)",
        "- [ ] parked item with a blocker",
        "> **Verified:** 2026-07-31 | commit `x` | 1/2",
        "> **Quality reviewed:** 2026-07-31 | Codex",
        "> **Deferred:** [blocked] waiting on the owner",
        "",
        "## 3. Shipped but only one stamp (NOT done -> reachable, EXEMPT)",
        "- [ ] item in a section triage still routes to",
        "> **Verified:** 2026-07-31 | commit `x` | 1/2",
        "",
        "## 4. Open section (EXEMPT)",
        "- [ ] ordinary reachable work",
        ""])
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "TODO-99.md"
        p.write_text(doc, encoding="utf-8")
        got = scan_file(p, tri)
        check("exactly one orphan section", len(got) == 1)
        check("it is section 1", got and got[0][0] == 1)
        check("one item reported", got and len(got[0][2]) == 1)
        check("the right item", got and "follow-up appended" in got[0][2][0])
        # Repairing the orphan the recommended way silences the check.
        fixed = doc.replace("- [ ] follow-up appended after the stamp",
                            "- [/] follow-up appended after the stamp")
        p.write_text(fixed, encoding="utf-8")
        check("[/] flip silences it", scan_file(p, tri) == [])
        # Closing matter after the LAST numbered section must not attribute to
        # it (the terminal-section over-capture, reproduced live 2026-07-31).
        tail = doc + "\n".join([
            "## Verification", "",
            "- [ ] file-level verification item one",
            "- [ ] file-level verification item two", ""])
        p.write_text(tail, encoding="utf-8")
        got2 = scan_file(p, tri)
        check("closing matter not attributed to a section",
              sum(len(g[2]) for g in got2) == 1)
    if fails:
        sys.stderr.write("todo-orphan-check selftest FAIL: "
                         + "; ".join(fails) + "\n")
        return 1
    print("todo-orphan-check selftest OK")
    return 0


if __name__ == "__main__":
    sys.exit(_selftest() if "--selftest" in sys.argv else main(sys.argv[1:]))
