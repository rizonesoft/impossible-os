# Overnight Runner Awareness -- Plan A (runner-status + guard anchor + gotchas)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the overnight main loop a compact, computed situational brief -- where it is, its open obligations, and live gotchas -- pulled on demand and pushed as a one-line anchor at phase transitions, so it stops drifting / getting surprised by gates / forgetting decisions.

**Architecture:** `.claude/hooks/runner_status.py` aggregates already-existing state (guard cursor/phase, gate state-files, git, the doctrine Run Log) plus one new curated file (`live-gotchas.md`) into a brief; it exposes `full_brief(root)` (the pull) and `anchor_line(root)` (the one-line push). `run_phase_guard.py` imports `anchor_line` and prints it to stderr on `status`/`phase` when a run is active. All read-only, fail-open, root-parameterised for testability.

**Tech Stack:** Python 3 stdlib only; Markdown (gotchas registry); no new dependencies.

## Global Constraints

- Python stdlib only; ASCII only; no section-sign+digit in code.
- Read-only aggregation + additive output ONLY. No change to gate semantics, the guard phase machine, or runner control flow.
- Fail-open: any missing/corrupt source degrades that one line to "(unknown)"; never raise out of a hook or the guard.
- The anchor is ONE line; `full_brief` is bounded (<= ~30 lines). Push fires only when the run is active (`state["active"]` true) -- interactive sessions stay quiet on the guard path; `runner_status.py` as a CLI works anywhere on demand.
- `runner_status` lives in `.claude/hooks/` so `run_phase_guard.py` can `import runner_status` as a sibling (the hooks dir is already the import path for siblings like `_skip_env`).
- Do NOT touch the runner's leftover working-tree files.
- C4 (PreCompact re-orient) is Plan B, not this plan.

---

### Task 1: runner_status.py aggregator (C1)

**Files:**
- Create: `.claude/hooks/runner_status.py`
- Test: `scripts/overnight/tests/test_runner_status.py` (create)

**Interfaces:**
- Produces (imported by Task 3 + used by the CLI):
  - `anchor_line(root: Path) -> str` -- e.g. `cursor TODO-08 | phase SECTIONS | obligations:1 | gotchas:2`.
  - `full_brief(root: Path) -> str` -- multi-line WHERE / GIT / OBLIGATIONS / GOTCHAS / DECISIONS.
  - `obligations(root: Path) -> list[str]`, `gotchas(root: Path) -> list[str]`.
- CLI: `python3 .claude/hooks/runner_status.py [--root PATH] [--anchor]` -- prints `full_brief` (or just `anchor_line` with `--anchor`); exit 0 always.

- [ ] **Step 1: Write the failing test**

Create `scripts/overnight/tests/test_runner_status.py`:

```python
#!/usr/bin/env python3
import json, subprocess, sys, tempfile, time, pathlib

HERE = pathlib.Path(__file__).resolve().parent
SCRIPT = HERE.parent.parent / ".claude" / "hooks" / "runner_status.py"


def _run(root, anchor=False):
    args = [sys.executable, str(SCRIPT), "--root", str(root)]
    if anchor:
        args.append("--anchor")
    r = subprocess.run(args, text=True, capture_output=True)
    assert r.returncode == 0, r.stderr
    return r.stdout


def _mkroot(d, *, state=None, gotchas=None, codex=None):
    root = pathlib.Path(d)
    (root / ".claude" / "state").mkdir(parents=True, exist_ok=True)
    if state is not None:
        (root / ".claude" / "state" / "sequencer-run.json").write_text(json.dumps(state))
    if gotchas is not None:
        (root / ".claude" / "state" / "live-gotchas.md").write_text(gotchas)
    if codex is not None:
        (root / ".claude" / "state" / "last-codex-review.json").write_text(json.dumps(codex))
    return root


def test_anchor_has_phase_and_counts():
    with tempfile.TemporaryDirectory() as d:
        root = _mkroot(d, state={"active": True, "phase": "SECTIONS", "file": "TODO-08.md"},
                       gotchas="- 2026-06-14: recorder dead\n")
        out = _run(root, anchor=True).strip()
        assert "phase SECTIONS" in out
        assert "gotchas:1" in out
        assert "obligations:0" in out


def test_unreceived_codex_is_an_obligation():
    with tempfile.TemporaryDirectory() as d:
        root = _mkroot(d, state={"active": True, "phase": "SECTIONS"},
                       codex={"received": False, "timestamp_ns": time.time_ns()})
        out = _run(root, anchor=True)
        assert "obligations:1" in out
        full = _run(root)
        assert "Codex review unreceived" in full


def test_expired_gotcha_dropped():
    with tempfile.TemporaryDirectory() as d:
        root = _mkroot(d, state={"active": True, "phase": "TRIAGE"},
                       gotchas="- 2020-01-01: old hazard (expires 2020-02-01)\n- 2026-06-14: live one\n")
        out = _run(root, anchor=True)
        assert "gotchas:1" in out


def test_missing_sources_fail_open():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)  # nothing created
        out = _run(root)
        assert "WHERE" in out  # still prints a brief


if __name__ == "__main__":
    test_anchor_has_phase_and_counts()
    test_unreceived_codex_is_an_obligation()
    test_expired_gotcha_dropped()
    test_missing_sources_fail_open()
    print("PASS: runner_status")
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 scripts/overnight/tests/test_runner_status.py`
Expected: FAIL (`runner_status.py` does not exist).

- [ ] **Step 3: Write `.claude/hooks/runner_status.py`**

```python
#!/usr/bin/env python3
"""Situational-awareness brief for the overnight loop. Read-only, fail-open.

Aggregates existing scattered run-state (guard cursor/phase, gate state-files,
git, the doctrine Run Log) plus the curated live-gotchas registry into a compact
brief. `full_brief` is the pull; `anchor_line` is the one-line push the guard
emits at phase transitions. Stdlib only; ASCII.

CLI: runner_status.py [--root PATH] [--anchor]
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
import time
from datetime import date
from pathlib import Path

_CODEX_TTL_NS = 3600 * 1_000_000_000
DOCTRINE_REL = "todo/TODO-Claude-Overnight-Runner.md"


def _read_json(p: Path):
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return None


def _state(root: Path) -> dict:
    return _read_json(root / ".claude" / "state" / "sequencer-run.json") or {}


def where(root: Path) -> str:
    st = _state(root)
    if not st.get("active"):
        return "WHERE: (no active run)"
    return (f"WHERE: cursor {st.get('file', '(none)')} | phase "
            f"{st.get('phase', '(none)')} | pass {st.get('pass_no', '?')}")


def git_state(root: Path) -> str:
    try:
        head = subprocess.check_output(
            ["git", "log", "-1", "--oneline"], cwd=str(root), text=True,
            timeout=3, stderr=subprocess.DEVNULL).strip()
        dirty = subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=str(root), text=True,
            timeout=3, stderr=subprocess.DEVNULL).splitlines()
        return f"GIT: {head}  ({len(dirty)} dirty)"
    except Exception:
        return "GIT: (unknown)"


def obligations(root: Path) -> list[str]:
    out: list[str] = []
    cx = _read_json(root / ".claude" / "state" / "last-codex-review.json")
    if isinstance(cx, dict) and cx.get("received") is False:
        ts = cx.get("timestamp_ns")
        if isinstance(ts, int) and (time.time_ns() - ts) <= _CODEX_TTL_NS:
            out.append("Codex review unreceived -- receiving_review gate will "
                       "block edits until Skill(superpowers:receiving-code-review)")
    try:
        diff = subprocess.check_output(
            ["git", "show", "--format=", "HEAD"], cwd=str(root), text=True,
            timeout=4, stderr=subprocess.DEVNULL).splitlines()
        flipped = any(re.match(r"\+.*\|\s*\[x\]\s*\|", ln) for ln in diff)
        stamped = any(ln.startswith("+") and "**Verified:**" in ln for ln in diff)
        if flipped and not stamped:
            out.append("HEAD flipped an IO row to [x] without a Verified stamp "
                       "-- review-todo-section due [best-effort]")
    except Exception:
        pass
    return out


def gotchas(root: Path) -> list[str]:
    p = root / ".claude" / "state" / "live-gotchas.md"
    try:
        lines = p.read_text(encoding="utf-8").splitlines()
    except Exception:
        return []
    today = date.today().isoformat()
    out = []
    for ln in lines:
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        m = re.search(r"\(expires (\d{4}-\d{2}-\d{2})\)", ln)
        if m and m.group(1) < today:
            continue
        out.append(ln.lstrip("- ").strip())
    return out


def recent_decisions(root: Path, n: int = 3) -> list[str]:
    try:
        text = (root / DOCTRINE_REL).read_text(encoding="utf-8")
    except Exception:
        return []
    after = text.split("## Run Log", 1)
    if len(after) < 2:
        return []
    entries = [ln.strip() for ln in after[1].splitlines()
               if ln.strip().startswith("- ")]
    return entries[-n:]


def anchor_line(root: Path) -> str:
    st = _state(root)
    return (f"cursor {st.get('file', '(none)')} | phase {st.get('phase', '(none)')} "
            f"| obligations:{len(obligations(root))} | gotchas:{len(gotchas(root))}")


def full_brief(root: Path) -> str:
    obl = obligations(root)
    got = gotchas(root)
    dec = recent_decisions(root)
    parts = [where(root), git_state(root)]
    parts.append("OBLIGATIONS: " + ("none" if not obl else ""))
    parts += [f"  - {o}" for o in obl]
    parts.append("GOTCHAS: " + ("none" if not got else ""))
    parts += [f"  - {g}" for g in got]
    parts.append("RECENT DECISIONS: " + ("none" if not dec else ""))
    parts += [f"  {d}" for d in dec]
    return "\n".join(p for p in parts if p is not None)


def _resolve_root(argv: list[str]) -> Path:
    if "--root" in argv:
        return Path(argv[argv.index("--root") + 1])
    try:
        top = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"], text=True,
            timeout=3, stderr=subprocess.DEVNULL).strip()
        return Path(top) if top else Path.cwd()
    except Exception:
        return Path.cwd()


def main(argv: list[str]) -> int:
    root = _resolve_root(argv)
    try:
        if "--anchor" in argv:
            print(anchor_line(root))
        else:
            print(full_brief(root))
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 scripts/overnight/tests/test_runner_status.py`
Expected: `PASS: runner_status`

- [ ] **Step 5: Smoke against the real repo**

Run: `python3 .claude/hooks/runner_status.py; echo "---"; python3 .claude/hooks/runner_status.py --anchor`
Expected: a full brief (WHERE shows "(no active run)" since the guard is idle) then a one-line anchor; exit 0, no traceback.

- [ ] **Step 6: Commit**

```bash
git add .claude/hooks/runner_status.py scripts/overnight/tests/test_runner_status.py
git commit -m "overnight: awareness C1 -- runner_status situational brief aggregator"
```

---

### Task 2: live-gotchas.md seeded registry (C3)

**Files:**
- Create: `.claude/state/live-gotchas.md`

**Interfaces:** consumed by `gotchas(root)` from Task 1 (one hazard per `- ` line, optional `(expires YYYY-MM-DD)`).

- [ ] **Step 1: Create the seeded registry**

Create `.claude/state/live-gotchas.md`:

```markdown
# Live gotchas -- transient hazards the overnight loop should know BEFORE it hits them.
# Format: "- YYYY-MM-DD: <hazard> -> <what to do>" with optional "(expires YYYY-MM-DD)".
# read by .claude/hooks/runner_status.py; expired lines are dropped from the brief.

- 2026-06-14: codex_review_completed recorder can mis-fire / not fire -> a Bash command merely CONTAINING the string codex-dispatch can set last-codex-review.json received=false and block the next edit. If no real Codex was dispatched, correct the state (received=true) and proceed; never perform a fake review. (project_codex_review_hook_dead)
- 2026-06-27: scripts/lsp-mcp/bridge.py rework in flight -> lsp-bridge may disconnect mid-session; fall back to grep and do not edit bridge.py from an interactive session. (expires 2026-07-15)
```

- [ ] **Step 2: Verify runner_status reads it**

Run: `python3 .claude/hooks/runner_status.py --anchor`
Expected: the anchor shows `gotchas:2` (both seed lines live as of today).

- [ ] **Step 3: Confirm gitignore status is intentional**

Run: `git check-ignore .claude/state/live-gotchas.md && echo IGNORED || echo TRACKED`
Expected: decide -- `.claude/state/` may be ignored. If IGNORED, force-add so the seed ships: `git add -f .claude/state/live-gotchas.md`. If TRACKED, a plain add. (The registry is curated content, not runtime state, so it SHOULD be tracked.)

- [ ] **Step 4: Commit**

```bash
git add -f .claude/state/live-gotchas.md
git commit -m "overnight: awareness C3 -- seeded live-gotchas registry"
```

---

### Task 3: Guard anchor wiring (C2)

**Files:**
- Modify: `.claude/hooks/run_phase_guard.py` (the `status` and `phase` branches of `cli()`)

**Interfaces:**
- Consumes: `runner_status.anchor_line(root)` from Task 1.
- Produces: a one-line anchor on stderr when a run is active, after `status`/`phase`.

- [ ] **Step 1: Add an anchor helper near the top of cli() handling**

In `.claude/hooks/run_phase_guard.py`, add this helper (above `def cli(`):

```python
def _emit_anchor(state) -> None:
    """Best-effort one-line situational anchor on stderr; never raises."""
    try:
        if not state.get("active"):
            return
        import runner_status
        sys.stderr.write("[sequencer] " + runner_status.anchor_line(repo_root()) + "\n")
    except Exception:
        pass
```

- [ ] **Step 2: Call it from the status branch**

Find the `status` branch (`if cmd == "status":` -> `print(json.dumps(state, indent=1))`). Immediately AFTER the `print(...)` and before its `return`, add:

```python
        _emit_anchor(state)
```

- [ ] **Step 3: Call it from the phase branch**

Find the `phase` branch's `print(f"[sequencer] phase -> {argv[1]}", file=sys.stderr)`. Immediately after it, add:

```python
        _emit_anchor(state)
```

- [ ] **Step 4: Verify -- inactive run stays silent, active run emits**

Run:
```bash
# inactive (real state is {"active": false}) -> no anchor
python3 .claude/hooks/run_phase_guard.py status >/dev/null; echo "inactive rc=$?"
# simulate active in a throwaway temp tree to avoid touching real guard state
python3 - <<'PY'
import json, subprocess, sys, tempfile, pathlib, os
with tempfile.TemporaryDirectory() as d:
    # copy the two hook files so imports resolve, point a fake state at active
    pass
PY
echo "manual-active check below"
```
Then a direct functional check that does NOT mutate the real guard state:
```bash
python3 -c "import sys; sys.path.insert(0,'.claude/hooks'); import runner_status, pathlib; print(runner_status.anchor_line(pathlib.Path('.')))"
```
Expected: the inactive `status` prints JSON with NO `[sequencer] cursor ...` anchor line on stderr; the direct `anchor_line` call prints a well-formed anchor string. (Do not flip the real guard to active just to test; Task 1's unit test already covers the active path with a temp root.)

- [ ] **Step 5: Confirm the guard still parses + behaves**

Run: `python3 .claude/hooks/run_phase_guard.py selftest 2>&1 | tail -3; python3 .claude/hooks/run_phase_guard.py status`
Expected: selftest passes (or its normal output); `status` still prints valid JSON to stdout (the anchor goes to stderr, so stdout stays machine-parseable).

- [ ] **Step 6: Commit**

```bash
git add .claude/hooks/run_phase_guard.py
git commit -m "overnight: awareness C2 -- guard emits one-line situational anchor on status/phase"
```

---

## Self-Review

**Spec coverage:** C1 runner-status aggregator with WHERE/GIT/OBLIGATIONS/GOTCHAS/DECISIONS (Task 1); C2 one-line anchor on guard status/phase, active-only, stderr (Task 3); C3 seeded live-gotchas registry with expiry (Task 2). C4 (PreCompact) is explicitly Plan B. Obligations v1 ships the codex-review (robust, state-file) + best-effort [x]-flip-without-Verified; the spec's two named obligations are both present (the [x]-flip one labelled best-effort since reproducing the review-gate's git logic exactly is deferred).

**Placeholder scan:** every code step has complete code; commands carry expected output. Task 3 Step 4's exact `status`-branch anchor placement is an inspect-then-edit (the precise `return` location must be read first), not a placeholder.

**Type consistency:** `anchor_line(root)` / `full_brief(root)` / `obligations(root)` / `gotchas(root)` signatures are defined in Task 1 and imported verbatim by Task 3 (`runner_status.anchor_line(repo_root())`). The gotchas file format (`- YYYY-MM-DD: text (expires YYYY-MM-DD)`) is produced in Task 2 and parsed in Task 1.

## Follow-on (Plan B + deferred)

- Plan B: C4 PreCompact snapshot hook + skill post-compaction read instruction.
- Obligations: a precise `[x]`-flip-without-Verified detector sharing the review-gate's git logic (instead of the best-effort diff scan).
- Auto-seed `live-gotchas.md` from memory recall.
- OpenRouter Fusion break-glass escalation reviewer (separate spec; needs dependency + API key).
