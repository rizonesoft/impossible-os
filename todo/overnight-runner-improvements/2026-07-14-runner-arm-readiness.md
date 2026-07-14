# Overnight Runner -- Arm-Readiness Worklist (2026-07-14)

> Carry-forward from the two 2026-07-1x runner-improvement files (the P0-P6 plan and the runlog-efficiency
> findings), both **CLOSED "not now" 2026-07-14**. Nearly all of that work SHIPPED (Stages 0-4: metrics,
> review-gate B3, F1, B4/F2, P3.1-P3.4, C-RECV, and the Phase-4 context-cap rotation code). This file holds
> ONLY the items that must happen **before an unattended 24/7 arm** -- the validation gates and one
> in-flight review. Everything else was deferred as backlog (see the two files' close banners). Same naming
> discipline (not `TODO-*.md`, not in a `todo/NN-domain/` dir) so the sequencer + todo-graph never parse it.

## Arm-readiness gate (do these before any unattended arm)

- [x] **[review] A1. Triage + fix the P4.5/P4.6 (mid-section WIP rollover) adversarial review. DONE 2026-07-14.**
  The review (`20260714-082529-adversarial.out`) returned needs-attention / do-not-ship with 3 HIGH + 1
  MEDIUM, all verified valid + fixed (commit `e282332c`, received through `receiving-code-review` first):
  F1 -- rollover-wip was an alternate ship path (cleared `rollover_refused`, no phase guard) -> require
  phase==SECTIONS, refuse if `rollover_refused` set, never clear it; F2 -- authorization survived a failed
  checkpoint write -> `_write_section_checkpoint()` first, fail-closed; F3 -- `received:true` did not prove
  the COMMITTED WIP was reviewed -> `_review_not_binding_head()` requires trigger_blobs to match HEAD; F4 --
  `_dirty_owner` classified by pathname only -> parse porcelain XY status, tolerate ONLY ` M` of a tracked
  generated file (also tightens the ship gate's B4/F2 tolerance). Tests grew to 13; suite 42/42. Same
  pattern as C-RECV: the review of the review-*gate* caught real spoofs a doctrine-only build would ship.

- [ ] **[canary] A2. C-RECV confirmation canary -- prove zero-`SKIP_*`.**
  Canary #1 validated the Stage 2/3 cluster but ended with ONE `SKIP_REVIEW_HOOK` (the `received:true`
  completion-binding gap). C-RECV (`ee9c8404`, hardened after its own review found 3 HIGH spoofs) closes it.
  A short WATCHED canary of a FIND-AND-FIX section should now ship with a clean re-adversarial AUTO-RECEIVED
  and **zero opt-outs** -- Canary #1's deferred success criterion. Arm `--force`, watch one section ship +
  one rollover, confirm no `SKIP_*` in the log.

- [ ] **[canary] A3. Canary #2 -- attended run of the full mid-section rotation (was P4.4 + P4.8).**
  Phase-4 code (rotate-hint, enriched checkpoint, WIP gate + `rollover-wip`) is built + unit-tested (suite
  42/42) but NOT canaried. **Prereq SHIPPED 2026-07-14:** the `rollover-wip` CONSUMER wiring was missing
  entirely (the hint only wrote a flag file no one read; no doctrine called the verb), so the rotation
  could never fire. Built the advisory `rotate_hint` reminder + the SECTIONS-phase doctrine (commits
  `9cd942f2`, `c153c48f`, + the r2-fix commit below); two adversarial rounds found + fixed 4 then 2 issues
  (A2 session-leak, A4 emit-after-persist, A1 unpushed guard, doctrine mismatches -- all real, a
  doctrine-only build would have shipped the counter-pollution that would have invalidated this canary).
  Before arming Phase-4 rotation unattended: (a) P4.4 -- a resumed worker re-orients from the enriched
  checkpoint (`next_action` + `open_findings`) WITHOUT re-deriving; (b) P4.8 -- a full mid-section
  `rollover-wip` fires only at a WIP-clean boundary, rotates, and the fresh worker resumes the SAME section
  safely. Do A1 -> A2 -> A3.

- [x] **[unattended-gate] A4. rollover-wip: a RELIABLE section-shipped signal (round-2 A1, HIGH). DONE 2026-07-14.**
  The `unpushed>0` guard refused the dominant post-ship boundary but a stamped-but-NOT-yet-pushed section
  still passed it. The reviewer's `section_idx`-based check was not reliably implementable (it is a loose
  display value). Instead keyed on the DEFINITIVE ship marker: `_head_adds_ship_stamp()` returns True when
  HEAD's `git show -- todo` diff adds a `**Verified:**` / `**Quality reviewed:**` line (same `_STAMP_ADDED_RE`
  the commit-gate uses), and `rollover-wip` refuses on it -- robust vs `section_idx`, closing the exact
  pre-push window. Tests: `test_head_adds_ship_stamp_detects_verified_stamp`, `test_rollover_wip_refused_on_ship_stamp_commit`.

- [x] **[unattended-gate] A5. rollover-wip: a durable review-resolution boundary (round-2 A3, HIGH). DONE 2026-07-14.**
  Built the receipt Codex asked for, minimally: a new `review-resolved` verb records a content-bound receipt
  `{head, review_run_id, ts}` ONLY after verifying the last review is received + binds HEAD (F3) + build & test
  receipts are content-valid (`_build_suite_receipts_ok`, reusing `receipts.py`); `rollover-wip` then refuses
  unless `_review_resolution_valid()` finds a receipt matching current HEAD (fail-closed on absent/stale). The
  SKILL doctrine + the `rotate_hint` reminder now instruct `review-resolved` THEN `rollover-wip`. Tests:
  `test_review_resolution_valid_head_bound`, `test_rollover_wip_refused_without_resolution_receipt`, and 4
  `review-resolved` verb cases (happy path, unbound review, red receipts, outside-SECTIONS).

- [x] **[bug] A6. Latent: `import subprocess` inside `cli()` shadowed the module global. FOUND+FIXED 2026-07-14.**
  While unit-testing A5 I hit `UnboundLocalError: subprocess` in the new verb. Root cause: an `import
  subprocess` inside the `fixpoint` cmd block (`run_phase_guard.py:926`) made `subprocess` FUNCTION-LOCAL
  across ALL of `cli()` (Python static scoping), so every bare `subprocess.run` reached before that line
  raised. This ALSO silently broke the full `rollover`'s inline `section-checkpoint.py write` (its
  `except: pass` swallowed the `UnboundLocalError`, so that checkpoint was never written). Fix: removed the
  redundant inner import. `_write_section_checkpoint` (the WIP-rollover helper) is module-scope and was
  unaffected, so P4.2/P4.5 were OK; only the FULL rollover's inline write was hit.

## Unattended-arm gates from the A4/A5 review (round 3, 20260714-103035) -- fix before UNATTENDED

> All three verified valid; NONE is critically broken for an ATTENDED canary (the gates work for the normal
> flow; each bypass needs a specific sequence that will not arise in a watched run). Deferred to the next
> unattended pass by operator decision 2026-07-14.

- [ ] **[unattended-gate] A7. `_head_adds_ship_stamp` only examines HEAD + fails open (HIGH).**
  A stamp commit followed by an unpushed FIXUP leaves `unpushed>0` but HEAD no longer shows the stamp, so the
  weaker WIP path proceeds (`run_phase_guard.py:561-576`; the follow-up-commit-returns-False behavior is
  codified at `test_p45_wip_rollover.py:242-246`). It also fails OPEN on git error/timeout/nonzero exit. Fix:
  scan the cumulative `@{u}..HEAD` range for the stamp (or persist a durable section-shipped marker), and
  return an unknown/error state that `rollover-wip` REFUSES (fail-closed).

- [ ] **[unattended-gate] A8. `review-resolved` does not prove findings were resolved (HIGH).**
  It writes a resolution receipt for a received-but-UNFIXED findings-bearing review when the unchanged blobs
  still match HEAD and existing build/test receipts are green; and with an absent/`{}` review record it skips
  both review checks and writes a receipt with an EMPTY run ID (`run_phase_guard.py:1069-1106`). Fix: require
  a present, well-formed current review with a nonempty `review_run_id` + machine-verifiable resolution
  evidence (a clean post-fix review, or complete finding dispositions bound to that run + HEAD).

- [ ] **[unattended-gate] A9. Resolution receipt ignores `review_run_id` (HIGH).**
  `_review_resolution_valid` checks only `receipt.head`, so a resolved receipt for review A at HEAD H stays
  valid after a newer findings-bearing review B is received at the SAME H -- the WIP gate then rotates before
  B is resolved (`run_phase_guard.py:608-631`). Fix: require `receipt.review_run_id` == the latest review
  state's `run_id` (nonempty), and invalidate the receipt whenever a new review is dispatched.

## Deferred backlog (closed "not now" 2026-07-14; re-open if the need resurfaces)

- **P3.5** -- reviewed-`[/]`-partial clean ship path. Largely SUBSUMED by B3 + C-RECV (the SKIP-deadlock it
  targeted is closed). Re-open only if a partial-ship still forces an opt-out after A2.
- **P3.4 expensive-exploration half** -- WARN->BLOCK for full-log greps / >50 KB reads / 3+-round inline
  exploration needs a SECTIONS-scoped PreToolUse gate (higher wedge risk); the build/test half shipped.
- **G3** -- a `section-manifest`/`sequencer_triage` cross-TODO prerequisite-readiness pre-check.
- **H1** -- a compile-time `DEBUG_EXEC_TRACE=1` toggle for exec/fork klog bisection (kernel work).
- **J2c** -- the Bash guard that false-matched a benign `waiver`/`split` grep; needs the exact transcript command.
- **J2d** -- long Codex review latency (~13-33 min); a Codex-behavior / P2-convergence concern.
- **J3** -- adversarial reviews of security-gate code use a correctness framing (recorded doctrine; applied to A1).
