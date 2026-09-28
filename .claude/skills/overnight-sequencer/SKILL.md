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
- **Never pipe a review dispatch through `head` or `tail`, and poll the broker's `logFile` VERBATIM.** the direct dispatch wrapper (`scripts/codex-dispatch`, the non-broker route) streams the review REPORT to stdout (it also tees a copy under `.claude/overnight/reviews/` and prints that path on stderr since 2026-08-29); three post-ship legs piped through `tail -2` on 2026-08-26 kept only their closing bullets and had to be re-run (~11 minutes). And a log path RECONSTRUCTED from a `jobId` (`...-35747.out`) instead of the returned `logFile` (`...-adversarial.out`) does not exist; the waiter now reports such a path as `MISSING` (exit 5), never as hung, so a wrong path is a wrong path and not a re-dispatch.
- **Do NOT narrate between polls.** One blocking Bash call absorbs the whole
  wait. A second poll call past the 10-min wall is fine; "holding for the
  verdict" turns are not.
- **Poll YOUR OWN artifact log, not the shared `last-artifact.json` slot.** `.claude/state/last-artifact.json` is ONE SLOT per worktree, not per session, so whenever an operator attends the run BOTH sessions write it and a poll can return the other one's verdict. Observed 2026-08-09 15:21: a poll loop read `label prov-tooling state running` -- a different session's job -- while its own was still going, and a `523/524` inside that overlapping window did not reproduce on two immediate standalone re-runs. `run-artifact.sh` already RETURNS the per-run path (`.claude/overnight/artifacts/<stamp>-<label>.log`); read that. The `state: running/complete` field added 2026-08-08 makes an in-flight record detectable but does NOT make the slot per-session -- it tells you the record is unfinished, never whose it is. The `flock` in `test-tooling.sh` correctly serialises the suites themselves; the state file is the part with no such protection.
- **A shell subshell does NOT outlive the tool call -- use the harness's own background flag.** `( long_thing ; echo "rc=$?" ) &` inside a Bash call looks detached and is not: when the call hits the 10-minute tool wall, the harness SIGTERMs the foreground process, and the subshell dies with it. CONFIRMED TWICE, most recently 2026-08-10, when a backgrounded ship `git push` was killed exactly this way -- its foreground `wait` was SIGTERMed and took the subshell down, leaving the commit unpushed and the run reading a success it never got. The ship push follows the same rule: it is issued with `run_in_background: true`, because a `( git push ... ) &` subshell was ALSO lost on a call that returned normally (2026-09-05), so "poll it from the same call" is not a safe mitigation either. The rule is about SURVIVAL, not syntax -- anything that must still be running after the call returns goes in the harness's background flag (`run_in_background: true`), which is a tracked process the harness re-invokes you about, not a shell job whose parent is about to be killed. `nohup`/`setsid`/`disown` are not the fix either: they survive the signal but nothing then reports the result, so the run polls an artifact that may never appear.
- **Multi-kind review rounds go through the broker.** One Bash call per kind in
  one parallel message (per-kind gate receipts attribute off the command line,
  so NEVER bundle several dispatches behind one opaque shell command). Poll all
  their logFiles in one long call `... wait-for-codex-verdict.sh --max 540 f1 f2
  f3` (+ tool `timeout: 600000`), then read ONE combined envelope SCOPED to this
  section: `python3 scripts/overnight/review-envelope.py . --todo
  <this-section's-todo-path> --section <n>`. **The `--todo` AND `--section`
  filters are REQUIRED** (the manifest accumulates across sections; a
  file-scoped-only read pulled an EARLIER section's legs from the SAME todo,
  observed 2026-08-14 -- their findings were about code the change never
  touched). An unattributed leg is excluded under `--section` by design:
  redispatching costs one leg, ingesting another section's verdict costs a
  wrong triage. The
  `.out` artifact + envelope IS the authoritative review body -- NEVER glob
  `~/.codex/sessions/**/*.jsonl` for it.
- **On a crashed leg, re-dispatch ONLY that leg -- NEVER the whole bundle.** The
  envelope's `needs_redispatch` names exactly the legs to re-run (missing OR
  crashed); reuse the already-clean legs' artifacts as-is. Envelope exit 0 =
  `all_clean`.
- **Broker exit 75 = the Codex BACKEND is down: defer, do not retry.** The broker
  refuses when the last 2 completed legs in 20 minutes failed at the backend
  (HTTP status, stream disconnect, model at capacity; `scripts/overnight/codex-outage-check.py`).
  Park the review with the outage as its blocker and do work that needs no Codex;
  the gate clears when a leg succeeds or the window passes. Measured 2026-09-03:
  five retries (10 calls, ~4m20s) before the run reached this conclusion by hand.
- **Every review prompt starts with a LITERAL `[review-kind: X]`** -- never
  `"$(cat file)"`, `"$VAR"` or a loop variable, which expand only at run time so
  the recorder attributes nothing (`review_kind_literal_required` refuses them;
  measured 2026-09-05: a five-leg approved wave re-dispatched).
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
  '<todo>#<section>' <K>` **at VERDICT time -- the moment K's verdict lands,
  BEFORE applying any fix it prompted**. `record` fingerprints the tree AT THE
  CALL, so recording after the fixes stores a fingerprint no reviewer has seen
  and the next `should-redispatch` answers CONVERGED over unreviewed edits --
  observed live 2026-08-11 (TODO-06 section 41 round 2: obeying that verdict
  would have skipped rounds 3-8, every one of which found a real defect).
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

**FIRST, before classifying: read the PREVIOUS section's CI verdict.** An `identity-gate` job that failed with rc 3 `INFRASTRUCTURE` is RE-RUNNABLE before it is diagnosable: its resolved base comes from `gh run list --status success`, which has intermittently served a five-week-old page, and the gate cannot self-clear because `IDENTITY_GATE_LAST_GATED_SHA` only advances on a PASS. One `gh run rerun --failed <run-id>` is the cheaper first probe and cleared it both times it was seen (v16 carry, doc half closed 2026-08-29).

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
- **REBASE BEFORE EVERY PUSH -- `main` can now move underneath you.** Attended control-plane repairs land on `main` from the operator's repair worktree while you are mid-section, so a ship push can be rejected as non-fast-forward through no fault of your own (observed 2026-07-31: `! [remote rejected] main -> main (cannot lock ref ...)`). The ship sequence is therefore `git pull --rebase origin main` and THEN `git push origin main`. **Run that sequence EXACTLY as shown below -- the SHAPE is load-bearing, not just the order.** Two things bite otherwise, both observed live 2026-08-08 on the TODO-06 section-22 ship. (1) A pipeline's exit status is its LAST command, so `git pull --rebase ... | tail -3 && git push ...` gates the push on `tail`, which always succeeds -- a FAILED rebase reaches the push anyway and strands the run mid-rebase reading a non-fast-forward error that does not name the cause. Never pipe the rebase; `pipefail` is off by default and you cannot rely on it. (2) `.githooks/pre-push` runs `scripts/test-tooling.sh` (its own comment prices it at ~6 minutes) whenever the push touches `scripts/lint/`, `scripts/todo-graph/`, `scripts/test-tooling.sh` or `.claude/hooks/`, on top of the CI-parity compile -- which EXCEEDS the 10-minute tool wall, kills the call at 143, leaves `git push` running as an orphan and the commit unpushed. So background the push and poll it, exactly as you already do for the J1 receipt chain. Do NOT reach for `SKIP_TOOLING_SUITE=1` to dodge this: the suite has not run anywhere else, so skipping it would be a false receipt.

```bash
git pull --rebase origin main || { echo "REBASE FAILED -- do not push"; exit 1; }
# Issue THIS line as its own Bash call with run_in_background: true (the harness
# flag, not a shell `&`): a `( ... ) &` subshell was observed to die even on a call
# that returned NORMALLY (2026-09-05, section-23 ship: no rc= line, commit unpushed).
git push origin main > /tmp/ship-push.log 2>&1; echo "rc=$?" >> /tmp/ship-push.log
# A SECTION SHIP: invoke Skill(review-todo-section) BEFORE polling. The post-ship
# gate refuses non-review Bash until the review is running (a poll issued first
# was refused 12+ times across 21 logs); once it runs, the poll below passes
# and belongs inside the review, before any stamp commit.
# Poll in a SEPARATE, BOUNDED call. 540 < the 600s wall, so this RETURNS
# instead of being killed: rc 0 = the push finished (read the rc= line),
# rc 124 = still running -> re-issue this exact call. Never an unbounded
# `until` loop -- that is what gets killed at the wall.
timeout 540 bash -c 'until grep -q "^rc=" /tmp/ship-push.log; do sleep 5; done'
grep -E "^rc=|main -> main" /tmp/ship-push.log | tail -3
# The push LANDED only if nothing is left ahead of the remote. This check needs
# no surviving subshell, so it is the one step a gate-refused poll cannot fake.
git fetch -q origin main && [ -z "$(git log --oneline @{u}..HEAD)" ] && echo "PUSH LANDED" || echo "PUSH NOT LANDED -- do not advance"
```
- **NO edits to any tooling-pack input between `git push` and its `rc=` line.** The backgrounded push runs `.githooks/pre-push`, which runs `scripts/test-tooling.sh`, which executes `scripts/todo-graph/tests/test_build.sh`, `scripts/tests/*.py` and the hooks under test; editing any of them while that runs desyncs bash's incremental read and produces a false `FAIL` (observed 2026-08-28: a comments-only edit to `test_build.sh` during the section-59 ship push cost one ~800s pre-push run plus a second push). `ps` is NOT a safe probe -- the pack may not have reached that suite yet. The push's `rc=` line is the write lock, exactly as an artifact envelope's `state: complete` is for `run-artifact.sh`.
- **A pack-only pre-push refusal is answered with the RECEIPT route, never `SKIP_TOOLING_SUITE=1`.** Since 2026-08-29 the refusal prints the nested suite's own `[FAIL]` lines and a `full nested output:` path; read that first. If the named assertion is green standalone, run the pack out of band (`bash scripts/overnight/run-artifact.sh pack-oob -- bash scripts/test-tooling.sh --quiet`), and when it is green write the receipt (`python3 scripts/tooling-receipt.py write --project .`) and push: the gate then reports `receipt: a green tooling suite already ran on these exact bytes` and skips honestly, because the suite genuinely passed on the pushed bytes.
- **A backgrounded push is not landed until `git log @{u}..HEAD` is empty.** The subshell survives only as long as the polling call does, and a PreToolUse gate can refuse the poll before it runs -- silently, with the run reading a started job and a clean tree. The fetch-and-compare line above is the ship sequence's own last step for that reason.
- **BOUND EVERY POLL BELOW THE WALL -- the ship push had no prescribed shape and that is a real gap, not a style note.** The Codex wait was given a bounded waiter for exactly this reason (`--max 540` + tool `timeout: 600000`, exit 3 = STILL RUNNING); the ship-push poll said only "poll `/tmp/ship-push.log` for the `rc=` line, as with J1" and left the shape to the reader. The natural reading is an unbounded `until` loop, which is KILLED at the 600s wall. Observed twice on 2026-08-11 in an attended session, on pushes that ran 963s and 755s: the push itself survived both times (it is backgrounded, so the kill only reaches the poller), but each kill costs a turn and reads like a failure. A killed poller is NOT a failed push -- re-issue the same bounded call and read the `rc=` line, which is the only verdict. The push is genuinely slow whenever the commit touches the tooling surface, because that invalidates the receipt and the pre-push suite runs in full; 900s+ is normal there, not a hang.
- **The harness exit code is NOT the verdict for anything you backgrounded.** `( bash scripts/overnight/run-artifact.sh ... ) &` returns to the harness the moment the SUBSHELL forks, so the task notification says `completed (exit code 0)` while the command runs for another six minutes -- observed 2026-08-08, where the same run's envelope said `FAIL 1/1293 tooling tests failed` and the exit code was the one the notification surfaced. The run nearly took that 0 as its green gate. Read the verdict from the envelope's own fields: `.claude/state/last-artifact.json` now carries `state` (`running` -> `complete`) alongside the real `exit`, so `state != "complete"` means the work has not finished and there is no verdict yet, whatever the tool result said. A green tool result on a backgrounded launch is evidence that the launcher forked, and nothing else.
- **Never run two `scripts/test-tooling.sh` instances in one worktree.** They lint and rebuild caches against the same live tree, so both verdicts are untrustworthy -- the repo's own gotcha records the spurious rc=1 that produces. The suite now holds a per-worktree `flock` and waits (then refuses after `TT_LOCK_TIMEOUT`, default 900s) rather than starting on a poisoned tree, so this is enforced rather than remembered; the reason it needed enforcing is that the overlap came from a backgrounded run that had already reported itself finished.

 Your tree is clean at ship time, so the rebase is safe and normally a no-op. If the rebase reports a CONFLICT, do NOT resolve it blind and do NOT force-push: the conflicting file is almost always a capture file or doctrine the operator just edited. Take their side for control-plane and doctrine files, keep yours for section work, re-run the affected verification, and if it is not obviously separable, leave the section unshipped and file the collision -- a forced push over an operator's repair is unrecoverable.
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

- **Repair this file's deferred-item SHAPE before the closure commit.** Enumerate
  with `python3 scripts/todo-reachability.py --json <cursor-file>` (it accepts a
  path, so this costs ONE file's output, not the corpus) and take the
  `open-in-deferred` records. Measured 2026-08-05: **1,595
  such items across 333 sections in 52 files** (~31 per affected file) -- a
  backlog carried since v06 precisely because no phase owned it. You are the
  cheapest place to fix it: you already have this file open and in context, and
  the judgment is per-item.

  An item sits in a section the oracle calls DONE **and** that carries a
  `> **Deferred:**` stamp, so a bare `- [ ]` there is seen by NOTHING -- the
  oracle skips DONE sections, orphan-check exempts Deferred ones by design, and
  `stranded_deferrals.py` tracks `[/]` ITEMS rather than items inside a Deferred
  SECTION. Decide each one on its own text:

  - **Genuinely parked work** -> `- [/]` naming its blocker/owner IN THE TEXT
    (the section's Deferred stamp is a section-level reason; it does not
    automatically describe this item). **SHAPE IS NOT ROUTING.**
    `stranded_deferrals.py` sweeps ONLY parked items naming a cross-TODO XREF
    owner (~a third of them do), so a park without one has no automatic way back.
    Give yours an owner where one honestly exists:
    - **an owning TODO exists** -> name it as a real XREF (`NN-domain/TODO-NN`
      + section) with a reciprocal item on the owner side. The sweep can then
      re-open it when the owner ships. This is the DEFAULT -- look for an owner
      before concluding there is none.
    - **no owning TODO can exist** (bare metal, VirtualBox/WHPX, external
      hardware, an operator decision) -> say so explicitly in the item text with
      the word **`operator-gated`**, so the park is searchable and honestly
      states that only a human will ever clear it.

    **This is a QUALITY BAR, not a checkable rule -- do not expect a gate, and do
    not invent one.** A detector for "parked without an owner" was built on
    2026-08-02 and REMOVED: `[/]` means IN PROGRESS in this repo, not "parked
    awaiting an owner", so it misread 329 ordinary progress markers as defects.
    Scoping it to terminal parks does NOT rescue it -- re-tested 2026-08-05
    against the removal's own counter-example (`- [/] GetEnvironmentVariableA(...)`
    in a DONE section), which it still flags. The two populations are not
    mechanically separable, so the judgment is yours at the moment you park the
    item, and that is the only place it can live.
  - **A standing or recurring task** -> keep it `- [ ]` and PREFIX it with
    `standing:`. Real example: *"Review this section's triggers once per calendar
    year"*. That is not blocked work, and flipping it to `- [/]` with an invented
    blocker manufactures a false park.
    **The marker is load-bearing, not decoration.** Without it the doctrine and
    the completion gate contradict each other: this skill says leave the item
    alone, and `todo-reachability.py` then refuses `phase FIXPOINT` on it
    forever, so a CORRECTLY-shaped corpus could never complete. The run found
    this itself on 2026-08-05, and it is masked only while other items also
    block the gate -- **the drain succeeding is what exposes it**. `standing:`
    is AUTHORED at park time, which is what makes it legitimate where the
    removed `parked-ownerless` detector was not: a human states the intent
    rather than a rule inferring it, and an unmarked item still flags.
  - **Already satisfied** by work that shipped since -> `- [x]` with the
    evidence, same bar as any other completion claim.
  - **Cannot decide from the file alone** -> leave it and say so in the closure
    commit. An honest remainder beats a wrong flip.

  **NEVER bulk-flip.** A blanket `- [ ]` -> `- [/]` across a file is the
  "mass rewrite moves the problem" failure this repo forbids: it converts
  standing tasks into fake parks and destroys the distinction the shape exists to
  carry. There is deliberately NO script for this.

  This is BEST-EFFORT and never blocks the close -- it is bookkeeping, and a
  file you cannot fully repair still closes. Check 24 reports the remaining count
  (items across sections), so progress is visible without a gate.

### 7. ADVANCE (`phase ADVANCE`)

- Append a one-line Run Log entry to `docs/overnight/run-log.md` (date, cursor,
  sections shipped, deferrals) -- NOT to the doctrine file (the log is archived
  there so the doctrine re-read stays small). Then `phase TRIAGE` and pick the
  next file (step 2).

- **Drain ONE unreachable file's deferred-item backlog per advance.** FILE_CLOSE
  (step 6) only reaches files that still close. Measured 2026-08-05: of the 1,595
  `open-in-deferred` items, **806 sit in NEEDS_WORK files (FILE_CLOSE reaches
  those), 702 sit in 28 files the oracle already calls DONE, and 87 sit in 3
  BLOCKED files** -- neither DONE nor BLOCKED files are revisited by the normal
  pipeline, so nothing would ever repair those 789. **BLOCKED files are included
  deliberately: this repair is BOOKKEEPING, so it does not need the file's
  blocker cleared** -- writing down what an item is waiting on is exactly the
  work that is possible while it waits.

  That remainder is not optional: `phase FIXPOINT` runs the reachability gate and
  **REFUSES completion while any of them exist** (`todo-reachability.py` exits 1
  on `open-in-deferred`). So the choice is not whether to do this work but WHEN.
  Left alone it arrives as one ~700-item batch at the finish line, with every
  file's context long gone -- which is exactly the pressure that produces a
  bulk-flip, the one repair explicitly forbidden. Draining one file per advance
  spreads it across the pass and keeps each batch small enough to judge properly.

  ```bash
  # DONE/BLOCKED files still carrying open-in-deferred items (pick ONE, smallest first)
  python3 scripts/todo-reachability.py --json | python3 -c '
  import json,re,subprocess,sys
  d=json.load(sys.stdin)
  for f,rs in d.items():
      n=sum(int(m.group(1)) for r in rs if r[0]=="open-in-deferred"
            and (m:=re.match(r"^(\d+) open",r[2])))
      if not n: continue
      o=json.loads(subprocess.run(["python3",".claude/hooks/sequencer_triage.py",
          "--classify",f],capture_output=True,text=True).stdout)
      if o.get("file_class") in ("DONE","BLOCKED"): print(n,f)' | sort -n | head -3
  ```

  Apply the SAME per-item judgment as step 6 (park with a named blocker / leave a
  standing task alone / complete with evidence / honest remainder) -- and the same
  **NEVER bulk-flip**. Commit it as a `todo:` bookkeeping commit, separate from any
  section ship. One file per advance; do not batch several, and never let it
  displace the actual section work.

## Creating a section: record WHERE it came from

**Every new section records its provenance** as the first line of its body:

```
> **Spawned-by:** root           <- nothing spawned it; it stands on its own
> **Spawned-by:** §N (split)     <- decomposition at authoring time, before code
> **Spawned-by:** §N (review)    <- created from section N's review findings
```

**The line is REQUIRED and `scripts/todo-staged-check.py` refuses a commit that adds a section without one.** A genuinely independent section (a gap audit, a new capability, an operator filing) declares `root` -- it contributes 0 depth exactly as omitting the line used to, so it changes no verdict. What it buys is that "nothing spawned this" becomes a claim someone made rather than the default you get by writing nothing.

**A `(review)` spawn must also state `> **User impact:** <what a user hits if this is NOT done>`**, and the same staged check refuses it otherwise. Required of review-spawns only: a `root` is a capability you set out to build and a `(split)` is justified work being partitioned; a section created FROM a review finding is the shape that runs away. The content is NOT judged and "nothing today" is a legitimate answer -- often the useful one, because it is the answer that talks you out of the section. Demanding a weighty impact would only teach you to invent one. Write it honestly, then choose: fix it where you are and name it `- [x]`, file `- [ ]` in the owning component's OPEN section, or keep the new section because the sentence you just wrote justifies it.

Omission was the whole hole: measured 2026-08-09, **20 markers across 2,410 sections**, all in one file, because that is where the run happened to be when the sensor shipped. An undeclared section is a root, so a cascade never accumulates depth, never reaches the limit, and never has to justify itself -- which is how one file went from 9 to 35 sections in a week with the sensor live and silent throughout. A brand-new TODO file is exempt: its sections are roots by construction and stamping ten identical lines on a scaffold is ceremony, not accountability.

**Why the two kinds are not the same thing.** A `(split)` is the same work
correctly partitioned and is healthy: TODO-06 sections 10/11 split because a
parser fix and a lint-reporting change carry OPPOSITE failure modes, and the
total work did not grow. A `(review)` spawn is how TODO-07 ran sections 20->26 --
seven consecutive links, each created from the previous section's review finding
another edge case in the same teardown path, on a DEV-TOOLING component whose
founding premise had already been disproven. Branching factor 1.0 for days, and
every counter read healthy the whole time: sections shipped, reviews passed, the
IO table filled. Nothing could see that section N existed only because of N-1.

**At a review-chain depth of 3 or more**, check before creating the section:

```bash
python3 scripts/overnight/section-manifest.py spawn-chain <todo-path>
```

At or past the limit, the new section must ALSO carry a structured continuation
waiver -- same contract as the SPLIT-RECOMMENDED override, and for the same
reason: it has to be an accountable prediction, not the word "needed".

```json
{"user_impact": "what a user hits if this is NOT done",
 "not_parkable": "why it cannot be a `- [/]` park in the parent",
 "severity_trend": "this round's finding severities vs the last",
 "surface": "the file or subsystem it touches"}
```

`user_impact` and `not_parkable` are the two that bite. A link that cannot name
what a user hits, or why parking would lose something, is refinement below the
depth the component warrants -- and that is the shape to stop.

**THIS DOES NOT WEAKEN COMPLETION-FIRST, and must never be used to.** The
finding is filed either way. Without a waiver it becomes a `- [/]` item in the
PARENT section naming its blocker -- still visible, still counted by Check 24,
still swept. The sensor chooses a DESTINATION; it never chooses whether to
record. If a TODO genuinely needs 30 more sections, it gets 30: section count is
not the metric, and a deep chain that CAN name its user impact simply writes the
waiver and continues.

## Self-improvement filing (standing rule -- record, then continue)

The run is expected to be self-observing: every runner defect, cost pattern, or improvement it cannot apply unattended is FILED in the same turn it is observed, then the run continues. A finding carried in-context to "report later" dies with the segment. Two capture surfaces, both OUTSIDE the traversal (nothing filed there is implemented by the run):

- **Runner/flow findings** (gate misfires, wedges, evasions you were tempted into, flow inefficiency, machinery correctness) -> the NEWEST `todo/overnight-runner-improvements/overnight-runner-improvements-vNN.md` **This surface also takes REASONING findings, and always did -- they were just never asked for.** "I should have thought about this differently" is a first-class item: a conclusion you later found unsupported, an approach committed to too early, an ambiguity you resolved silently that deserved a question, a shortcut you ALMOST took and what made it tempting. So is a workaround you used to route around a gate (the route is the finding), and anything you deliberately did NOT do (silence reads as "not encountered", which is how a known gap goes invisible). Two disciplines make any of these actionable: state what you OBSERVED separately from what you INFERRED and mark which you actually tested; and verify the PROBE before trusting a result -- include a control that must fire, and say that it did. The capture file's "Reasoning and autonomy findings" heading lists the full set with the incident behind each one.
- **Cost/token findings** (measured waste, context growth, offload misses; the >= 2% bar applies -- below it, record as excluded, not as work) -> the NEWEST `todo/token-saver/token-saver-vNN.md`.

What may NOT be edited unattended -- file it instead: `.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json` (the control plane), and the RECEIPT SURFACE -- `Makefile*`, `scripts/build.sh`, the ABI generator -- which `receipt_surface_guard.py` enforces with a BLOCK that names this rule. Ordinary work (src/, user/, tests, docs, TODO files) is fixed in place as normal; self-correction on ordinary code is the job, not a finding.

House style for a filed item: what was observed live (timestamps, file:line), the mechanism confirmed at source, measured cost or risk; a projection is not a finding; if the fix is obvious, describe it -- do not apply it. Lead <= 250 chars, sub-bullet bodies <= 1,000.

**Never file a bare `- [ ]` into a section that already carries its Verified + Quality-reviewed stamps** -- the triage oracle never reads a stamped section's body, so the item is invisible to every later pass and `scripts/lint.sh` Check 23 will ERROR your commit. A follow-up on a stamped section is filed as `- [/]` with its owner/blocker named in the text, into a still-open NEEDS_WORK section, or as a NEW section (next free number, body LAST) with a reciprocal XREF.

## Working-discipline lessons (promoted from the capture files, 2026-08-29)

Each of these was paid for at least once by a run. They are stated as rules because the capture files kept re-filing them.

- **A verification you cannot read is not a verification.** Three faults in one section shared this root: an edit to a script a background job was executing (bash reads incrementally, so the syntax error lands thousands of lines away in a file `bash -n` parses cleanly); a launch guard `pgrep -fa "test_build.sh" && abort` that matched its OWN command line and aborted every time; and a python edit combined into one `run_in_background` call with a suite launch, whose `AssertionError` the harness never displayed. Wait for the envelope's `state: complete` (it is a WRITE lock, not just a read signal); confirm a probe reports NOT-running when nothing runs before trusting it; background ONLY the long-running command, and keep anything whose output is load-bearing in its own foreground call.
- **A refusal is the emitted TOKEN LINE, never a word in the output.** A fixture required token A present and token B absent, and the fix under test rewrote A's diagnostic to EXPLAIN which token would have claimed an earlier divergence, naming B. The behaviour was right and the case failed. Match the token prefix (`INFRASTRUCTURE: <token>`) the gate publishes; a fixture that greps the bare name asserts something weaker than the contract it guards.
- **Review rounds that keep paying are not a spiral.** Five adversarial rounds on one check each returned a MEASURED `[medium]` that defeated the previous round's fix with ordinary shell syntax. The spiral warning is about rounds that find new ways to defeat a FIXTURE; when the fixture IS the shipped guarantee those are the same set. The distinguishing question is whether the finding is measured and whether it changes what the check can catch. And when three attempts at the same class of rule each fail, take the reviewer's structurally different alternative (an exact allowlist over a comment rule) rather than iterating the class.
- **A filed mechanism can be right while its blamed SUB-mechanism is wrong.** Three reports of one symptom blamed a per-kind write race; the fault was first-match truncation in a shared path, and the tell was that the stuck kind ROTATED, which no per-kind theory explains. When several reports of one symptom disagree about the component, the shared ancestor is the suspect.
- **Verify a consequence against the real PARSER, not the filed prose; measure a new check's PRECISION against the live corpus before shipping it; re-measure a number rather than copying it forward.** Real-mechanism-wrong-consequence, self-matching probes and precision-blind checks are the three shapes the capture files re-file most.
- **Root-fix reflex and premise checks.** Two consecutive rounds each fixing a defect the previous one introduced means stop and simplify. When rejecting a finding on its premise, state the premise as its own checkable sentence. Grep the CONCEPT (every field carrying a semantic), not only the symbol whose definition moved. Never suppress stderr on `git add` in the ship sequence; use `git show <rev>:<path>` for read-only history questions.

- **A control must be checked for SEMANTIC effect, not for being a textual difference** (v18, 2026-08-30). A codegen-equivalence probe reported two objects IDENTICAL because `llvm-objcopy -O binary --only-section=.text foo.o /dev/stdout` wrote nothing and the pipeline hashed zero bytes (`e3b0c442...` is the SHA-256 of the empty string); the control then FAILED for a second reason, because `> CAP` was mutated to `> CAP - 1` over `uint64_t`, which the compiler folds to the same code. A mutation the compiler folds away is not a mutation. Before trusting any probe, run a control whose effect you can predict AND confirm the probe saw it; if the control reads the same as the bug, the control is wrong in the same direction.
- **A REQUIRED POST16 code is a claim about OBSERVABILITY, not about emission** (v18, 2026-08-30). A code emitted before `serial_early_init()` reaches port 0x80 only (`serial_early_putchar` is a no-op while `s_serial_port == 0`), and the smoke test greps the SERIAL log, so classifying it REQUIRED burned a full 4-leg matrix proving it was never observable. Before classifying a code REQUIRED, read `post_code16`'s body for the channel it writes and grep `build/smoke-test.stripped.log` after ONE leg; `build/smoke-matrix/*.log` are the runner's summaries, not the serial capture, and a zero-hit grep against the wrong file reads exactly like a real absence.
- **A skill step's `Agent(...)` dispatch IS user-requested.** The harness prompt says not to call the Agent tool unless the user asked; arming the sequencer is the user asking for every skill the pipeline invokes, and `review-todo-section` step 7 makes the auditor dispatch the PRIMARY path on every `src/kernel/` and `src/boot/` section. Dispatch them. The inline fallback walk is for a dispatch that FAILED, not for one you decided not to make; measured on the TODO-10 section-32 review, the inline walk pulled ~14 KB of skill text into the main context on a review that also ran 5 Codex legs.
- **Deferring on a hard blocker preserves the work as a patch.** Write `.claude/state/deferred-<todo>-s<N>.patch` before reverting (the run's established convention: five such files from the v18 cycle), and build it with `git add -N <new files> && git diff HEAD -- <paths>` so UNTRACKED files are included; a bare `git diff` omits them, and a deferred section whose new test file is missing from its patch is re-implemented from scratch when the blocker clears.

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
