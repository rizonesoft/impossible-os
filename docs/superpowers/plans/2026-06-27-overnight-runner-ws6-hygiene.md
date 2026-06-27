# Overnight Runner Plan 3 -- WS6 deterministic-first hygiene + Sonnet auditor

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Push the rock-solid, rule-checkable TODO-hygiene classes into a deterministic script (so the Opus main loop reads a verdict instead of reasoning them out), and add a read-only Sonnet `todo-hygiene-auditor` for the genuinely-fuzzy residue -- with every Sonnet item structurally verified, never self-certified (WS8).

**Architecture:** `scripts/todo-hygiene.py` is a standalone stdlib enumerator over `todo/**.md` that owns *completeness* for one zero-false-positive class: unambiguous unfilled placeholder tokens. It does NOT duplicate the todo-graph validator or the triage oracle (which already own file-level XREFs, IO-table section drift, and section-level stamps). The `todo-hygiene-auditor` (Sonnet, read-only) handles the fuzzy residue (prose-vs-code claims, and blocker-ownership judgment) and returns a punch-list the main loop validates before applying. Both are wired into `complete-todo-file`.

**Implementation deviation (2026-06-27):** the ownerless-`BLOCKED`-bullet class was prototyped but DROPPED from the deterministic enumerator -- deciding whether a blocker names a real owner (a literal XREF clause vs a domain/TODO shorthand vs an upstream issue link vs an arrow-reference to another TODO) is a judgment call, not a rule, so it produced false positives (28 on the real tree, several genuinely owned). That judgment moved to the fuzzy auditor (Task 2). The `<hash>` token was likewise excluded from placeholders (it appears in real format descriptions such as a Windows SID form; all 10 tree hits were false). Net: the enumerator ships one rock-solid class.

**Tech Stack:** Python 3 stdlib; Markdown (agent + skill); no new dependencies.

## Global Constraints

- Python stdlib only; ASCII only; no section-sign+digit in code.
- `todo-hygiene.py` must NOT duplicate existing deterministic checks: file-level dangling XREF + IO-table section drift live in `scripts/todo-graph/validate.py` (run via `build-and-validate.sh`); section-level DONE / `[/]`-stamp lives in `sequencer_triage.py --classify`. WS6 covers only the classes those miss.
- WS8 trust contract: every Sonnet auditor item is structurally backstopped -- script-verified or Opus-validated before action; never self-certified. The deterministic enumerator owns completeness for its classes; the model only adds the fuzzy residue.
- New agent stays read-only: `tools:` subset of `{Read, Grep, Glob}` (Check 14 auto-enforces).
- `.claude/skills/README.md` agent rows use `[name](path)` (not backticked).
- WS5b is empty (operator kept parity + kernel-explorer on Opus); no model edits in this plan.
- Do NOT touch the runner's leftover working-tree files.
- Deferred follow-ups (NOT this plan): section-level validation for prose/Inputs/stamp XREFs (requires editing the runner-critical `validate.py`); test-count drift detection (fuzzy prose parsing).

---

### Task 1: scripts/todo-hygiene.py deterministic enumerator

**Files:**
- Create: `scripts/todo-hygiene.py`
- Test: `scripts/overnight/tests/test_todo_hygiene.py` (create)

**Interfaces:**
- CLI: `todo-hygiene.py [PATH ...]` -- scans the given TODO markdown files (default: all of `todo/**.md`). Prints one finding per line `class<TAB>path:line<TAB>message`; exit 1 if any finding, 0 if clean.
- Classes:
  - `placeholder` -- a line containing an unfilled placeholder token (`<commit>`, `<hash>`, `<TBD>`, `<DATE>`, `<commit-hash>`, `<X commits>`).
  - `ownerless-blocker` -- a checklist bullet (`- [ ]` / `- [/]`) whose text contains the uppercase word `BLOCKED` but has no `XREF:` clause on the line (a blocker with no owner).

- [ ] **Step 1: Write the failing test**

Create `scripts/overnight/tests/test_todo_hygiene.py`:

```python
#!/usr/bin/env python3
import subprocess, sys, tempfile, pathlib

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE.parent.parent / "todo-hygiene.py"


def _run(text):
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d) / "TODO-x.md"
        p.write_text(text, encoding="ascii")
        r = subprocess.run([sys.executable, str(SCRIPT), str(p)],
                           text=True, capture_output=True)
        return r.returncode, r.stdout


def test_placeholder_flagged():
    rc, out = _run("Shipped in <commit> after review.\n")
    assert rc == 1, out
    assert "placeholder" in out and "<commit>" in out


def test_ownerless_blocker_flagged():
    rc, out = _run("- [ ] Resume sync -- BLOCKED on S3 power mgmt\n")
    assert rc == 1, out
    assert "ownerless-blocker" in out


def test_blocked_with_xref_is_clean():
    rc, out = _run("- [ ] Resume sync -- BLOCKED on S3 -> XREF: power-management.md\n")
    assert rc == 0, out


def test_clean_file():
    rc, out = _run("- [x] Done and stamped.\n> **Verified:** 2026-01-01 -- ok\n")
    assert rc == 0, out


if __name__ == "__main__":
    test_placeholder_flagged()
    test_ownerless_blocker_flagged()
    test_blocked_with_xref_is_clean()
    test_clean_file()
    print("PASS: todo-hygiene")
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 scripts/overnight/tests/test_todo_hygiene.py`
Expected: FAIL (`todo-hygiene.py` does not exist).

- [ ] **Step 3: Write scripts/todo-hygiene.py**

```python
#!/usr/bin/env python3
"""Deterministic TODO-hygiene enumerator (completeness-owning, low false-positive).

Covers the classes the todo-graph validator + triage oracle do NOT:
  placeholder        -- unfilled <commit>/<hash>/<TBD>/... tokens
  ownerless-blocker  -- a checklist bullet marked BLOCKED with no XREF: owner

Does NOT duplicate file-level XREF / section-drift (validate.py) or section-level
stamp classification (sequencer_triage.py). Stdlib only; ASCII output.

Usage: todo-hygiene.py [PATH ...]   (default: every todo markdown file)
Prints: class<TAB>path:line<TAB>message ; exit 1 if any finding else 0.
"""
from __future__ import annotations

import os
import re
import sys

PLACEHOLDER_TOKENS = (
    "<commit>", "<hash>", "<commit-hash>", "<TBD>", "<DATE>", "<X commits>",
)
_BULLET_RE = re.compile(r"^\s*-\s*\[[ /]\]\s")  # open or partial checklist bullet


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
        if _BULLET_RE.match(ln) and "BLOCKED" in ln and "XREF:" not in ln:
            findings.append(("ownerless-blocker", i,
                             "BLOCKED bullet has no XREF owner"))
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 scripts/overnight/tests/test_todo_hygiene.py`
Expected: `PASS: todo-hygiene`

- [ ] **Step 5: Baseline the real tree (informational, not a gate)**

Run: `python3 scripts/todo-hygiene.py | cut -f1 | sort | uniq -c; echo "exit=$?"`
Expected: a count per class of any existing findings in the todo tree (may be nonzero -- that is real hygiene debt, not a test failure). Record the count; do not fix here.

- [ ] **Step 6: Commit**

```bash
git add scripts/todo-hygiene.py scripts/overnight/tests/test_todo_hygiene.py
git commit -m "todo: WS6 -- deterministic todo-hygiene enumerator (placeholders + ownerless blockers)"
```

---

### Task 2: todo-hygiene-auditor agent (Sonnet, read-only)

**Files:**
- Create: `.claude/agents/todo-hygiene-auditor.md`
- Modify: `CLAUDE.md` (table row + "Six"->"Seven")
- Modify: `.claude/skills/README.md` (agent row)

**Interfaces:** dispatchable via `Agent(subagent_type="todo-hygiene-auditor", ...)`; returns a checkable punch-list of fuzzy-residue hygiene issues. Read-only; main session verifies + applies.

- [ ] **Step 1: Create the agent file**

```markdown
---
name: todo-hygiene-auditor
description: Read-only TODO-hygiene auditor for Impossible OS. Dispatched during complete-todo-file close-out to read a TODO and return a checkable punch-list of the FUZZY-residue hygiene issues that deterministic tools cannot enumerate -- prose claims that contradict the code, a Notes block that drifted from what shipped, a stamp whose description does not match its section. Does NOT re-find what scripts/todo-hygiene.py, scripts/todo-graph/validate.py, or sequencer_triage.py already own (placeholders, dangling XREFs, section stamps). Read-only; proposes a punch-list only. Does not edit, build, commit, dispatch Codex, or invoke skills. Every item is checkable at file:line; the main session verifies each before applying it (trust contract).
model: sonnet
tools: Read, Grep, Glob
---

# TODO Hygiene Auditor

You audit ONE TODO file for the fuzzy-residue hygiene issues that deterministic
tooling cannot catch. Return a checkable, itemized punch-list -- never prose, never
an edit.

## In scope (fuzzy -- needs judgment)

- A prose claim ("X is wired", "all paths covered") that the cited code does not
  support.
- A Notes block or stamp description that drifted from what actually shipped.
- A section whose checklist says done but whose body reveals an obvious adjacent
  gap a real user would hit next.

## Out of scope (already owned by deterministic tools -- do NOT report)

- Unfilled placeholders, ownerless BLOCKED bullets -> `scripts/todo-hygiene.py`.
- Dangling XREFs / IO-table section drift -> `scripts/todo-graph/validate.py`.
- Missing Verified/Quality-reviewed/Deferred stamps -> `sequencer_triage.py`.

## Return shape

For each finding: `file:line` -- one-line description -- how the main session can
CONFIRM it at that line. Order most-important first; cap at 10.

## Hard rules

- Read-only. No edits, builds, commits, Codex, or skills.
- Every item MUST be checkable at a file:line. You propose; the main session
  verifies each item before applying it (trust contract). You never assert a fix.
- ASCII only. No section-sign+digit references.
```

- [ ] **Step 2: Add the CLAUDE.md row + bump the count**

In `CLAUDE.md` specialist-agents table, after the `diagnostic-digester` row add:
```markdown
| `todo-hygiene-auditor` | sonnet | `complete-todo-file` close-out (fuzzy-residue hygiene punch-list; script-verified before applied) |
```
And change `Six subagents in [\`.claude/agents/\`]` to `Seven subagents in [\`.claude/agents/\`]`.

- [ ] **Step 3: Add the README.md row**

In `.claude/skills/README.md`:
```markdown
| [todo-hygiene-auditor](../agents/todo-hygiene-auditor.md) | sonnet | `complete-todo-file` close-out |
```

- [ ] **Step 4: Verify registration + read-only allowlist**

Run:
```bash
bash scripts/lint.sh 2>&1 | grep -iE "todo-hygiene-auditor|read-only allowlist" || echo "no agent-tools violation"
grep -c "Seven subagents" CLAUDE.md
```
Expected: no Check-14 violation; `Seven subagents` count = 1.

- [ ] **Step 5: Commit**

```bash
git add .claude/agents/todo-hygiene-auditor.md CLAUDE.md .claude/skills/README.md
git commit -m "agents: WS6 -- todo-hygiene-auditor (read-only Sonnet fuzzy-residue punch-list)"
```

---

### Task 3: Wire both into complete-todo-file

**Files:**
- Modify: `.claude/skills/complete-todo-file/SKILL.md`

**Interfaces:** the close-out sweep runs the deterministic enumerator first, then optionally the auditor for the residue, then the main session verifies each auditor item before applying.

- [ ] **Step 1: Read the sweep step to find the anchor**

Run: `grep -n "stale XREF\|placeholder\|loose-end\|sweep\|Verification" .claude/skills/complete-todo-file/SKILL.md | head`
Identify the loose-end sweep step (the one listing stale-XREF / placeholder greps).

- [ ] **Step 2: Add the deterministic-first instruction to the sweep**

In the sweep step, prepend a bullet:
```markdown
- **Deterministic first:** run `python3 scripts/todo-hygiene.py <todo file>` and resolve every line it prints (unfilled placeholders, ownerless BLOCKED bullets) -- this owns completeness for those classes, so you do not hand-grep them. Then run `bash scripts/todo-graph/build-and-validate.sh --keep-cache` for XREF/section drift and `python3 .claude/hooks/sequencer_triage.py --classify <todo file>` for stamp state.
- **Then the fuzzy residue:** dispatch `Agent(subagent_type="todo-hygiene-auditor", <todo file>)` for prose-vs-code drift only. VERIFY each punch-list item at its `file:line` before applying it -- the auditor proposes, you dispose (trust contract); never apply a punch-list item blind.
```

- [ ] **Step 3: Verify the wiring landed + lint**

Run:
```bash
grep -c "scripts/todo-hygiene.py" .claude/skills/complete-todo-file/SKILL.md
grep -c "todo-hygiene-auditor" .claude/skills/complete-todo-file/SKILL.md
bash scripts/lint.sh 2>&1 | tail -3
```
Expected: both greps `>= 1`; lint exits 0 (warnings ok).

- [ ] **Step 4: Commit**

```bash
git add .claude/skills/complete-todo-file/SKILL.md
git commit -m "skills: WS6 -- wire deterministic todo-hygiene + auditor into complete-todo-file"
```

---

## Self-Review

**Spec coverage (WS6):** deterministic-first enumerator for the rule-checkable classes (Task 1); Sonnet auditor for the fuzzy residue with the WS8 verify-before-apply contract (Task 2); wired into the close-out sweep (Task 3). Deliberately does not duplicate validate.py / sequencer_triage.py (Global Constraints + the agent's "out of scope" block). The harder section-level XREF extension and test-count drift are explicitly deferred (would touch runner-critical validate.py / need fuzzy parsing).

**Placeholder scan:** every code/edit step has complete content; commands carry expected output. Task 3 Step 1 is an inspect-then-edit (the exact sweep wording must be read first), not a placeholder.

**Type consistency:** the enumerator's output contract (`class<TAB>path:line<TAB>message`, exit 1 on findings) is asserted by the Task 1 test and consumed verbatim by the Task 3 wiring text. Placeholder token list + `_BULLET_RE` are defined once in Task 1.

## Follow-on (deferred, noted)

- Extend `validate.py` to section-validate prose / Inputs / stamp XREFs (today only IO-table deps) -- touches runner-critical tooling, separate review.
- Test-count-drift detector (prose "N kernel PASS" vs a real `scripts/test.sh` run).
- WS1b BLOCK promotion + triviality classifier (after WARN observed on a real overnight run).
- WS4 lsp-bridge hardening (after the in-flight `bridge.py` rework commits).
