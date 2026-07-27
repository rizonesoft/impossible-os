# Token Saver -- Cost Reduction Backlog (zero quality loss)

> **Deliberately outside the sequencer.** Named `token-saver.md` (not `TODO-NN-*.md`, not under `todo/NN-domain/`) so
> `sequencer_triage.is_impl_todo` never traverses it and the todo-graph `rglob("TODO-*.md")` never parses it. Same
> convention as `todo/overnight-runner-improvements.md`. Keep the name this shape.
>
> Companion to `todo/overnight-runner-improvements.md`. That file is a **flow / correctness** backlog whose cost items are
> framed around *wait time and dispatch shape*. This file is framed around the **measured token arithmetic** and reaches
> mostly different conclusions. Where an item here sharpens one there, the cross-reference is named inline.
>
> **Hard invariant for every item in this file: no quality is traded away.** The acceptance criterion on each item names
> what stays untouched. The non-negotiables are listed once, at the bottom, under "Never cut".

---

## The measurement (2026-07-27, 53 run segments, 2026-07-20 -> 2026-07-26)

Source: `.claude/overnight/metrics/*.jsonl` (53 segments), `.claude/state/tool-history.jsonl` (8,951 events),
`.claude/state/subagent-log.jsonl`, `.claude/state/offload-events.jsonl`, `.claude/state/codex-review-history.jsonl`.

| Bucket                      |            Tokens | Cost @ Opus list |     Share |
| --------------------------- | ----------------: | ---------------: | --------: |
| **Main-session cache READ** | **1,337,159,751** |    **$2,005.74** | **73.8%** |
| Sidechain cache READ        |       127,278,940 |          $190.92 |      7.0% |
| Main-session output         |         3,428,390 |          $257.13 |      9.5% |
| Main-session cache CREATION |         7,690,691 |          $144.20 |      5.3% |
| Sidechain cache creation    |         6,300,096 |          $118.13 |      4.3% |
| Sidechain output            |            14,584 |            $1.09 |      0.0% |
| Main-session fresh input    |             7,641 |            $0.11 |      0.0% |
| **Total**                   |                   |    **$2,717.32** |           |

(Sidechain priced at Opus rates as an upper bound. The fleet is Sonnet-by-doctrine, so the true sidechain figure is closer
to **$62** -- which only strengthens the conclusion below. Dollar figures are list-rate arithmetic for *relative sizing*,
not a bill.)

### The three conclusions that should drive everything in this file

1. **Cache-read is ~81% of spend. Output tokens are ~10%.** Every "write less" / "be more concise" instinct targets the
   9.5% slice. The lever that matters is **how big the context is on every turn**, multiplied by **how many turns**.
   Measured average context per turn: **327,124 tokens** across 4,087 turns. Cutting steady-state context from ~327K to
   ~160K halves the run cost and changes not one thing about what gets built.

2. **Subagents are already cheap and are being UNDER-used, not over-used.** The entire sidechain is 2-11% of spend and
   produced 1,016 greps + 758 reads for 14,584 output tokens. Meanwhile the main session made **4,400 Bash calls, 1,412
   Reads, and 1,343 Greps in its own context**. The user's instinct ("lean towards subagents") is correct and the data
   says it is nowhere near saturated. But the routing must **replace** main-session reads, never layer on top of them --
   that is already doctrine in `CLAUDE.md`, it is just not being obeyed (see T2-1).

3. **The biggest single defect is re-reading. 87% of all Reads are re-reads of a file already in context.** 1,412 Reads
   over **180 distinct files**; 1,232 redundant. `src/kernel/quota/quota.c` was read **131 times** in one working period.
   Each re-read re-appends the whole body AND every later turn pays cache-read on both copies. This is a compounding
   cost, not a linear one.

---

## T1 -- Context economics (the 81% slice)

- [x] **T1-1. Kill redundant re-reads with a hook-enforced content-hash read cache.**
  **SHIPPED 2026-07-27** as [`.claude/hooks/read_cache_block.py`](../.claude/hooks/read_cache_block.py) (PreToolUse
  `Read` BLOCK + PostToolUseFailure `Edit|MultiEdit` `invalidate` mode), wired in `.claude/settings.json`, documented in
  `.claude/hooks/MANIFEST.md` and `docs/infrastructure/hook-codes.md` (code `[READ-CACHED]`).
  **Evidence:** 1,412 Reads / 180 distinct files / **1,232 redundant (87%)**; top offenders `quota.c` x131, `quota.h`
  x100, `TODO-25-...md` x93, `task.c` x91, `quota_ledger.c` x81. `.claude/state/read-history.json` (7.8 KB) already
  tracks reads but only drives an advisory reminder (`read_offload_reminder.py`).
  **Built stronger than specified in three places.** (1) Key is a **sha256 of the content**, not `(mtime, offset,
  limit)` -- mtime moves on `git checkout` / `touch` with identical bytes, which would have released blocks that should
  hold and, worse, would have let a real content change slip through on a preserved mtime. (2) Coverage is **range
  containment**, not slice equality: a whole-file read covers a later slice inside it (the dominant measured shape), and
  a wider or disjoint slice is correctly allowed. (3) A whole-file read is credited only for the Read tool's real
  2000-line default, so `Read(offset=2500)` on a 3000-line file is never wrongly blocked.
  **Anti-wedge (a Read gate must never trap the model):** three independent valves -- the same request is blocked at
  most twice then released and re-armed; one-shot file `.claude/state/read-cache-override`; kill switch
  `READ_CACHE_DISABLE=1`. Every error path fails open.
  **Acceptance met at build time:** the post-Edit re-read is free *by construction*, not by exception -- a successful
  edit changes the hash, and a FAILED edit (the case a hash cannot see, where the model's memory is stale but the file
  is not) is cleared by the `invalidate` wiring. `pre_compact_flush.py` deletes the table so the block's "already in
  context" premise can never outlive a compaction; a `grep` assertion in `scripts/test-tooling.sh` stops those two files
  drifting apart. Subagents are exempt (separate context) and state is session-bound (rollover starts clean).
  **Checks run:** `read_cache_block --selftest` 30 cases green; `scripts/test-tooling.sh` **552/552**;
  `scripts/audit-hooks.sh` PASS (no drift); `scripts/lint.sh` 0 errors; live end-to-end against the real repo state
  confirmed allow -> BLOCK -> subagent-exempt -> PreCompact-clears -> allow.
  **Still to measure (T4-3 owns it):** the predicted **20-30%** saving. Re-read ratio in `tool-history.jsonl` must drop
  below 20% on the next full run with no increase in Codex-found defects.

- [/] **T1-2. Put the skill bodies on a diet (the injected volume is ours, not the plugin's).**
  **Evidence:** 203 `Skill` invocations. Injected body by owner -- `implement-todo-section` 48.4 KB x17 = 823 KB,
  `review-todo-section` 37.5 KB x18 = 674 KB, `overnight-sequencer` 27.7 KB x23 = 638 KB, `kernel-code-quality`
  14.7 KB x21 = 309 KB, rest ~100 KB. **Repo skills = ~2.54 MB (~79%)**; the two plugin skills we actually invoke
  (`receiving-code-review` 6.2 KB x103, `verification-before-completion` 3.6 KB x15) = ~694 KB (~21%). Every body lands
  early in a session and is then re-read in the cached prefix on every later turn.
  **Fix: split every SKILL.md over ~12 KB into a thin driver + `references/` files loaded on demand.** The driver keeps
  the step list, the gates, and the mandatory triggers; rationale, incident histories, and worked examples move to
  `references/*.md` that the step text names by path. This is the standard skill-authoring shape
  (`docs/infrastructure/skill-authoring.md`) and loses nothing -- the content is one Read away at the moment it is
  actually needed, and T1-1 now keeps that Read from being paid for twice.
  **PARTIALLY SHIPPED 2026-07-27 -- the 4 highest-cost skills done, 10 smaller ones outstanding. The ~60% ratio in the
  original item was wrong; see the correction below.**

| Skill                    | Before |  After |                                 Cut | Invocations | Injection saved |
| ------------------------ | -----: | -----: | ----------------------------------: | ----------: | --------------: |
| `implement-todo-section` | 48,417 | 38,303 |                                -21% |         x17 |          172 KB |
| `review-todo-section`    | 37,461 | 31,195 |                                -17% |         x18 |          113 KB |
| `overnight-sequencer`    | 27,735 | 25,920 |                               -6.5% |         x23 |           42 KB |
| `kernel-code-quality`    | 14,701 | 14,701 | 0% (assessed, correctly left whole) |         x21 |              -- |

  Reference files created: `implement-todo-section/references/{review-triage,test-wiring,todo-bookkeeping,implementation-rules}.md`,
  `review-todo-section/references/{stamp-fields,fix-loop}.md`, `overnight-sequencer/references/wait-discipline.md`.
  `stamp-fields.md` is now the canonical repo-wide stamp grammar; `quality-review-section` and
  `implement-todo-section` were repointed at it.

  **Correction to the estimate.** Achieved **~327 KB (~82K tokens)** of injection saved per measured window, not the
  ~380K tokens projected -- roughly a fifth. The projection assumed these files were padded; they are not. They are
  dense operative content, and three categories are immovable by the acceptance criterion: step actions, hook-enforced
  rules, and the anti-corner-cutting prose (which must be IN context exactly when the agent is deciding whether to skip
  a step -- moving it one Read away defeats its purpose). `kernel-code-quality` was assessed and left whole: all ten
  gates are operative checklist items, so any cut would have removed gate content. **Realistic ceiling for this item is
  ~15%, not 60%; revised tier estimate 1.5-3%, not 6-10%.**

  **Prerequisite fixed along the way:** `scripts/audit-ai-system.sh` checks 2/3/4 scanned only `SKILL.md`, so Codex
  prose moved into `references/` would have gone unaudited (it already missed the pre-existing
  `kernel-code-quality/references/incidents.md`). Widened to `.claude/skills/<slug>/**/*.md`, scoped to slug dirs so
  the loose shared fragments (`README.md`, `TEMPLATE.md`, `codex-prompt-shape.md`) stay excluded as
  `scripts/lint.sh` Check 12 already does. `scripts/lint.sh` Check 12 needed no change (it already walked every `.md`).

  **Checks run:** `scripts/lint.sh` 0 errors; `scripts/audit-ai-system.sh` 7/7; `scripts/audit-hooks.sh` no drift;
  `scripts/test-tooling.sh` 552/552; all 20 `implement-todo-section` steps and all 17 `review-todo-section` steps
  present post-split; every mandatory-gate token still in its driver; relative-link sweep clean (the 7 hits are
  pre-existing `<path>` placeholders in README/TEMPLATE).

  **Outstanding -- 10 skills over 12 KB not yet split**, all low-invocation in the measured window (0-1 each), so their
  share of the 2.54 MB is small: `gap-audit-todo` 35.2 KB, `implement-unit-tests` 32.7, `diagnose-serial-log` 30.9,
  `complete-todo-file` 26.5, `validate-todo-file` 24.6, `overnight-todo-runner` 16.5, `create-todo` 16.4,
  `boot-code-quality` 13.9, `implement-todo-item` 13.0, `codex-design-review` 12.0. They matter over a full
  repo-completion run where the sequencer invokes each many times; at the measured ~15% ratio the whole batch is worth
  roughly another 30 KB of driver. Do them opportunistically when a skill is being edited anyway, not as a batch.
  The shape to follow is documented in
  [docs/infrastructure/skill-authoring.md "Progressive disclosure"](../docs/infrastructure/skill-authoring.md).

  **Acceptance (met for the 3 split):** no skill's mandatory step list, gate, or hook trigger removed -- only
  relocated; catalog + hook audits pass; step sequences verified identical.

- [ ] **T1-2b. Collapse the in-pass `receiving-code-review` repeats at their SOURCE (batch the review legs).**
  **Measured 2026-07-27 -- this replaces the "idempotent Skill re-invocation" hook originally filed here.** Gap
  distribution between consecutive invocations of the same skill: `receiving-code-review` **75 repeats within 30 min**
  (plus 2 within 5 min) against 27 at >=30 min, while EVERY other high-volume skill is >=30 min apart --
  `kernel-code-quality` 20/20, `review-todo-section` 17/17, `implement-todo-section` 16/16, `overnight-sequencer` 21/22,
  `verification-before-completion` 14/14. A >=30 min gap is a genuinely NEW pass (new section, new pipeline run) where
  re-injection is correct and suppressing it would be wrong. So there is exactly ONE redundant shape in the whole
  catalog, worth ~75 x 6.2 KB = **465 KB (~116K tokens)**.
  **Why NOT the suppression hook that was filed here.** A PreToolUse hook can only allow or deny -- it cannot truncate a
  body -- so "inject a one-line pointer instead" means DENYING the Skill call. `codex_review_completed.py` runs
  PostToolUse on `Bash|Skill` and its `kind == "receive"` branch is what flips `received: true` in
  `last-codex-review.json`, which is what releases `receiving_review_required` (blocks EVERY Edit) and feeds
  `run_phase_guard`'s rollover checks. Deny the call and PostToolUse never fires, `received` stays false, and the gate
  never clears: a hard deadlock on the one skill the hook was aimed at. It could be "fixed" by having the cost hook
  write the receipt itself -- which puts a review-gate receipt flip inside a token optimizer, where a bug silently marks
  a review received that was never applied. That is the Never-cut line; 116K tokens does not buy it.
  **Fix instead:** remove the REASON for the repeats. The 75 in-pass receptions exist because each Codex leg's findings
  are received separately. Batch the legs (`review-envelope.py` already combines legs; the concurrent-dispatch item in
  `overnight-runner-improvements.md` wants the same shape) and the repeat receptions collapse at the source -- same
  saving, no gate coupling, no duplicated receipt, and it makes that backlog item cheaper too.
  **Acceptance:** every finding is still received through the discipline; `received: true` is still set only by the real
  PostToolUse path; no reduction in findings triaged per review round.
  **Rejected: vendoring the superpowers skills into the repo.** Injection cost is `size x invocations` and vendoring
  changes neither -- the same 6.2 KB body still lands on every call. It only pays if we then edit it smaller (~230 KB
  best case), against renaming `superpowers:receiving-code-review` across 9 `codex-*` skills plus the hook messages, and
  forking a plugin that is actively versioned (6.0.3 / 6.1.1 / 6.2.0 all cached locally) and pinned load-bearing by
  `CLAUDE.md`. 79% of the injected volume is our own skills, which T1-2 diets without touching the plugin at all.

- [ ] **T1-3. Trigger rollover on CONTEXT SIZE, not only on section ship.**
  **Evidence:** per-segment context/turn climbs to **600-660K** before the section boundary arrives
  (`run-20260725-052736` idx1/idx2 at 654K/660K; `run-20260725-020933` idx3 at 630K). The sequencer's only context-hygiene
  exit is a verified rollover after a **fully-shipped** section (`overnight-sequencer/SKILL.md:372`), so a long section
  drags a 600K context through every one of its turns.
  **Fix:** add a size trigger to `run_phase_guard.py`: at a step boundary, if the running context estimate exceeds a
  threshold (start at 250K), the next legal step is a `rollover-wip` -- commit-and-push the WIP, rotate, resume the same
  section from the section pack. The section pack + checkpoint machinery (`section-pack.py`, `section-checkpoint.py`)
  already exists to make the resume lossless.
  **Sharpens:** `overnight-runner-improvements.md` "Context-cap rollover ... (Path B)" -- that item is scoped as a flow
  fix; this adds the measured threshold and the arithmetic that justifies it.
  **Expected saving:** caps the worst-case multiplier. On the measured distribution, holding steady state at 250K instead
  of letting it run to 650K is **~15-20%**.
  **Acceptance:** no section resumes with a lost finding, lost review receipt, or lost stamp; `rollover-wip` never fires
  mid-review-round (only at a step boundary with a clean gate).

- [ ] **T1-4. Budget the hook injections -- 1,609 reminder fires, 226 dispatches.**
  **Evidence:** `.claude/state/offload-events.jsonl`: `fire` **1,609**, `dispatch` **226** (14% follow-through), `follow`
  638. 34 of 78 hooks emit `systemMessage` / `additionalContext`. Every fire is context that is then re-read for the rest
  of the session, and 86% of them changed no behavior.
  **Fix:** (a) **rate-limit per hook per session** -- a given advisory fires at most twice; the third time it is silently
  logged to state, not injected. (b) **Retire or promote:** any advisory hook under a ~20% follow rate is either promoted
  to a hard block (if the behavior genuinely matters) or deleted (if it does not). A warning nobody follows is pure cost.
  (c) **Compress the survivors** to one line plus a path -- several are currently multi-paragraph.
  **Sharpens:** `overnight-runner-improvements.md` "Make offload actually bite" -- same root cause, opposite prong: that
  item promotes hints to enforcement, this one deletes the hints that enforcement makes redundant. Do both together.
  **Expected saving:** **3-5%**, and a measurable drop in attention dilution.
  **Acceptance:** every hook retired or rate-limited is recorded in `.claude/hooks/MANIFEST.md` with its measured follow
  rate; `scripts/audit-hooks.sh` stays green; no *blocking* gate is touched by this item.

- [ ] **T1-5. Stop the high-context / low-work tail segments.**
  **Evidence:** **25 of 53 segments** did <=10 turns each at >300K context/turn -- 88 turns total (2% of work) burning
  48.9M cache-read tokens (**$73**, 3.7% of cache-read spend). These are post-rollover stubs that inherit a full context,
  do a handful of bookkeeping turns, and exit.
  **Fix:** move end-of-segment bookkeeping (metrics write, status refresh, stamp verification, advance-work) out of the
  model turn entirely -- it is already deterministic script work (`advance-work.py`, `run-status.py`, `metrics-report.py`).
  A segment that has nothing but bookkeeping left should exit to the launcher, which runs the scripts with zero model.
  **Expected saving:** ~3% and a cleaner segment boundary.
  **Acceptance:** the same artifacts land in `.claude/overnight/{metrics,reports}` with identical schemas.

---

## T2 -- Offload that actually replaces work (the user's lean)

- [ ] **T2-1. Enforce "dispatch REPLACES the read" -- today it demonstrably layers.**
  **Evidence:** 48 agent dispatches in the metrics window against 4,400 main-session Bash calls, 1,412 Reads, 1,343 Greps.
  The sidechain did 1,016 greps for 14,584 output tokens -- it is doing the cheap work correctly, but the main session
  kept doing its own anyway. `CLAUDE.md` already states the rule ("An agent dispatch must REPLACE expected main-session
  reads, never add a layer on top"); nothing enforces it.
  **Fix:** after an agent returns, the `agent_dispatch_recorder` hook records the file set the agent covered. A
  main-session `Read` / `Grep` on a path inside that covered set within the same phase gets a PreToolUse warning naming
  the agent report line that already answers it, escalating to a block on the third occurrence. Explicit carve-out: the
  **trust-contract verification read** (main session confirming ONE load-bearing finding at file:line before acting) is
  always allowed -- that is the read that protects quality and it is cheap because it is a slice.
  **Expected saving:** **8-15%**, and it is the item that makes every other offload item stick.
  **Acceptance:** the trust contract is preserved verbatim -- verification-before-completion quotes still come from the
  main session's own read of the artifact; only *exploratory* re-reads are suppressed.

- [ ] **T2-2. Fix the agent result cache: 155 stores, 0 hits, all time.**
  **Evidence:** `.claude/state/offload-events.jsonl` shows `cache-store` **155** and `grep -c cache-hit` returns **0**.
  `.claude/state/agent-cache/` holds 53 entries. The cache described in `CLAUDE.md` as making "repeat dispatches free"
  has never served a single hit.
  **Fix:** diagnose before changing anything (`superpowers:systematic-debugging`). The likely candidates, in order: the
  key includes the full prompt text so trivial wording drift misses; the key includes a timestamp or run id; the content
  hash includes files irrelevant to the query. Then: normalize the prompt into a canonical key (agent type + target paths
  + normalized question), and log a `cache-hit` with the estimated token saving so the claim is measurable next time.
  **Expected saving:** unknown until measured, but the mechanism is currently 100% overhead and 0% benefit.
  **Acceptance:** a deliberately repeated identical dispatch produces a logged `cache-hit`; a materially different
  question still produces a fresh run (no false hits -- a false hit IS a quality loss).

- [ ] **T2-3. Route the four highest-volume main-session shapes to standing agents by default.**
  **Evidence:** main-session tool mix is `bash 1,810 / bash_search 885 / edit 969 / read 282 / agent 48`. The
  `bash_search` bucket (885 calls) is grep/sed-through-Bash -- the exact shape doctrine says must go to the Grep tool or
  an agent.
  **Fix:** make these four routes automatic rather than judgment calls, since the judgment call is measurably being lost:
  (a) any TODO structural question -> `scripts/todo-graph/query.py` first, `todo-validation-mapper` for the residue;
  (b) any "where is X used / what calls Y" -> `mcp__lsp-bridge__references` when loaded, else `kernel-explorer` /
  `section-context-mapper` -- never an in-context grep sweep;
  (c) any build/test/lint/smoke run -> `run-artifact.sh` (already deterministic), `diagnostic-digester` on FAIL only;
  (d) any log over ~50 KB -> `serial-log-auditor` / `overnight-log-explorer`, never an in-context read.
  **Expected saving:** **5-8%**, mostly by moving 885 bash-search calls' output out of the main context.
  **Acceptance:** `bash_search` in the next run's `main_tools` drops below 200; no reduction in what gets found (spot
  check: the agent report names the same file:line the inline grep would have).

- [ ] **T2-4. Make every analyst return the typed envelope, and cap its size.**
  **Evidence:** `review-result-v1` + `evidence-schema.py` exist and are used by the command wrappers, but the 21 agent
  definitions largely return prose. 1,407 historical subagent dispatches at prose length is a lot of main-context text
  for findings that could be 20 lines of JSON.
  **Fix:** add the envelope requirement to every analyst's SKILL frontmatter/body, validate mechanically on
  `SubagentStop`, and REJECT (do not summarize) an over-length or malformed return. Cap the envelope at a fixed budget --
  a report that needs more than the cap is a signal the dispatch was scoped too wide, which is itself worth catching.
  **Expected saving:** **2-4%**, plus it removes the "model reads a long report to decide it was useless" tax.
  **Acceptance:** findings per dispatch does not drop; `unknowns[]` is populated rather than silently truncated.

---

## T3 -- Deterministic work that is currently spending a model

- [ ] **T3-1. Collapse the `cd`-prefix and shell-wrapper habit.**
  **Evidence:** **2,976 of 4,400 Bash calls start with `cd`.** The working directory persists between calls; the `cd`
  prefix is pure repetition, and it also defeats permission-allowlist matching (every distinct `cd X && Y` is a new
  string), which drives permission churn.
  **Fix:** a PreToolUse rewrite/warn on `cd <project-dir> &&` when the cwd already matches, plus one line in `CLAUDE.md`.
  Pair with `/fewer-permission-prompts` to rebuild the allowlist against the un-prefixed shapes.
  **Expected saving:** small in tokens (**~1%**) but a large drop in permission-prompt friction and hook re-evaluation.
  **Acceptance:** no command loses its working directory; the sandbox behavior is unchanged.

- [ ] **T3-2. Targeted suites during the fix loop; ONE full suite + smoke at the section boundary.**
  Already filed in `overnight-runner-improvements.md` ("Cadence: targeted suites DURING the fix loop") and **not yet
  done**. Restating it here because the cost framing changes its priority: a full-suite run is cheap in wall-clock (~24s)
  but its *output* lands in context and is then re-read for the rest of the session. Repeating it every fix round is a
  cache-read multiplier, not just a time cost.
  **Fix as filed**, plus: pipe every suite run through `run-artifact.sh` so only the JSON verdict enters context, never
  the raw PASS lines. `QUIET=1` on everything that is not the boundary run.
  **Acceptance:** the section boundary still runs the FULL suite + smoke and still quotes real output for the Verified
  stamp. Only the intra-loop runs are narrowed.

- [ ] **T3-3. Never re-dispatch an unchanged review kind on unchanged inputs.**
  Filed in `overnight-runner-improvements.md` as two items (convergence-based review as a RULE; per-kind x per-file
  invalidation) and **not yet done**. The cost case: a Codex round is cheap on our side but the *reception* is not -- the
  verdict body, the finding triage, and the `receiving-code-review` skill body (6.2 KB, invoked 103 times) all land in
  main context and compound. Cutting a redundant round removes all three.
  **Fix as filed.** Add: key the invalidation on the review-relevant file set from `diff-facts.py`, and log every
  suppressed round so the saving is countable.
  **Acceptance:** a round is only suppressed when BOTH the review kind and every file in its relevant set are unchanged
  since the receipt. Any doubt re-dispatches. **Never** suppress a re-adversarial round that follows a fix.

- [ ] **T3-4. Move the remaining bookkeeping/verdict parsing out of model turns.**
  **Evidence:** `main_tools` shows `bash 1,810` and `other 144`; the deterministic script fleet
  (`preflight.py`, `receipts.py`, `run-outcome.py`, `section-cost-report.py`, `advance-work.py`) already exists and is
  correct -- it is just being supplemented by model-side parsing of the same data.
  **Fix:** audit the runner skill step list for any step where the model reads a script's output and then restates it.
  Each such step becomes: script writes the structured artifact, the model reads only the one-line verdict.
  **Expected saving:** **2-3%**.
  **Acceptance:** no gate loses its evidence; the artifact on disk remains the source of truth for stamps.

---

## T4 -- Measurement, so the savings cannot silently regress

- [ ] **T4-1. Put cost in the run report, in the units that matter.**
  **Evidence:** `metrics-report.py` and `section-cost-report.py` exist, but nothing surfaces the cache-read-dominance
  finding -- which is why the backlog to date optimized the 10% slice.
  **Fix:** every run report ends with: total cache-read, average context/turn, peak context/turn, re-read ratio,
  skill-injection bytes, hook-fire count, agent dispatch count and cache-hit rate, and an estimated dollar figure with
  the per-bucket split. One table, same shape every run.
  **Acceptance:** the numbers reconcile with `.claude/overnight/metrics/*.jsonl` when checked by hand.

- [ ] **T4-2. Regression gate on the context metrics.**
  **Fix:** after T1 lands, record the achieved baseline (target: average context/turn <= 180K, re-read ratio <= 20%,
  hook fires <= 400/run). A run that regresses more than 25% past baseline writes a WARN into
  `.claude/overnight/NEEDS-OPERATOR.md`. Warning only -- never block a run on a cost metric.
  **Acceptance:** the gate cannot stop shipping work; it can only report.

- [ ] **T4-3. Measure one section end-to-end before and after, and publish the delta.**
  **Fix:** pick one representative shipped section, replay the same work with T1-1 / T1-2 / T2-1 active, and record
  actual vs predicted saving. If the prediction is off by more than 2x, the model of where cost goes is wrong and the
  rest of this file needs re-deriving before more items land.
  **Acceptance:** the delta is published in this file as a postscript with the raw numbers, whatever they say.

---

## Expected combined effect

| Tier                     | Items                      |       Estimated saving |
| ------------------------ | -------------------------- | ---------------------: |
| T1 Context economics     | T1-1 .. T1-5 (incl. T1-2b) |                 45-60% |
| T2 Offload that replaces | T2-1 .. T2-4               |                 15-25% |
| T3 Deterministic work    | T3-1 .. T3-4               |                   5-8% |
| T4 Measurement           | T4-1 .. T4-3               | 0% (protects the rest) |

These do not sum -- they overlap heavily (T1-1 and T2-1 both attack re-reading from different ends). A realistic
combined target is **50-65% cost reduction with zero change to what gets built or how hard it is reviewed.** Sequence:
**T1-1 (done) -> T2-2 -> T1-2 -> T2-1 -> T1-3**, then measure (T4-3) before doing the rest. T1-2b rides along with the
concurrent-leg work in `overnight-runner-improvements.md` rather than being scheduled on its own.

---

## Never cut (the quality floor these items must not touch)

Every item above was scoped to leave the following completely intact. If an implementation of any item requires touching
one of these, the item is wrong and gets re-scoped, not the floor.

- **The Codex review pipeline.** Adversarial, consistency, perf, design, re-adversarial, and the fix loop stay as-is.
  Nothing here caps review rounds (already evaluated and rejected in `overnight-runner-improvements.md`).
- **`kernel-quality-auditor` stays on Opus.** It is the sole SMP / lock-order / bare-metal net for the repo's most
  expensive bug class.
- **The full unit suite + smoke test at every section boundary**, and the CI-parity pre-push gate. Intra-loop runs narrow;
  boundary runs do not.
- **The trust contract.** Load-bearing findings are verified by the main session at file:line. Verification-before-
  completion quotes come from the main session's own read of the on-disk artifact.
- **Every blocking hook gate**: `section_commit_gate`, `run_phase_guard`, `review_dispatch_gate`, `broker_dispatch_required`,
  `codex_review_completed`, `runner_bash_guard`, the test-policy and code-style blocks. T1-4 touches advisories only.
- **Bare-metal-first validation.** No item here trades a hardware-truth check for a cheaper proxy.
