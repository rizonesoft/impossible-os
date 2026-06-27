---
name: overnight-sequencer
description: The unattended OS-completion driver. Runs the fixpoint loop over every TODO file in `todo/` using the exact per-file pipeline (triage -> validate -> gap-audit -> per-section implement/review -> close -> advance), enforced by run_phase_guard.py so it cannot deviate, ask, or stop before fixpoint. Invoked by the headless overnight run (armed via arm-sequencer.sh); follows todo/TODO-Claude-Overnight-Runner.md as the source of truth.
---

# Overnight Sequencer

> **You are the unattended completion driver.** Your job: drive every TODO file
> under `todo/` to completion using the exact pipeline below, looping until a
> whole pass makes zero progress (fixpoint). You never ask a human, never stop
> mid-queue, and never ship broken code. `run_phase_guard.py` hard-blocks any
> deviation; this skill is the program it enforces.

## Source of truth

[`todo/TODO-Claude-Overnight-Runner.md`](../../../todo/TODO-Claude-Overnight-Runner.md)
is the control program (traversal order, per-file pipeline, hard rules). **Re-read
it now and after every context compaction.** This skill is the operational
procedure; the doctrine file is the law. If they ever disagree, the doctrine file
wins and this skill is the bug.

## The phase machine you drive

You move the run through phases by calling the guard CLI. The guard then blocks
any tool call that does not belong to the current phase, so you MUST keep the
phase in sync with what you are doing:

```
python3 .claude/hooks/run_phase_guard.py <cmd>
  start <date>            begin a run (phase PREFLIGHT)
  phase <PHASE>           PREFLIGHT|TRIAGE|VALIDATE|GAP_AUDIT|SECTIONS|FILE_CLOSE|ADVANCE
  cursor <domain> <file>  record the cursor
  progress                mark that this pass shipped/cleared something
  next-pass               start the next full sweep (resets progress)
  fixpoint                run complete -> stops the watchdog
  status                  print the cursor state
```

## Situational awareness (read before acting)

Every `run_phase_guard.py status`/`phase` prints a one-line anchor to stderr:
`[sequencer] cursor <file> | phase <PHASE> | obligations:N | gotchas:M`.

- When `obligations:>0` or `gotchas:>0`, and at the start of each section, run
  `python3 .claude/hooks/runner_status.py` and read the full brief (WHERE / GIT /
  OBLIGATIONS / GOTCHAS / RECENT DECISIONS) BEFORE acting. It is computed from live
  state, so it is the ground truth for "where am I and what is pending."
- **After a compaction or resume the brief is AUTO-INJECTED** for you by the
  `session_brief_inject` SessionStart hook -- read that injected brief to
  re-orient; pull a fresh `runner_status.py` only if you then act and need current
  detail.
- **Obligations** are unmet gates that will block you (e.g. an unreceived Codex
  review). Clear them, do not fight them.
- **Gotchas** are transient hazards in `.claude/state/live-gotchas.md`. When you
  discover a new one (a flaky gate, an in-flight file, a tool that mis-fires),
  append a dated line `- YYYY-MM-DD: <hazard> -> <what to do>` (optional
  `(expires YYYY-MM-DD)`) so the next pass is not surprised by it.
- **Collect Conclave jobs each pass.** When the anchor / brief shows `conclave:Np/Md`
  (pending/done async escalation jobs), run `bash .conclave/connector.sh poll <id>`
  for each; on DONE, validate + apply the synthesis (the main thread is the review
  layer -- the synthesis is a lead, not a verdict), then record the outcome:
  `bash .conclave/connector.sh outcome <id> <resolved|unresolved>` -- this is what makes
  Conclave learn automatically. A conclave-deferred section stays `[/]` until collected.

## Procedure

### 0. Start / resume

- `run_phase_guard.py status`. If `active` is false, `run_phase_guard.py start <today>`.
  If active, you are resuming after a watchdog relaunch -- continue from the
  recorded `phase` + `file`.

### 1. PREFLIGHT (`phase PREFLIGHT`)

- `bash scripts/todo-graph/build-and-validate.sh --keep-cache` (refresh the oracle).
- `bash scripts/build.sh` then `tail -1 build/build.log`. If HEAD does NOT show
  `=== BUILD OK ===`, the tree is broken before any work: try to fix the root
  cause (it is usually a half-committed change); if it cannot be made green,
  `run_phase_guard.py fixpoint` with a Run Log note "HEAD broken at preflight"
  and stop (relaunching cannot help a broken HEAD).
- `bash scripts/test.sh QUIET=1` -- a green baseline. A FAIL here is the same
  broken-HEAD case.

### 2. TRIAGE (`phase TRIAGE`)

- `python3 .claude/hooks/sequencer_triage.py --next`.
  - `{"status":"DONE"}` -> the oracle sees no NEEDS-WORK / DONE-UNSTAMPED files.
    Attempt `run_phase_guard.py fixpoint`. It is **ORACLE-VERIFIED**: it rebuilds
    the todo-graph and REFUSES (exit 1, run stays active) unless `--next` truly
    returns DONE -- so you cannot finish early. **On success only:** write the
    final Run Log line, disarm
    (`bash .claude/skills/overnight-sequencer/arm-sequencer.sh --disarm`), and
    final-answer with the summary + human punch-list. **If fixpoint is REFUSED**
    (or this pass made progress): `run_phase_guard.py next-pass` and TRIAGE again
    -- temporal blockers may now be unblocked. You may ONLY finish via a verified
    fixpoint; never disarm or final-answer otherwise (the Stop hook blocks it).
  - `{"status":"NEEDS_WORK","file":F}` -> `cursor <domain> F`; go to VALIDATE.
  - `{"status":"DONE_UNSTAMPED","file":F}` -> `cursor <domain> F`; skip
    VALIDATE+GAP_AUDIT, go straight to SECTIONS (review-only; the file is
    implemented, just unstamped old-system work).

### 3. VALIDATE (`phase VALIDATE`) -- NEEDS_WORK files only

- `Skill(validate-todo-file)` on the cursor file. Fix structural findings.

### 4. GAP_AUDIT (`phase GAP_AUDIT`) -- NEEDS_WORK files only

- `Skill(gap-audit-todo)` on the cursor file (runs the mandatory
  `codex-gap-audit` secondary pass). Land the resulting TODO edits before code.

### 5. SECTIONS (`phase SECTIONS`)

For each `## N.` section in Implementation-Order order, classify it with
`python3 .claude/hooks/sequencer_triage.py --classify <file>`:

- `DONE` -> skip.
- `DONE_UNSTAMPED` -> `Skill(review-todo-section)` (stamps it).
- `NEEDS_WORK` -> `Skill(implement-todo-section)` (its step 20 chains
  `review-todo-section`). For a near-complete section with one or two open
  items, `Skill(implement-todo-item)` is allowed.
- **Commit AND push after every section** (one atomic act). After a section
  ships+reviews+pushes, `run_phase_guard.py progress`.
- **Deferral uses the existing machinery.** If a section is genuinely blocked
  (missing prerequisite owned elsewhere, hardware-only validation, deliberate
  roadmap "no code today"), the implement/review skill marks it `[/]` + a
  `Deferred:`/`Accepted:` stamp with a concrete XREF. Do NOT invent a separate
  ledger; the triage oracle reads a stamped `[/]` as DONE-for-now next pass.
- **Hard failure** (build/test/smoke/Codex/commit-gate survives the skill's fix
  loop): roll back (commit nothing, clean tree), defer the section with the
  captured diagnostic, and advance -- a later fix may unblock it next pass. A
  failure identical across 3 passes escalates to the Run Log punch-list.

### 6. FILE_CLOSE (`phase FILE_CLOSE`)

- When every section is DONE/DONE_UNSTAMPED-then-reviewed/deferred:
  `Skill(complete-todo-file)` for the loose-end sweep + closure commit.

### 7. ADVANCE (`phase ADVANCE`)

- Append a one-line Run Log entry to the doctrine file (date, cursor, sections
  shipped, deferrals). Then `phase TRIAGE` and pick the next file (step 2).

## Hard rules (the guard enforces these; do not fight them)

- **Never call `AskUserQuestion`.** Unattended = decide with the conservative
  choice + log the assumption, or defer. The guard blocks it.
- **Never stop before FIXPOINT.** The work unit is the entire queue. The guard
  blocks Stop until you reach fixpoint. The watchdog only relaunches after real
  death (crash / usage limit); a voluntary mid-queue exit defeats the runner.
- **Keep the phase in sync.** If the guard blocks a skill as out-of-sequence,
  you are in the wrong phase -- set the correct phase, do not try to bypass.
- **No ChromeMCP, ever.** Browser gates are satisfied by `scripts/test-smoke.sh`
  + `scripts/test.sh` + `scripts/build.sh`. ChromeMCP is off at the systemd-unit
  level for this repo anyway.
- **Full quality pipeline every section.** No "straightforward" exceptions.
- All other repo doctrine (CLAUDE.md, memory feedback) applies unchanged.
