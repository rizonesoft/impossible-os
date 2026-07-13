# Overnight Runner -- 24/7 Sustainability Implementation Plan (2026-07-12)

> **Ordered, safe-first worklist** consolidated from the 2026-07-12 cost pass, two Codex
> efficiency/flow-cadence reviews, and the overnight-log-explorer digest. Full rationale +
> file:line evidence lives in [`../overnight-runner-improvements.md`](../overnight-runner-improvements.md)
> (Cost / Codex-efficiency / Flow-cadence sections); this file is the de-duplicated, consistency-
> checked, **implementation-ordered** checklist. Same naming discipline as the parent file (not
> `TODO-*.md`, not in a `todo/NN-domain/` dir) so the sequencer and todo-graph never parse it.

## Goal + corrected baseline

Run the sequencer 24/7 without exhausting the weekly token budget while retaining Opus quality and
the full Codex review pipeline. Baseline (run-20260712-021311, ~6h main segment, 591 turns,
`opus-4-8[1m]`): burn ~20%/day -> x7 = ~140% of the weekly budget, so today it must stop+resume on
reset. Required reduction ~28.6% (target ~13-14%/day).

**Cost shape (corrected):** dominated by cache-read input (~90-92%); output is a real ~5-8% slice,
NOT the "negligible ~0.2%" the earlier analysis reported -- that came from a broken sidecar count
(12,553 for §12; the de-duplicated API records show ~588,840, to be re-confirmed against raw
records). Cache-read averaged ~479-576K re-read PER turn and peaked at ~1.01M before auto-
compaction. Net: the only levers that move cost are **context-size x turn-count**.

**Root cause:** the whole budget went into ONE section (TODO-21 §12 Pledge/Unveil) stuck in a ~21-
round Codex review-fix loop that never rotated context, inflated by fixable infra (13 crashed Codex
legs of ~67 forcing full-bundle re-dispatches; stale-gate re-dispatches + 2 `SKIP_REVIEW_HOOK`
bypasses from `last-codex-review.json` mis-binding; ~31 `task.c` re-reads with no standing evidence
map; ~75 inline calls duplicating 4 agent dispatches). §13 compounded it: a `SPLIT-RECOMMENDED`
verdict was overridden and a refused rollover did not block advancing into it.

## Implementation order (safe-first) + the gate legend

Ordering principle: measurement + pure-correctness (no quality risk) first; churn-reduction next;
structural flow/cadence after; the high-risk context-cap gate late; budget-pacing last as the
backstop.

**Gate tag on every item** (this replaces the blunt "any control-plane change re-arms the watched
canary" rule; the tiering itself is P0.0):

- **`[det]`** -- failure mode is fully caught by the DETERMINISTIC suite (`scripts/overnight/tests/run-all.sh`
  + `test-launch.sh` dry-run + `runner-doctor.py`), in seconds. Ships on green tests + its own new
  test. Does NOT re-arm the watched canary. ~18 of the items below.
- **`[canary]`** -- alters the runner's LIVE model-driven flow (arming, phase transitions, rollover
  firing, split behavior, context rotation, commit-gate flow); the failure mode is emergent and only
  shows in a real run, so it needs ONE watched attended run (>=1 section ship + >=1 rollover) before
  an unattended arm. ~9 items -- and these are the ones worth watching anyway.

Ship one phase (or sub-step) per iteration; batch several `[det]` changes and prove them with one
green deterministic run rather than a watched canary each.

### Phase 0 -- Measurement + gate tiering (do FIRST; unblocks the cadence of everything after)

- [x] **[canary] P0.0 Tier the control-plane-manifest so only flow-critical changes re-arm the live canary.**
  SHIPPED 2026-07-12 (impl + test + docs; one-time attended bless still pending before it governs a live arm). Added `scripts/overnight/control-plane-deterministic.txt` (strict allowlist of ~21 test-backed metrics/receipts/reporting files + `tests/`), a `--flow-critical` mode on `control-plane-match.sh` that subtracts it (default path byte-unchanged), and wired both `arm-sequencer.sh` canary-gate call sites to it; `test_control_plane_tiering.py` (5 cases) + suite 20/20 green; manifest header + CLAUDE.md updated. End-to-end verified: a `review-envelope.py`-only change no longer trips the canary, while `arm-sequencer.sh` / phase-guard / split changes still do. Original scope:
  Split `control-plane-manifest.txt` (and the `arm-sequencer.sh` canary gate) into FLOW-CRITICAL paths (arm/launch/watchdog, phase machine, rollover gate, split-override, context rotation -- emergent/live failure mode) vs DETERMINISTICALLY-COVERED paths (metrics, receipts, snooze, review-envelope, offload reminders -- fully caught by `run-all.sh` + `test-launch.sh` dry-run + `runner-doctor`). A flow-critical edit re-arms the watched canary; a deterministically-covered edit is gated by a green deterministic suite (and MUST ship a test), NOT a live run. This is the fix for the "every runner change needs a canary" bottleneck -- it unblocks the ~18 `[det]` items below. Bless the tiering itself with ONE attended canary. Optional follow-on: extend `test-launch.sh`'s dry-run to simulate a section-ship + rollover so more flow becomes deterministically provable, shrinking the `[canary]` set further.
- [x] **[det] P0.1 Fix the metrics pipeline: full-run capture, correct output accounting, per-commit token attribution.**
  **IMPLEMENTED 2026-07-13 (root cause found + fixed + proven on real data).** The undercount was
  ROOT-CAUSED, not guessed: replayed `run-20260713-044429`'s real session transcript through
  `stream-report.py` and the logic reproduced the true 285,961 output exactly -- so the parser was
  never the bug. The LIVE feed is lossy: `input`/`cache-read` are known when a request is dispatched
  (the streamed events carry them correctly -- live cache-read was a real 45M), but the COMPLETED
  `output_tokens` never arrives in the streamed assistant events; it lands only in the session
  transcript the CLI writes AFTER each message finishes. Live 044429 recorded 2,019 output vs a real
  285,961 (~142x). Fixes shipped:
  (1) **Transcript-authoritative reconciliation** -- `stream-report.py` latches `session_id`/`cwd`
  from the stream, records each section's message-id set as it streams (no wall-clock/timezone
  bucketing), and at a clean finish rewrites every record's `output_tokens`/`sidechain_output_tokens`
  from the authoritative transcript (`_resolve_session_transcript` + `_transcript_output_maps` +
  `reconcile_from_transcript`), stamping `output_source: transcript|stream-approx`. Fail-open (no
  transcript -> keep stream values). END-TO-END PROVEN: a lossy stream (output forced to 1) + the real
  transcript reconciled back to exactly 285,961 with correct per-section split (277,408 + 8,553).
  (2) **`todo`/`section` from the live sequencer cursor** (`.claude/state/sequencer-run.json`) when the
  launcher does not export `OVERNIGHT_TODO`/`OVERNIGHT_SECTION` (`_seq_cursor`).
  (3) **`.live` cleanup at clean finish** (`cleanup_snapshot`, skipped on a crash so the orphan
  survives) -- kills the 7 stale duplicate snapshots.
  (4) **Orphan ingestion in the aggregator** -- `metrics-report.py load()` = finalized record ELSE the
  orphan `.live` (marker `orphan-live`), preserving the "missing file + no orphan -> exit 2" contract.
  Tests: +8 cases in `test_stream_report_metrics.py` (reconcile main/sidechain/per-section/fail-open +
  `.live` write + cleanup + cursor attribution) and +2 in `test_metrics_report.py` (orphan ingest +
  missing-and-no-live-still-exits-2); full runner suite 24/24 green.
  **Capture decision:** did NOT add a raw-stream tee to the launcher -- the session transcript IS the
  retained authoritative capture (it already carries complete per-message usage), so reconciling from
  it is strictly better than re-capturing the lossy stream, and it avoids a flow-critical launcher
  edit + multi-MB-per-run disk cost. "Test against a captured real stream" is honored by the
  transcript-derived fixtures + the real-transcript end-to-end validation above.
  **P5 gate:** implementation + tests + real-data validation are DONE; P5 still wants one confirming
  MULTI-SECTION LIVE run (runner is disarmed) to watch reconciliation fire in situ before budget
  pacing keys off these numbers -- that is a validation checkpoint, not remaining code. Original notes
  follow:
  REOPENED 2026-07-13 (Codex audit, verified at file:line) -- superseded by IMPLEMENTED above:
  SHIPPED 2026-07-12. `stream-report.py`: (1) EOF/error flush via try/finally in `main()` -- a trailing section with no progress/result marker is now captured (marker `eof`) instead of vanishing; (2) output accounting fixed -- `add_usage` now keeps the MAX per message-id per field instead of first-seen, because output_tokens GROWS across the per-content-block events (input/cache-read are stable, which is why only output was wrong); (3) atomic live snapshot (`<metrics>.live`, temp+os.replace) rewritten each turn so a crash mid-section keeps the partial; (4) every record + snapshot now carries `run_id` / `todo` / `section` / `start_sha` / `end_sha`. Tests: +4 cases in `test_stream_report_metrics.py` (13 total), suite 20/20 green. NOTE: the ~588,840 figure could NOT be re-confirmed against that past run -- the raw stream-json feeds `stream-report` directly (`overnight-launch.sh:344-351`) and is not retained, only the formatted log is; the fix's correctness is proven instead by `test_growing_output_uses_max_per_msg` (max 500 vs first-seen 2), and the NEXT run's metrics will be trustworthy. Original scope:
  `stream-report.py` flushes only at `progress`/`final`, so the §13 tail (08:22-10:35) is absent from the jsonl. Fix: flush at EOF/error, atomic live snapshot, record final/max usage per message, stamp run/TODO/section/start+end SHA. Re-confirm the ~588,840 output figure vs raw API records. Prerequisite for measuring every fix below and for the Phase-5 governor.

### Phase 1 -- Pure-correctness wins (safe; zero quality risk; high value)

- [x] **[det] P1.1 Crashed-leg re-dispatch: on `rc=1`, re-run ONLY the crashed leg(s), never the full bundle.**
  SHIPPED 2026-07-12. Complete across all review paths: the crash-aware envelope (`review-envelope.py` `needs_redispatch`/`all_clean`) + `test_review_envelope.py` (this session); the broker (`review-broker-codex-dispatch.sh`) is already per-kind so the runner re-invokes it per crashed kind (no `--legs` arg needed); doctrine wired into the two broker-using skills (review-todo-section, overnight-sequencer) + a "crash != verdict" note in the two single-dispatch skills (codex-adversarial-review-section, codex-fix-review). Suite 21/21. Original scope:
  The crash-aware envelope (`review-envelope.py` `needs_redispatch`/`all_clean`) + tests shipped this session. Remaining (parent 2a-2e): broker `--legs` arg, per-leg verdict persistence, reuse-clean-verdicts, wire into the review skills, test single-leg-crash. Pure waste removal, zero quality cost.
- [x] **[det] P1.2 Bind `last-codex-review.json` to the actual staged diff (paths + head SHA); reject non-intersecting records.**
  IMPLEMENTED 2026-07-13 (surgical, fail-safe -- every change can only make the gate stricter or clearer,
  never looser). `section_commit_gate.py _review_evidence`: (a) **the churn fix** -- when the single latest
  record's `received:false` was reset by a NEWER multi-kind trigger (the exact cause of the 2
  `SKIP_REVIEW_HOOK` bypasses: `last-codex-review.json` holds only ONE review, so dispatching the next kind
  clobbers an already-received earlier kind), fall back to the received-review ring buffer and accept ONLY if
  a genuinely-received review blob-covers the EXACT current staged content (content-bound via
  `_history_covers`, never a bare timestamp); (b) reject a **non-intersecting STALE record** (its
  `trigger_blobs` share nothing with the staged source) with a precise message + head-SHA corroboration,
  instead of the confusing "uncovered source" that historically sent runs into a re-stage / SKIP loop.
  Tests: `test_review_gate.py` (received-true+covered accepts, non-intersecting rejected as STALE,
  received-false+history-covers accepts, received-false+no-history blocks). The existing blob-content binding
  + coverage were already present; this closes the multi-kind reset hole on top. Suite 25/25 green.
- [x] **[det] P1.3 Make Codex receipts completion-bound, not dispatch-bound (transactional broker state).**
  IMPLEMENTED 2026-07-13 (surgical scope, per the fail-safe decision). ROOT-CAUSED: the broker DETACHES its
  review (`systemd-run`/`setsid`) and returns a `{logFile}` instantly, yet `is_background_dispatch` returned
  FALSE for it (empirically confirmed) -- so the recorder treated the broker's instant return as a completed
  foreground review. Shipped: `_codex_dispatch.py` `_segment_is_background` now recognizes
  `review-broker-codex-dispatch.sh` (its immediate return no longer reads as a completed review), and a new
  `is_review_broker_dispatch()` lets the recorder KEEP stamping the broker (a completed broker leg is real
  review proof) while a generic `task --background` stays excluded -- so `background_dispatch` becomes honest
  with ZERO change to stamping behavior (fail-safe). `codex_review_completed.py` gates the stamp on
  `not background_dispatch OR is_broker`. Tests in `test_review_gate.py` (broker=background+broker,
  fg-wrapper unaffected, generic bg-task not broker); receipt-state firewall selftest still green.
  **DEFERRED (Option B, explicitly out of surgical scope):** the full receipt STATE MACHINE
  `running -> completed(rc=0) -> received -> content-valid` with per-leg completion tracking (poll each broker
  leg's logFile for `Turn completed (rc=0)` before its stamp counts). Flipping the detector to SUPPRESS the
  broker stamp -- the literal "only content-valid satisfies the gate" -- would wedge every broker review
  without that machinery, so it needs a watched attended canary and is filed as the P1.3 follow-on. What
  shipped closes the telemetry-honesty half; the completion-gate half is the deferred canary piece.
- [x] **[det] P1.4 Fix the broken usage-limit snooze parser.**
  SHIPPED 2026-07-12. Extracted the parser into a standalone, test-backed `scripts/overnight/parse-usage-limit.py` that reads the report FILE by path (killing the heredoc/stdin collision); `overnight-launch.sh` now calls it in one line. `test_usage_limit_snooze.py` (8 cases: session/weekly/no-minutes/fallback/8-day-cap + end-to-end file-not-stdin). Suite 21/21. (NOTE: the deterministic parser is canary-free, but the one-line `overnight-launch.sh` call is a flow-critical wiring edit -> rides on the batch's canary.) Original scope:
  `overnight-launch.sh:366` -- the heredoc clobbers the piped report so `sys.stdin.read()` gets EOF and the limit banner never parses; the runner cannot snooze on a real limit. Fix: pass the report by argv/path, end-to-end test with a captured banner. Correctness fix now; hard prerequisite for Phase 5.

> **Field observation (2026-07-12 attended canary, TODO-21 §14) -- corroborates P1.2 + P1.3:**
> the `codex_review_completed` recorder mis-fired repeatedly this session (didn't persist review verdicts despite clean approves), forcing several logged gate opt-outs
> (`SKIP_REVIEW_HOOK` / `SKIP_SKILL_STEP_BLOCK` / `SKIP_DISPATCH_GATE`) -- all with documented reasons. That's a runner-infrastructure flake, not a review shortcut (every review
> genuinely ran), but it slowed the canary considerably and is worth fixing before it costs the unattended run the same friction. Concretely: `last-codex-review.json` stayed at
> `verdict=None / received=False` even after a `Turn completed` clean approve (foreground AND background dispatches), and its `trigger_blobs` froze at a pre-fix state so the
> section-commit gate read stale evidence. P1.2 (bind the record to the actual staged diff) and P1.3 (completion-bound, transactional receipt state: `running -> completed(rc=0) ->
> received -> content-valid`) are the direct fixes; prioritize both before the next unattended arm.

### Phase 2 -- Convergence + churn reduction (low-medium risk; big turn savings, no depth loss)

- [x] **[det] P2.1 Convergence-based review: never redispatch an UNCHANGED review kind on UNCHANGED relevant inputs.**
  IMPLEMENTED 2026-07-13. New deterministic decision CLI `.claude/hooks/review_convergence.py`:
  `should-redispatch <slice> <kind>` fingerprints the kind's relevant inputs and returns CONVERGED (exit 1,
  skip) when they are unchanged since that kind's last recorded verdict, else REDISPATCH (exit 0); `record
  <slice> <kind>` stores the verdict fingerprint. NO round cap (stall detection stays in
  `review_round_guard.py`, a complementary signal). Fingerprint = HEAD tree + worktree diff + untracked
  CONTENTS over the kind's scope (mirrors the receipts/agent-cache content-addressing). Fail-open EVERYWHERE
  (unknown kind / git error -> REDISPATCH), so it can only skip a redundant review, never suppress a needed
  one. Wired as a REQUIRED pre-redispatch consult into review-todo-section (step 6), codex-fix-review (8c),
  and overnight-sequencer. Tests: hook `--selftest` (7 assertions) + `test_review_convergence.py` (CLI
  exit-code contract). Suite 26/26.
- [x] **[det] P2.2 Per-kind x per-file review invalidation: one changed file must not re-trigger ALL kinds.**
  IMPLEMENTED 2026-07-13 as the per-kind scope of P2.1's `review_convergence.py`: `adversarial` / `perf` /
  `re-adversarial` fingerprint SOURCE only; `consistency` / `design` fingerprint SOURCE + TODO. So a
  docs/TODO-only fix leaves the source-only kinds' fingerprints unchanged (they CONVERGE -> skip) and re-runs
  only the TODO-aware kinds; a source edit re-runs the source kinds. **Deviation (documented):** `perf` stays
  scoped to ALL source rather than a guessed "hot-path" subset -- narrowing it further risks SKIPPING a real
  perf review (the unsafe direction for a review-quality gate), so "kernel-path change does NOT rerun perf" is
  intentionally left conservative (perf reruns on any source change). Hot-path narrowing is filed as a later,
  separate optimization. Covered by the P2.1 selftest (docs-only-edit and source-edit per-kind cases).
- [x] **[det] P2.3 Standing evidence map across review rounds (route round-N verification through `review-evidence-mapper` for rounds >= ~4).**
  **RE-CLOSED 2026-07-13 (both reopened gaps fixed).** (1) Cache-key volatility fixed: `agent_result_cache.py`
  now canonicalizes a mapper dispatch's prompt to its STABLE scope (todo path + section + source file paths
  with `:line/:col` stripped) via `_canonical_scope_from_prompt`, keyed per content-deterministic mapper type
  (researchers excluded -- their prompt IS the question). So rounds 4/5/6 naming the same section/files over an
  UNCHANGED tree collapse to ONE key -> a real cache hit, killing the 22-stores/0-hits volatility; a different
  section keeps a distinct key (no wrong cross-scope hit). Tests: 3 new cases in `test_agent_cache.py`
  (hits-across-volatile-prompts, distinguishes-sections, researcher-prompt-still-load-bearing). (2) Round-4
  routing upgraded from a REMINDER to a REQUIREMENT in review-todo-section step 6, codex-fix-review 8c, and
  overnight-sequencer (the "verify at file:line" step MUST go through one mapper dispatch, not inline
  re-reads). MANIFEST updated for the new key semantics. Suite 26/26. Prior REOPENED note follows:
  **REOPENED 2026-07-13 (Codex audit, verified at file:line):** shipped NARRATIVELY only -- the wiring exists
  but never actually fired in the live runs. Confirmed this session: `.claude/state/offload-events.jsonl` holds
  `cache-store: 22` and **0 `cache-hit` events** (the `agent_result_cache` for `review-evidence-mapper` is
  written but never reused), and round 4 of `run-20260712-231031.log:738` still performed inline verification
  rather than a mapper dispatch. **Remaining fix:** add a round-4 mapper-receipt REQUIREMENT (not just a
  reminder) and canonicalize the cache key around the SCOPED evidence set (src/todo content digest) rather than
  the volatile prompt text -- volatile keys are why 22 stores produced 0 hits. Preserves convergence-based
  review with no fixed round cap. Original SHIPPED note follows:
  SHIPPED 2026-07-12. The pieces already existed but were dormant/unwired: `review_round_guard.py` (per-section round counter + STALL-based convergence cap -- K no-new rounds + a 30-round ceiling, NOT a fixed cap) and `agent_result_cache` (already caches `review-evidence-mapper` by src/todo content, so an unchanged-tree map is reused for free). Wired both into the fix-loop doctrine of review-todo-section (step 6), codex-fix-review (8c), and overnight-sequencer: bump the round guard each re-dispatch + honor CAP; for rounds >= 4, verify at file:line via one mapper dispatch instead of inline re-reads (task.c was re-read ~31x). Added `test_review_round_guard.py` (4 cases: selftest + productive-never-caps + stall-caps + new-resets); suite 22/22. Note: this also activates the safe, receipt-INDEPENDENT half of convergence (stall detection); the receipt-dependent per-kind x per-file invalidation (P2.1/P2.2) stays with the P1.2/P1.3 pass. Original scope:
  Kills the ~31x `task.c` re-read churn. Reuse the map when the hot-file set is unchanged.

### Phase 3 -- Structural flow / cadence (the `[canary]` cluster: these change live flow)

- [ ] **[canary] P3.1 Hard-enforce SPLIT-RECOMMENDED + fix the split-predictor boundary.**
  `section-manifest.py` verdict is advisory -- §13 overrode it. Make override require a STRUCTURED preflight waiver (estimated files/subsystems/tests/context budget), not a free-form "cohesive". Fix the `> 8` boundary + ABI weighting (`abi_impact and open_items > 8` at line 150 misses exactly-8-item ABI/SSDT sections; lower to `>= ~6` when `abi_impact`). Upstream half of the context-cap. The gate is unit-testable, but the split BEHAVIOR is live-flow -- canary.
- [ ] **[canary] P3.2 A REFUSED rollover must BLOCK starting the next section, not "continue in-session" into it.**
  Flow invariant (`SKILL.md:314`, log:1480 -- §12->§13 advanced un-rotated). A refused rollover may repair ONLY the current checkpoint; it must not orient or begin the next section. Boundary-side complement to the context-cap. Pure live-flow control -- canary.
- [ ] **[det] P3.3 Verify cadence: targeted suites DURING the fix loop; ONE full suite + smoke at the section boundary.**
  Not 40 builds / 17 smoke mid-loop. Frequency discipline (a reminder/gate; can't hang the run). Smoke stays unconditional AT the boundary (see Rejected).
- [ ] **[canary] P3.4 Make offload bite: promote `build_offload_reminder` / `inline_churn_monitor` from WARN to enforced routing.**
  Block-with-reroute the expensive shapes: full-log greps, single reads > ~50 KB, 3+-search-round exploration, and inline exploration run beside a concurrent Agent dispatch. Keep the deterministic `run-artifact.sh` path for build/test. A bad BLOCK can wedge a live run -- canary.
  **PREREQUISITE before WARN -> BLOCK (Codex audit 2026-07-13, verified at file:line):** the current
  `build_offload_reminder.py:37` matcher `\bbash\s+scripts/(build|test|...)\.sh\b` fires on the INNER
  `bash scripts/build.sh` EVEN WHEN it is correctly wrapped as
  `bash scripts/overnight/run-artifact.sh <label> -- bash scripts/build.sh` -- it fired 9x on the latest
  properly-wrapped build/test sequence (`.claude/state/offload-events.jsonl:472`). Promoting the matcher to
  BLOCK as-is would BLOCK THE SANCTIONED ROUTE. Before enforcing: (1) exempt an OUTER `run-artifact.sh`
  wrapper (match only when the bare script is the outermost command); (2) remove the deprecated `checks-runner`
  routing text from the reminder; (3) deduplicate reminders (it fired 9x for one sequence); (4) unit-test the
  compliant wrapped shape vs the bare shape before flipping the hook. Fold into the same `[canary]` bless.
- [ ] **[canary] P3.5 Give a reviewed [/]-partial section a clean ship+stamp path (retire the gate deadlock + opt-out reliance).**
  Recognize a partial ship carrying fresh review evidence (scoped delta receipt + a valid cross-session record for unchanged bytes) as satisfying the stamp gate, so a reviewed partial ships without a full re-run or a `SKIP_*` bypass. (Direct fix for the deadlock hit landing §13.) Commit-gate flow change -- canary.

### Phase 4 -- Context-cap rollover / Path B (highest structural; strictly phased)

Risk is concentrated ENTIRELY in the relaxed gate (P4.5/P4.6). P4.1-P4.3/P4.7 are additive and
`[det]`; P4.4/P4.5/P4.6/P4.8 alter live rotation flow and are `[canary]`. Ship the `[det]` pieces
first; if Phases 1-3 stop sections ballooning, most sections may never need the relaxed gate.

- [ ] **[det] P4.1 Size/turn trigger, advisory only.**
  Set a "rotate at next safe point" flag when context (or a turn-count proxy) crosses the doctrine band (~200-250K per `TODO-Claude-Overnight-Runner.md:246`, NOT the earlier ~350K; tune via canary). Sets the flag only -- cannot corrupt state.
- [ ] **[det] P4.2 Enrich `section-checkpoint.py gather()`.**
  Capture open Codex findings + verdicts (`finding-ledger.py`), decisions (`decision-registry.py`), current phase, next intended action. Additive, fail-open per field.
- [ ] **[det] P4.3 Surface + act on the enriched checkpoint on resume.**
  CORRECTION to the old "1c": the resume path ALREADY calls `section-checkpoint.py show` (`SKILL.md:162`), so P4.2 is NOT dead weight. Real work: ensure the enriched fields appear in the `show` output AND the resuming session re-orients from them without re-deriving; optionally also auto-inject via `session_brief_inject.py` for reliability.
- [ ] **[canary] P4.4 Attended canary of the enriched re-orient.**
  Prove a resumed session re-orients without re-deriving the same file:line facts. Gates whether P4.1-P4.3 pay off before any gate work is built. (This one IS a canary by nature.)
- [ ] **[canary] P4.5 Parallel `_rollover_failures_wip()` gate (HIGH-RISK).**
  Accepts a WIP-commit-clean (committed, unpushed, unstamped) tree but KEEPS the review-received + no-background-jobs checks. Do NOT weaken the shipped `_rollover_failures()` -- it governs the ship rollover too.
- [ ] **[canary] P4.6 Safe-boundary firing (HIGH-RISK).**
  Rotate ONLY at a WIP-commit-clean tree / between Codex rounds / after a green fix-loop round. Hard-forbid rotation during a review wait, an uncommitted edit, or mid-fix-loop (the fix-then-regress guard the 2026-07-03 self-diff gate protects).
- [ ] **[det] P4.7 Tests for the WIP gate + boundary guard.**
  Rejects open review / active bg job / dirty tree; accepts committed-clean-unpushed; boundary guard blocks a mid-fix-loop rotation.
- [ ] **[canary] P4.8 Mandatory attended canary of full mid-section rotation before ANY unattended arm.**
  The control-plane-manifest change mandates the green attended canary.

### Phase 5 -- Budget-aware pacing (backstop; LAST; depends on P0.1 + P1.4 + a reduced baseline)

> **PHASE 5 CLOSED 2026-07-13 -- WON'T-BUILD (operator decision).** Automated weekly-budget projection +
> self-snooze is a LAST-priority backstop layered on P0.1 + P1.4 + a reduced baseline, and the guardrails
> that already exist bound spend without it: manual `arm-sequencer.sh` arm/disarm, the per-run circuit
> breaker + backoff, and the operator watching burn (P0.1 metrics now report accurately after the
> transcript-reconciliation fix). A projected-burn auto-snooze also carries its own "mostly sleeps ships
> nothing" failure mode (P5.4's own caveat) -- adding a self-throttling control loop to save tokens can
> cost more shipped work than it saves. Net: not worth the build for a backstop. **Re-trigger condition:**
> revisit only if the runner moves to genuinely unattended 24/7 operation AND manual arm/disarm + the
> breaker prove insufficient to keep weekly burn under budget. Items retained below `[x]` for the record.

- [x] **[det] P5.1 Track cumulative 7-day token burn** in the arm/launch layer (rolling window from run reports; needs P0.1 metrics). -- WON'T-BUILD (see Phase 5 close).
- [x] **[det] P5.2 Project 7-day burn** and surface it in the status brief / monitor. -- WON'T-BUILD (see Phase 5 close).
- [x] **[det] P5.3 Self-snooze until reset** via the existing snooze/backoff plumbing when projected burn would exceed 100% of the weekly budget (needs the P1.4 snooze fix). -- WON'T-BUILD (see Phase 5 close).
- [x] **[det] P5.4 Activation floor** so the backstop can't dominate; gate it behind Phases 1-4 landing (a runner that mostly sleeps ships nothing). -- WON'T-BUILD (see Phase 5 close).
- [x] **[det] P5.5 Test the project -> snooze -> resume transition** at the ceiling. -- WON'T-BUILD (see Phase 5 close).

### Phase 6 -- Blocked-item recovery completeness (correctness gap; surfaced 2026-07-12 canary)

Not a cost item -- a completeness gap in the fixpoint loop, found while explaining the runner's
blocked-item behavior. **Terminal-park** cross-TODO deferrals (`[/]` + a plain `Deferred:` / `Accepted:`
+ XREF, with NO `awaiting-<token>`) are DONE-equivalent to the triage oracle (`sequencer_triage.py`
3-state model), so the runner never re-visits them. When the XREF owner section later ships, the
owner's `implement/review` step-18 inbound Accepted/Deferred sweep only TIDIES the stale stamp -- it
never re-opens the now-unblocked dependent work. Net: cross-TODO work that has BECOME runnable is
stranded until a human flips it back to `[ ]`. (The `awaiting-<token>` recoverable-park and plain
`[ ]` blocker-noted items do NOT have this gap -- only terminal-park `[/]` deferrals do.)

> **PHASE 6 CLOSED 2026-07-13.** Delivered: P6.2, a deterministic READ-ONLY stranded-deferral audit
> (`scripts/overnight/stranded_deferrals.py`), wired as a non-blocking advisory at `run_phase_guard.py
> fixpoint` + documented in `.claude/hooks/MANIFEST.md` and `todo/TODO-Claude-Overnight-Runner.md`. The
> auto-action items (P6.1 forward flip / P6.3 blocking fixpoint gate / P6.4 backfill) are RESOLVED as
> won't-build / done-by-triage: the audit's own triage supplied the "accuracy proof" they were gated on,
> and the proof's verdict is that auto-action is unsafe AND has no actionable instances. `flip=0` on the
> tree (current audit: **34 candidates -- flip=0 / clean=0 / blocked=23 / review=11**), and the
> flip-vs-blocked-vs-review decision is an irreducible code-and-design read that a heuristic classifier
> cannot make safely -- so the correct terminal automation is a human-reviewed advisory, not an auto-editor.
> **Re-trigger condition:** if a future audit ever shows a STABLE non-zero `flip` set (a real Deferred item
> blocked only on a now-shipped prerequisite that the human confirms), re-open this phase to build the
> gated `--apply` flow; until then the standing workflow is `python3 scripts/overnight/stranded_deferrals.py`
> + hand-act on any non-empty `flip`/`clean` set.

- [x] **[canary] P6.1 Owner-side sweep re-opens unblocked dependents (not just tidies the stamp).**
  RESOLVED (won't-build, 2026-07-13): the P6.2 audit + full triage delivered the accuracy proof this item
  was gated on, and the proof's answer is "do NOT auto-flip." `flip=0` on the current tree (34 candidates:
  flip=0 / clean=0 / blocked=23 / review=11) and the flip-vs-blocked-vs-review call is an irreducible
  code-and-design read -- the triage had to verify code + design history per item (e.g. the TODO-03
  canary-seed candidate was design-OBSOLETED, not stranded: TODO-10 sec 12 shipped an RDRAND seed and
  DECLINED csprng-seeding by design review, so re-opening it would implement rejected work). A forward
  auto-flip keyed on the "owner shipped" signal alone reintroduces the exact phantom-work risk the audit
  was deliberately made read-only to avoid; owner-shipped is necessary but NOT sufficient. Terminal
  design: the non-blocking advisory (P6.2, wired into `fixpoint`) surfaces the set and a human reviews it
  when non-empty. Superseded-by: P6.2. Original deferral note + scope retained below for the record.
  DEFERRED 2026-07-13 (pending P6.2 accuracy proof). P6.2 shipped as a READ-ONLY audit precisely because its
  first-proposed signal over-matched ~6x (see P6.2 stamp); auto-re-opening dependents mid-run is exactly the
  phantom-work risk that over-match would realize, so P6.1 waits until the audit's 34 candidates are
  human-validated as truly stranded and the flip-vs-clean classifier is proven. When built, it reuses
  `stranded_deferrals.py`'s per-item classification and stays `[canary]` (live-flow: re-opens work mid-run).
  Original scope:
  When a section ships and its step-18 inbound sweep finds an Accepted/Deferred stamp whose concern
  this section just FULLY satisfied, flip the dependent item from `[/]` back to `[ ]` -- re-classifying
  its file NEEDS_WORK for the next fixpoint pass -- instead of only deleting the stale line. Gate the
  flip on the sweep's existing fully-vs-partially-resolved decision: only a fully-resolved concern
  flips; a partially-resolved one keeps its remaining XREFs untouched. Live-flow behavior (re-opens
  work mid-run, changes what the runner picks next pass) -- canary.
- [x] **[det] P6.2 Deterministic stranded-deferral audit.**
  IMPLEMENTED 2026-07-13 as `scripts/overnight/stranded_deferrals.py` -- READ-ONLY diagnostic only (it
  prints a human-reviewable list; never edits a TODO, never gates fixpoint). **Signal corrected during
  implementation:** the plan's first-proposed signal (section-level `stamps_xrefs` whose owner shipped)
  OVER-MATCHED ~6x on the live tree -- 171 "deferred" + 73 "accepted" hits, 143 of the 171 self-XREFs to
  concerns already tracked as OPEN `[ ]` items the loop picks up normally (spot-verified: TODO-22 sec 3's
  "Large-volume scalability" is a live `[ ]`, not a stranded `[/]`). Auto-acting on that set would wedge
  fixpoint or spawn ~150 phantom items. The shipped audit is item-level + conservative instead: only `- [/]`
  items, CROSS-TODO only (self-XREF excluded), `awaiting-<token>` excluded (recoverable-park), owner-shipped
  = target section carries BOTH Verified AND Quality-reviewed (a terminal-Deferred owner punted too), and it
  emits only on an unambiguously-resolved (target_file, target_section) pair. Suggests clean / reopen /
  review per item. **Refined classifier (2026-07-13, after a full triage):** clean (owner-completed, do not
  re-open) / blocked (a LIVE dependency beyond the shipped owner remains -- "until the hive I/O bodies exist",
  "DEFERRED (dual-log)", "needs C:", "not implemented") / flip (gated ONLY on the now-shipped owner, safe to
  re-open) / review (none matched -- usually a correctly-partial [/]). **Triage result on the current 34:
  flip=0, clean=3, blocked=21, review=10 -- ZERO genuinely ready to auto-flip.** Owner-shipped is necessary
  but NOT sufficient: every candidate read + code-verified was blocked on another dependency, owner-completed,
  correctly-partial, or design-obsoleted (the one mechanical flip candidate -- TODO-03 "canary seed sharing" --
  turned out obsolete: TODO-10 sec 12 shipped the canary with an RDRAND seed and DECLINED csprng-seeding by
  design review, so re-opening it would implement work the owner rejected). Net finding: the Phase-6 gap is
  real in principle but has no genuinely-actionable instances on the current tree -- leaving these parked is
  correct in every examined case, which is itself why P6.3/P6.4 auto-action stays deferred. Each candidate also
  carries a
  `stranded` flag = its OWN section is DONE-parked (V+Q or terminal-Deferred), so the fixpoint loop never
  re-visits it and it will NOT flip naturally on an overnight run: 33 of the 34 are stranded, 1 sits in a
  still-worked section. (Stranded != lost work: only the `reopen`-classified subset is genuinely un-done
  in-scope work; `clean` items are owner-completed and just cosmetically stale.) Uses
  `sequencer_triage.section_stamps` as the DONE oracle. Tests: hook `--selftest` + `test_stranded_deferrals.py`
  (schema + read-only-invariant via git-porcelain diff). Suite 27/27.
  **WIRED IN (permanent, 2026-07-13):** the audit stays a deterministic SCRIPT (the engine); it is surfaced
  as a NON-BLOCKING advisory at `run_phase_guard.py fixpoint` (prints the flip/clean/blocked/review summary
  when the run completes; fail-open, never changes the verdict) -- NOT a skill (overkill for a read-only
  query) and NOT a blocking gate (that is the deferred P6.3). Documented in `.claude/hooks/MANIFEST.md`
  (run_phase_guard row) and `todo/TODO-Claude-Overnight-Runner.md` (fixpoint section). Operator entry point:
  `python3 scripts/overnight/stranded_deferrals.py`. **Classifier hardened + hygiene applied (2026-07-13):**
  blocker-check now precedes clean/flip (a live dependency overrides incidental "wired in"/"tracked in"
  prose), so `clean` dropped from 3 to 1 real (the other two were false positives -- one blocked on
  boot-mount, one pending deep-copy setters). The 1 genuine `clean` (TODO-22 sec 2 "recovery boots via
  kind=recovery", code-verified: `BOOT_ENTRY_KIND_RECOVERY`/`BOOT_PATH_RECOVERY` shipped by TODO-07 sec 4)
  was marked `[x]`. Final tree: flip=0, clean=1, blocked=23, review=10.
- [x] **[det] P6.3 Fixpoint DONE-check consults the stranded-deferral audit.**
  RESOLVED (won't-build, 2026-07-13): a BLOCKING fixpoint gate is net-negative. `fixpoint` is the runner's
  ONLY clean-stop path, so gating it on the audit trades a hard stop against a heuristic classifier -- and
  with `flip=0` there is nothing for the gate to act on today, while a single mis-classified `blocked`->
  `flip` would WEDGE the runner's only way to finish. Risk >> benefit. The safe version of this idea already
  shipped in P6.2: a NON-BLOCKING advisory printed at `run_phase_guard.py fixpoint` (fail-open, never changes
  the verdict) that surfaces the flip/clean/blocked/review summary for a human. That advisory IS the fixpoint
  integration; a blocking gate is explicitly declined. Superseded-by: P6.2 (advisory). Original deferral note
  + scope retained below for the record.
  DEFERRED 2026-07-13 (pending P6.2 accuracy proof). Gating fixpoint on the audit is only safe once the audit
  yields ~zero false positives: fixpoint is the runner's ONLY clean-stop path, so blocking it on today's 34
  advisory (heuristic-classified) candidates would WEDGE the runner. Build after the 34 are human-triaged and
  P6.4's backfill has cleared the true stranded set to a small, stable residue -- then block fixpoint only on
  the high-confidence `reopen` subset. Original scope:
  Before `run_phase_guard.py fixpoint` declares true DONE, run the P6.2 audit; a non-empty stranded
  set is NOT-fixpoint (re-open the dependents + `next-pass`), not a false finish. Safety net for when
  the owner-side sweep (P6.1) misses a satisfy-mapping, and it closes the "runner stops with
  recoverable cross-TODO work still parked" hole directly. Deterministic gate + test.

- [x] **[det] P6.4 One-time backfill sweep of the EXISTING stranded backlog (run P6.2, then act by stamp type).**
  RESOLVED (done-by-triage, 2026-07-13): the human triage this item required WAS the one-time backfill. All
  34 candidates were read + code-verified; the actionable result was a single `clean` item (TODO-22 sec 2
  "recovery boots via kind=recovery", verified against shipped `BOOT_ENTRY_KIND_RECOVERY`/`BOOT_PATH_RECOVERY`)
  which was flipped `[x]`, and ZERO `reopen`/`flip` cases. The current re-run confirms the backfill is drained:
  34 candidates, flip=0 / clean=0 / blocked=23 / review=11 -- no `Deferred:` item is blocked-only-on-a-shipped-
  prerequisite, and every `review` item is a correctly-partial `[/]`. There is nothing to auto-edit; a
  standing `--apply` mode would be an idle high-blast-radius tool. The permanent workflow is: run
  `python3 scripts/overnight/stranded_deferrals.py` and hand-act on any future non-empty `flip`/`clean` set.
  Superseded-by: P6.2 (the audit is the standing backfill tool). Original deferral note + scope retained below.
  DEFERRED 2026-07-13 (pending human triage of P6.2's 34 candidates). The audit is READ-ONLY by design; a
  one-time auto-editor that flips `[/]` -> `[ ]` and strips stamps across the tree is the highest-blast-radius
  action in Phase 6. Next step is operator-driven: run `python3 scripts/overnight/stranded_deferrals.py`,
  review the 34 candidates (2 reopen / 2 clean / 30 review), then apply the confirmed decisions (Deferred ->
  flip, Accepted -> stamp-clean) by hand or via a follow-up `--apply` mode gated on a reviewed allowlist.
  Original scope:
  P6.1 is forward-only -- it fires only when a section ships from now on, so it never touches deferrals
  already stranded by owners that shipped in PAST runs. After P6.2 lands, run it once against the
  current tree and act per the stamp's SEMANTIC, not blanket re-open: a **`Deferred:`** item (in-scope,
  was blocked on a now-shipped prerequisite) flips `[/]` -> `[ ]` so the loop implements it -- these are
  the actual "unblock" cases; an **`Accepted:`** item (out-of-scope, work OWNED by the now-shipped XREF
  target) is confirmed-done -- clean the stale stamp only, do NOT re-open (the owner did the work; the
  dependent never does, and re-opening would spawn phantom duplicate work). Deterministic to enumerate +
  decide; the re-opened items then flow through the normal (already-canaried) work loop, so no new
  canary. Clears the debt immediately instead of waiting for P6.3's next fixpoint attempt.

## Acceptance spec -- the 9 runner invariants (the next canary asserts these pass/fail)

1. Never start another section after a refused rollover.
2. Never directly edit `last-codex-review.json` in normal operation.
3. Never override SPLIT-RECOMMENDED unattended without a structured waiver.
4. Never repeat an unchanged build/review without changed input or a recorded retry reason.
5. Don't rerun all review kinds because one relevant file changed.
6. Preserve unlimited productive Critical/High review convergence.
7. Run full validation once at the stable section boundary.
8. Rotate context after every shipped section.
9. Re-open (never strand) a terminal-park deferral once its XREF owner section ships.

## Canary run log (2026-07-13 watched run)

First watched attended canary of this batch, armed with `--force` on branch
`overnight-runner-improvements-2026-07-13` (run-20260713-205030), completing TODO-22's env work
(the pre-disarm cursor) naturally. Live validations observed:

- **Phase machine intact** -- PREFLIGHT -> TRIAGE proceeded cleanly; the run picked TODO-22 and
  dispatched a real adversarial Codex review within ~4 min.
- **B1 waiter adopted + working** -- the run used `wait-for-codex-verdict.sh` (not the old `sleep 60`
  loop); ~6 re-invoke cycles at ~100-118s each with NO `Exit code 143 / timed out after 2m` kill.
  The re-invoke pattern is exactly the intended behavior.
- **NEW finding -> B2 (see 2026-07-13 findings file).** A ~14-min-but-ALIVE adversarial review of
  `nt_env.c` (Codex still running `rg` + wait tools, no crash marker) could not be distinguished from
  a hung one by the waiter's binary signal; the runner manually crash-checked (found none) then
  re-dispatched a fresh leg, abandoning the live one. Clean recovery, but wasted ~14 min. Fix: waiter
  should emit elapsed + last-activity (`.out` mtime/growth) so the caller re-dispatches only a
  genuinely-silent review. This is the canary surfacing a signal-quality gap the deterministic suite
  could not -- exactly its purpose.
- **§5 shipped + full review pipeline validated.** The post-commit `review-todo-section` ran all four
  Codex legs + the Opus `kernel-quality-auditor`; the adversarial leg found a REAL bug (NULL-deref in
  the empty-value query path), the runner fixed it (6-line NULL guard + regression test), rebuilt green
  (ABI 1343 kernel + 16 user-mode, smoke OK), and the post-fix re-adversarial returned `Verdict: approve`
  / no material findings. The pipeline did exactly its job: caught + closed a genuine defect.
- **KEY finding -> B3 (see 2026-07-13 findings file).** The review-gate STILL forced TWO honest
  `SKIP_REVIEW_HOOK` opt-outs to land §5 (section commit + review-stamp commit), each also needing
  `SKIP_SKILL_STEP_BLOCK`, via two content-binding/attribution gaps P1.2/P1.3 did not cover:
  (1) `skill_step_observer.py` does not recognize the review-broker (P1.3 only fixed the recorder), so
  broker legs are not attributed -> empty `trigger_files` -> BLOCK; (2) the gate binds to the FIRST
  review's blobs, so a find-and-fix section drifts out of binding even after the re-adversarial approves
  the fix diff. Honest opt-outs, run NOT wedged -- but the SKIP churn the whole review-gate effort
  targeted was NOT eliminated. Two concrete fixes filed in B3.
- **Rollover gate is fail-safe (observed).** At ~22:26 the runner attempted a verified session rollover
  (`run_phase_guard.py rollover`); it REFUSED on a dirty tree ("operator files + stale receipts +
  SKIP_REVIEW_HOOK reset residue") and, per doctrine, continued in-session instead of rolling a fresh
  worker over uncommitted state. So the rollover mechanism was exercised and correctly declined -- a
  SUCCESSFUL rollover needs a clean tree (the operator's own in-progress backlog docs -- these files --
  were part of what it saw dirty; the runner correctly declined to commit content it did not author).
- **Coordination note.** Recording live canary findings into repo docs makes the tree dirty, which
  blocks the runner's clean-tree rollover gate; an interactive operator + the autonomous runner sharing
  one tree is inherently at odds at rollover boundaries. Operator committed these findings docs
  separately (doc-only, this branch) to clear the gate.
- **SUCCESSFUL rollover observed (~22:45).** After the operator committed the findings docs and the
  runner committed its own auto-generated `coverage.json`/`coverage.md` (from the full-suite run,
  20671->20680 tests), the tree went clean, all three receipts were re-recorded (build + suite-all +
  smoke, content hash `9e4c3e07d976`, smoke passed), and `run_phase_guard.py rollover` fired
  (`run outcome: {"checkpoint_kind": "rollover"}`). A FRESH worker resumed at phase SECTIONS, cursor
  TODO-22, via the durable cursor -- validating the rollover path end to end: fail-safe dirty-tree
  refusal -> clean tree + content-bound receipts -> successful handoff -> correct-cursor resume.
- **NEW minor friction (B4 candidate -- coverage-artifact + operator-file misattribution at rollover).**
  Two rollover attempts refused on a dirty tree that was NOT operator files: the runner's OWN full-suite
  run regenerates `coverage.json`/`coverage.md`, which dirties the tree and invalidates the content-bound
  receipts, so the rollover cannot pass until those auto-gen artifacts are committed. On the second such
  refusal the runner initially MIS-ATTRIBUTED them to "the operator live-editing the canary-log files"
  (the operator was NOT editing then -- the findings commit had already landed at 22:29), self-correcting
  in ~30s once it diffed the actual paths. **Fix candidates:** (a) have the rollover receipt step commit
  (or `.gitignore`-scope) runner-generated `coverage.*` before the clean-tree check, so a routine test run
  does not block rollover; (b) make the dirty-tree diagnostic name the actual paths + their probable owner
  (auto-gen vs source vs untracked) instead of guessing "operator." Low severity; self-corrected; filed so
  the recurring coverage-dirties-tree friction is owned.
- **Canary complete; runner disarmed (~22:46).** `arm-sequencer.sh --disarm` removed timers + watchdog +
  launch.lock + run state; the post-rollover worker halted at its next arm check. §6/§7/§8/§9 were
  deferred (blocked on user-mode runtime / shell / desktop -- legitimate), everything pushed, tree clean.
  Net canary verdict: phase machine, B1 waiter, review pipeline (real bug caught), and the rollover path
  all validated; B3 (review-gate SKIP churn) is the one must-fix before an unattended arm; B2 + B4 are
  signal-quality/ergonomic follow-ups.

## Recorded decisions (do NOT re-litigate)

- **Rejected: conditional rollover smoke.** `run_phase_guard.py:442` runs `check_smoke` unconditionally at rollover; keep it -- smoke is ~3s + deterministic (zero model tokens), so weakening it trades a real boot safety net for a negligible saving. The win is not re-running smoke mid-loop (P3.3), not dropping the boundary check.
- **Rejected: any hard cap on review rounds.** Late §12 rounds caught real criticals (INT 0x80 sandbox bypass, path traversal, read-only-handle mutation). Convergence (P2.1) is the mechanism, never a fixed cap.
- **Path A (proactively lower the compaction threshold): not cleanly buildable** -- no auto-compact threshold knob, headless cannot invoke `/compact`, native compaction fires ~950K. The only native lever is the window size (model variant), a separate decision.

## Consistency / accuracy corrections applied during consolidation

- **Output is not negligible.** Corrected the parent intro's "12.5K output / 96% cache-read": real output ~5-8% (broken sidecar count), cache-read ~90-92%. Still context x turns-dominated, so the plan is unchanged, but P0.1 must re-confirm the figure.
- **Rotation trigger aligned to the ~200-250K doctrine band** (`TODO-Claude-Overnight-Runner.md:246`), not the parent's ~350K. Tune via canary.
- **"1c" corrected:** the resume path DOES consume the checkpoint via `section-checkpoint.py show` (`SKILL.md:162`); the enrich work (P4.2) is not dead weight. P4.3 reframed to "surface + act on the enriched fields," not "wire a reader that doesn't exist."
- **Counts normalized:** ~21 review rounds / ~67 Codex legs (~3/round) / 13 crashed legs (16 raw crash log lines). Round vs leg vs dispatch were used interchangeably in the source.
- **Savings are overlapping, not additive:** Phases 1-3 (churn/spiral) and Phase 4 (context-cap) both target the same ~50% cache-read area -- do not sum them. Phases 1-3 may make Phase 4's high-risk gate unnecessary.
- **Canary tiering (P0.0):** replaces the blunt "any control-plane change re-arms the watched canary" with `[det]` (deterministic-suite-gated) vs `[canary]` (live-flow) tags, so ~18 items ship on green tests in seconds instead of each waiting on a watched overnight run.
