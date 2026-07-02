# Overnight Runner Plan 2 -- Context Offload + Enforcement + Digester + Discipline

> **Update 2026-07-02:** WS1b's deferred BLOCK promotion shipped (operator instruction): `agent_dispatch_required.py` now BLOCKs (exit 2) kernel/boot source edits in the overnight SECTIONS phase with no fresh agent dispatch, WARNs for other source; `SKIP_AGENT_DISPATCH_HOOK=1` is the triviality escape. Plan 3's WS5b model flips (explorer + parity -> Sonnet) also shipped, plus five new read-only agents (ssdt-auditor, serial-log-auditor, test-coverage-mapper, doc-sync-auditor, spec-research-analyst) -- fleet is 12, Sonnet-default with Opus only on kernel-quality-auditor.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the runner's existing read-only agents fire by default (WS1), add a failure-log digester agent (WS2), make the dispatch un-bypassable in overnight runs via a WARN-first hook (WS1b), and lint-guard against &-bundled Codex dispatch (WS7).

**Architecture:** WS1 is pure skill-doc threshold edits. WS2 adds one read-only Sonnet agent + its three registration surfaces. WS1b adds a PostToolUse recorder (`agent_dispatch_recorder.py` -> `.claude/state/last-agent-dispatch.json`) and a PreToolUse gate (`agent_dispatch_required.py`) that, **only during a headless overnight run in the SECTIONS phase**, WARNs (never blocks, this phase) when a source edit happens with no recent agent dispatch. WS7 adds a WARN-only lint check mirroring Check 12.

**Tech Stack:** Python 3 stdlib (hooks, lint scanners); Markdown (skills/agents/docs); Bash (lint + test harness). No new dependencies.

## Global Constraints

- ASCII only; no Unicode dashes; no section-sign+digit in code comments (markdown is exempt). Project code-style policy.
- Hooks MUST fail open: any exception or malformed stdin -> `return 0` (allow). Mirror `design_review_required.py`.
- WS1b is **WARN-only this plan** (exit 0 + stderr reminder). The hard BLOCK + triviality classifier is a later plan, gated on observing WARN behavior + WS3 data.
- WS1b must be **invisible in interactive sessions**: if `OVERNIGHT_SEQUENCER_RUN` is unset OR the guard phase is not `SECTIONS`, the gate returns 0 silently. It acts only inside a real overnight run. (`OVERNIGHT_SEQUENCER_RUN` is the headless discriminator; guard state is `.claude/state/sequencer-run.json` with `{active, phase}`.)
- Subagents stay read-only: `tools:` frontmatter must be a subset of `{Read, Grep, Glob, WebSearch, WebFetch}` (lint Check 14 auto-discovers every `.claude/agents/*.md` and enforces this; no lint edit needed to register an agent).
- `.claude/skills/README.md` agent rows use `[name](path)` (NOT backtick-wrapped) or the skill-catalog check misregisters them.
- Do NOT touch the runner's leftover working-tree files (`scripts/lsp-mcp/bridge.py`, `docs/test-coverage/*`, `todo/TODO-Claude-Overnight-Runner.md`).
- WS5b (parity-analyst -> Sonnet) is NOT in this plan: WS1 leaves the parity analyst at "optional" so default-on does not cause an Opus+WebSearch cost regression before Plan 3 flips its model.

---

### Task 1: WS1 -- Default-on the read-only agents (skill-doc edits)

**Files:**
- Modify: `.claude/skills/implement-todo-section/SKILL.md` (step 3)
- Modify: `.claude/skills/review-todo-section/SKILL.md` (step 1, step 7 intro, step 9 left as-is)

**Interfaces:** None (documentation behavior change).

- [ ] **Step 1: Default-on the explorer in implement step 3**

In `.claude/skills/implement-todo-section/SKILL.md`, replace the sentence:
`For very-large unfamiliar surface areas (first time touching a new domain, > 20 candidate files to read), dispatch a read-only explorer agent instead of tracing by hand -- it returns a focused file list to read.`
with:
`Dispatch a read-only explorer agent BY DEFAULT to spare this session the bulk exploration reads (it returns a focused file list + integration surface in a throwaway context). Skip the explorer ONLY for genuinely tiny sections -- docs/stamp-only, or a single function with < ~5 candidate files -- where dispatching one would cost more than it saves.`

- [ ] **Step 2: Default-on the evidence-mapper in review step 1**

In `.claude/skills/review-todo-section/SKILL.md`, replace:
`**For large sections, first dispatch \`Agent(subagent_type="review-evidence-mapper", ...)\`**`
with:
`**By default, first dispatch \`Agent(subagent_type="review-evidence-mapper", ...)\`** (skip only for tiny stamp-only/docs sections)`

- [ ] **Step 3: Confirm step 7 auditor is already default-on; adjust only if gated**

Run: `sed -n '36,46p' .claude/skills/review-todo-section/SKILL.md`
If step 7's auditor dispatch is phrased as "optionally"/"for large", change that qualifier to "by default (skip only tiny sections)". If it already reads as an unconditional path-routed dispatch, leave it unchanged and note so.

- [ ] **Step 4: Verify wording landed + lint clean**

Run:
```bash
grep -c "BY DEFAULT to spare this session" .claude/skills/implement-todo-section/SKILL.md
grep -c "By default, first dispatch" .claude/skills/review-todo-section/SKILL.md
bash scripts/lint.sh .claude/skills/implement-todo-section/SKILL.md .claude/skills/review-todo-section/SKILL.md
```
Expected: both greps print `1`; lint exits 0.

- [ ] **Step 5: Commit**

```bash
git add .claude/skills/implement-todo-section/SKILL.md .claude/skills/review-todo-section/SKILL.md
git commit -m "skills: WS1 -- default-on read-only explorer + evidence-mapper dispatch"
```

---

### Task 2: WS7 -- Lint check rejecting &-bundled Codex dispatch

**Files:**
- Modify: `scripts/lint.sh` (add Check 15 after Check 14)
- Modify: `scripts/test-tooling.sh` (add a sub-test group)

**Interfaces:**
- Produces: a `warn:` line for any file under `scripts/`, `.claude/skills/`, `docs/` whose text contains two `codex-dispatch.sh` invocations joined by `&` / `&&` on one logical command, or a `codex-dispatch.sh ... &` background-bundle. WARN-only (mirrors Check 12; the harm is documented-example drift).

- [ ] **Step 1: Add Check 15 to scripts/lint.sh**

Immediately before the final verdict block (the `if [ "$ERRORS" -gt 0 ]` summary near line 1015), insert:

```bash
# --- Check 15: no &-bundled Codex dispatch in documented examples ---------
# Two codex-dispatch.sh in one command (joined by & / && / ;) records only the
# first review-kind and breaks the section-commit gate. Serial, one Bash call
# each. WARN-only: catches drift in skills/scripts/docs examples.
if [ "${SKIP_LINT_CODEX_BUNDLE:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 15 (codex-&-bundle) skipped via SKIP_LINT_CODEX_BUNDLE=1"
    WARNINGS=$((WARNINGS + 1))
else
    LINT15_OUT="$(python3 - "$REPO_ROOT" <<'PYEOF'
import os, re, sys
root = sys.argv[1]
roots = ["scripts", ".claude/skills", "docs"]
exts = (".sh", ".md", ".py", ".mjs")
self_name = "lint.sh"
# A line that mentions codex-dispatch.sh at least twice, OR once followed by a
# control operator and another codex dispatch / a trailing '&'.
two = re.compile(r"codex-(?:bg-)?dispatch\.sh.*(?:&&|&|;).*codex-(?:bg-)?dispatch\.sh")
bg = re.compile(r"codex-(?:bg-)?dispatch\.sh[^\n]*\s&\s*$")
warns = []
for base in roots:
    for dirpath, _dirs, files in os.walk(os.path.join(root, base)):
        for fn in files:
            if not fn.endswith(exts) or fn == self_name:
                continue
            rel = os.path.relpath(os.path.join(dirpath, fn), root)
            try:
                lines = open(os.path.join(dirpath, fn), encoding="utf-8",
                             errors="replace").read().splitlines()
            except OSError:
                continue
            for i, ln in enumerate(lines, 1):
                if two.search(ln) or bg.search(ln):
                    warns.append(f"{rel}:{i}: &-bundled codex dispatch -- run each "
                                 f"dispatch as its own Bash call")
for w in warns:
    print(f"WARN {w}")
PYEOF
)"
    if [ -n "$LINT15_OUT" ]; then
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            case "$line" in
                WARN\ *)
                    echo -e "${YELLOW}warn${NC}: ${line#WARN }"
                    WARNINGS=$((WARNINGS + 1))
                    ;;
            esac
        done <<< "$LINT15_OUT"
    fi
fi
```

- [ ] **Step 2: Verify clean tree passes (no &-bundle today)**

Run: `bash scripts/lint.sh 2>&1 | grep -c "codex dispatch -- run each"`
Expected: `0` (no existing &-bundled examples).

- [ ] **Step 3: Verify it fires on a planted fixture**

Run:
```bash
mkdir -p /tmp/lint15 && cat > docs/_lint15_fixture.md <<'EOF'
Example: bash scripts/codex-dispatch.sh '[review-kind: adversarial] x' & bash scripts/codex-dispatch.sh '[review-kind: perf] y'
EOF
bash scripts/lint.sh 2>&1 | grep "codex dispatch -- run each"
rm -f docs/_lint15_fixture.md
```
Expected: one `warn:` line naming `_lint15_fixture.md`. Then the file is removed.

- [ ] **Step 4: Add a test-tooling.sh sub-test group**

After the `codex_model_flag_block` group's closing `fi` (near line 933), add:

```bash
# --- Check 15 codex-&-bundle lint -----------------------------------------
[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[lint_codex_bundle]${NC}"
_BUNDLE_FIX="$REPO_ROOT/docs/_tooling_lint15_fixture.md"
printf '%s\n' "x: bash scripts/codex-dispatch.sh '[review-kind: adversarial] a' && bash scripts/codex-dispatch.sh '[review-kind: perf] b'" > "$_BUNDLE_FIX"
if bash "$REPO_ROOT/scripts/lint.sh" 2>&1 | grep -q "codex dispatch -- run each"; then
    t_pass "lint Check 15 flags &-bundled codex dispatch"
else
    t_fail "lint Check 15 missed &-bundled codex dispatch"
fi
rm -f "$_BUNDLE_FIX"
if bash "$REPO_ROOT/scripts/lint.sh" 2>&1 | grep -q "codex dispatch -- run each"; then
    t_fail "lint Check 15 false-positive on clean tree"
else
    t_pass "lint Check 15 clean on clean tree"
fi
```

- [ ] **Step 5: Run the tooling tests**

Run: `bash scripts/test-tooling.sh 2>&1 | grep -iE "lint_codex_bundle|Check 15|FAIL" | head`
Expected: both Check-15 sub-tests pass; no FAIL.

- [ ] **Step 6: Commit**

```bash
git add scripts/lint.sh scripts/test-tooling.sh
git commit -m "lint: WS7 -- Check 15 rejects &-bundled Codex dispatch (WARN) + tooling test"
```

---

### Task 3: WS2 -- diagnostic-digester agent (+ WS8 trust contract)

**Files:**
- Create: `.claude/agents/diagnostic-digester.md`
- Modify: `CLAUDE.md` (specialist-agents table + "Five"->"Six")
- Modify: `.claude/skills/README.md` (agent rows)
- Modify: `.claude/skills/implement-todo-section/SKILL.md` (fix-loop wiring)
- Modify: `.claude/skills/review-todo-section/SKILL.md` (build-fail wiring)

**Interfaces:**
- Produces: an agent `diagnostic-digester` dispatchable via `Agent(subagent_type="diagnostic-digester", ...)`, returning a structured failure digest (hypotheses + offending `file:line` + minimal log slice). Read-only; the main loop validates before fixing.

- [ ] **Step 1: Create the agent file**

Create `.claude/agents/diagnostic-digester.md`:

```markdown
---
name: diagnostic-digester
description: Failure-log digester for Impossible OS. Dispatched on a build/test/smoke/Codex failure to read the full serial/build log + failing output in a throwaway context and return a structured digest -- candidate root-cause hypotheses, offending file:line refs, and the minimal relevant log slice -- so the main session does not swallow a multi-hundred-KB log. Read-only; proposes hypotheses only. Does not propose or apply fixes, edit, build, commit, dispatch Codex, or invoke skills. The main session validates each hypothesis before any fix (WS8 trust contract).
model: sonnet
tools: Read, Grep, Glob
---

# Diagnostic Digester

You are a read-only failure-log digester. You are given a path to a serial/build
log (and optionally a failing test name or symptom). Read it and return a compact,
itemized digest -- never prose, never a fix.

## Return shape

1. **Symptom** -- one line: what failed (panic/assert/build error/test name).
2. **Offending locations** -- up to 5 `file:line` references the log points at
   (RIP/addr2line output, failing assertion file:line, compiler error location).
3. **Candidate hypotheses** -- up to 3, each one line, ordered most-likely-first.
   Each MUST be checkable (the main session can confirm/refute it at a file:line).
4. **Minimal log slice** -- the smallest contiguous excerpt (<= ~30 lines) that
   carries the failure signal.

## Hard rules

- Read-only. You do NOT edit, build, commit, dispatch Codex, or invoke skills.
- You PROPOSE hypotheses; you never assert a root cause as fact and never propose
  a code change. The main session validates each hypothesis at file:line before
  fixing -- your output is a lead, not a verdict (WS8 trust contract).
- ASCII only. No section-sign+digit references.
```

- [ ] **Step 2: Add the CLAUDE.md table row + bump the count**

In `CLAUDE.md`, in the `### Specialist agents` table add after the `parity-research-analyst` row:
```markdown
| `diagnostic-digester` | sonnet | `implement-todo-section` fix loop + `review-todo-section` build-fail (failure-log digest) |
```
And change the sentence `Five subagents in [\`.claude/agents/\`]` to `Six subagents in [\`.claude/agents/\`]`.

- [ ] **Step 3: Add the README.md row**

In `.claude/skills/README.md`, in the `## Specialist agents` table add:
```markdown
| [diagnostic-digester](../agents/diagnostic-digester.md) | sonnet | `implement-todo-section` fix loop + `review-todo-section` build-fail |
```

- [ ] **Step 4: Wire it into the fix-loop steps**

In `.claude/skills/implement-todo-section/SKILL.md` fix-loop step (step 15), append:
`When a build/test/smoke failure produces a large log, dispatch \`Agent(subagent_type="diagnostic-digester", <log path>)\` first and validate its hypotheses at file:line before editing -- do not read the whole log into this context.`

In `.claude/skills/review-todo-section/SKILL.md` build-fail step (step 6), append the same sentence.

- [ ] **Step 5: Verify registration + read-only allowlist**

Run:
```bash
bash scripts/lint.sh 2>&1 | grep -iE "diagnostic-digester|Check 14|read-only allowlist" || echo "lint: no agent-tools violation"
grep -c "diagnostic-digester" CLAUDE.md .claude/skills/README.md .claude/agents/diagnostic-digester.md
```
Expected: no Check-14 violation (tools are read-only); each file shows >= 1 match.

- [ ] **Step 6: Commit**

```bash
git add .claude/agents/diagnostic-digester.md CLAUDE.md .claude/skills/README.md .claude/skills/implement-todo-section/SKILL.md .claude/skills/review-todo-section/SKILL.md
git commit -m "agents: WS2 -- diagnostic-digester (read-only Sonnet failure-log digest) + WS8 validate-before-fix"
```

---

### Task 4: WS1b -- WARN-mode agent-dispatch enforcement (recorder + gate)

**Files:**
- Create: `.claude/hooks/agent_dispatch_recorder.py` (PostToolUse on Task)
- Create: `.claude/hooks/agent_dispatch_required.py` (PreToolUse on Edit|Write|MultiEdit)
- Modify: `.claude/settings.json` (wire both)
- Modify: `scripts/test-tooling.sh` (sub-test group)

**Interfaces:**
- `agent_dispatch_recorder.py`: on a `Task` tool call, atomic-writes `.claude/state/last-agent-dispatch.json` = `{"timestamp_ns": int, "subagent_type": str, "head_sha": str}`.
- `agent_dispatch_required.py`: reads that file + guard state; WARNs (stderr, exit 0) when an overnight SECTIONS-phase source edit has no agent dispatch within the last 1800s. Silent (exit 0) otherwise.

- [ ] **Step 1: Write the recorder**

Create `.claude/hooks/agent_dispatch_recorder.py`:

```python
#!/usr/bin/env python3
"""PostToolUse: record the last Task/Agent dispatch for the WS1b gate.

State hook (warning-only; never exits 2). Writes
.claude/state/last-agent-dispatch.json atomically. Fail-open.
"""
from __future__ import annotations

import json
import os
import secrets
import subprocess
import sys
import time
from pathlib import Path


def _repo_root() -> Path | None:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return None
    return Path(out) if out else None


def _head_sha(root: Path) -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=str(root),
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return ""


def _write_atomic(path: Path, data: dict) -> None:
    tmp = path.with_suffix(f"{path.suffix}.{os.getpid()}.{secrets.token_hex(4)}.tmp")
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        os.replace(str(tmp), str(path))
    except Exception:
        try:
            tmp.unlink()
        except Exception:
            pass


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if d.get("tool_name") != "Task":
        return 0
    root = _repo_root()
    if root is None:
        return 0
    ti = d.get("tool_input") or {}
    state = {
        "timestamp_ns": time.time_ns(),
        "subagent_type": ti.get("subagent_type") or "",
        "head_sha": _head_sha(root),
    }
    _write_atomic(root / ".claude" / "state" / "last-agent-dispatch.json", state)
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Write the WARN-mode gate**

Create `.claude/hooks/agent_dispatch_required.py`:

```python
#!/usr/bin/env python3
"""PreToolUse: WARN when an overnight SECTIONS-phase source edit happens with no
recent agent dispatch. WARN-only (exit 0 + stderr); invisible in interactive
sessions. Fail-open. The BLOCK promotion + triviality classifier is a later plan.
"""
from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

FRESH_NS = 1800 * 1_000_000_000  # 30 min


def _is_source_target(path: str) -> bool:
    if not path:
        return False
    if path.endswith(".md"):
        return False
    for seg in (".claude/", "build/", "docs/"):
        if seg in path:
            return False
    return path.endswith((".c", ".h", ".asm", ".S", ".py", ".sh"))


def _repo_root() -> Path | None:
    here = Path(__file__).resolve()
    for p in here.parents:
        if (p / ".claude").is_dir():
            return p
    return None


def _guard_in_sections(root: Path) -> bool:
    try:
        st = json.loads((root / ".claude" / "state" / "sequencer-run.json").read_text())
    except Exception:
        return False
    return bool(st.get("active")) and st.get("phase") == "SECTIONS"


def _recent_dispatch(root: Path) -> bool:
    try:
        st = json.loads((root / ".claude" / "state" / "last-agent-dispatch.json").read_text())
    except Exception:
        return False
    ts = st.get("timestamp_ns")
    return isinstance(ts, int) and (time.time_ns() - ts) <= FRESH_NS


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    # Invisible outside a headless overnight run.
    if not os.environ.get("OVERNIGHT_SEQUENCER_RUN"):
        return 0
    if os.environ.get("SKIP_AGENT_DISPATCH_HOOK") == "1":
        return 0
    if d.get("tool_name") not in ("Edit", "Write", "MultiEdit"):
        return 0
    ti = d.get("tool_input") or {}
    if not _is_source_target(ti.get("file_path") or ti.get("path") or ""):
        return 0
    root = _repo_root()
    if root is None or not _guard_in_sections(root):
        return 0
    if _recent_dispatch(root):
        return 0
    sys.stderr.write(
        "[agent-dispatch reminder] WS1b: a source edit in the SECTIONS phase with "
        "no read-only explorer/auditor dispatch in the last 30 min. Default-on "
        "agents keep the main context lean. Dispatch one, or set "
        "SKIP_AGENT_DISPATCH_HOOK=1 with a logged reason if this section is tiny.\n"
    )
    return 0  # WARN-only this plan


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Verify both hooks behave (manual probes)**

Run:
```bash
# recorder writes state
echo '{"tool_name":"Task","tool_input":{"subagent_type":"kernel-explorer"}}' | python3 .claude/hooks/agent_dispatch_recorder.py; echo "rec rc=$?"
cat .claude/state/last-agent-dispatch.json
# gate is SILENT in interactive (env unset)
echo '{"tool_name":"Edit","tool_input":{"file_path":"src/kernel/x.c"}}' | python3 .claude/hooks/agent_dispatch_required.py; echo "gate(interactive) rc=$? (expect 0, no stderr)"
```
Expected: recorder rc=0 and the JSON has `subagent_type` + `timestamp_ns`; gate rc=0 with NO stderr (env unset -> invisible).

- [ ] **Step 4: Wire both into settings.json**

In `.claude/settings.json`, add to the `PreToolUse` array a new block (sibling of the existing standalone blocks):
```json
      {
        "matcher": "Edit|Write|MultiEdit",
        "hooks": [
          {
            "type": "command",
            "command": "python3 \"${CLAUDE_PROJECT_DIR:-.}/.claude/hooks/agent_dispatch_required.py\"",
            "timeout": 5
          }
        ]
      },
```
And add to the `PostToolUse` array:
```json
      {
        "matcher": "Task",
        "hooks": [
          {
            "type": "command",
            "command": "python3 \"${CLAUDE_PROJECT_DIR:-.}/.claude/hooks/agent_dispatch_recorder.py\"",
            "timeout": 5
          }
        ]
      },
```

- [ ] **Step 5: Validate settings.json parses**

Run: `python3 -c "import json; json.load(open('.claude/settings.json')); print('settings.json OK')"`
Expected: `settings.json OK`.

- [ ] **Step 6: Add a test-tooling.sh sub-test group**

After the Check-15 group, add:
```bash
# --- WS1b agent-dispatch gate ---------------------------------------------
[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[agent_dispatch_gate]${NC}"
_AD_GATE="$REPO_ROOT/.claude/hooks/agent_dispatch_required.py"
_ad_probe() {  # <expected_rc> <desc> <env-assignments...> -- payload on stdin via $PAYLOAD
    local want="$1" desc="$2"; shift 2
    local got
    got=$(printf '%s' "$PAYLOAD" | env "$@" python3 "$_AD_GATE" 2>/dev/null; echo " rc=$?")
    local rc=${got##* rc=}
    if [ "$rc" = "$want" ]; then t_pass "agent_dispatch_gate: $desc (rc=$rc)";
    else t_fail "agent_dispatch_gate: $desc (want $want got $rc)"; fi
}
PAYLOAD='{"tool_name":"Edit","tool_input":{"file_path":"src/kernel/x.c"}}'
_ad_probe 0 "silent when OVERNIGHT_SEQUENCER_RUN unset" "PATH=$PATH"
_ad_probe 0 "silent with SKIP opt-out" "PATH=$PATH" "OVERNIGHT_SEQUENCER_RUN=1" "SKIP_AGENT_DISPATCH_HOOK=1"
# md target is never a source edit
PAYLOAD='{"tool_name":"Edit","tool_input":{"file_path":"docs/x.md"}}'
_ad_probe 0 "silent on markdown target" "PATH=$PATH" "OVERNIGHT_SEQUENCER_RUN=1"
```

- [ ] **Step 7: Run the gate sub-tests**

Run: `bash scripts/test-tooling.sh 2>&1 | grep -iE "agent_dispatch_gate|FAIL" | head`
Expected: all three gate sub-tests pass; no FAIL.

- [ ] **Step 8: Commit**

```bash
git add .claude/hooks/agent_dispatch_recorder.py .claude/hooks/agent_dispatch_required.py .claude/settings.json scripts/test-tooling.sh
git commit -m "hooks: WS1b -- WARN-mode agent-dispatch enforcement (recorder + gate, overnight-only)"
```

---

## Self-Review

**Spec coverage:** WS1 default-on (Task 1); WS1b enforcement as WARN-first, overnight-scoped, fail-open (Task 4); WS2 digester + WS8 validate-before-fix (Task 3); WS7 &-bundle lint (Task 2). WS5b deliberately excluded (noted -- avoids a pre-Plan-3 cost regression). WS8 for the digester is realized as its agent-file contract + the fix-loop "validate before editing" wiring.

**Placeholder scan:** every code/edit step shows complete content; commands have expected output. Step 1.3 is a conditional inspect-then-edit (legitimate -- the exact step-7 wording must be read first), not a placeholder.

**Type consistency:** `last-agent-dispatch.json` fields (`timestamp_ns`, `subagent_type`, `head_sha`) are written by the recorder (Task 4 Step 1) and read by the gate (`timestamp_ns`, Task 4 Step 2) -- consistent. Guard state fields (`active`, `phase`) match `sequencer-run.json` as documented in `run_phase_guard.py`.

**Risk note:** the only globally-wired change is Task 4's settings.json hooks. Both fail open and the gate returns 0 immediately when `OVERNIGHT_SEQUENCER_RUN` is unset, so interactive sessions (including the implementing session) are unaffected. Verified by Task 4 Step 3.

## Follow-on (Plan 3, gated on WS3 data)

WS5a (main-loop Medium-vs-High A/B), WS5b (parity + explorer -> Sonnet, default-on parity then), WS6 (deterministic-first checks + `todo-hygiene-auditor` + WS8 verification), and the WS1b BLOCK promotion + triviality classifier.
