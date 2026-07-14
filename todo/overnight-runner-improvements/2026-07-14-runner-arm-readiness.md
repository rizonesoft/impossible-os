# Overnight Runner -- Arm-Readiness Worklist (2026-07-14)

> Carry-forward from the two 2026-07-1x runner-improvement files (the P0-P6 plan and the runlog-efficiency
> findings), both **CLOSED "not now" 2026-07-14**. Nearly all of that work SHIPPED (Stages 0-4: metrics,
> review-gate B3, F1, B4/F2, P3.1-P3.4, C-RECV, and the Phase-4 context-cap rotation code). This file holds
> ONLY the items that must happen **before an unattended 24/7 arm** -- the validation gates and one
> in-flight review. Everything else was deferred as backlog (see the two files' close banners). Same naming
> discipline (not `TODO-*.md`, not in a `todo/NN-domain/` dir) so the sequencer + todo-graph never parse it.

## Arm-readiness gate (do these before any unattended arm)

- [ ] **[review] A1. Triage + fix the P4.5/P4.6 (mid-section WIP rollover) adversarial review.**
  DISPATCHED 2026-07-14 (`.claude/overnight/reviews/20260714-082529-adversarial.out`, correctness framing
  per J3), IN FLIGHT at close-out. It attacks the one property that matters: can `_rollover_failures_wip()`
  accept a state where in-progress work is LOST/CORRUPTED across a rotation, can rotation fire mid-fix-loop
  / during a review wait, and does it weaken the shipped `_rollover_failures()` ship gate. When it lands:
  process through `Skill(superpowers:receiving-code-review)`, verify each finding at file:line, fix the
  valid ones (the C-RECV precedent found 3 real HIGH spoofs in an analogous gate), re-test. Code: `5975d72a`.

- [ ] **[canary] A2. C-RECV confirmation canary -- prove zero-`SKIP_*`.**
  Canary #1 validated the Stage 2/3 cluster but ended with ONE `SKIP_REVIEW_HOOK` (the `received:true`
  completion-binding gap). C-RECV (`ee9c8404`, hardened after its own review found 3 HIGH spoofs) closes it.
  A short WATCHED canary of a FIND-AND-FIX section should now ship with a clean re-adversarial AUTO-RECEIVED
  and **zero opt-outs** -- Canary #1's deferred success criterion. Arm `--force`, watch one section ship +
  one rollover, confirm no `SKIP_*` in the log.

- [ ] **[canary] A3. Canary #2 -- attended run of the full mid-section rotation (was P4.4 + P4.8).**
  Phase-4 code (rotate-hint, enriched checkpoint, WIP gate + `rollover-wip`) is built + unit-tested (suite
  42/42) but NOT canaried. Before arming Phase-4 rotation unattended: (a) P4.4 -- a resumed worker
  re-orients from the enriched checkpoint (`next_action` + `open_findings`) WITHOUT re-deriving; (b) P4.8 --
  a full mid-section `rollover-wip` fires only at a WIP-clean boundary, rotates, and the fresh worker
  resumes the SAME section safely. Do A1 (fix WIP-gate findings) -> A2 -> A3.

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
