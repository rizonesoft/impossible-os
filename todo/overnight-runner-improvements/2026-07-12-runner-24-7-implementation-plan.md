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
  SHIPPED 2026-07-12. `stream-report.py`: (1) EOF/error flush via try/finally in `main()` -- a trailing section with no progress/result marker is now captured (marker `eof`) instead of vanishing; (2) output accounting fixed -- `add_usage` now keeps the MAX per message-id per field instead of first-seen, because output_tokens GROWS across the per-content-block events (input/cache-read are stable, which is why only output was wrong); (3) atomic live snapshot (`<metrics>.live`, temp+os.replace) rewritten each turn so a crash mid-section keeps the partial; (4) every record + snapshot now carries `run_id` / `todo` / `section` / `start_sha` / `end_sha`. Tests: +4 cases in `test_stream_report_metrics.py` (13 total), suite 20/20 green. NOTE: the ~588,840 figure could NOT be re-confirmed against that past run -- the raw stream-json feeds `stream-report` directly (`overnight-launch.sh:344-351`) and is not retained, only the formatted log is; the fix's correctness is proven instead by `test_growing_output_uses_max_per_msg` (max 500 vs first-seen 2), and the NEXT run's metrics will be trustworthy. Original scope:
  `stream-report.py` flushes only at `progress`/`final`, so the §13 tail (08:22-10:35) is absent from the jsonl. Fix: flush at EOF/error, atomic live snapshot, record final/max usage per message, stamp run/TODO/section/start+end SHA. Re-confirm the ~588,840 output figure vs raw API records. Prerequisite for measuring every fix below and for the Phase-5 governor.

### Phase 1 -- Pure-correctness wins (safe; zero quality risk; high value)

- [x] **[det] P1.1 Crashed-leg re-dispatch: on `rc=1`, re-run ONLY the crashed leg(s), never the full bundle.**
  SHIPPED 2026-07-12. Complete across all review paths: the crash-aware envelope (`review-envelope.py` `needs_redispatch`/`all_clean`) + `test_review_envelope.py` (this session); the broker (`review-broker-codex-dispatch.sh`) is already per-kind so the runner re-invokes it per crashed kind (no `--legs` arg needed); doctrine wired into the two broker-using skills (review-todo-section, overnight-sequencer) + a "crash != verdict" note in the two single-dispatch skills (codex-adversarial-review-section, codex-fix-review). Suite 21/21. Original scope:
  The crash-aware envelope (`review-envelope.py` `needs_redispatch`/`all_clean`) + tests shipped this session. Remaining (parent 2a-2e): broker `--legs` arg, per-leg verdict persistence, reuse-clean-verdicts, wire into the review skills, test single-leg-crash. Pure waste removal, zero quality cost.
- [ ] **[det] P1.2 Bind `last-codex-review.json` to the actual staged diff (paths + head SHA); reject non-intersecting records.**
  Quality-POSITIVE: removes the false blocks that forced the 2 `SKIP_REVIEW_HOOK` bypasses. Sub-steps 3a-3e in parent. Do alongside P1.1 -- both attack the review-receipt churn.
- [ ] **[det] P1.3 Make Codex receipts completion-bound, not dispatch-bound (transactional broker state).**
  Teach `is_background_dispatch` (`_codex_dispatch.py:223`) the `review-broker-codex-dispatch.sh` shape so its immediate return is not mistaken for a completed review. Model the receipt as `running -> completed (rc=0) -> received -> content-valid`; only `content-valid` satisfies the commit gate. Root of the commit-gate disagreement; complements P1.1 + P1.2.
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

- [ ] **[det] P2.1 Convergence-based review: never redispatch an UNCHANGED review kind on UNCHANGED relevant inputs.**
  Primary mechanism. Continue a kind while it yields a NEW Critical/High; stop when neither its file set nor content moved since its last verdict; disposition Medium/Low via Fix/Reject/Accept. NO round cap (see Rejected). Absorbs parent 4a-4e; the round-8/12 spiral alarm becomes a secondary signal. Implement as a deterministic redispatch-decision gate + test.
- [ ] **[det] P2.2 Per-kind x per-file review invalidation: one changed file must not re-trigger ALL kinds.**
  Docs/TODO-only fix reruns nothing (or consistency); kernel-path change reruns adversarial + affected consistency, NOT perf; hot-path change reruns perf. Mechanical half of P2.1.
- [x] **[det] P2.3 Standing evidence map across review rounds (route round-N verification through `review-evidence-mapper` for rounds >= ~4).**
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

- [ ] **[det] P5.1 Track cumulative 7-day token burn** in the arm/launch layer (rolling window from run reports; needs P0.1 metrics).
- [ ] **[det] P5.2 Project 7-day burn** and surface it in the status brief / monitor.
- [ ] **[det] P5.3 Self-snooze until reset** via the existing snooze/backoff plumbing when projected burn would exceed 100% of the weekly budget (needs the P1.4 snooze fix).
- [ ] **[det] P5.4 Activation floor** so the backstop can't dominate; gate it behind Phases 1-4 landing (a runner that mostly sleeps ships nothing).
- [ ] **[det] P5.5 Test the project -> snooze -> resume transition** at the ceiling.

### Phase 6 -- Blocked-item recovery completeness (correctness gap; surfaced 2026-07-12 canary)

Not a cost item -- a completeness gap in the fixpoint loop, found while explaining the runner's
blocked-item behavior. **Terminal-park** cross-TODO deferrals (`[/]` + a plain `Deferred:` / `Accepted:`
+ XREF, with NO `awaiting-<token>`) are DONE-equivalent to the triage oracle (`sequencer_triage.py`
3-state model), so the runner never re-visits them. When the XREF owner section later ships, the
owner's `implement/review` step-18 inbound Accepted/Deferred sweep only TIDIES the stale stamp -- it
never re-opens the now-unblocked dependent work. Net: cross-TODO work that has BECOME runnable is
stranded until a human flips it back to `[ ]`. (The `awaiting-<token>` recoverable-park and plain
`[ ]` blocker-noted items do NOT have this gap -- only terminal-park `[/]` deferrals do.)

- [ ] **[canary] P6.1 Owner-side sweep re-opens unblocked dependents (not just tidies the stamp).**
  When a section ships and its step-18 inbound sweep finds an Accepted/Deferred stamp whose concern
  this section just FULLY satisfied, flip the dependent item from `[/]` back to `[ ]` -- re-classifying
  its file NEEDS_WORK for the next fixpoint pass -- instead of only deleting the stale line. Gate the
  flip on the sweep's existing fully-vs-partially-resolved decision: only a fully-resolved concern
  flips; a partially-resolved one keeps its remaining XREFs untouched. Live-flow behavior (re-opens
  work mid-run, changes what the runner picks next pass) -- canary.
- [ ] **[det] P6.2 Deterministic stranded-deferral audit.**
  A `todo-graph` / `sequencer_triage` verb that enumerates every terminal-park `[/]` deferral whose
  XREF owner section is now DONE (both stamps present) but which is still parked -- the
  stranded-unblocked backlog. Reuse `query.py deferred-by` + the stamp resolver + the section DONE
  oracle. Surfaces the gap on the CURRENT tree today, validates P6.1, and feeds P6.3. Deterministic +
  unit-testable, no live run needed.
- [ ] **[det] P6.3 Fixpoint DONE-check consults the stranded-deferral audit.**
  Before `run_phase_guard.py fixpoint` declares true DONE, run the P6.2 audit; a non-empty stranded
  set is NOT-fixpoint (re-open the dependents + `next-pass`), not a false finish. Safety net for when
  the owner-side sweep (P6.1) misses a satisfy-mapping, and it closes the "runner stops with
  recoverable cross-TODO work still parked" hole directly. Deterministic gate + test.

- [ ] **[det] P6.4 One-time backfill sweep of the EXISTING stranded backlog (run P6.2, then act by stamp type).**
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
