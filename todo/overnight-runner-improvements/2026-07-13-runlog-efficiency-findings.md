# Overnight Runner -- 2026-07-13 Runlog Efficiency + Fix Findings

> **New findings** from the 2026-07-12 evening through 2026-07-13 midday run family (7 substantive
> runlogs, ~740 KB: `run-20260712-223931` .. `run-20260713-082218`), digested by three parallel
> `overnight-log-explorer` passes. This file is the **de-duplicated complement** to
> [`2026-07-12-runner-24-7-implementation-plan.md`](./2026-07-12-runner-24-7-implementation-plan.md)
> (the P0-P6 cost plan) and the parent backlog
> [`../overnight-runner-improvements.md`](../overnight-runner-improvements.md). Every item here was
> checked against the P0-P6 list and is a NEW pattern or a distinct facet -- none re-proposes a
> P-item. Same naming discipline as the sibling plan (not `TODO-*.md`, not in a `todo/NN-domain/`
> dir) so the sequencer and todo-graph never parse it.

## Relationship to the 2026-07-12 plan (read first -- these are additive, not contradictory)

- The 07-12 plan's cost thesis (cost = context-size x turn-count; churn from review-receipt
  content-binding P1.2/P1.3 and review convergence P2.1/P2.2) stands. Nothing here revisits it.
- These findings are the **mechanical papercuts and concrete bugs layered ON TOP of** that known
  root cause -- they shrink per-section wall-clock and tool-call count independent of any
  Codex-convergence fix. The 07-13 §-1 section (TODO-22 env storage) spent ~108 min; ~50 duplicated
  exploration calls + ~15-30 line-length recount calls sit on top of the known review-round churn.
- Where an item is adjacent to a P-item, the adjacency is called out inline (e.g. A1 vs the
  codex_review_completed mis-fire; E3/F1 vs P1.2; D1-D3 vs P3.4 offload enforcement). No item
  weakens or reverses a P-item.
- **Gate legend reused from the sibling plan:** `[det]` ships on a green deterministic suite
  (`scripts/overnight/tests/run-all.sh`) + its own new test, no watched canary; `[canary]` alters
  live model-driven flow and needs one watched attended run before an unattended arm.

## Confidence tiering

- **Confirmed at file:line THIS session** (main-session verification, trust contract): A1, C1.
- **Corroborated across all three independent digests** (very high signal): B1, C2 (line-length +
  bare-section-ref edit-retry cascade), the agent-offload-discipline cluster (D-cluster).
- **Single-digest, evidence-anchored** (verify at the cited log line before implementing): the rest.

---

## A. Confirmed code bugs -- ship a fix + regression test

- [x] **[det] A1. `worktree_hash` import path is broken in BOTH review hooks -- the deterministic
  diff-facts fast-path silently never runs.** FIXED 2026-07-13: `.parent.parent` -> `.parent.parent.parent`
  (repo root) in `review_dispatch_gate.py:149` AND `agent_dispatch_required.py:112` (both instances -- the
  only two in the tree). Regression test `test_hook_worktree_import.py` EXTRACTS each hook's actual `.parent`
  chain from source and asserts it resolves to a real `scripts/overnight` containing `worktree_hash.py` + that
  `worktree_key` imports -- so a regression back to `.parent.parent` is caught here, not silently on a live
  run (verified: the old depth resolves to the nonexistent `.claude/scripts/overnight`). Suite 28/28. Original
  finding follows:
  VERIFIED this session.
  `review_dispatch_gate.py:149-151` and `agent_dispatch_required.py:112-114` both build the import
  path as `Path(__file__).resolve().parent.parent / "scripts/overnight"`. For a hook at
  `.claude/hooks/<name>.py`, `.parent.parent` is `.claude/`, so the path resolves to
  `.claude/scripts/overnight` -- **which does not exist** (real file:
  `scripts/overnight/worktree_hash.py`). The `from worktree_hash import worktree_key` therefore
  raises, is swallowed by `except Exception: return False`, and the deterministic
  `_fresh_diff_facts` / diff-facts check ALWAYS reports "not fresh." Net cost: every stamp/kernel
  edit that has a valid fresh diff-facts receipt STILL gets forced through a full
  `review-evidence-mapper` agent dispatch (observed live at `run-20260713-044429.log:494-517`,
  where the runner hand-diagnosed it and wrote a live-gotcha note but did NOT fix the code).
  **Fix:** change `.parent.parent` -> `.parent.parent.parent` (repo root) in BOTH hooks; add a unit
  test that imports `worktree_key` through each hook's path-construction and asserts it resolves.
  This is a different script and a literal wrong-directory bug -- distinct from the known
  `codex_review_completed.py` recorder mis-fire (P1.2/P1.3 field note). Recurs on every gated edit
  until fixed. **Highest value / lowest risk item in this file.**

## B. Poll / wait mechanics

- [x] **[det] B1. The taught Codex-verdict poll loop out-runs the Bash tool's own 2-minute default
  timeout, so the FIRST review poll of nearly every session dies at 2m and is re-learned from
  scratch.** FIXED 2026-07-13: shipped `scripts/overnight/wait-for-codex-verdict.sh [--max SECONDS]
  <logfile>...` -- it bounds EACH invocation to ~100s (default), safely under the Bash tool's ~120s
  default, so a BARE call is never killed and no `timeout:` arg must be remembered (exit 0 = DONE +
  verdict tail; exit 3 = STILL RUNNING -> re-invoke; accepts multiple logs to wait a whole multi-kind
  round in one call). Replaced the hand-rolled `for i in $(seq 1 N); do ... sleep S; done` snippet in
  the canonical wait-discipline (`codex-design-review` SKILL, referenced by every codex-* skill), the
  `overnight-sequencer` SKILL poll block, and the runner doctrine (`TODO-Claude-Overnight-Runner.md`).
  Tests: `test_wait_for_codex_verdict.py` (sentinel->DONE, no-sentinel->STILL-RUNNING, missing-log,
  multi-log-waits-for-all, bounded-wait-never-overshoots-max, usage) + updated `test_runner_lifecycle.py`
  to pin the new waiter (was pinning the deprecated `seq 1 9`/`sleep 60` loop). Suite 29/29. Chose the
  self-bounding SCRIPT over "just pass timeout: 300000 in SKILL.md" because the `timeout:` is a Bash-TOOL
  arg (not part of the command string), which is exactly what the model kept forgetting session to
  session -- the script needs no such memory. Distinct from P1.3. Original finding follows:
  Corroborated across all three digests: ~9 `Exit code 143 / timed out after 2m 0s`
  kills (e.g. `run-20260712-223931.log:220`, `run-20260712-231031.log:240`,
  `run-20260713-020113.log:62`, `run-20260713-025424.log:188`, `run-20260713-044429.log:62`,
  `run-20260713-055754.log:175/179`, `run-20260713-082218.log:89/638`). The SKILL.md pattern
  `for i in $(seq 1 9); do grep -q "Turn completed" <log> && break; sleep 60; done` is 9x60 = 540s
  but is issued WITHOUT an explicit `timeout:` arg, so the Bash tool's ~120s default kills it,
  losing the elapsed wait; the lesson does not persist across the rollover/session boundary.
  **Fix:** ship a canonical `scripts/overnight/wait-for-codex-verdict.sh <logfile>` that internally
  chunks its sleep so each Bash invocation finishes inside ~110s (loop-and-return, caller re-invokes
  if unfinished), OR make the poll snippet in SKILL.md always pass `timeout: 300000` explicitly.
  Removes a guaranteed-per-session wasted call + recovery turn. Distinct from P1.3 (broker/receipt
  shape) -- this is the interactive Bash tool's own default ceiling, not the review dispatch.
- [ ] **[det] B2. The waiter's BINARY still-running signal cannot distinguish a slow-but-ALIVE review
  from a hung one, so the runner abandons + re-dispatches a live review.** SURFACED LIVE in the
  2026-07-13 watched canary (branch `overnight-runner-improvements-2026-07-13`, run-20260713-205030):
  an adversarial review of `src/kernel/nt/nt_env.c` ran ~14 min and was genuinely ALIVE (the `.out`
  showed Codex still running `rg` + "collaboration tool: wait", 0 `Turn completed`, NO crash marker).
  `wait-for-codex-verdict.sh` (B1) correctly returned STILL RUNNING every ~100s cycle, but because its
  only signal is "sentinel present vs not" the runner could not tell "still writing -> keep waiting"
  from "hung -> re-dispatch"; at ~14 min it manually grepped for crash markers (found none) then
  RE-DISPATCHED a fresh adversarial leg (`20260713-210842`), abandoning the in-flight one. Not a wedge
  (clean recovery), but wasted a ~14-min review + its tokens. **Fix:** on each STILL-RUNNING return
  have the waiter print elapsed wait + the `.out`'s last-mtime / byte-growth (last activity), and add
  an explicit STALE verdict (exit 4?) when the `.out` has not grown for M minutes -- so the caller
  re-dispatches ONLY a genuinely-silent review and keeps waiting on one still producing output. This
  is the SEMANTICS of the signal; distinct from B1 (the 2-min-kill of the poll CALL itself, fixed).
  (This is the canary doing its job: a real watched run surfaced a signal-quality gap the deterministic
  tests could not.)
- [ ] **[det] B3. The review-gate STILL forces `SKIP_REVIEW_HOOK` on broker-reviewed and find-and-fix
  sections -- P1.2/P1.3 did not close it.** SURFACED LIVE in the same 2026-07-13 watched canary: landing
  TODO-22 §5 required TWO honest `SKIP_REVIEW_HOOK` opt-outs (each ALSO needing `SKIP_SKILL_STEP_BLOCK`),
  via TWO distinct content-binding/attribution mechanisms neither P1.2 nor P1.3 patched:
  1. **Broker-attribution gap (section commit, ~21:39).** Step-5 adversarial + step-8 consistency/perf
     were dispatched through the overnight review-broker (doctrine-mandated for unattended runs). P1.3
     taught the RECORDER (`codex_review_completed.py`) to recognize the broker, but the ATTRIBUTION
     tagger `skill_step_observer.py` was NOT updated -- so the four verdicts land on disk under
     `.claude/overnight/reviews` but the section-commit four-dispatch gate cannot attribute them
     (`trigger_files` stays empty) -> BLOCK -> forced opt-out. **Fix:** teach `skill_step_observer.py`
     the broker dispatch shape (mirror the `is_review_broker_dispatch()` recognition P1.3 added to
     `_codex_dispatch.py`) so broker legs populate the dispatch attribution + `trigger_files`.
  2. **Post-fix source-drift gap (review-stamp commit, ~22:23).** When the adversarial review
     finds+fixes a bug (here a real NULL-deref in the empty-value query path), the committed source
     drifts from the ORIGINALLY-reviewed blob. The gate binds to the FIRST review's `trigger_blobs`;
     even though the post-fix re-adversarial re-reviewed the fix diff and approved (`Verdict: approve`,
     no material findings), the gate still binds to the first review, sees drift, and blocks -> forced
     opt-out. **Fix:** on a find-and-fix cycle, re-bind the gate's content anchor to the re-adversarial's
     approved diff (record the re-adversarial's covered blobs as the authoritative `trigger_blobs`), so
     an approved re-review supersedes the stale first-review binding.
  Net: the honest-opt-out escape valve worked both times (accurate, detailed reasons; run NOT wedged),
  but the review-gate did not hit its design goal of eliminating SKIP churn -- it forced two bypasses in
  one section. This is the exact churn the P1.2/P1.3 work targeted, via two facets it did not cover.
  Distinct from B2 (waiter signal quality) and from the original field observation (single-record
  `received` reset, which P1.2's history-fallback DOES rescue -- these two do not, because `received`
  is True but the binding/attribution is empty or stale).
- [ ] **[det] B4. Runner-generated `coverage.*` dirties the tree at rollover, and the dirty-tree
  diagnostic mis-attributes ownership.** SURFACED LIVE in the 2026-07-13 canary: two `run_phase_guard.py
  rollover` attempts refused because the full-suite run regenerated `coverage.json`/`coverage.md`
  (20671->20680 tests from §5), which dirties the tree and invalidates the content-bound rollover
  receipts -- so a routine test run blocks the rollover until those auto-gen artifacts are committed. On
  the second refusal the runner MIS-ATTRIBUTED them to "the operator live-editing the canary-log files"
  (the operator's findings commit had already landed ~15 min earlier; no operator edit was in flight),
  self-correcting in ~30s after diffing the actual paths. **Fix:** (a) commit (or receipt-scope) runner-
  generated `coverage.*` in the rollover receipt step BEFORE the clean-tree check, so a test run does not
  block rollover; (b) make the dirty-tree diagnostic name the actual paths + probable owner (auto-gen vs
  tracked-source vs untracked) instead of guessing "operator." Low severity, self-corrected; filed so the
  recurring coverage-dirties-tree friction + the operator/runner file-ownership ambiguity is owned.
  Distinct from B3 (review-gate binding) -- this is the rollover clean-tree gate + its diagnostics.

## C. Hook-block ergonomics + edit-retry churn

- [x] **[det] C1. The 250-char line-length hook actively RECOMMENDS the wasteful manual recount
  loop.** FIXED 2026-07-13: added `python3 .claude/hooks/todo_item_line_length.py --check "<line>"` ->
  one JSON line `{len, cap, overage, is_checklist_item, whitelisted, ok}` (Commit-whitelist aware), and
  REPLACED the block message's `python3 -c 'print(len(...))'` advice with a pointer to `--check` (one call
  confirms fit, no blind recount loop). Test `test_todo_item_line_check.py` (fit query, Commit-whitelist,
  non-item, and block-message asserts `--check` present + the old `print(len(` advice gone). Original finding:
  VERIFIED: `todo_item_line_length.py:65-66` already reports the exact overage ("over by
  N"), but `:78-79` then instructs the model to `python3 -c 'print(len("<the exact line>"))'` before
  every retry. The model obeys literally -- ~30 dedicated recount Bash calls in
  `run-20260713-082218.log` alone (e.g. `:791-802`, `:851-855`) against only ~9 actual BLOCKs.
  **Fix:** add a deterministic companion query mode `python3 .claude/hooks/todo_item_line_length.py
  --check "<candidate line>"` returning `{len, overage, cap}` in one call, and REPLACE the
  hand-`len()` advice in the block text with a pointer to it. One call confirms fit instead of a
  3-6-round blind trim. (The cap mechanism itself is known/shipped -- this is the retry ERGONOMICS.)
- [x] **[det] C2. PreToolUse Edit BLOCKs cascade into "String to replace not found" because the
  retry assumes the blocked edit partially landed.** FIXED 2026-07-13 (message half): all three
  content gates -- `todo_item_line_length.py`, `bare_section_refs.py`, `citation_block.py` -- now state in
  their BLOCK message "this Edit did NOT apply; the file is UNCHANGED -> retry with the SAME old_string, only
  fix new_string (re-Reading and reconstructing old_string is what causes the cascade)." Each gate already
  names the exact offending line(s) (the span). The "retry re-reads on-disk state" behavior is a
  model-discipline consequence of that message, not a separate mechanical gate. Covered by the block-message
  assertions in `test_todo_item_line_check.py` + `test_content_lint.py`. Original finding:
  Corroborated: ~9 `String to replace not found`
  errors across the 07-13 pair (`run-20260713-055754.log:147,153,168,388`;
  `run-20260713-082218.log:122,152,558,577,611`) and matching pairs in the 07-12 pair. A blocked
  Edit (line-length / `bare_section_refs` / `citation_block`) leaves the file unchanged, but the
  next Edit's `old_string` is built against the never-applied text (or drifted multi-line
  C-comment content), so it misses -> forces a fresh Read+Grep+reconstructed Edit. One case
  (`082218.log:577`) was the Edit tool's own `\uXXXX` normalization on an emoji table cell.
  **Fix:** on a hook-blocked Edit, the retry MUST re-read current on-disk state before constructing
  the replacement; and have `bare_section_refs` / `todo_item_line_length` return the exact
  character span of the offending region so the retry patches just that span.
- [x] **[det] C3. The first Write of a new kernel/boot file walks a 5-hook sequential gauntlet, each
  hook revealing the next only after the prior is fixed.** FIXED 2026-07-13 (the CONTENT half): extracted
  the two source-comment detectors into a shared `.claude/hooks/_content_lint.py` (single source of truth,
  applicability-aware; mirrors lint Check 5/13), refactored `bare_section_refs.py` + `citation_block.py` to
  use it, and each gate now CROSS-REPORTS the other's findings in its block ("ALSO (citation gate / bare
  section-ref gate, fix in the SAME edit): ..."), so one Write surfaces BOTH content classes at once instead
  of two sequential block-then-fix cycles. Scope note: the 3 SKILL/workflow-prerequisite gates in the gauntlet
  (`step5_quality_gate` -> `receiving_review_required` -> `agent_dispatch_required`) genuinely require
  sequential resolution (they gate on workflow state, not editable content) and cannot be content-preflighted;
  the content collapse is the addressable half. `test_content_lint.py` (detector applicability, both gates
  block/exempt/opt-out, both cross-reports). Gate behavior verified identical to pre-refactor via the tests +
  `scripts/lint.sh` (0 errors; Check 5/13 pass). Original finding:
  `run-20260713-025424.log:93-157`
  (~9 min for one header): `step5_quality_gate` -> `receiving_review_required` ->
  `agent_dispatch_required` -> `bare_section_refs` -> `citation_block` -> success. Each hook is
  individually correct; the waste is six block-then-retry cycles instead of one.
  **Fix:** a single pre-flight lint pass (bare-section-refs + citation-attribution +
  skill-prerequisite check) run once before the first Write on a new kernel/boot file, returning ALL
  violations at once.

## D. Agent-offload discipline (complements P3.4, which promotes offload reminders WARN -> BLOCK)

> P3.4 makes existing offload reminders BITE. This cluster is about THREE different offload failures
> P3.4 does not address: racing an in-flight dispatch, never dispatching at all, and narrating a
> dispatch that never fired.

- [x] **[det] D1. Inline exploration races/duplicates an in-flight `kernel-explorer` dispatch,
  producing ~50-55 net-zero tool calls.** FIXED 2026-07-13: new `inflight_race_guard.py` records the
  source-file set an in-flight EXPLORATORY agent was dispatched to map (record on PreToolUse Task/Agent, clear
  on SubagentStop, 15-min TTL backstop), and on the FIRST Grep/Read whose path/pattern overlaps a mapped file
  BASENAME injects a one-line "you are racing the in-flight <agent>" reminder (block on it, or do
  NON-overlapping work). Overlap-by-basename keeps false positives low: it stays SILENT on non-overlapping
  work (the correct parallel pattern -- a couple of spot-checks while legs run) and on non-exploratory
  dispatches; warns ONCE per dispatch. Wired PreToolUse(Task|Agent) / PostToolUse(Grep|Read) / SubagentStop;
  MANIFEST row added. Test `test_inflight_race_guard.py` (overlap-warns-once, non-overlap-silent,
  non-exploratory-no-arm, SubagentStop-clears). Complements P3.4 (which makes reminders BITE). Original:
  Twice a kernel-explorer was dispatched for an
  integration-surface question, then the main session ran 20-30 of its OWN Grep/Read calls over the
  identical surface while "waiting," so the agent's report only reconfirmed already-covered ground
  (`run-20260713-025424.log:113-143`, `run-20260713-044429.log:48-106`; the correct contrast is
  `044429.log:353-358` -- 2 targeted spot-checks while legs run in parallel). Also the
  split-then-re-split rework at `run-20260712-223931.log:125-202`, where §15's item list was edited,
  then re-cut, because mutating edits proceeded before the in-flight explorer's findings arrived.
  **Fix:** after dispatching an exploratory agent, restrict main-session calls before it returns to
  NON-overlapping work (unrelated design/scaffolding), or simply block on the dispatch instead of
  racing it. A reminder/gate keyed on "an Agent dispatch is in flight AND the main session is
  issuing Grep/Read on the same file set."
- [x] **[det] D2. Step-3 kernel exploration is dispatched only REACTIVELY (from an
  `agent_dispatch_required` BLOCK), never proactively -- so the runner does 5-10 rounds of manual
  grep/Read before ever reaching it.** FIXED 2026-07-13, two levers: (1) extended
  `interactive_offload_router.py` with a helper/symbol-LOCATION route -- "where do the X helpers live / is
  there a Y primitive / do we have a Z allocator" now routes to `kernel-explorer` at prompt time (was only
  "map the integration surface" / "trace"); (2) sharpened `implement-todo-section` step 3 with the D2
  discipline: do NOT hand-grep to orient BEFORE the pack/explorer -- a helper-location question is answered by
  the pack's symbol DEFINITION resolution or ONE explorer dispatch, never a 3-6-round inline grep hunt; run
  the pack (and explorer if the surface is ambiguous) FIRST for ABI-impacting kernel/boot sections, and drift
  into inline fan-out trips `inline_churn_monitor` + `inflight_race_guard`. Test `test_offload_router.py`
  (helper-hunt -> explorer, ordinary prompt no false-fire, existing routes intact). Original:
  `run-20260713-055754.log` + `082218.log`: 0 proactive
  `kernel-explorer`/`section-context-mapper` dispatches despite both sections being pure kernel
  surface; `055754.log:39-46` and `082218.log:43-155` are exactly the 3+-round inline shape the
  routing table assigns to that agent. Also the 6-round inline string-helper hunt at
  `run-20260713-025424.log:46-56` (same session that correctly offloaded 10 min later).
  **Fix:** enforce/hint the step-3 exploration-agent dispatch for ABI-impacting kernel sections
  BEFORE `implement-todo-section` step 3 hand-explores; extend `interactive_offload_router.py` to
  trigger on "where do existing X helpers live" queries, not just "map the integration surface."
- [x] **[det] D3. An announced parallel dispatch silently never fired.**
  FIXED 2026-07-13 as DOCTRINE (the honest scope): hooks see tool calls + the USER prompt, NOT the model's
  own narration mid-turn, so a purely mechanical "you said X and Y but issued only X" check is not cleanly
  implementable with the available hook surface. Instead, `review-todo-section` step 7 (the exact site of the
  miss) now carries the D3 rule: when dispatching the SMP auditor + concurrency-evidence-mapper in parallel,
  issue BOTH `Agent` calls in ONE message (multiple tool_use blocks, per `superpowers:dispatching-parallel-
  agents`), NEVER narrate "kick off X and Y" then issue only one, and BEFORE triaging confirm you hold BOTH
  results -- with the §17 lock-semantics-regression consequence spelled out so the cost is concrete. A hard
  mechanical gate would also need a reliable "SMP-heavy section" trigger (a judgment call), which is why the
  doctrine + the D1 `inflight_race_guard` (catches the racing that the missing mapper's absence enabled) are
  the pragmatic net. Original:
  `run-20260712-231031.log:771` narrates "kick off kernel-quality-auditor + concurrency-evidence-
  mapper in parallel" but only `kernel-quality-auditor` is dispatched (`:772`);
  `concurrency-evidence-mapper` never appears. §17 was exactly the SMP-heavy section that routes to
  that mapper, and it went on to suffer the live lock-semantics regression (G1) a standing lock/
  atomic inventory might have pre-flagged. **Fix:** when a skill step narrates "dispatching X and Y
  in parallel," a mechanical post-check asserts both Agent-dispatch lines fired before proceeding,
  or the narration is dropped rather than left descriptive.

## E. Codex dispatch / artifact robustness

- [x] **[det] E1. A single-quoted Codex dispatch prompt is broken by a literal apostrophe in
  natural-English prose.** FIXED 2026-07-13. Root scope: the break happens in the CALLER's shell BEFORE
  `codex-dispatch.sh` runs, so the wrapper cannot pre-scan it (and advising on a RECEIVED apostrophe would
  false-fire on the safe heredoc form, which legitimately contains apostrophes); a `--file`/stdin mode would
  break hook attribution (the `[review-kind:]` marker must stay on the command line for `_review_kind` /
  `codex_review_completed`). So the mechanical net is at the SOURCE (mirroring the existing escaping doctrine):
  extended `scripts/lint.sh` Check 12 to flag a single-quoted dispatch body with an INNER apostrophe that
  closes the quote early (the `exec'd` shape -- apostrophe immediately followed by an alnum, after removing the
  valid `'\''` escape). Conservative: string concatenation, comments, and escaped apostrophes do NOT trip it
  (unit-verified). The wrapper + CLAUDE.md already document the heredoc form as the apostrophe-safe shape; this
  makes a documented single-quoted-with-apostrophe example fail lint instead of a live dispatch. Full lint
  clean on the tree (0 new warnings). Original finding:
  `run-20260712-231031.log:437-439`: a prompt body containing "exec'd"
  produces `/bin/bash: eval: line 41: syntax error near unexpected token '('`, forcing a full
  redispatch with the contraction removed. This is a DIFFERENT failure mode from the documented
  double-quote/`$()` escaping doctrine (that guards expansion in double quotes) -- here the
  single-quote wrapper itself cannot contain a literal `'`. **Fix:** pre-scan dispatch prompt bodies
  for a bare `'` in `codex-dispatch.sh` / `review-broker-codex-dispatch.sh` and reject/normalize
  before shelling out, or switch apostrophe-bearing prompts to `$'...'` with `\'` escaping.
- [x] **[det] E2. The full Codex review body is not reliably persisted to the per-dispatch `.out`
  artifact, forcing a glob over `~/.codex/sessions/**/*.jsonl` that twice returned an unrelated,
  stale session.** FIXED 2026-07-13 (doctrine, the in-our-control fix). Investigated the premise:
  `codex-companion.mjs` is a VENDORED marketplace plugin (do not modify) that DOES write the final review to
  stdout, so the `.out` already carries the verdict + severity findings + Next-steps (verified on a real
  adversarial `.out`); the only truncation is intermediate reasoning log lines, not the verdict. And the
  Codex thread-id printed in the `.out` does NOT map to a session filename (verified: `find` by thread-id
  returns nothing), which is exactly why the glob returned a FOREIGN session (a Conclave run). So the correct
  fix is NOT to parse the non-deterministic session store: the `.out` (+ `review-envelope.py`) IS the
  authoritative review body, and the glob is banned. Wired into the canonical wait-discipline
  (`codex-design-review` SKILL, referenced by every codex-* skill) and the `overnight-sequencer` SKILL: "NEVER
  glob `~/.codex/sessions/**/*.jsonl`; slice-read the `.out` for detail." Original finding:
  `run-20260713-044429.log:190-196` (design) and `:285-288` (adversarial, pulled an
  unrelated Conclave run) -- only the truncated "Next steps" summary reached the `.out` path, so the
  runner globbed the raw session store and risked misreading a foreign session as the real verdict.
  **Fix:** the dispatch wrapper always persists the FULL review body to the same deterministic
  per-dispatch `.out` path the runner already polls, so `~/.codex/sessions/` is never globbed.
- [x] **[det] E3. `review-envelope.py` returns cross-session STALE content for the in-session
  self-check, so the runner reads it, distrusts it, and falls back to a raw `.out` read every time.**
  FIXED 2026-07-13 (real code fix). Added `--todo <path>` and `--since <epoch>` scope filters to
  `review-envelope.py`: the broker manifest is append-only across sections AND runs, so unscoped
  "newest-per-kind" pulls a PRIOR section's review (measured: a TODO-22 self-check aggregating stale TODO-12
  reviews). `--todo` restricts to the manifest entries whose `todo` matches the section under review; `--since`
  restricts to this dispatch round. Both default off (back-compat). Wired `--todo` into the `overnight-
  sequencer` SKILL invocation as REQUIRED, and the doctrine into the canonical wait-discipline. Tests: 2 new
  cases in `test_review_envelope.py` (todo-scope pulls the CORRECT section not the newest; --since window
  excludes a prior round). Distinct from P1.2 (that binds the COMMIT-gate's `last-codex-review.json` to the
  staged diff; this scopes the in-session envelope AGGREGATOR). Original finding:
  `run-20260712-223931.log:226-228`, `run-20260712-231031.log:244-246`, `:808` -- three wasted
  envelope reads, each aggregating a prior run's reviews (e.g. "stale morning reviews TODO-12 §13").
  This is a DIFFERENT consumer than P1.2 (which binds the commit gate's `last-codex-review.json` to
  the staged diff) -- this is the in-session self-check aggregator scoping too wide. **Fix:** scope
  `review-envelope.py` to the current section's `.claude/overnight/reviews/<timestamp>-*.out` files
  only (or stop calling it during synchronous single-review polls and read the `.out` directly).

## F. Gate / commit-attribution flow

- [ ] **[canary] F1. The section-commit gate attributes the commit to the FIRST staged `.md`, not
  the active section's target file.** `run-20260713-025424.log:317-329`: with TODO-22 (valid stamp)
  and TODO-12/TODO-07 (stale stamps, touched only for reciprocal XREF edits) staged together,
  `_attribute_review_todo` picked a wrong file and blocked, forcing `git restore --staged` of the
  XREF files, a solo TODO-22 commit, then a separate `todo:` XREF commit. Distinct from P1.2
  (content binding) -- this is FILE-SELECTION logic. **Fix:** prefer the file named in the active
  section-commit context (the TODO path passed to `implement-todo-section`) over "first staged .md,"
  falling back to first-staged only when there is no active section context. Live-flow -- canary.
- [ ] **[det] F2. Rollover receipt re-record churn caused by the suite's own coverage-doc
  auto-refresh.** `run-20260713-055754.log:503-519`: rollover records rebuild+suite+smoke receipts;
  running the suite auto-dirties `docs/test-coverage/coverage.md`; that forces a separate docs-only
  commit+push; which forces re-recording all three receipts against the new HEAD even though no
  source changed. **Fix:** make the rollover receipt check tolerant of a coverage-doc-only tree
  delta, or auto-fold the coverage-doc commit into the same commit the receipts bind to.
- [x] **[det] F3. `SKIP_AGENT_DISPATCH_HOOK` cannot gate a subsequent Edit-tool call, but the runner
  tried to use it that way.** `run-20260713-055754.log:198-206`: the runner ran
  `SKIP_AGENT_DISPATCH_HOOK=1 true # reason: ...` as a standalone Bash command hoping the env var
  would carry to the NEXT Edit's PreToolUse hook -- it can't (separate process, no shared env),
  costing ~71s + a section-pack-refresh dead end before a real Agent dispatch cleared the gate.
  Distinct from the memory-noted env-prefix ORDERING quirk (`skill-step-block-env-prefix`) -- this is
  cross-tool-call env NON-propagation. **Fix:** document that `SKIP_AGENT_DISPATCH_HOOK` only works
  when prefixed on the gated command itself and cannot pre-clear a later Edit; route the
  `agent_dispatch_required` gate to a documented, working satisfier.
- [x] **[det] F4. `review_round_guard.py` fails on bare invocation, rediscovered per session.**
  `run-20260713-025424.log:270-271` ("Round-guard needs python3 invocation"). Low severity.
  **Fix:** add a shebang + `chmod +x`, or document the required `python3` prefix once in the skill.
- [x] **[doctrine] F5. `SKIP_REVIEW_HOOK` was used outside its documented "revert / stamp-only"
  scope for a post-review fix implementing the reviewer's OWN recommendation.**
  `run-20260713-082218.log:667-672`: gated a substantive one-line `ARGV_FRAME_RESERVE` fix after the
  review-diff-binding gate blocked twice (P1.2 root cause). The runner's "honest opt-out
  convergence" rationale is reasonable, but the opt-out's documented scope doesn't literally cover
  it. **Fix (doctrine wording only):** re-scope the CLAUDE.md wording to explicitly cover "a
  post-review fix implementing the reviewer's exact recommendation, re-verified by full suite+smoke."
  Distinct from the P1.2 mechanism fix.

## G. Review discipline / self-inflicted-regression prevention (live behavior -- canary/doctrine)

- [ ] **[canary] G1. A reviewer-suggested primitive swap was implemented repo-wide BEFORE reading
  the primitive's own contract, self-inflicting a CRITICAL.** `run-20260712-231031.log:1003-1046`:
  the perf reviewer suggested `spin_lock_irqsave` -> plain `spin_lock`; the runner applied it to 5
  functions + header + callers before checking semantics. The next round flagged CRITICAL --
  `spinlock.h:52-53` shows plain `spin_unlock` does UNCONDITIONAL cli/sti, re-enabling interrupts
  inside the IF=0 INT 0x80 fork path (schedulable-incomplete-child hazard). ~10 min implement ->
  build -> test -> dispatch -> revert, net-zero. `superpowers:receiving-code-review` fired, but the
  "verify at file:line before acting" step covered the finding's SYMPTOM, not the swapped
  primitive's CONTRACT. **Fix (receiving-code-review discipline):** before implementing a
  reviewer-suggested primitive swap (lock type, atomic ordering, allocator, memory-order), read the
  primitive's own header/contract FIRST, not just the finding text.
- [ ] **[canary] G2. Unbounded perf-suggestion chase with no stopping heuristic.**
  `run-20260712-231031.log:966-1063`: §17 chased perf findings across ~8 sequential dispatches, each
  surfacing a NEW marginal issue rather than confirming convergence, netting zero on the lock type
  (irqsave in, irqsave out) after ~30 min and self-inflicting G1's bug. DISTINCT from P2.1
  (convergence on UNCHANGED inputs) -- here every round's input genuinely changed; the gap is no
  stopping rule for MARGINAL non-Critical perf. **Fix:** cap live-implementation of perf-only
  (non-Critical/High) suggestions at ~1-2 rounds inside a section's gate loop; beyond that, file the
  remaining perf suggestions as an XREF'd follow-up item rather than iterating in-section.
- [ ] **[det] G3. Cross-TODO prerequisite unreadiness is only caught by spending a full Codex
  design-review round per section.** Three consecutive TODO-21 sections (§15, §16, §18) were all
  deferred at design review because the headline feature is unenforceable/unsafe without a
  prerequisite owned in a DIFFERENT TODO (TODO-06 stable-PID + SMP reap barrier; TODO-12 §7
  handle-rights-model). Correct completion-first behavior -- but each cost a full design-review
  round to discover. **Fix:** a `section-manifest.py` / `sequencer_triage.py`-level pre-check that
  asks "does this section's headline mechanism have a live enforcement path (its enforcing subsystem
  shipped)?" before entering the design-review dispatch; a NO defers cheaply without a Codex round.

## H. Kernel-debug ergonomics

- [ ] **[det] H1. Ad-hoc `klog` instrumentation churn dominates exec/fork bisection debugging.**
  `run-20260713-082218.log:330-428` + `:500-553`: two ~28.5-min bisection arcs (~29% of that run's
  wall clock, ~28 of 113 Edits / ~25%) spent inserting and reverting temporary
  `DBGEXEC`/`DBGFRAME`/`DBGHS` klog lines by direct source Edit -- one Edit+rebuild+retest+revert
  round-trip per hypothesis. **Fix:** a compile-time debug-trace toggle (`DEBUG_EXEC_TRACE=1 bash
  scripts/build.sh`) gating pre-placed `klog` call sites at the exec/fork hot spots via a macro, so
  bisection flips a build flag instead of editing+reverting source each hypothesis.

## I. Codex follow-on audit (2026-07-13) -- verified infra findings

> Source: a Codex read-only follow-up audit over the same 8-run family (deliberately non-duplicative
> of both plan docs). Every item below was **re-verified at file:line by the main session this
> session** (trust contract) before folding in -- all held. The Codex-proposed reopens of the 07-12
> plan's P0.1 / P2.3 and the P3.4 prerequisite were folded into that sibling doc; the items here are
> the NEW infra findings plus the A1 follow-on.

- [x] **[det] I0. A1 FOLLOW-ON: fixing the `worktree_hash` import exposes a second section-pack
  receipt defect.** VERIFIED. Once A1's import resolves, a fresh section-pack authorizes the FIRST
  edit, but that edit changes the pack's content digest, so the SECOND edit in the same section goes
  stale and re-blocks -- even though a real agent dispatch remains valid for the whole implement pass
  (`agent_dispatch_required.py:68` receipt-lifetime logic; `scripts/overnight/tests/test_dispatch_receipt.py:51`
  explicitly makes an edit stale). **Fix:** bind a once-accepted pack to the CURRENT implement pass
  (retain its pre-edit digest for audit, but do not re-block on intra-pass content drift); add a
  multi-edit regression test. Ship together with A1 so the import fix does not just move the block.
- [x] **[det] I1. Artifact FAILURES are masked by caller pipelines -- a real compile failure did not
  surface as a Bash tool error.** VERIFIED. `run-artifact.sh:24` preserves the child exit status in
  its JSON envelope, but callers pipe it into `tail`/`python3` or follow it with a passing command, so
  the Bash-tool-visible exit code is the LAST pipeline stage's, not the build's
  (`run-20260713-120304.log:185`: `... run-artifact.sh build1 -- bash scripts/build.sh 2>&1 | tail -20`;
  the actual compile failure at `.claude/overnight/artifacts/20260713-123043-build1.log:90` never raised
  a tool error). The `failure-ledger.py` (`:2` contract) is referenced only in doctrine/manifest, never
  wired into the flow. **Fix:** require STANDALONE `run-artifact.sh` invocation (guard unsafe
  `| tail`/`| python` shapes), have receipts validate the JSON `exit` field, and wire a nonzero result
  into the currently-unused failure ledger. Highest-risk of this cluster -- a broken build can pass
  silently.
- [x] **[det] I2. Subagent-runaway telemetry is DEAD -- wrong payload field name.** VERIFIED.
  `subagent_audit.py:120` reads `d.get("transcript_path", "")`, but the SubagentStop payload provides
  `agent_transcript_path` (correct-field precedent: `agent_result_cache.py:238`). Result: a
  kernel-explorer that used 36 tools (over the threshold of 30) recorded ZERO tools and no duration
  (`.claude/state/subagent-log.jsonl:1034`; the 36-call dispatch at `run-20260713-120304.log:119`).
  **Fix:** read `agent_transcript_path` (fall back to `transcript_path`), count tool uses + timestamps
  from the bounded leaf transcript, add a real SubagentStop fixture. (One-line field fix started
  2026-07-13 as a quick win.)
- [ ] **[det] I3. The deterministic orientation packet does NOT eliminate orientation work.** VERIFIED.
  `advance-work.py:99` stores only `section-pack.py`'s stdout SUMMARY (`section-pack.py:421-435` =
  counts + `pack_path` + digest), while the rich evidence it computed -- `input_files`,
  `relevant_tests`, `required_gates`, `symbol_defs`, `xrefs` -- is written to `pack.json` on disk
  (`section-pack.py:370-390`) and NOT surfaced in the packet. So the worker re-runs the pack anyway:
  across 8 launches at least ~25 calls directly repeated checkpoint/triage/manifest/pack work
  already done by `advance-work.py` (~9.4M cache-read-token proxy), and the latest worker reran the
  pack + ~19 searches/reads (`run-20260713-120304.log:26`). **Fix:** make the packet the SOLE initial
  orientation source by embedding a BOUNDED evidence subset from the full pack (top symbol_defs +
  input_files + tests + gates), not just the summary counts.
- [ ] **[det] I4. Finding/decision reuse is structurally ineffective.** VERIFIED. Of 56 records in
  `.claude/state/finding-triage.jsonl`, 43 have EMPTY evidence and NONE store a `todo` (the schema has
  no `todo` field: keys are `id/kind/severity/loc/title/decision/class/evidence/epoch`), so
  `finding-ledger.py list --todo` (`:87-88`, which filters on `e.get("todo")`) can never match --
  and no retained run calls `lookup`. The decision registry was also staler than the TODO cache
  (`decision-registry.py:100` manual lifecycle). Net: the latest review spent minutes re-triaging
  settled systemic usercopy/SMP findings for ZERO source changes (`run-20260713-120304.log:268`).
  **Fix:** require `todo` + `evidence` on every recorded disposition; auto-refresh the decision
  registry when the TODO cache advances; attach compact source-backed precedents to review prompts.
  Full review still runs -- precedents only prevent repeated triage of UNCHANGED systemic policy.
- [ ] **[det] I5. ChromeMCP is operational waste on the kernel queue.** VERIFIED against doctrine. All
  8 kernel runs started a browser lane and none used a browser tool, despite `overnight-launch.sh:221`
  and `SKILL.md:357` both saying kernel runs disable it (`run-20260713-120304.log:5`). The
  `OVERNIGHT_NO_CHROMEMCP` env drop-in is not taking effect at launch. **Fix:** guard `--with-browser`
  to browser-owned (gh-pages) work only, and verify the drop-in is actually effective at launch (assert
  the lane is skipped when the env is set).
- [x] **[det] I6. Housekeeping: P0.0's status says "attended bless pending" but a canary stamp already
  records a verified attended rollover** (`.claude/state/sequencer-canary-ok:1`). Update the P0.0
  status in the sibling plan to avoid an unnecessary repeat canary.

## Recorded observations (measured, NOT filed as actionable)

- **Malformed-JSON Read-tool call after a dense parallel-Agent report** appeared identically in two
  independent sessions (`run-20260712-223931.log:71`, `run-20260712-231031.log:323`),
  self-recovered next call. Possibly a systematic tool-call-construction trigger after a large
  preceding text block, but not clearly repo-fixable from the logs alone -- flagged for a
  main-session correlation check, not a repo change.
- **Two/three sections deferred with zero code** (§15/§16/§18, TODO-22 partials) were correct
  completion-first behavior, not defects -- the only actionable angle is G3 (make the discovery
  cheaper), already filed above.
- **No refused-rollover / un-rotated-advance / split-override / deadlock anomalies** were found in
  any of the seven runs. Every initially-refused rollover was refused for legitimate
  receipt-freshness reasons (P1.2 territory) and succeeded within the same session. The P3.2/P4.x
  flow invariants held in this run family.

## Suggested first slice (safe-first, matches the sibling plan's ordering principle)

1. **A1** (confirmed bug, both hooks, ~2-line fix + test) -- pure correctness, highest value.
2. **B1 + C1** (poll-timeout wrapper + line-length `--check` mode) -- guaranteed-per-session wins,
   `[det]`, batchable under one green deterministic run.
3. **C2 + C3 + F4** (edit-retry re-read, consolidated new-file pre-flight, round-guard shebang) --
   `[det]` ergonomics batch.
4. **E1/E2/E3 + F2/F3** (dispatch/artifact robustness + receipt churn) -- `[det]`, next batch.
5. **D1/D2/D3** (offload discipline) -- coordinate with P3.4 so the reminders and these BITE together.
6. **F1 + G1/G2/G3** (`[canary]`/doctrine) -- fold into the next watched attended canary alongside
   the sibling plan's `[canary]` cluster.
