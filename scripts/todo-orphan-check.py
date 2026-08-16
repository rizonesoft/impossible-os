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

import functools
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


@functools.lru_cache(maxsize=1)
def _fence():
    """The shared fence tracker (section 38), loaded the same way as `_triage`.

    CACHED. `exec_module` re-runs the whole 413-line module on every call, so a
    call from inside a per-line loop costs a module import per line of every
    TODO file. That is not hypothetical: `scan_file` did exactly that at the
    `is_h2` boundary test and it took this check from ~1s to 133.7s over the
    281-file corpus, which in turn took `scripts/lint.sh` from 22s to 148s and
    pushed CI's 25-minute `Build Impossible OS` job over its budget -- 16 of 30
    runs cancelled, no green build between 2026-08-12 05:35 and the repair.
    The caller-side fix (use the already-bound `fence`) is the real one; this
    cache makes the whole defect class cost nothing.
    """
    spec = importlib.util.spec_from_file_location(
        "todo_fence", _repo_root() / "scripts/todo_fence.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


OPEN_ITEM_RE = re.compile(r"^\s*- \[ \]")
# An Implementation Order row: `| <marker> | <order> | ... | [x] |`. The status
# is the LAST cell; the order number is the first all-digit cell.
IO_ROW_RE = re.compile(r"^\|.*\|\s*\[([ x/])\]\s*\|\s*$")


def _io_statuses(lines, mask):
    """{section_number: status_char} parsed from the Implementation Order table.

    FENCE-AWARE (section 38): a fenced Implementation Order row is an example,
    and taking it as real invents a section status -- which is the input to the
    DONE test below, so it decides whether an item is called an orphan.
    """
    out = {}
    for i, ln in enumerate(lines):
        if mask[i]:
            continue
        m = IO_ROW_RE.match(ln)
        if not m:
            continue
        cells = [c.strip() for c in ln.strip().strip("|").split("|")]
        num = next((c for c in cells if c.isdigit()), None)
        if num is not None:
            out[int(num)] = m.group(1)
    return out


def scan_file(path: Path, tri, fence=None) -> list:
    """[(section, heading, [item lines])] orphans in one TODO file.

    A malformed document (unclosed fence or unclosed `<!--`) is NOT scanned:
    `malformed()` reports it and `main` refuses. Returning [] here would read
    as "no orphans" on a file this walk cannot see (Codex design review,
    section 38, [high]).
    """
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return []
    fence = fence or _fence()
    scan = fence.scan_text(text)
    if scan.unclosed_reason():
        return []
    lines, mask = scan.lines, scan.mask
    statuses = _io_statuses(lines, mask)
    if not statuses:
        return []
    # Walk sections once, collecting stamps and open items per section.
    cur = None
    stamps: dict = {}
    items: dict = {}
    heads: dict = {}
    for i, ln in enumerate(lines):
        # A fenced `## N.` heading, `- [ ]` item or stamp is an EXAMPLE. Taking
        # the heading as real re-scopes every following line to a section that
        # does not exist, and taking a fenced `> **Verified:**` as real is the
        # highest-impact shape of all: it is half of the DONE test that decides
        # whether the items below it are orphans (section 38).
        if mask[i]:
            continue
        # THE SHARED CLASSIFIER (v14 close-out): this used to borrow the
        # triage oracle's private grammar; that grammar is retired. An
        # over-long heading falls through to the `is_h2` boundary below and
        # correctly ends the current section without starting one.
        h = fence.classify_heading(ln)
        if h.kind == "ok":
            cur = h.n
            heads[cur] = ln.strip()
            continue
        # ANY non-numbered `## ` heading ends the current section. Without this
        # the file's closing matter (## OS Comparison / Unit Tests /
        # Verification / History) attributes to the LAST numbered section --
        # the exact terminal-section over-capture that section_block() paid for
        # in 2026-07 (21 phantom items on TODO-10, 95 on TODO-25) and that this
        # scanner reproduced on its first run: 291 phantom items vs the real
        # 117, every extra one a file-level Verification checklist line.
        # The SHARED boundary rule (section 43 post-ship review): a column-0
        # test misses CommonMark's legal 0-3 indent, so an indented heading did
        # not end the current section here while it did in the producer.
        if fence.is_h2(ln):
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


def malformed(path: Path, fence) -> str:
    """The unclosed-delimiter reason for one file, or "" when it is well-formed."""
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return ""
    return fence.scan_text(text).unclosed_reason() or ""


def main(argv) -> int:
    root = _repo_root()
    if "--repo" in argv:
        root = Path(argv[argv.index("--repo") + 1]).resolve()
    tri = _triage(_repo_root())          # regex source is always the live hook
    fence = _fence()
    paths = [Path(a) for a in argv if not a.startswith("-")
             and a != str(root)] or None
    if paths is None:
        paths = [Path(p) for p in sorted(
            glob.glob(str(root / "todo/**/*.md"), recursive=True))]
    rc = 0
    for p in paths:
        why = malformed(p, fence)
        if why:
            rc = 1
            rel = p.relative_to(root) if p.is_absolute() else p
            print(f"{rel}: UNSCANNABLE -- {why}")
            continue
        for n, head, its in scan_file(p, tri, fence):
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
