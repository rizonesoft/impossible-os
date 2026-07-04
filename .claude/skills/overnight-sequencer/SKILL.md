---
name: overnight-sequencer
description: The unattended OS-completion driver. Runs the fixpoint loop over every TODO file in `todo/` using the exact per-file pipeline (triage -> validate -> gap-audit -> per-section implement/review -> close -> advance), enforced by run_phase_guard.py so it cannot deviate, ask, or stop before fixpoint. Invoked by the headless overnight run (armed via arm-sequencer.sh); follows todo/TODO-Claude-Overnight-Runner.md as the source of truth.
---

# Overnight Sequencer

> **External-Reviewer Contract:** This skill invokes Codex indirectly through child workflows in the phase machine (`gap-audit-todo`, `implement-todo-section`, `implement-todo-item`, `review-todo-section`, and `complete-todo-file`). Every Codex finding goes through `superpowers:receiving-code-review` (verify at file:line or source section, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex).

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
- **Read discipline:** one full read per file per pass is orientation; every
  later read of the cursor TODO or a big source is a SLICE
  (`Read(offset, limit)` around the region in play -- it satisfies the Edit
  freshness gate just like a full read). The slice_read_reminder hook nags on
  whole-file re-reads of > 32 KB files; act on it.
- **TODO write discipline:** `- [ ]` checklist lines cap at 250 chars (OS
  Comparison rows at 200). COUNT before writing -- aim <= 230 so margin
  survives tweaks; a third of line-length-block retries were still over the
  cap because the trim was done by feel. Detail belongs in commit messages.
- **Oracle-cache ordering:** `bash scripts/todo-graph/build-and-validate.sh
  --keep-cache` is the LAST action before a commit that touches todo/ --
  any TODO edit AFTER a rebuild re-stales the cache and the pre-commit
  lint's Check 7 blocks (this exact rebuild-then-edit-again ordering bug
  recurred 3x in one run). Order: all TODO edits -> rebuild -> commit.
- **MCP may be ABSENT this session (headless).** The `mcp__todo-graph__*` /
  `mcp__lsp-bridge__*` tools lose the cold-start connection race ~2/3 of the
  time under `claude -p`; if they are not in your tool list, do NOT emulate
  them with manual `grep -n "^## N\."` / `sed -n`. Use the deterministic CLIs
  the servers wrap -- `python3 scripts/todo-graph/query.py <verb>`,
  `scripts/todo-graph/resolve_symbol.py` -- plus the Grep/Glob tools and
  slice reads (CLAUDE.md "MCP Usage").

## Wait discipline (background Codex verdicts, agent results, CI watches)

The single biggest measured token leak of the 2026-07-02 run (~1,100 wasted
turns): every background wait from section 25 onward was covered by a Monitor
AND a manual ~10s poll loop on top of it -- 531 `Holding ...` narrator turns +
610 `Check ... status` Bash polls against only 40 Monitor arms. The rules:

- **One wait mechanism per wait.** Once a Monitor (or a background Bash with a
  completion condition) is armed, HOLD until it fires: no Bash re-polls, no
  per-poll "holding for the verdict" narrator turns. The notification is the
  signal; emitting turns while waiting buys nothing and burns the budget.
- **Prefer ONE foreground Bash call that absorbs the whole wait** when you need
  the result before anything else can proceed:
  `for i in $(seq 1 90); do grep -q "Turn completed" <out> && break; sleep 10; done`
  held a 6-min verdict wait in a SINGLE turn (2026-07-02 log line 1838; a
  13-min wait at line 497). This is the early-run pattern that later sections
  regressed away from.
- **If a manual poll is genuinely unavoidable** (Monitor timed out, host-load
  slowdown made the ETA unknowable), poll at a 30-60s cadence, never ~10s, and
  do not narrate between polls.
- **Batch multi-kind review waits into ONE Monitor condition**
  (`grep -q A f1 && grep -q B f2 && grep -q C f3`-style, as done correctly at
  log line 4996) instead of serial per-kind poll clusters -- the section-28
  antipattern was 43 polls for one verdict, x3 kinds.
- While a wait is armed you may do UNRELATED forward work (prep the next
  section's reads, update the cursor) -- what you may not do is spend turns
  checking or narrating the wait itself.

## Procedure

### 0. Start / resume

- `run_phase_guard.py status`. If `active` is false, `run_phase_guard.py start <today>`.
  If active, you are resuming after a watchdog relaunch -- continue from the
  recorded `phase` + `file`.

### 1. PREFLIGHT (`phase PREFLIGHT`)

- `bash scripts/todo-graph/build-and-validate.sh --keep-cache` (refresh the oracle).
- `python3 scripts/overnight/preflight-stamp.py . check` -- exit 0 means the
  build+test baseline below is ALREADY green for this exact tree (HEAD + dirty
  diff unchanged since the recorded stamp, < 12h old): note the cached summary
  in the report and SKIP the `build.sh` + `test.sh` gates, jump to the CI
  check. Exit 1 (any tree change or expiry) -> run the gates as written.
- `bash scripts/build.sh` then `tail -1 build/build.log`. If HEAD does NOT show
  `=== BUILD OK ===`, the tree is broken before any work: try to fix the root
  cause (it is usually a half-committed change); if it cannot be made green,
  `run_phase_guard.py fixpoint` with a Run Log note "HEAD broken at preflight"
  and stop (relaunching cannot help a broken HEAD).
- `bash scripts/test.sh QUIET=1` -- a green baseline. A FAIL here is the same
  broken-HEAD case.
- After BOTH gates are green:
  `python3 scripts/overnight/preflight-stamp.py . record --summary "<BUILD OK + test PASS line>"`
  -- an unchanged tree never pays the build+test baseline twice (any commit,
  tracked edit, or new untracked path invalidates the stamp automatically).
- **CI check (self-heal red CI instead of pushing past it).** After the local
  baseline is green, dispatch `Agent(subagent_type="gh-query-runner", ...)` to
  report the latest `build.yml` + `todo-graph.yml` conclusions and head SHAs
  (`gh run list --workflow=<wf> --limit 3`). Triage the report:
  - **failure AND the run's head SHA is an ancestor of local HEAD**
    (`git merge-base --is-ancestor <sha> HEAD`) -> our pushed work broke CI.
    Treat it like the broken-HEAD case ABOVE new section work: dispatch
    gh-query-runner again for the failing step's output (`--log-failed`
    slices), diagnose the root cause (superpowers:systematic-debugging; the
    2026-07-03 incident class was non-hermetic fixtures -- reproduce with
    `git clone --no-local . /tmp/cirepo && TEST_TOOLING_SKIP_LSP_MCP=1 bash
    scripts/test-tooling.sh` when CI fails but local passes), fix, commit,
    push, then dispatch gh-query-runner to WATCH the new run to conclusion
    (`gh run watch --exit-status`; one watch dispatch, no manual poll loops)
    before starting section work. If it cannot be made green this session,
    handle as the broken-HEAD case (Run Log note "CI broken at preflight",
    fixpoint, stop).
  - **failure but the head SHA is NOT ours** (not an ancestor, or the failing
    step is provider infra) -> Run Log NOTE and continue; do not chase other
    branches' failures.
  - **in_progress / queued** -> NOTE and continue (never wait on a running CI
    at preflight; the next relaunch re-checks).
  - **gh unauthenticated or network error** -> NOTE "CI check unavailable"
    and continue -- the CI check must NEVER block the run on tooling absence.
    (`gh` IS authenticated headless: rizonesoft token in `~/.config/gh`,
    scopes include `workflow`.)

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
