# Overnight Runner Awareness -- Plan B (C4 post-compaction re-orient)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After a context compaction (or a resume), automatically re-inject the situational brief so the overnight loop rebuilds awareness instead of flailing -- without relying on it to remember to pull.

**Architecture:** A new SessionStart hook (`session_brief_inject.py`) fires on `source=compact`/`resume`; when a run is active it recomputes `runner_status.full_brief(root)` and emits it as a `systemMessage`, so the brief lands in the post-compaction context as a push.

**Tech Stack:** Python 3 stdlib; `.claude/settings.json` hook wiring; no new dependencies.

## Global Constraints

- Python stdlib only; ASCII only.
- Read-only + additive output; never blocks; fail-open (any error -> no injection, return 0).
- Active-runs-only: gate on `sequencer-run.json` `active == True` so interactive sessions are not injected into. Interactive re-orientation is the operator's job.
- Inject only on the lost-context sources: `compact` and `resume` (NOT `startup`/`clear` -- the skill's step 0 already orients a fresh start).
- `session_brief_inject.py` lives in `.claude/hooks/` so it can `import runner_status` (the Plan-A aggregator) as a sibling.

**Spec deviation (improvement) vs the C4 design.** The spec said "PreCompact snapshots the brief; the skill reads the snapshot after compaction." Two facts make a better design: (1) `pre_compact_flush.py` ALREADY snapshots every `.claude/state/*.json` before compaction, so state preservation is done; (2) `runner_status` recomputes the brief from that live on-disk state, so a stale snapshot adds nothing and re-running it post-compaction would still rely on the loop remembering. So C4 ships as an AUTO-INJECT (push) on SessionStart(compact/resume) instead of a snapshot+manual-read (pull) -- same goal, robust against the very "forgets to look" failure this whole layer fixes.

---

### Task 1: session_brief_inject.py SessionStart hook

**Files:**
- Create: `.claude/hooks/session_brief_inject.py`
- Test: `scripts/overnight/tests/test_session_brief_inject.py` (create)

**Interfaces:**
- Consumes: `runner_status.full_brief(root)` (Plan A).
- Produces: `brief_for_event(event: dict, root: Path) -> str | None` -- the brief when the event is a compact/resume of an active run, else `None`. `main()` wires stdin + repo root + prints `{"systemMessage": ...}`.

- [ ] **Step 1: Write the failing test**

Create `scripts/overnight/tests/test_session_brief_inject.py`:

```python
#!/usr/bin/env python3
import json, sys, tempfile, pathlib

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
HOOKS = REPO / ".claude" / "hooks"
sys.path.insert(0, str(HOOKS))
import session_brief_inject as sbi  # noqa: E402


def _mkroot(d, active):
    root = pathlib.Path(d)
    (root / ".claude" / "state").mkdir(parents=True, exist_ok=True)
    (root / ".claude" / "state" / "sequencer-run.json").write_text(
        json.dumps({"active": active, "phase": "SECTIONS", "file": "TODO-X.md"}))
    return root


def test_compact_active_returns_brief():
    with tempfile.TemporaryDirectory() as d:
        root = _mkroot(d, active=True)
        b = sbi.brief_for_event({"source": "compact"}, root)
        assert b is not None and "WHERE" in b


def test_resume_active_returns_brief():
    with tempfile.TemporaryDirectory() as d:
        root = _mkroot(d, active=True)
        assert sbi.brief_for_event({"source": "resume"}, root) is not None


def test_startup_returns_none():
    with tempfile.TemporaryDirectory() as d:
        root = _mkroot(d, active=True)
        assert sbi.brief_for_event({"source": "startup"}, root) is None


def test_inactive_returns_none():
    with tempfile.TemporaryDirectory() as d:
        root = _mkroot(d, active=False)
        assert sbi.brief_for_event({"source": "compact"}, root) is None


if __name__ == "__main__":
    test_compact_active_returns_brief()
    test_resume_active_returns_brief()
    test_startup_returns_none()
    test_inactive_returns_none()
    print("PASS: session_brief_inject")
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 scripts/overnight/tests/test_session_brief_inject.py`
Expected: FAIL (`ModuleNotFoundError: session_brief_inject`).

- [ ] **Step 3: Write `.claude/hooks/session_brief_inject.py`**

```python
#!/usr/bin/env python3
# block-via: warning-only (SessionStart context injection; never blocks)
"""SessionStart hook -- re-inject the situational brief after a compaction/resume.

On source in {compact, resume} for an ACTIVE overnight run, recompute the
runner_status brief and emit it as a systemMessage so the post-compaction context
starts oriented. Interactive sessions (no active run) get nothing. Fail-open.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path


def _repo_root() -> Path | None:
    here = Path(__file__).resolve()
    for p in here.parents:
        if (p / ".claude").is_dir():
            return p
    return None


def brief_for_event(event: dict, root: Path) -> str | None:
    if not isinstance(event, dict):
        return None
    if event.get("source") not in ("compact", "resume"):
        return None
    try:
        st = json.loads(
            (root / ".claude" / "state" / "sequencer-run.json").read_text(encoding="utf-8"))
    except Exception:
        return None
    if not st.get("active"):
        return None
    try:
        sys.path.insert(0, str(root / ".claude" / "hooks"))
        import runner_status
        return runner_status.full_brief(root)
    except Exception:
        return None


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    root = _repo_root()
    if root is None:
        return 0
    brief = brief_for_event(d, root)
    if brief:
        print(json.dumps({"systemMessage":
                          "[post-compaction re-orient -- situational brief]\n" + brief}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 scripts/overnight/tests/test_session_brief_inject.py`
Expected: `PASS: session_brief_inject`

- [ ] **Step 5: Manual probe -- inactive real repo injects nothing**

Run: `echo '{"source":"compact"}' | python3 .claude/hooks/session_brief_inject.py; echo "rc=$? (expect 0, NO output -- real run is inactive)"`
Expected: rc=0, no stdout (guard is inactive, so no injection).

- [ ] **Step 6: Commit**

```bash
git add .claude/hooks/session_brief_inject.py scripts/overnight/tests/test_session_brief_inject.py
git commit -m "overnight: awareness C4 -- SessionStart re-injects situational brief post-compaction"
```

---

### Task 2: Wire the hook into settings.json + adjust the skill note

**Files:**
- Modify: `.claude/settings.json` (SessionStart array)
- Modify: `.claude/skills/overnight-sequencer/SKILL.md` (the compaction line from Plan A)

**Interfaces:** registers the SessionStart hook; updates the skill so the loop knows the brief is auto-injected.

- [ ] **Step 1: Add the hook to the SessionStart array**

In `.claude/settings.json`, find the `"SessionStart"` array (where `session_start.py` is registered). Add a sibling block:
```json
      {
        "hooks": [
          {
            "type": "command",
            "command": "python3 \"${CLAUDE_PROJECT_DIR:-.}/.claude/hooks/session_brief_inject.py\"",
            "timeout": 5
          }
        ]
      },
```

- [ ] **Step 2: Validate settings.json parses**

Run: `python3 -c "import json; d=json.load(open('.claude/settings.json')); print('SessionStart blocks:', len(d['hooks']['SessionStart']))"`
Expected: `settings.json` parses; SessionStart block count incremented by 1.

- [ ] **Step 3: Update the skill's compaction line**

In `.claude/skills/overnight-sequencer/SKILL.md`, change the Situational-awareness bullet that says "immediately after any context compaction -- run `python3 .claude/hooks/runner_status.py`" to note the auto-injection:
```markdown
- When `obligations:>0` or `gotchas:>0`, and at the start of each section, run
  `python3 .claude/hooks/runner_status.py` and read the full brief BEFORE acting.
  (After a compaction or resume the brief is AUTO-INJECTED for you by the
  `session_brief_inject` SessionStart hook -- read that injected brief; pull a
  fresh one only if you then act and need current detail.)
```

- [ ] **Step 4: Verify wiring + skill**

Run:
```bash
grep -c "session_brief_inject" .claude/settings.json
grep -c "AUTO-INJECTED" .claude/skills/overnight-sequencer/SKILL.md
python3 scripts/overnight/tests/test_session_brief_inject.py
```
Expected: settings grep `>= 1`; skill grep `1`; test PASS.

- [ ] **Step 5: Commit**

```bash
git add .claude/settings.json .claude/skills/overnight-sequencer/SKILL.md
git commit -m "overnight: awareness C4 -- wire post-compaction brief inject + note auto-inject in skill"
```

---

## Self-Review

**Spec coverage:** C4 post-compaction re-orient (Task 1 hook + Task 2 wiring). Delivered as auto-inject on SessionStart(compact/resume) rather than PreCompact-snapshot + manual-read -- documented deviation (improvement) above; state preservation is already handled by the existing `pre_compact_flush.py`.

**Placeholder scan:** every code step has complete code; commands carry expected output. Task 2 Step 1/Step 3 are inspect-then-edit against the live SessionStart block / Plan-A skill line (exact surrounding text must be read first), not placeholders.

**Type consistency:** `brief_for_event(event, root)` defined in Task 1 and exercised by the Task 1 test; `runner_status.full_brief(root)` matches the Plan-A signature. The `{"systemMessage": str}` output mirrors the existing `session_start.py` injection contract verbatim.

## Follow-on (deferred)

- WS1b BLOCK promotion + triviality classifier (after WARN observed on a real run).
- WS4 lsp-bridge hardening (after the in-flight bridge.py commits).
- A real overnight run to produce WS3 metrics validating the whole awareness + offload effort.
