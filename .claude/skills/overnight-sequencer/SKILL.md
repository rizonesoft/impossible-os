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
  status                  print the cursor state (incl. woke_from_wait)
  relifecycle <reason>    one-shot Stage 1-2 override for a mature file that
                          grew a genuinely NEW section
  wait <timeout_s> <reason> <path> <pattern> [...]
                          declare a structural wait on background artifact(s);
                          the Stop hook then permits ending the session and a
                          non-model watcher wakes a fresh one when they finish
  rollover                verified worker-context rotation after a fully
                          shipped section (machine-gated; see below)
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

**Structural waiting is the default (2026-07-11).** When a background wait has
nothing you can usefully do in parallel, do not hold the session open at all --
declare the wait and END the session; a non-model watcher wakes a fresh one
exactly once when the artifacts complete. Zero model turns are spent waiting,
and no review is skipped (the woken session must still receive the verdict):

```
python3 .claude/hooks/run_phase_guard.py wait 3600 "codex section review" \
  /path/to/task1.output "Turn completed" \
  /path/to/task2.output "Turn completed"
# then final-answer with a one-line status; the Stop hook permits THIS stop.
```

- Batch every outstanding verdict of the wait into ONE declaration (all
  path/pattern pairs must match before the wake).
- **Multi-kind review rounds go through the broker.** Dispatch each kind as
  its own `bash scripts/overnight/review-broker-codex-dispatch.sh
  '[review-kind: X] <todo> <body>'` call (one Bash call per kind, in one
  parallel message -- the per-kind gate receipts attribute off the command
  line, so NEVER bundle several dispatches behind one opaque shell command).
  Then declare ONE wait over the returned logFile paths. On wake, read ONE
  combined envelope: `python3 scripts/overnight/review-envelope.py .` --
  per-kind status, every severity-marked finding, artifact path + sha256.
  Slice-read an artifact only for findings needing full context; never pull
  whole review transcripts into the session.
- On resume, `run_phase_guard.py status` shows `woke_from_wait` (also injected
  into the session brief): FIRST read the artifact(s) and receive the review
  (`superpowers:receiving-code-review`), then continue the pipeline.
- A wait that EXPIRES (timeout, default 3600s) wakes you anyway -- handle the
  timeout (re-dispatch or defer with the captured diagnostic).
- The Stop hook REFUSES the stop when the artifacts are already complete --
  that means read the verdict now, not wait.

**In-session waits are the exception**, justified only when you have genuine
parallel forward work (prep the next section's reads, unrelated TODO edits):

- **One wait mechanism per wait.** Once a Monitor (or a background Bash with a
  completion condition) is armed, HOLD until it fires: no Bash re-polls, no
  per-poll "holding for the verdict" narrator turns (the 2026-07-02 run burned
  ~1,100 turns on 531 `Holding ...` narrations + 610 manual re-polls).
- **Prefer ONE foreground Bash call that absorbs a short wait** when the
  result gates everything anyway and the wait is minutes, not tens of minutes:
  `for i in $(seq 1 90); do grep -q "Turn completed" <out> && break; sleep 10; done`
- If Monitor keeps handing control back before its condition is met, that is
  still "checking the wait" -- do not narrate each resume; for a long stall,
  convert to a structural wait (`wait` verb) and end the session.

## Procedure

### 0. Start / resume

- `run_phase_guard.py status`. If `active` is false, `run_phase_guard.py start <today>`.
  If active, you are resuming after a watchdog relaunch -- continue from the
  recorded `phase` + `file`.

### 1. PREFLIGHT (`phase PREFLIGHT`)

**One deterministic call -- no model-shepherded mechanics, no subagent on the
success path:**

```
python3 scripts/overnight/preflight.py .
```

It runs the todo-graph rebuild, the tree-hash stamp check (an unchanged tree
never pays the build+test baseline twice), `build.sh` + `test.sh QUIET=1` when
needed, records the green stamp, and queries CI directly via `gh run list`
(a JSON query needs no gh-query-runner dispatch). Read the JSON verdict:

- `"ok": true` -> note the one-line baseline in the report and go to TRIAGE.
- `"ok": false` -> the failures array names each failing step + evidence
  artifact. Dispatch `diagnostic-digester` (Sonnet) on the named log to
  digest it; YOU diagnose the root cause and decide the fix
  (superpowers:systematic-debugging). A broken HEAD that cannot be made green
  is the fixpoint-and-stop case (Run Log note "HEAD broken at preflight").
  For `ci` failures (ours_red): dispatch gh-query-runner for `--log-failed`
  slices, fix, push, then ONE `gh run watch --exit-status` dispatch -- no
  manual poll loops. CI unavailability is a NOTE in the JSON, never a
  blocker.

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
  - `{"status":"NEEDS_WORK","file":F}` -> `cursor <domain> F`, then **route by
    the `stages_1_2_done` field IN THE SAME JSON -- never re-run mature
    lifecycle stages** (the guard hard-blocks Stage 1-2 skills on a file that
    carries both preamble stamps):
    - `"stages_1_2_done": true` -> `phase SECTIONS` directly. Do NOT re-run
      validate/gap-audit (each costs a Codex pass) on a mature file.
    - `false` with `lifecycle.validated: true` only -> `phase GAP_AUDIT`
      (skip the already-stamped VALIDATE).
    - `false` with neither stamp -> `phase VALIDATE` and run both stages.
    - Exception: a genuinely NEW `## N.` section since the stamps ->
      `run_phase_guard.py relifecycle "<which section>"` then run Stages 1-2.
  - `{"status":"DONE_UNSTAMPED","file":F}` -> `cursor <domain> F`; skip
    VALIDATE+GAP_AUDIT, go straight to SECTIONS (review-only; the file is
    implemented, just unstamped old-system work).

### 3. VALIDATE (`phase VALIDATE`) -- NEEDS_WORK files WITHOUT a `Validated:` stamp only

- `Skill(validate-todo-file)` on the cursor file. Fix structural findings.

### 4. GAP_AUDIT (`phase GAP_AUDIT`) -- NEEDS_WORK files WITHOUT a `Gap-audited:` stamp only

- `Skill(gap-audit-todo)` on the cursor file (runs the mandatory
  `codex-gap-audit` secondary pass). Land the resulting TODO edits before code.

### 5. SECTIONS (`phase SECTIONS`)

For each `## N.` section in Implementation-Order order, classify it with
`python3 .claude/hooks/sequencer_triage.py --classify <file>`:

**Per-section efficiency discipline (every expensive turn must carry new
information or judgment; none of this weakens a gate):**

- **Start oriented, not exploring:** `python3
  scripts/overnight/section-manifest.py <todo> <n>` gives the deterministic
  manifest (open items, likely files, tests, XREFs, gates, blob hashes).
  Feed it as the seed of the enrichment dispatch (`enrich_with` names the
  agent); read the manifest + the enriched map, then do your 2-3
  verification slice reads and design.
- **Batch coherent edits.** Gather ALL evidence first, then apply ONE
  carefully scoped patch per file (or per concern), then verify -- never
  alternate read/edit/read/edit across the same files (the 2026-07-10 run
  spent 500+ Edit calls that way). MultiEdit or a single large Edit per
  file over five small ones.
- **Review the impact cone, not the repo:** `python3
  scripts/overnight/impact-cone.py` (staged) derives changed symbols,
  callers, header includers, tests, and registration touches -- scope Codex
  prompts and your own reads to that set. `global_state_escalation: true`
  widens the scope; the final whole-diff review pipeline is unchanged.
- **Targeted verification in fix loops; full gates at the boundary.** After
  a local fix, run the OWNING suite (`bash scripts/test.sh SUITE=<cat>`),
  not the world; `receipts.py check-suite . <cat>` skips a suite that is
  already green over unchanged inputs (record with `record-suite` after a
  green run). The section boundary still runs the unchanged full
  build + test + (boot-path) smoke gates.
- **Never rediscover a failure:** on any build/test/smoke/Codex failure,
  `python3 scripts/overnight/failure-ledger.py check <kind> < <log>` FIRST
  -- a hit returns the stored diagnosis (reuse it; 3+ recurrences =
  defer-and-escalate). After diagnosing a new failure, `record` it with
  `--diagnosis`.
- **Never re-triage a settled finding:** before classifying a Codex
  finding, `python3 scripts/overnight/finding-ledger.py lookup <kind>
  <severity> <file:line> <title>` -- a hit is a prior Fix/Reject/Accept
  with evidence (IDs are blob-bound: changed content = new finding =
  re-triage). `record` every decision as you make it; compaction and
  rollover then cannot trigger reclassification.
- **Unchanged-input retry rule (soft):** do not repeat a build, review,
  agent dispatch, or failed edit unless its input hash changed (the
  receipts/caches tell you) or you record a one-line reason for the
  deliberate re-run.

- `DONE` -> skip.
- `DONE_UNSTAMPED` -> `Skill(review-todo-section)` (stamps it).
- `NEEDS_WORK` -> `Skill(implement-todo-section)` (its step 20 chains
  `review-todo-section`). For a near-complete section with one or two open
  items, `Skill(implement-todo-item)` is allowed.
- **Commit AND push after every section** (one atomic act). After a section
  ships+reviews+pushes, `run_phase_guard.py progress`.
- **Verified session rollover after every fully-shipped section.** A fresh
  worker context is cheaper and sharper than a long-tail one; the durable
  cursor (sequencer-run.json) carries all run state. After `progress`, run
  `python3 .claude/hooks/run_phase_guard.py rollover`:
  - **VERIFIED** (clean tree, nothing unpushed, todo-graph rebuild OK,
    content-bound build receipt valid, no unreceived review, no declared
    wait) -> final-answer with a one-line checkpoint summary. The Stop hook
    permits exactly this stop; the watcher relaunches a fresh session that
    resumes from the recorded phase. This is a CONTEXT ROTATION, not a stop
    -- the run stays armed throughout (doctrine: "Rollover is not a stop").
  - **REFUSED** -> the listed failures are unfinished work: finish/clean them
    and continue in-session (never force a rollover past a red gate).
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

- Append a one-line Run Log entry to `docs/overnight/run-log.md` (date, cursor,
  sections shipped, deferrals) -- NOT to the doctrine file (the log is archived
  there so the doctrine re-read stays small). Then `phase TRIAGE` and pick the
  next file (step 2).

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
