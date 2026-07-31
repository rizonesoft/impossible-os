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
  cursor <domain> <file>  record the cursor. The section index is optional and
                          [idx]         DERIVED when omitted (first section the
                          triage oracle still classes NEEDS_WORK), so metrics
                          attribute turns to a real section instead of the
                          constant 0 every record carried before 2026-07-28.
                          Pass it explicitly only to override that.
  progress                mark that this pass shipped/cleared something
  next-pass               start the next full sweep (resets progress)
  fixpoint                run complete -> stops the watchdog
  status                  print the cursor state
  relifecycle <reason>    one-shot Stage 1-2 override for a mature file that
                          grew a genuinely NEW section
  rollover                verified worker-context rotation after a fully
                          shipped section (machine-gated; the watchdog then
                          relaunches a fresh session -- see below)
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

**Poll a gating review IN-SESSION, in ONE blocking Bash call. Do NOT exit to
wait (2026-07-11).** A sleeping shell costs ~0 model tokens -- the model emits
one Bash call and is idle while `sleep` runs -- so holding the session open
during a review is free. The expensive antipattern is MANY separate poll calls
with narration between them (the 2026-07-02 run: 531 `Holding...` turns + 610
re-polls). The fix is one blocking call, no narration:

```
# 1. Dispatch the review (harness backgrounds it, or the broker for a bounded
#    multi-kind envelope). Reviews are READ-ONLY (adversarial-review sandbox).
bash scripts/overnight/review-broker-codex-dispatch.sh '[review-kind: design] <todo> <body>'
#    -> returns {logFile: ...}; works for design/adversarial/consistency/perf.
# 2. Wait with the CANONICAL waiter (B1) in ONE LONG call (R3, 2026-07-19).
#    The headless shape is MANDATORY (codex_wait_discipline.py BLOCKs short
#    polls): `--max 540` AND the Bash tool parameter `timeout: 600000` -- the
#    tool's ~120s default kill would otherwise end the wait early (B1). The
#    script exits the moment every verdict lands, so the long bound is FREE
#    when the review is already done. Measured cost of the short-poll loop
#    this replaces: 51 polls x ~350K cached tokens in run-20260719-022200
#    (~27% of that session's Bash calls). One model turn, idle while it sleeps:
bash scripts/overnight/wait-for-codex-verdict.sh --max 540 <logFile>   # + tool timeout: 600000
#    (watches <logFile> for the "Turn completed" sentinel) exit 0 = DONE
#     (prints the verdict tail); exit 3 = STILL RUNNING (reports bytes +
#     last-growth age); exit 4 = STALE (no growth for --stale-secs, ~hung --
#     re-dispatch THAT log only, keep waiting on the rest; B2).
# 3. On exit 3 (review outlived the 9-min bound), re-issue the SAME long call
#    (the review runs detached; nothing is lost). It is `--max <secs>`; a bare
#    trailing number is REJECTED as a stray arg. Then read the verdict and
#    continue -- SAME session. (Interactive sessions may still use short bare
#    calls; R3 gates only the headless run.)
```

- **The session NEVER exits to wait on a review.** The only session exits are a
  verified `rollover` (watchdog relaunches), external death, or an
  oracle-verified `fixpoint`. There is no `wait`/`wake` verb and no watcher -- that apparatus deadlocked the runner and was removed.
- **Do NOT narrate between polls.** One blocking Bash call absorbs the whole
  wait. A second poll call past the 10-min wall is fine; "holding for the
  verdict" turns are not.
- **Multi-kind review rounds go through the broker.** One Bash call per kind in
  one parallel message (per-kind gate receipts attribute off the command line,
  so NEVER bundle several dispatches behind one opaque shell command). Poll all
  their logFiles in one long call `... wait-for-codex-verdict.sh --max 540 f1 f2
  f3` (+ tool `timeout: 600000`), then read ONE combined envelope SCOPED to this
  section: `python3 scripts/overnight/review-envelope.py . --todo
  <this-section's-todo-path>`. **The `--todo` filter is REQUIRED** (the manifest
  accumulates across sections; an unscoped read pulls a stale prior review). The
  `.out` artifact + envelope IS the authoritative review body -- NEVER glob
  `~/.codex/sessions/**/*.jsonl` for it.
- **On a crashed leg, re-dispatch ONLY that leg -- NEVER the whole bundle.** The
  envelope's `needs_redispatch` names exactly the legs to re-run (missing OR
  crashed); reuse the already-clean legs' artifacts as-is. Envelope exit 0 =
  `all_clean`.
- **Receive ONCE per WAVE, not once per leg (T1-2b).** A wave is the set of legs
  dispatched together and waited on together. Wait for all of them, read the ONE
  combined envelope, then apply `superpowers:receiving-code-review` ONCE across
  every finding in it, and continue the pipeline in the same session. Each
  dispatch fires its own PostToolUse reminder -- that is the harness prompting
  per Bash call, NOT a per-leg obligation. `codex_review_completed.py`
  OVERWRITES `last-codex-review.json` on every trigger, so a parallel bundle
  collapses to one `received: false` record that one reception clears. Measured
  2026-07-27: 75 of 102 receptions landed within 30 min of the previous one,
  ~465 KB of repeated discipline body for no added rigor. Cadence changes;
  rigor does not -- every finding still gets Fix / Reject / Accept at file:line.
- **Convergence gate (P2.1/P2.2) -- the primary churn mechanism.** BEFORE
  re-dispatching kind K in a fix loop: `python3
  .claude/hooks/review_convergence.py should-redispatch '<todo>#<section>' <K>`
  (exit 1 = CONVERGED -> SKIP K; exit 0 = redispatch), then `... record
  '<todo>#<section>' <K>` once K's round resolves.
- **Round counter (P2.3).** After each re-dispatch: `python3
  .claude/hooks/review_round_guard.py --bump '<todo>#<section>' --progress
  <new|none>`. Exit 2 = CAPPED -> stop the loop, spin unresolved findings to a
  concrete follow-up `[ ]` + XREF, advance. For rounds >= 4 (REQUIRED,
  `--status`), verify at file:line through ONE `review-evidence-mapper`
  dispatch, not inline re-reads.

> Measured costs behind every rule above (the 531-turn poll burst, the 27%-of-
> Bash short-poll loop, the 16-crash re-dispatch sink, the task.c 31x re-read),
> the waiter exit-code table, and the E2/E3 incidents:
> [references/wait-discipline.md](references/wait-discipline.md).
**Non-gating background watches** (a CI run you monitor while doing unrelated
forward work) follow the same rule: ONE wait mechanism, no per-poll narration.
Convert any long stall into a single blocking Bash poll -- never a burst of
separate re-poll turns.

## Procedure

### 0. Start / resume

- `run_phase_guard.py status`. If `active` is false, `run_phase_guard.py start <today>`.
  If active, you are resuming after a watchdog relaunch -- continue from the
  recorded `phase` + `file`.
- **Orient DETERMINISTICALLY before spending a model turn on mechanics.** Run
  `python3 scripts/overnight/advance-work.py` once: it returns ONE bounded packet
  -- the oracle's action (work / fixpoint / blocked), the next open section, that
  section's `section-pack` (symbol defs, tests, registrations, ABI), the durable
  checkpoint from the prior session, and the cached preflight verdict -- so a
  fresh or rolled-over worker starts oriented instead of re-running status,
  triage, classify, and manifest turn by turn. You still drive the phase machine
  below (its transitions are the gate), but act on the packet's answers rather
  than re-deriving them.
- **On resume** (active run), also run `python3 scripts/overnight/section-checkpoint.py show`.
  If `stale` is false, REUSE its settled facts (section-pack digest+path, verified
  receipts, outstanding-review flag) -- do NOT re-discover the same symbols and
  constants the prior session already established (measured waste: two resumes
  re-derived identical facts). **P4.2/P4.3: the checkpoint now also carries
  `phase`, `open_findings` (the current section's recorded Codex findings +
  verdicts), `decisions_indexed`, and a derived `next_action` -- ACT on
  `next_action` directly (e.g. "receive the pending review", "continue section N:
  build + commit + stamp") and treat `open_findings` as already-triaged rather
  than re-dispatching a review or re-reading the finding set.** If `stale`, the
  tree moved since the checkpoint -- re-orient via the packet above.

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

**FIRST, before classifying: read the PREVIOUS section's CI verdict.**

```
python3 scripts/overnight/ci-check.py .
```

Deterministic JSON, no model tokens, and it NEVER waits. Sections are pushed
individually but CI used to be read only at PREFLIGHT and at file close-out, so
a section that broke CI stayed invisible until its file closed or the next
night's preflight -- which is how a fork+exec panic sat in a red
`Build Impossible OS` job. Checking here moves detection from per-file to
per-section for one JSON query.

It does not block on purpose: `gh run watch` costs ~9 min per section
serialized, and by the time the next section starts the previous push has
already had a full section of wall-clock to finish. A run still `in_progress` is
NOT a verdict -- carry on and let the next boundary pick it up.

- `"ours_red": false` -> proceed.
- `"ours_red": true` -> a FAILED run's head SHA is an ancestor of local HEAD.
  **Fix it before shipping the next section.** Pushing on top of a red CI
  compounds the bisect surface for whoever diagnoses it. Dispatch
  `gh-query-runner` for the `--log-failed` slices, diagnose
  (`superpowers:systematic-debugging`), fix, push.
- CI unreachable is a NOTE in the JSON, never a blocker.

For each `## N.` section in Implementation-Order order, classify it with
`python3 .claude/hooks/sequencer_triage.py --classify <file>`:

**Per-section efficiency discipline (every expensive turn must carry new
information or judgment; none of this weakens a gate):**

- **Start oriented, not exploring:** `python3
  scripts/overnight/section-manifest.py <todo> <n>` gives the deterministic
  manifest (open items, likely files, tests, XREFs, gates, blob hashes,
  complexity verdict). A `SPLIT-RECOMMENDED` complexity verdict is handled
  BEFORE implementation: split the section (TODO edit, quality pipeline per
  resulting section), never implement past one worker context. Then build
  the shared evidence bundle ONCE -- `python3
  scripts/overnight/evidence-bundle.py --manifest <manifest.json>` -- and
  reference its `dir` in EVERY agent prompt for this section (parity,
  coverage, XREF, quality, context agents all consume the same
  structure.md + diff with lens-specific prompts; nobody re-reads the same
  source into a fresh sidechain). Read the manifest + the enriched map,
  then do your 2-3 verification slice reads (FULL source) and design.
- **Settled questions have IDs, not re-research:** before researching an
  ABI/ownership/security/architecture question, `python3
  scripts/overnight/decision-registry.py search <terms>` -- a hit is an
  approved decision; cite its ID and move on. Rebuild the index after
  landing decisions (`build`). Agents citing a dead ID re-check the source.
- **Agent uncertainty contracts are load-bearing:** every mapper response
  ends with confidence/unknowns/unverified_claims/inputs/escalate. Treat
  `confidence: low` or `escalate: yes` as "verify everything at file:line
  yourself"; security/ABI/SMP-sensitive surfaces always escalate to you
  regardless of the agent's confidence.
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
- **REBASE BEFORE EVERY PUSH -- `main` can now move underneath you.** Attended control-plane repairs land on `main` from the operator's repair worktree while you are mid-section, so a ship push can be rejected as non-fast-forward through no fault of your own (observed 2026-07-31: `! [remote rejected] main -> main (cannot lock ref ...)`). The ship sequence is therefore `git pull --rebase origin main` and THEN `git push origin main`. Your tree is clean at ship time, so the rebase is safe and normally a no-op. If the rebase reports a CONFLICT, do NOT resolve it blind and do NOT force-push: the conflicting file is almost always a capture file or doctrine the operator just edited. Take their side for control-plane and doctrine files, keep yours for section work, re-run the affected verification, and if it is not obviously separable, leave the section unshipped and file the collision -- a forced push over an operator's repair is unrecoverable.
- **Verified session rollover after every fully-shipped section.** ENFORCED
  (R1, 2026-07-19): when a ship-stamp commit landed after the last verified
  rotation, the guard BLOCKS the next section-starter skill
  (implement-todo-section / implement-todo-item / implement-ssdt-range) AND
  any `cursor` move to a different section -- a shipped section's only legal
  next step is `rollover`. Deferrals add no ship stamp and advance in-session
  as before; review-todo-section / complete-todo-file stay unblocked (they
  legitimately run post-ship). A fresh
  worker context is cheaper and sharper than a long-tail one; the durable
  cursor (sequencer-run.json) carries all run state. After `progress`, FIRST
  refresh the receipts in ONE shot, THEN request the rollover -- the section's
  own work has invalidated the smoke receipt taken earlier in the section, so
  requesting the rollover first is a GUARANTEED refusal ("smoke receipt not
  content-valid (built image changed since the receipted smoke)"). Measured
  2026-07-31: TODO-04 §37 and §38 each paid exactly that refusal, then ran the
  same chain anyway. The J1 sequence lived only inside the guard's refusal text
  and `receipts.py:270`, so it could only be learned by triggering the very
  refuse-fix-refuse cascade the message warns about:
  ```bash
  bash scripts/build.sh && bash scripts/test.sh QUIET=1 && bash scripts/test-smoke.sh   # SMOKE LAST, no rebuild after it
  python3 scripts/overnight/receipts.py record-rollover .
  ```
  Then run `python3 .claude/hooks/run_phase_guard.py rollover`:
  - **VERIFIED** (clean tree incl. untracked, nothing unpushed, todo-graph
    rebuild OK, content-bound build + test + smoke receipts valid, no
    unreceived review, no outstanding background jobs) -> final-answer with a
    one-line checkpoint summary and END the turn. The Stop hook permits exactly
    this stop; the `*:0/10` watchdog relaunches a fresh session on its next
    tick, which resumes from the recorded phase. This is a CONTEXT ROTATION,
    not a stop -- the run stays armed throughout. (Since reviews are now polled
    in-session, rollover is the main deliberate exit; the watchdog is the sole
    relaunch mechanism.)
  - **REFUSED** -> the listed failures are unfinished work: finish/clean them
    and continue in-session (never force a rollover past a red gate).
- **Mid-section context-cap rotation -- ACTIVE (P4.6/P4.8; retired B1 2026-07-14, RE-ENABLED 2026-07-30 on corrected evidence).** A single section can outrun the doctrine context band before it ships (long fix loops, multi-round reviews), and the per-section rollover does NOT bound that. The retirement rested on two premises; one was disproved by measurement and the other was an artifact of the retired doctrine itself.
  - **FALSIFIED -- "140 tool-events ~= one section, so the per-section rollover already bounds context."** Measured 2026-07-30 across three consecutive canary segments: **334, 420 and 768 tool-events** (2.4x, 3x, 5.5x the threshold). The 334 segment shipped exactly ONE section (TODO-04 §34), ran 335 turns to a 492K end-of-segment context, and cost **$179.27** with cache-read at 85.3% of spend. `~132 tool-events` was a property of Canary #2's section sizes, not of the runner.
  - **RE-OPENED then RESOLVED FOR REAL 2026-07-31 -- "you commit AND push atomically at ship, so there is no committed-but-unpushed WIP window."** This was marked RESOLVED on the claim that "the restored firing procedure below does exactly that". IT DID NOT: the procedure described what to do WHEN a WIP-clean boundary arrives and never created one, so the objection remained true and the feature stayed inert. MEASURED: across every segment of 2026-07-31 the hint fired on schedule (at exactly 90) and was delivered to the run (4 occurrences in its own transcript), yet there were **0 `rollover-wip` attempts** -- because every run commit is a `test:`/`review:`/`todo:` commit that is PUSHED, so the unpushed window never existed. The runner's real shape (edit -> dispatch review -> fix findings -> ship) makes its FIRST commit at ship time. The firing procedure below now CREATES the boundary as an explicit step.
  - **Splitting is NOT the answer on its own.** §34 had ALREADY been split by the predictor (`section-manifest.py:207`, `open_items >= 5`) and the remainder still ran 330 turns, because the length driver was the review loop (23 findings fixed), which is only knowable AFTER the review runs and which no item-count threshold can predict.
  - **Arm-readiness A7/A8/A9 are closed (2026-07-30)**, which is what made re-enabling safe: A7 scans the cumulative `@{u}..HEAD` range for a ship stamp and fail-CLOSES on a git error; A8 refuses to mint a resolution receipt for an absent/empty review state or one with no `review_run_id`; A9 binds the receipt to that run id, so a NEWER review round at the same HEAD invalidates it.
  - **When you see the `rotate_hint` reminder, CREATE the boundary -- do not wait for one to arrive.** Waiting is why this feature was inert for two canaries: the boundary never arrives on its own. Do NOT stop mid-edit; finish the unit of work in hand, then at the FIRST point where the tree builds and tests green, perform the WIP commit as a deliberate, scheduled step:
    - **Commit what you have LOCALLY and do NOT push** -- `git commit -m "wip: <section> <what>" -- <explicit paths>`. THIS STEP IS THE POINT: it is what creates the committed-but-unpushed state the verb requires, and nothing else in the runner's normal flow produces one. Unlike the full-section rollover the WIP gate does NOT require a push, and the fresh worker resumes the same on-host tree.
    - If everything is already pushed AND the section's next action is the ship itself, skip the rotation and let the full `rollover` handle it -- but "the ship is close" is NOT a reason to skip when the next action is another review round or fix loop, which is exactly the shape that runs long.
    - **no in-flight OR unresolved Codex review** -- not merely "received". If a review is open, its findings must be triaged, fixed, and the fix loop green FIRST. A `received` review whose findings are unresolved is NOT a safe boundary, and its content-binding must match HEAD (the verb enforces this; a stale binding cannot be repaired by waiting).
    - no background job running.
  - **Then, IN ORDER:** (1) `python3 .claude/hooks/run_phase_guard.py review-resolved` -- records a content-bound receipt attesting the review cycle is resolved + GREEN at this HEAD (it verifies a real received review exists, binds HEAD, carries a `review_run_id`, and that build + test receipts are content-valid; REFUSES otherwise). If it refuses you are not at a resolved+green boundary: fix/receive/re-verify (green `build.sh` + `test.sh` with recorded receipts) and retry, or continue in-session. (2) `python3 .claude/hooks/run_phase_guard.py rollover-wip`.
  - **VERIFIED** -> the verb has already written the enriched section-checkpoint (fail-closed: no checkpoint, no authorization) and armed the cursor. Final-answer with a one-line "mid-section rotation at section N" summary and END the turn. The watchdog relaunches a fresh worker that resumes this SAME section from the checkpoint's `next_action` (step 0's resume path) -- NOT a new section. A context rotation, not a stop; the run stays armed.
  - **REFUSED** -> READ the reason; it is NOT always "wait and retry". Only a transient boundary (uncommitted WIP mid-edit, a still-running background job) is repaired by reaching the next clean boundary. A structural refusal -- everything already pushed (post-ship: use the full `rollover`), a stale/missing review binding, a newer review round at this HEAD (A9), `rollover_refused` set, or phase != SECTIONS -- will NOT change by retrying: continue in-session and let the next natural exit (ship -> full `rollover`) handle rotation. Never force it, never abandon in-flight work to rotate, and never use it to escape a refused ship rollover -- `rollover-wip` refuses outside `phase SECTIONS` and never clears `rollover_refused`.
  - **Orthogonal to the full-section rollover above**: that one fires AFTER a section ships (clean tree, PUSHED, receipts, no WIP); this one fires DURING a long section at a committed-but-unpushed boundary, and only when hinted.
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

## Self-improvement filing (standing rule -- record, then continue)

The run is expected to be self-observing: every runner defect, cost pattern, or improvement it cannot apply unattended is FILED in the same turn it is observed, then the run continues. A finding carried in-context to "report later" dies with the segment. Two capture surfaces, both OUTSIDE the traversal (nothing filed there is implemented by the run):

- **Runner/flow findings** (gate misfires, wedges, evasions you were tempted into, flow inefficiency, machinery correctness) -> the NEWEST `todo/overnight-runner-improvements/overnight-runner-improvements-vNN.md`.
- **Cost/token findings** (measured waste, context growth, offload misses; the >= 2% bar applies -- below it, record as excluded, not as work) -> the NEWEST `todo/token-saver/token-saver-vNN.md`.

What may NOT be edited unattended -- file it instead: `.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json` (the control plane), and the RECEIPT SURFACE -- `Makefile*`, `scripts/build.sh`, the ABI generator -- which `receipt_surface_guard.py` enforces with a BLOCK that names this rule. Ordinary work (src/, user/, tests, docs, TODO files) is fixed in place as normal; self-correction on ordinary code is the job, not a finding.

House style for a filed item: what was observed live (timestamps, file:line), the mechanism confirmed at source, measured cost or risk; a projection is not a finding; if the fix is obvious, describe it -- do not apply it. Lead <= 250 chars, sub-bullet bodies <= 1,000.

**Never file a bare `- [ ]` into a section that already carries its Verified + Quality-reviewed stamps** -- the triage oracle never reads a stamped section's body, so the item is invisible to every later pass and `scripts/lint.sh` Check 23 will ERROR your commit. A follow-up on a stamped section is filed as `- [/]` with its owner/blocker named in the text, into a still-open NEEDS_WORK section, or as a NEW section (next free number, body LAST) with a reciprocal XREF.

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
