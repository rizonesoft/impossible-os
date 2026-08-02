# Token Saver v01 -- Cost Reduction Backlog (measured 2026-07-20 -> 2026-07-26)

> **CLOSED 2026-07-28. Historical record -- do not file into this file.** Every item is terminal: 14 `[x]` done, 3 `[/]` MOVED to [`token-saver-v02.md`](token-saver-v02.md) with a stub left here so existing XREFs still resolve. The measured outcome of the whole effort is the POSTSCRIPT at the end (T4-3): predictions were off by ~11x and ~26x, so the model of where cost goes was wrong and v02 re-derives it. New cost findings go to v02.
> **Deliberately outside the sequencer.** Lives at `todo/token-saver/` (a versioned directory, 2026-07-27) as
> `token-saver-v01.md`, so `sequencer_triage.is_impl_todo` -- which requires `todo/\d\d-<domain>/TODO-\d+-*.md` --
> never traverses it, and the todo-graph never parses it. Same intent as `todo/overnight-runner-improvements/overnight-runner-improvements-v01.md`.
>
> **Naming rule for the version files added here.** The directory name defeats the sequencer regardless of filename
> (`token-saver` is not `\d\d-<domain>`), but the todo-graph glob is `root.rglob("TODO-*.md")` -- it matches on
> FILENAME, recursively, at any depth. So a version file called `TODO-something.md` in this directory WOULD be pulled
> into the graph and validated as an implementation TODO. Name versions `token-saver-vN.md` / `token-saver-YYYY-MM-DD.md`
> -- anything that does not start with `TODO-`.
>
> Companion to `todo/overnight-runner-improvements/overnight-runner-improvements-v01.md`. That file is a **flow / correctness** backlog whose cost items are
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
  **SHIPPED 2026-07-27** as [`.claude/hooks/read_cache_block.py`](../../.claude/hooks/read_cache_block.py) (PreToolUse
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
  **MOVED 2026-07-28 to [`token-saver-v02.md`](token-saver-v02.md) "Migrated from v01", unchanged, when this file was CLOSED.** Still live but re-ranked: skill injection measured ~10% of per-turn context (142 KB/segment, ~$44/night), so it is real but a tenth of the accumulation prize.

- [x] **T1-2b. Collapse the in-pass `receiving-code-review` repeats at their SOURCE (batch the review legs).**
  **SHIPPED 2026-07-27.** The root cause was not the dispatch shape -- it was the skill prose contradicting itself.
  `review-todo-section` step 8 already described the batched broker shape ("ONE structural wait + ONE
  `review-envelope.py` read"), while the same step's tail said "triage findings from each independently" and step 8's
  closing line said "follow it on every finding from every dispatch". That per-dispatch phrasing is what produced one
  reception per leg; `overnight-sequencer` carried the same shape as "receive every findings set".
  **Mechanism confirmed before changing anything:** `codex_review_completed.py` OVERWRITES
  `.claude/state/last-codex-review.json` on every trigger -- a single record with one `trigger` label and
  `received: false`, no per-kind accumulation. So legs dispatched in ONE parallel message collapse to one record that
  ONE reception clears, and `receiving_review_required` (which reads that single record) is satisfied by it. Receiving
  per leg was never required by any gate.
  **Fix:** a "Reception cadence" rule in both drivers -- ONE reception per WAVE, where a wave is the set of legs
  dispatched together and waited on together. Step 5 adversarial is a wave of one; 8a+8b are one wave of two; the
  unattended broker bundle is one wave of three. The rule names the overwrite mechanism so it reads as a fact rather
  than an assertion, and states explicitly that each dispatch's PostToolUse reminder is the harness prompting per Bash
  call, NOT a per-leg obligation.
  **Rigor is unchanged and says so:** every finding from every leg still goes through Fix / Reject / Accept at
  file:line; reading the legs together is strictly better for catching the same defect reported twice; and a leg whose
  findings must be fixed before another can be scoped is explicitly its own wave.
  **No enforcement hook, deliberately.** T1-4 in this same file says an advisory under ~20% follow-through should be
  promoted or deleted, not added to -- and a BLOCK on the reception path is precisely the review-gate coupling T1-2b
  refused in the first place. The effect is measurable from existing data instead: `tool-history.jsonl` already records
  every `Skill` invocation, which is how the 75 repeats were found.
  **Checks run:** no per-leg phrasing remains in any skill; all 17 `review-todo-section` steps present;
  `scripts/lint.sh` 0 errors; `scripts/audit-ai-system.sh` 7/7; control-plane suite 47/47.
  **Acceptance (to confirm on the next run):** consecutive `receiving-code-review` gaps under 30 min drop from 75
  toward ~0, with no drop in findings triaged per round.

  <details><summary>Original item text (superseded)</summary>
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
  `todo/overnight-runner-improvements/overnight-runner-improvements-v01.md` wants the same shape) and the repeat receptions collapse at the source -- same
  saving, no gate coupling, no duplicated receipt, and it makes that backlog item cheaper too.
  **Acceptance:** every finding is still received through the discipline; `received: true` is still set only by the real
  PostToolUse path; no reduction in findings triaged per review round.
  **Rejected: vendoring the superpowers skills into the repo.** Injection cost is `size x invocations` and vendoring
  changes neither -- the same 6.2 KB body still lands on every call. It only pays if we then edit it smaller (~230 KB
  best case), against renaming `superpowers:receiving-code-review` across 9 `codex-*` skills plus the hook messages, and
  forking a plugin that is actively versioned (6.0.3 / 6.1.1 / 6.2.0 all cached locally) and pinned load-bearing by
  `CLAUDE.md`. 79% of the injected volume is our own skills, which T1-2 diets without touching the plugin at all.

  </details>

- [/] **T1-3. Trigger rollover on CONTEXT SIZE, not only on section ship.**
  **MOVED 2026-07-28 to [`token-saver-v02.md`](token-saver-v02.md) "Migrated from v01", unchanged, when this file was CLOSED.** Still the highest-value open cost item: only sub-item 1c landed (`bf3f8899`), the size trigger itself was never implemented, and the 2026-07-28 canary reproduced exactly the growth it predicts.

- [x] **T1-4. Budget the hook injections -- 1,609 reminder fires, 226 dispatches.**
  **Closed 2026-07-28:** DELIVERED -- the only T1 item that met its target. Hook fires measured 12 / 26 / 73 per segment on the canary run against the ~1,609 that motivated it. Raw numbers in the POSTSCRIPT at the end of this file.
  **(a) SHIPPED 2026-07-27; (b) RESOLVED BY MEASUREMENT -- nothing to retire; (c) not needed.**
  **The headline conflated two different things.** Re-measured per hook from `offload-events.jsonl` (1,974 fire
  events), the fires split by hook CLASS -- and a BLOCK's "fire" means it BLOCKED, not that it advised and was
  ignored, so a follow-rate is meaningless for one and rate-limiting it would punch a hole in a gate:
  `interactive_offload_router` 487 injections / 0 follows (REMINDER), `build_offload_reminder` 441 (BLOCK),
  `codex_wait_discipline` 228 (BLOCK), `websearch_offload_gate` 180 (BLOCK), `agent_dispatch_required` 26
  (BLOCK+WARN). Only ONE genuine advisory candidate existed, not 34 -- and it is the least-heeded of all.
  **(a)** `.claude/hooks/_advisory_budget.py`: per-SESSION cap (default 2) that COUNTS its suppressions in
  `.claude/state/advisory-budget.json` rather than discarding them, so the volume stays measurable for a later
  retire-or-promote call. Wired into `interactive_offload_router` only. Fail-open in the EMIT direction -- a budget
  bug must never silence a hook. No session id means NO budget (the contract is per-session; counting without one
  would accumulate across unrelated invocations and silence the hook forever) -- caught by the existing
  `offload_router` tests, which drive the hook with no session and went silent from the third case on.
  **(b)** No hook was retired: after excluding the BLOCK gates the acceptance criterion protects, the only advisory
  with a poor follow rate is the one now budgeted. Retiring it outright would lose the routing hint entirely; the
  budget keeps the first two per session, which is where the teaching value is.
  **(c)** Skipped -- the surviving advisory is already a single formatted line.
  **Found on the way:** `build_offload_reminder` was recorded as REMINDER in `MANIFEST.md` but is a P3.4
  BLOCK-with-reroute (`return 2`). Class corrected. `audit-hooks.sh` cannot catch that direction -- it flags a
  BLOCK-labelled hook with NO exit-2 path, not a REMINDER-labelled hook that blocks.
  **Checks run:** `_advisory_budget --selftest`; `scripts/test-tooling.sh` **554/554** (incl. a new invariant that no
  hook with a `return 2` imports the budget); control-plane suite 48/48; `audit-hooks.sh` no drift; lint 0 errors.
  **Still open:** measuring the achieved reduction on the next full run (T4-1 owns the reporting).
  **Evidence:** `.claude/state/offload-events.jsonl`: `fire` **1,609**, `dispatch` **226** (14% follow-through), `follow`
  638. 34 of 78 hooks emit `systemMessage` / `additionalContext`. Every fire is context that is then re-read for the rest
  of the session, and 86% of them changed no behavior.
  **Fix:** (a) **rate-limit per hook per session** -- a given advisory fires at most twice; the third time it is silently
  logged to state, not injected. (b) **Retire or promote:** any advisory hook under a ~20% follow rate is either promoted
  to a hard block (if the behavior genuinely matters) or deleted (if it does not). A warning nobody follows is pure cost.
  (c) **Compress the survivors** to one line plus a path -- several are currently multi-paragraph.
  **Sharpens:** `todo/overnight-runner-improvements/overnight-runner-improvements-v01.md` "Make offload actually bite" -- same root cause, opposite prong: that
  item promotes hints to enforcement, this one deletes the hints that enforcement makes redundant. Do both together.
  **Expected saving:** **3-5%**, and a measurable drop in attention dilution.
  **Acceptance:** every hook retired or rate-limited is recorded in `.claude/hooks/MANIFEST.md` with its measured follow
  rate; `scripts/audit-hooks.sh` stays green; no *blocking* gate is touched by this item.

- [x] **T1-5. Stop the high-context / low-work tail segments.**
  **CLOSED 2026-07-27 -- PREMISE INVALIDATED, not implemented. The $73 is real but it is DOUBLE-COUNTED.**
  The item calls these "post-rollover stubs that inherit a full context". They are not. `overnight-launch.sh` states
  it outright: *"Every launch is a fresh headless agent (there is no real conversation resume); the guard cursor in
  sequencer-run.json is what carries state across relaunches."* A segment is NOT a spawn -- one run is ONE session,
  sliced into segments at commit boundaries. `run-20260725-052736` is the proof: idx0 = 275 turns at 377K/turn, then
  idx1 (2 turns) and idx2 (5 turns) at 654K and 660K -- the SAME session, its context having grown.
  So the tail turns are expensive BECAUSE THE SESSION GREW, not because of what they do. Moving bookkeeping to the
  launcher (the stated fix) would change nothing: those 7 turns would still cost ~660K each. And the measured shape
  contradicts the fix anyway -- the tail segments run 61 `bash` + 12 `bash_search` calls with zero Reads, Edits or
  Agent dispatches, and that is rollover VERIFICATION, which is inherently model-side (the guard's `rollover` verb is
  invoked and its verdict read), not the `advance-work.py` / `run-status.py` / `metrics-report.py` bookkeeping the item
  names. The launcher already runs `run-outcome.py`, `parse-usage-limit.py` and `stream-report.py` post-run, plus
  `run-status.py` on the fixpoint path.
  **There is no separate tail-segment problem.** It is the same context-growth cost that T1-1, T1-2 and T1-3 target,
  measured at the end of the session where it is most visible. Counting its $73 as an additional 3% saving on top of
  those items would be double-counting the same tokens.
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

- [x] **T2-1. Enforce "dispatch REPLACES the read" -- today it demonstrably layers.**
  **SHIPPED 2026-07-27** as `.claude/hooks/agent_coverage_gate.py` (PreToolUse `Read` gate + PostToolUse
  `Task|Agent` recorder), code `[AGENT-COVERED]`.
  **The half that already existed:** `inflight_race_guard` (D1) warns when a read RACES a still-running agent -- but it
  CLEARS its map at SubagentStop, so the moment the report lands and the coverage becomes durable, it stops caring.
  This hook owns exactly that other half: coverage AFTER the agent returned. `agent_dispatch_recorder` records type and
  timestamp but no file set, so the covered-path map is new.
  **The carve-out is the whole design, not a footnote.** Only WHOLE-FILE reads are gated; a slice read
  (`offset`/`limit`) is ALWAYS free. That is the trust-contract verification read -- the main session confirming ONE
  load-bearing finding at file:line before acting -- and it is the read that PROTECTS quality. Gating it would trade a
  real guarantee for tokens, which is the Never-cut line. Acceptance met verbatim.
  **Escalation:** WARN twice, BLOCK from the third. The first read after a report is often a legitimate spot-check; by
  the third the message has been shown twice and it is the layering this exists to stop.
  **Exempt by construction:** subagents (their reads ARE the delegated work), other sessions (a fresh worker holds
  neither report nor context), non-covering agent types (a judgment/executor dispatch maps no file surface, so recording
  it would suppress reads nothing covered), and any path the agent never touched. Fail-open everywhere; kill switch
  `AGENT_COVERAGE_DISABLE=1`.
  **Checks run:** `--selftest` (12 cases incl. the slice carve-out and the non-covering-agent case);
  `scripts/test-tooling.sh` **555/555**; control-plane suite 48/48; `audit-hooks.sh` no drift; lint 0 errors; live
  end-to-end confirmed warn -> warn -> BLOCK with the slice read still free.
  **Still to measure (T4-1/T4-3 own it):** the predicted 8-15%.
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

- [x] **T2-2. Fix the agent result cache: 155 stores, 0 hits, all time.**
  **SHIPPED 2026-07-27 -- root-caused via `superpowers:systematic-debugging`, not guessed.**
  **Root cause (none of the three hypotheses in this item).** Prompt drift was already handled (P2.3 canonical scope)
  and there was no timestamp in the key. The defect: the key included a fingerprint of the WHOLE source tree
  (`SRC_PATHS` = src, include, user, resources, tools, Makefile, ...), so ANY edit anywhere invalidated EVERY entry.
  The designed hit case is a mapper re-dispatched across review rounds -- and a fix loop EDITS CODE between those
  rounds. The cache was keyed on something guaranteed to change between any two dispatches in a working session, so it
  could never hit even once. Editing `src/desktop/foo.c` invalidated a `kernel-explorer` report about
  `src/kernel/quota/`.
  **Reproduced deterministically before any fix:** same prompt, covered file untouched, one unrelated file edited ->
  key changed. After the fix, the key is stable.
  **Fix:** the filename key is now the SCOPE (agent + canonical scope prose). Freshness became a SEPARATE check against
  `covered_fp` -- a fingerprint over the paths the report actually covered, taken from the prompt AND the report,
  because an explorer follows callers beyond the files it was handed and a prompt-only scope could serve a stale map.
  **No-false-hit acceptance met, and it is the half that matters:** an edit to a COVERED file still misses (verified);
  a missing fingerprint, unreadable tree, or any mismatch fails toward a MISS. The 53 pre-existing entries carry no
  `covered_fp` and are never served -- they re-populate naturally.
  **Checks run:** 3 new contract tests (unrelated-edit-hits, covered-edit-misses, legacy-entry-never-hits) plus the
  existing round-trip suite; control-plane suite 48/48; lint 0 errors.
  **Still to measure:** the hit RATE on a real run. The mechanism was 100% overhead and 0% benefit; it should now hit
  on mapper re-dispatches within a fix loop, which is where T2-1 also routes work. T4-1 owns the reporting.
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

- [x] **T2-3. Route the four highest-volume main-session shapes to standing agents by default.**
  **Closed 2026-07-28:** CLOSED BY REVERT -- (b) `search_offload_gate` was unwired after the live run disproved its premise (every tool result lands in context, the Grep tool's included); (c) and (d) already had enforcement; (a) died with (b). Full reasoning in the item body below.
  **(b) REVERTED 2026-07-28 -- shipped 2026-07-27 as `.claude/hooks/search_offload_gate.py`, unwired the next day when
  the live canary run disproved it. (c) and (d) already had enforcement; (a) was folded into (b)'s message and dies
  with it.**
  **WHY (b) WAS WRONG -- the saving never existed.** The premise, written into `MANIFEST.md` as "the Grep/Glob TOOL, whose output does not land in the main context", is false: EVERY tool result lands in context, the Grep tool's included. So the blocked `grep -n X file | head -20` and the piped form it was pushed into, `cat file | grep -n X | head -20`, cost exactly the same. Measured live: 3 tool calls where 1 would do (block, retry, reformulate), zero context saved, and the run settled on `cat file | grep` as its standing idiom. Retired rather than reworded -- bounded output and agent offload are already owned by other gates.
  **A SECOND FAULT WAS CLAIMED HERE ON 2026-07-28 AND IS WRONG.** The first revert note asserted that the headless run "has no Grep tool", citing one `Grep is not available in this session` error at 02:03:44. The same run then made **14 successful Grep tool calls** from 02:26 onward. Grep is a DEFERRED tool whose schema loads on demand via `ToolSearch`; that single error was an unloaded schema, not an absent tool. The gate's message never naming `ToolSearch` was a message defect, not a fatal one.
  **The lesson generalizes past this item, twice.** The original premise ("Bash search output lands in context, Grep tool output does not") was never checked against a running session -- the same unverified-premise failure this file catalogues in T1-2, T1-5, T2-4 and T3-2, committed by the author of the catalogue. Then the revert itself repeated it, generalizing a tool-availability claim from a single error line. Verify at the source, including when the finding is your own and points the way you already want to go.
  **The 885 figure is not all addressable, and that IS the design.** Re-counting `tool-history.jsonl` with shlex
  tokenization instead of a regex: **669 LEAD file-searches** (gateable), **563 searches after a pipe** -- these filter
  another command's STDOUT, which the Grep tool structurally cannot do -- and **1,619 commands (a third of all Bash
  traffic) that do not shlex-parse at all** and must fail open. So the gate reroutes ~663, not 885, and is deliberately
  blind to a third of the traffic. Expected saving revised DOWN accordingly.
  **Tokenized, never regex.** A `\|` alternation inside a quoted grep pattern reads as a pipe to a regex, so a naive
  matcher confuses a file search with an output filter in both directions.
  **Two bugs caught by validating against real command history rather than fixtures:** (1) `cd <dir> && grep ...` -- the
  single most common real shape -- was not gated, because the first cut treated a sequence operator like a pipe;
  (2) `sed -i '...' file && grep -c '...' file` WAS gated, which would have rejected the WRITE along with the search.
  The gate now judges only the FIRST real segment, and 0 write/build compound commands are gated across the whole
  history (verified).
  **Scope:** headless only (`OVERNIGHT_SEQUENCER_RUN=1`), following `websearch_offload_gate`'s precedent -- the
  measurement comes from unattended runs and an interactive operator can make the call themselves. Subagents never
  gated (their searches ARE the delegated work).
  **(c) build/test/lint/smoke** was already enforced by `build_offload_reminder` (a BLOCK-with-reroute, despite its
  name -- class corrected in T1-4). **(d) big-log reads** are covered by `read_offload_reminder` plus T1-1's read cache
  and T2-1's coverage gate. **(a) TODO structural** is folded in: a gated search targeting `todo/` names
  `scripts/todo-graph/query.py` in its message.
  **Checks run:** `--selftest` (14 cases incl. the alternation trap and the piped/stdin exemptions);
  `scripts/test-tooling.sh` **556/556**; control-plane suite 48/48; `audit-hooks.sh` no drift; lint 0 errors; live
  scoping confirmed inert interactively, active headless, piped allowed, subagent allowed.
  **Still open:** the acceptance metric -- `bash_search` below 200 in the next run's `main_tools`. Note the metric
  counts the BROADER `_SEARCH_RE` (it includes piped searches this gate must allow), so the realistic floor is ~563,
  not 200. The acceptance threshold needs restating against the addressable subset.
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

- [x] **T2-4. Make every analyst return the typed envelope, and cap its size.**
  **COST CLAIM INVALIDATED; the useful half SHIPPED 2026-07-27.**
  **The 2-4% is off by ~25-50x.** Agent reports are already small: total sidechain OUTPUT across the measured window is
  **14,584 tokens over 48 dispatches = ~303 tokens (~1.2 KB) per report**. That is already envelope-sized -- converting
  it to JSON could make it BIGGER. Even assuming a report is carried for 150 later turns, the whole corpus is 0.16% of
  cache-read spend, so HALVING every report saves **~0.08%**, not 2-4%. The item's "1,407 historical dispatches" also
  mixes an all-time count with a windowed cost -- the same double-counting T1-5 was closed for.
  **NOT implemented, deliberately: REJECT on `SubagentStop`.** It is the wrong mechanism in the wrong direction. The
  agent has already RUN by then, so its tokens are spent; rejecting discards the work and forces a re-dispatch, which
  costs MORE. `SubagentStop` is also not used as a gate anywhere in this repo (`subagent_audit` is STATE-only,
  `inflight_race_guard` uses it to CLEAR), so exit-2 semantics there are unproven. Chasing 0.08% with an unproven
  gate that is cost-negative when it fires is a bad trade.
  **SHIPPED instead -- the part with non-cost value.** "Every analyst" was the item's flaw: 6 of the 21 return
  NARRATIVES (the three researchers, `diagnostic-digester`, `serial-log-auditor`, `overnight-log-explorer`) and forcing
  a citation narrative or an anomaly timeline into a findings array degrades it. The envelope went to the **11
  findings-shaped analysts** only (auditors + mappers). Each block carries the schema, the validator command, and the
  two rules that matter more than the format: **`unknowns[]` is not optional padding** (an envelope that silently omits
  what it could not determine is worse than prose, because it reads as complete), and **a ~400-line cap where overflow
  goes to `unknowns[]` rather than being truncated** -- exceeding it signals the dispatch was scoped too wide.
  **Doc drift fixed:** `CLAUDE.md` implied the envelope discipline applied to analysts generally while 0 of 21 carried
  it; it now states the 11/6 split and why the narrative six are excluded.
  **Acceptance met on the term that matters:** findings per dispatch cannot drop (nothing rejects a return), and
  `unknowns[]` is explicitly required rather than silently truncated.
  **Checks run:** a sample envelope validates against `evidence-schema.py` (exit 0); lint 0 errors (Check 14 read-only
  allowlist unaffected).
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

- [x] **T3-1. Collapse the `cd`-prefix and shell-wrapper habit.**
  **SHIPPED 2026-07-28** as `.claude/hooks/cd_prefix_reminder.py` + a `CLAUDE.md` rule.
  **Both premises verified, and the count is larger than stated:** cwd persistence confirmed LIVE (a call with no
  prefix reports the project root), and re-measuring gives **3,187 of 4,729 Bash calls (67%)** carrying
  `cd <project-dir> &&` against only **4** legitimate `cd <other dir>` -- so the redundant shape can be targeted
  precisely with essentially no false-positive surface.
  **The item's "rewrite" half is NOT buildable and was not attempted.** No hook in this repo modifies `tool_input`,
  and the only documented JSON envelope is `permissionDecision: deny` -- a block, not a rewrite. Blocking was also
  rejected on its merits: the prefixed command is CORRECT and runs fine, so blocking it would trade real friction for a
  cosmetic gain. Warning-only.
  **Budgeted at 2 injections/session via `_advisory_budget` (T1-4's helper, first reuse).** A 3,187-fire nag would
  itself become the cost problem this item describes -- exactly what T1-4 measured on `interactive_offload_router`
  (487 injections, zero follows). The habit is learned in one or two reminders.
  **The real payoff is permission friction, not tokens** (the item says as much). An allowlist entry like
  `Bash(bash scripts/test.sh:*)` cannot match a `cd /long/path && bash scripts/test.sh` string, so every prefixed
  variant is a fresh permission decision.
  **Acceptance met:** nothing loses its working directory (the hook never blocks and never edits a command), and
  sandbox behaviour is untouched. Silent on `cd <other dir>`, on subdirectories, and on unprefixed commands --
  verified live.
  **Checks run:** `--selftest` (13 cases incl. never-blocks and the budget cap); `scripts/test-tooling.sh`
  **557/557**; control-plane suite 48/48; `audit-hooks.sh` no drift; lint 0 errors.
  **Operator follow-up (yours, not automatable here):** run `/fewer-permission-prompts` once the unprefixed shapes
  start appearing, so the allowlist is rebuilt against them.
  **Evidence:** **2,976 of 4,400 Bash calls start with `cd`.** The working directory persists between calls; the `cd`
  prefix is pure repetition, and it also defeats permission-allowlist matching (every distinct `cd X && Y` is a new
  string), which drives permission churn.
  **Fix:** a PreToolUse rewrite/warn on `cd <project-dir> &&` when the cwd already matches, plus one line in `CLAUDE.md`.
  Pair with `/fewer-permission-prompts` to rebuild the allowlist against the un-prefixed shapes.
  **Expected saving:** small in tokens (**~1%**) but a large drop in permission-prompt friction and hook re-evaluation.
  **Acceptance:** no command loses its working directory; the sandbox behavior is unchanged.

- [x] **T3-2. Targeted suites during the fix loop; ONE full suite + smoke at the section boundary.**
  **CLOSED 2026-07-28 -- substantially ALREADY DONE, and the cost rationale no longer holds.**
  **The "plus" clauses are already enforced or moot.** `build_offload_reminder` BLOCKS a bare
  `bash scripts/{build,test,test-smoke,lint,test-tooling}.sh` and explicitly EXEMPTS the sanctioned
  `run-artifact.sh <label> -- bash scripts/...` route, so every suite run already goes through the wrapper. And
  `QUIET=1` is moot: the wrapper emits only `{exit, errors (<=40), tail (8 lines), artifact, sha256}` -- the raw PASS
  lines never reach context whatever QUIET does.
  **Measured: the envelope is 2,744 bytes (~686 tokens) against a full suite's ~25,000 PASS lines.** That is ~99% of
  the output cost already gone. So this item's premise -- "its output lands in context and is then re-read for the
  rest of the session ... a cache-read multiplier" -- is NO LONGER TRUE. A repeated full suite now costs a bounded
  envelope plus ~24s wall-clock, not a compounding cache-read.
  **(1) targeted-suite cadence stays a WARN, deliberately.** `verify_cadence_reminder` (P3.3) already nudges at >=3
  full-suite or >=2 smoke runs per section, with targeted `SUITE=` runs never counted. Promoting it to a BLOCK is not
  justified: the cost it was meant to prevent is gone, and the section BOUNDARY *requires* a full suite + smoke -- a
  block that misjudged boundary-vs-mid-loop would wedge the ship. Wall-clock discipline does not warrant that risk.
  **Acceptance already satisfied by the existing design:** the boundary still runs FULL suite + smoke, and
  verification-before-completion still quotes the on-disk artifact (the skill requires the main session to read
  `build/build.log` / the suite summary itself), so the Verified stamp rests on real output.
  **Fixed on the way:** the `build_offload_reminder` MANIFEST row still read "Warning-only BY DESIGN ... a BLOCK would
  deadlock the sanctioned path" after T1-4 corrected its Kind to BLOCK -- residue from that correction. The row now
  explains the actual mechanism: the wrapped route is EXEMPTED before the block, which is what makes blocking safe.
  Already filed in `todo/overnight-runner-improvements/overnight-runner-improvements-v01.md` ("Cadence: targeted suites DURING the fix loop") and **not yet
  done**. Restating it here because the cost framing changes its priority: a full-suite run is cheap in wall-clock (~24s)
  but its *output* lands in context and is then re-read for the rest of the session. Repeating it every fix round is a
  cache-read multiplier, not just a time cost.
  **Fix as filed**, plus: pipe every suite run through `run-artifact.sh` so only the JSON verdict enters context, never
  the raw PASS lines. `QUIET=1` on everything that is not the boundary run.
  **Acceptance:** the section boundary still runs the FULL suite + smoke and still quotes real output for the Verified
  stamp. Only the intra-loop runs are narrowed.

- [x] **T3-3. Never re-dispatch an unchanged review kind on unchanged inputs.**
  **"Not yet done" was STALE -- the filed items shipped as P2.1 + P2.2. The one genuine gap, LOGGING, is now closed
  (2026-07-28).**
  **Already done:** `review_convergence.py` implements convergence-as-a-rule (P2.1) and per-kind scope (P2.2);
  adversarial/perf/re-adversarial fingerprint SOURCE only, consistency/design SOURCE+TODO, so a docs-only fix converges
  the source-only kinds. Both source items in `overnight-runner-improvements-v01` were closed on this evidence earlier
  today; this item's "not yet done" predates that check.
  **ADD (1) -- keying on `diff-facts.py` -- REJECTED, because it cuts against this item's own acceptance.** The gate
  currently fingerprints the whole scope family. Narrowing it to "the files this kind's findings touched" would make
  suppression MORE likely, and the acceptance says "Any doubt re-dispatches" and "**Never** suppress a re-adversarial
  round that follows a fix". Note this is the OPPOSITE call from T2-2, and deliberately so: in the CACHE a false hit
  serves a stale map, so the scope had to narrow; in the REVIEW gate a false "converged" SKIPS a needed review, so the
  broad scope is the conservative choice. Both fail toward more work, which is why they move in opposite directions.
  **ADD (2) -- SHIPPED.** Every decision is logged to `offload-events.jsonl` as `converged` / `redispatch`, and
  `offload-report.py` prints the suppression ratio. BOTH outcomes are logged on purpose: a suppression count alone
  cannot distinguish "the gate is working" from "the gate is never consulted" -- which is precisely how the agent
  cache sat at 155 stores / 0 hits unnoticed until T2-2. The report calls that case out explicitly when it sees 0/N.
  **Acceptance unchanged and still enforced by the existing code:** suppression requires the kind's ENTIRE scope to be
  unchanged since its recorded verdict; any git failure or unknown kind fails open to REDISPATCH.
  **Checks run:** `review_convergence --selftest`; live end-to-end confirmed a REDISPATCH and a CONVERGED each log
  their own event with the verdict unchanged; `offload-report.py` renders the ratio; `audit-hooks.sh` no drift.
  Filed in `todo/overnight-runner-improvements/overnight-runner-improvements-v01.md` as two items (convergence-based review as a RULE; per-kind x per-file
  invalidation) and **not yet done**. The cost case: a Codex round is cheap on our side but the *reception* is not -- the
  verdict body, the finding triage, and the `receiving-code-review` skill body (6.2 KB, invoked 103 times) all land in
  main context and compound. Cutting a redundant round removes all three.
  **Fix as filed.** Add: key the invalidation on the review-relevant file set from `diff-facts.py`, and log every
  suppressed round so the saving is countable.
  **Acceptance:** a round is only suppressed when BOTH the review kind and every file in its relevant set are unchanged
  since the receipt. Any doubt re-dispatches. **Never** suppress a re-adversarial round that follows a fix.

- [x] **T3-4. Move the remaining bookkeeping/verdict parsing out of model turns.**
  **AUDIT DONE 2026-07-28. The audit found the OPPOSITE problem, and fixing that is what shipped.**
  **No step restates script output.** Grepping the sequencer skill for restate/summarize/report-the-output patterns
  returns nothing, and every script the skill tells the model to run already emits a bounded result:
  `section-checkpoint.py show` 893 B, `preflight.py` 1,497 B, `runner_status.py` 2,956 B, `advance-work.py` 4,265 B
  (its "ONE bounded packet"). `run-outcome.py` is called only by the LAUNCHER, so it is already out of model turns.
  **The premise does not hold either.** Classifying all 4,757 Bash calls: only **154 (3%)** invoke the deterministic
  overnight fleet at all. The traffic is search 15%, build/test 14% (already wrapped by `run-artifact.sh` via the
  `build_offload_reminder` block), text/file utils 14%, inline python 11%, git 6% -- so "the fleet is being
  supplemented by model-side parsing of the same data" has almost no traffic behind it. The 2-3% had no target.
  **What the audit DID find: `section-cost-report.py` has NO CALLER anywhere.** A fully-built per-section cost report
  -- turns, output tokens, cache-read, sidechain share, agents dispatched, cost normalized by changed LOC and per
  shipped commit, with advisory soft-SLO flags (e.g. high turns with zero agent dispatches = offload miss) -- sitting
  unrun. Looking for bookkeeping still done in model turns turned up a finished deterministic report nobody was
  running.
  **Shipped:** wired into `overnight-launch.sh` so every run appends the report with ZERO model cost. Best-effort and
  never fatal -- guarded on the metrics file existing, `timeout 120`, `|| true` -- so a reporting failure cannot affect
  the run-outcome classification that follows it.
  **This is most of T4-1 already built.** T4-1 asks for exactly these numbers per run; it is now largely a matter of
  extending this report (re-read ratio, skill-injection bytes, hook-fire counts, cache-hit rate) rather than building
  reporting from scratch.
  **Acceptance met trivially:** no gate was touched, so none lost evidence; the on-disk artifact remains the source of
  truth for stamps.
  **Checks run:** launcher `bash -n` clean; the block simulated against a real metrics file renders the report;
  fail-safe verified (missing metrics file emits nothing, exits clean); control-plane suite 48/48.
  **Evidence:** `main_tools` shows `bash 1,810` and `other 144`; the deterministic script fleet
  (`preflight.py`, `receipts.py`, `run-outcome.py`, `section-cost-report.py`, `advance-work.py`) already exists and is
  correct -- it is just being supplemented by model-side parsing of the same data.
  **Fix:** audit the runner skill step list for any step where the model reads a script's output and then restates it.
  Each such step becomes: script writes the structured artifact, the model reads only the one-line verdict.
  **Expected saving:** **2-3%**.
  **Acceptance:** no gate loses its evidence; the artifact on disk remains the source of truth for stamps.

---

## T4 -- Measurement, so the savings cannot silently regress

- [x] **T4-1. Put cost in the run report, in the units that matter.**
  **SHIPPED 2026-07-28** as `scripts/overnight/cost-summary.py`, wired into `overnight-launch.sh` so every run ends
  with the table (zero model cost). All eight requested figures are present: bucket table with tokens/cost/share,
  average and peak context/turn, re-read ratio, skill-injection bytes, hook fires, agent dispatches, and agent
  cache-hit rate -- plus the review-convergence ratio that T3-3 made loggable.
  **Acceptance MET by hand.** Every figure reconciles against the raw jsonl, checked independently:
  segments 4, turns 201, main cache-read 50,142,178, sidechain 9,309,295, output 112,887, cache-write 338,175,
  agents 2, avg context/turn 249,463, peak 354,291, cache-read cost $75.21 -- identical from both paths.
  **It reproduces the finding it exists to surface:** on a real run, cache-read is **82%** of cost (69.3% main +
  12.9% sidechain) against 7.8% output. That is the number whose absence let the backlog optimize the 10% slice.
  **Scoping is the load-bearing design choice.** Two of four sources (`tool-history.jsonl`, `offload-events.jsonl`)
  are append-only across runs with no run id, so they are filtered to this run's WINDOW -- start parsed from the
  metrics filename `run-YYYYMMDD-HHMMSS`, end from its mtime. Verified isolating: 49 in-window Reads out of 9,437
  total events. When the filename carries no stamp the rows are OMITTED with an explicit note, never reported as
  all-time: a per-run table that quietly mixes in an all-time count reads as authoritative, and that exact
  double-count is what inflated two backlog items (T1-5, T2-4).
  **Dollars are labelled as list-rate RELATIVE sizing, not a bill**, with sidechain priced at Opus as an upper bound.
  **Checks run:** 4 contract tests (reconciliation, cache-read ranked first, unscopable-run omission, fail-open on
  missing/malformed); launcher `bash -n` clean; control-plane suite 49/49; lint 0 errors.
  **What this unblocks:** T4-3 can now compare a before/after run instead of arguing from estimates -- and several
  estimates in this file are already known wrong (T1-2 ~5x, T1-5 double-counted, T2-3 ~25%, T2-4 ~30x).
  **Evidence:** `metrics-report.py` and `section-cost-report.py` exist, but nothing surfaces the cache-read-dominance
  finding -- which is why the backlog to date optimized the 10% slice.
  **Fix:** every run report ends with: total cache-read, average context/turn, peak context/turn, re-read ratio,
  skill-injection bytes, hook-fire count, agent dispatch count and cache-hit rate, and an estimated dollar figure with
  the per-bucket split. One table, same shape every run.
  **Acceptance:** the numbers reconcile with `.claude/overnight/metrics/*.jsonl` when checked by hand.

- [/] **T4-2. Regression gate on the context metrics.**
  **MOVED 2026-07-28 to [`token-saver-v02.md`](token-saver-v02.md) "Migrated from v01", unchanged, when this file was CLOSED.** Remains DEFERRED there on the context-growth investigation -- its precondition (a baseline worth defending) is still unmet.

- [x] **T4-3. Measure one section end-to-end before and after, and publish the delta.**
  **DONE 2026-07-28. The delta is published as the POSTSCRIPT at the end of this file. The verdict clause fired: predictions were off by ~11x and ~26x against a 2x threshold, so the model of where cost goes IS wrong and this file needs re-deriving before more items land.** Measured across whole runs rather than a single replayed section (see the postscript's stated confound); the magnitude is far past anything section variance explains.
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
concurrent-leg work in `todo/overnight-runner-improvements/overnight-runner-improvements-v01.md` rather than being scheduled on its own.

---

## Never cut (the quality floor these items must not touch)

Every item above was scoped to leave the following completely intact. If an implementation of any item requires touching
one of these, the item is wrong and gets re-scoped, not the floor.

- **The Codex review pipeline.** Adversarial, consistency, perf, design, re-adversarial, and the fix loop stay as-is.
  Nothing here caps review rounds (already evaluated and rejected in `todo/overnight-runner-improvements/overnight-runner-improvements-v01.md`).
- **`kernel-quality-auditor` stays on Opus.** It is the sole SMP / lock-order / bare-metal net for the repo's most
  expensive bug class.
- **The full unit suite + smoke test at every section boundary**, and the CI-parity pre-push gate. Intra-loop runs narrow;
  boundary runs do not.
- **The trust contract.** Load-bearing findings are verified by the main session at file:line. Verification-before-
  completion quotes come from the main session's own read of the on-disk artifact.
- **Every blocking hook gate**: `section_commit_gate`, `run_phase_guard`, `review_dispatch_gate`, `broker_dispatch_required`,
  `codex_review_completed`, `runner_bash_guard`, the test-policy and code-style blocks. T1-4 touches advisories only.
- **Bare-metal-first validation.** No item here trades a hardware-truth check for a cheaper proxy.

---

## POSTSCRIPT -- T4-3 measured delta (2026-07-28)

T4-3 required this published "with the raw numbers, whatever they say". They are not good.

|                      | pre-T1 (2026-07-25/26, 4 runs, 984 turns) | post-T1 (2026-07-28 canary, 3 segments, 897 turns) | predicted      |
| -------------------- | ----------------------------------------- | -------------------------------------------------- | -------------- |
| avg context/turn     | 311,479                                   | 299,988 (**-3.7%**)                                | 180,000 (-42%) |
| re-read ratio        | 132/206 = **64%**                         | 106/170 = **62%** (-2pp)                           | 20% (-44pp)    |
| hook fires / segment | ~1,609 (2026-07-10 measurement)           | 12 / 26 / 73                                       | <= 400         |

**Prediction error: ~11x on context per turn, ~26x on re-read ratio.** T4-3's threshold was 2x, so its verdict clause applies without argument: the model of where cost goes is wrong, and the remaining items in this file must be re-derived before any more of them land.

**What actually delivered:** hook-fire budgeting (T1-4), and decisively -- from ~1,609 fires to 12-73 per segment. That item's premise was correct and its mechanism worked.

**What did not:** the read-side items. `read_cache_block` fired 26 times across the canary run and moved the re-read ratio by 2 percentage points. `agent_result_cache` recorded 5 stores and 0 hits, the same zero-hit symptom T2-2 was written to fix. T2-3 was reverted outright as negative (see its entry). So the read/context half of this backlog projected ~40% and returned ~4%.

**Why the projections were wrong is itself unresolved**, and that is the finding worth carrying forward. The estimates assumed re-reads and searches were the dominant repeated cost. The canary data says per-turn context is dominated by something else: context/turn RISES across rollover segments (248K -> 294K -> 347K) when rollover exists to reset it. Until that mechanism is understood, any further estimate in this file is a guess with a known ~10x error bar. Owner: [`token-saver-v02.md`](token-saver-v02.md) "Context per turn RISES across rollover segments".

**Confound, stated plainly:** this is a whole-run before/after, not the single-section replay T4-3 literally specified. The two windows worked different TODO sections, and section difficulty affects context. It is published anyway because a 11-26x gap is far outside what section variance explains, and because a controlled replay would cost another full run at ~$500 to sharpen a number whose direction is already unambiguous. Pre-T1 is 4 runs and 984 turns, not a single sample.

**Cost, for scale:** the 2026-07-28 canary run cost **$509** across three segments (list-rate arithmetic, ~82% of it cache-read).

**DID IT ACTUALLY SAVE MONEY? Per turn yes, per unit of work shipped no -- and the second number is the one that matters.**

|                                     | pre-T1 (2026-07-25/26)   | post-T1 (2026-07-28)   |
| ----------------------------------- | ------------------------ | ---------------------- |
| total                               | $906.94 over 1,373 turns | $509.01 over 897 turns |
| cost per turn                       | $0.661                   | **$0.567 (-14.1%)**    |
| commits flipping an IO row to `[x]` | 4                        | 1                      |
| cost per section shipped            | **~$227**                | **~$509 (+124%)**      |

The per-turn improvement is real and reasonably sampled. The per-section figure is NOT a safe conclusion and is recorded with its confounds rather than as a result: n=1 on the post side; section 20 consumed most of segment 3 (~$220) and shipped nothing, so the post window is charged for work-in-progress the pre window is not; sections 19 and 20 are unusually hard SMP-lifetime bugs (7 and 9+ Codex dispatches) rather than average sections; and an operator was working in the same tree, causing four index cross-sweeps plus turns the runner spent diagnosing HEAD movement.

What is NOT in doubt: the read/context half of this backlog projected roughly 40% and returned roughly 4%. Whether cheaper turns translate into cheaper WORK depends on turns-per-section, which this sample cannot settle. Settling it needs a post-fix run on comparable sections with no operator in the tree -- which is the measurement the v02 context-growth item should produce as a side effect.
