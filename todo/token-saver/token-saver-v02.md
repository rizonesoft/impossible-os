# Token Saver v02 -- Cost Reduction Backlog (measured 2026-07-28 ->)

Successor to [`token-saver-v01.md`](token-saver-v01.md), whose H1 bounds it to measurements taken 2026-07-20 -> 2026-07-26. New cost findings belong here; a CORRECTION to an existing v01 item (a re-measurement, a revert, a closed premise) stays in v01 next to the item it corrects, so the history is not orphaned.

The v01 "Never cut" floor still applies unchanged: quality gates, adversarial review, bare-metal validation, and the trust contract are not cost levers.

---

- [ ] **WHERE THE REMAINING MONEY IS: cache-read is 76.7% of the bill, and 86% of that is accumulated tool output, not skills or doctrine.**
  Measured 2026-07-28 on the costliest canary segment (run-064228, 325 turns, $220.41). This item exists so the next cost effort starts from the decomposition instead of re-deriving it, and so effort goes to the 86% rather than the visible-but-small 10%.

  | bucket | tokens | cost | share |
  |---|---|---|---|
  | main cache-read | 112,723,268 | $169.08 | **76.7%** |
  | main output | 252,168 | $18.91 | 8.6% |
  | sidechain cache-read | 8,755,265 | $13.13 | 6.0% |
  | main cache-write | 562,967 | $10.56 | 4.8% |
  | sidechain cache-write | 461,595 | $8.65 | 3.9% |

  Cache-read is `context_size x turns`, so the whole question is what sits in context on every turn. At 346,840 avg context/turn, the decomposition is: **skill injection ~35K tokens (10%)** -- 142,223 bytes across 6 repo-skill invocations, the largest being `implement-todo-section` 38.7 KB, `review-todo-section` 32.0 KB; **CLAUDE.md ~13K (4%)**, charged from turn 1; and **~300K (86%) accumulated conversation and tool results**.
  So the ranked levers are: (1) stop the accumulation, worth up to ~86% of 76.7%; (2) trim skill injection, worth ~$15/segment and ~$44/night -- real, but a tenth of the prize; (3) CLAUDE.md size, ~4%. Do not start with (2) because it is the easiest to see.
  **The fix for (1) is already written and NOT implemented: v01 T1-3's size trigger.** Only sub-item 1c (section-checkpoint plumbing, `bf3f8899`) landed. `run_phase_guard.py` still has exactly one rollover trigger -- R1, after a fully-SHIPPED section -- so a long section drags its whole context through every remaining turn, which is what T1-3's own evidence predicted ("600-660K before the section boundary arrives") and what this run reproduced (peaks 406,963 / 466,548 / 578,526). XREF: `token-saver/token-saver-v01.md` (item: "T1-3. Trigger rollover on CONTEXT SIZE, not only on section ship.").
  Two smaller confirmed leaks in the same segment, both already-shipped mechanisms that are not working: `agent_result_cache` 0 hits / 4 stores, and `review convergence 0/2 rounds suppressed` -- the T3-3 gate suppressed nothing across two re-dispatches.

- [ ] **Context per turn RISES across rollover segments (248K -> 294K -> 347K). Rollover is not resetting context, and that is the bill.**
  First post-T1 measurement, the attended canary run of 2026-07-28, via `scripts/overnight/cost-summary.py` over the three segment metrics files:

  | segment | turns | avg context/turn | peak | re-reads | hook fires | agent dispatches | cost |
  |---|---|---|---|---|---|---|---|
  | run-020308 | 256 | 248,041 | 406,963 | 38/63 (60%) | 73 | 1 | $123.74 |
  | run-035000 | 316 | 293,886 | 466,548 | 15/25 (60%) | 12 | 0 | $164.86 |
  | run-064228 | 325 | 346,840 | 578,526 | 53/82 (64%) | 26 | 4 | $220.41 |

  Rollover exists to reset the worker context, so the per-turn average should FALL after each one. It rises monotonically instead, ~40% across two rollovers, and cost tracks it exactly ($509 for the night, ~82% of it cache-read). Per-turn context IS the bill, so this is the largest single lever in either backlog -- a 347K -> 180K move is worth more than every other open cost item combined.
  Investigate the mechanism before proposing a fix. Candidate explanations, none yet tested: the relaunched worker re-reads a large orientation packet each segment (section-pack, brief, cursor state) that grows as the TODO accumulates stamps; the later segments worked a harder section (page-table lifetime) with more files in play; or the rollover checkpoint carries forward more state than intended. The segment-3 numbers are confounded by section difficulty, so a clean comparison needs two segments on comparable work.
  Two supporting facts from the same run, both worth resolving alongside: `read_cache_block` fired 26 times yet the re-read ratio stayed at 60-64% against a 20% target, so either it catches only a thin slice or most re-reads are legitimately different ranges (determine WHICH -- the answer decides whether T1-1 needs widening or retiring); and `agent_result_cache` recorded 5 stores and 0 hits, the same zero-hit symptom v01 T2-2 was supposed to fix.
  What DID work, for contrast: hook-fire budgeting (T1-4). Fires fell to 12-73 per segment from the 1,609 that motivated the item. That one is confirmed delivered.
  XREF: `token-saver/token-saver-v01.md` (item: "T4-2. Regression gate on the context metrics." -- now `[/]` Deferred, unblocks when this investigation yields a baseline worth defending). Its stated precondition is "after T1 lands, record the achieved baseline", and T1 did not reach its targets, so recording now would enshrine the miss as the baseline and then warn only at +25% of that.
  The measured size of that miss is published in the v01 POSTSCRIPT (T4-3, closed `[x]` 2026-07-28): against the pre-T1 baseline of 311,479 context/turn and a 64% re-read ratio, T1 delivered 299,988 (-3.7%) and 62% (-2pp), versus predictions of 180,000 (-42%) and 20% (-44pp). Prediction error ~11x and ~26x against T4-3's 2x threshold, so its verdict clause fired: the model of where cost goes is wrong. Explaining THAT is the point of this item.

- [ ] **Re-baseline every projected saving in v01: one of its items was reverted outright and the others were never measured post-change.**
  T2-3 shipped `search_offload_gate` on 2026-07-27 projecting a saving over ~669 rerouted searches, and was REVERTED on 2026-07-28 when the live run disproved its premise: every tool result lands in context, the Grep tool's included, so the blocked `grep -n X file | head -20` and the piped form it forced cost the same. The measured effect was three tool calls where one would do, and no context saved. So one line item in the v01 projection is not merely smaller than claimed, it is negative.
  This matters beyond that one item because v01 itself records that FIVE of its estimates were already materially wrong when re-checked (T1-2 off ~5x, T1-5 double-counted and closed, T2-3 ~25% smaller and now negative, T2-4 ~30x, T3-2 rationale expired). An aggregate projection built from estimates with that error rate is not a plan, it is a guess. Re-derive the remaining projections from a real post-change run before spending further effort against them.
  Concrete blocker: no post-change baseline exists. The T1-T4 work landed 2026-07-27 20:17 -> 2026-07-28, and the previous run ended 2026-07-26 03:36, so nothing in `.claude/overnight/metrics/` measures the current control plane. The 2026-07-28 canary run is the first, and `scripts/overnight/cost-summary.py` should be run against its metrics file as the new baseline. XREF: v01 T4-2 (regression gate on context metrics) and T4-3 (measure one section before/after) are both blocked on exactly this and should be re-scoped once the baseline exists.

- [ ] **Count the cost of operator/runner index contamination -- four cross-sweeps cost diagnosis turns on both sides.**
  Observed 2026-07-28 (four incidents in ~4 hours; see overnight-runner-improvements-v02 "Concurrency: operator and runner share ONE index"). The correctness angle is filed there. The COST angle belongs here and is unmeasured: each sweep cost the other party turns to notice HEAD had moved, re-read git state, and reason about whether its own work survived -- the runner spent an explicit turn concluding "HEAD moved because the operator committed and pushed `c305c165` mid-session; my 16 staged files sit intact on top of it".
  Worth measuring before fixing, because the fix (a separate `git worktree` for the unattended run) has its own cost: a second checkout of the tree plus a second build directory. If the contamination only bites when an operator works alongside the run -- which is not the normal unattended case -- the cheap fixes (drop `git add -A`, explicit `--` pathspec) may be the whole answer.

---

## Migrated from v01 (2026-07-28)

These arrived from `token-saver-v01.md` when that file was CLOSED. They are unchanged work, not new findings -- their original evidence and reasoning are preserved verbatim below. v01 is now a historical record and is not edited further.

- [ ] **T1-3. Trigger rollover on CONTEXT SIZE, not only on section ship.**
  **Evidence:** per-segment context/turn climbs to **600-660K** before the section boundary arrives
  (`run-20260725-052736` idx1/idx2 at 654K/660K; `run-20260725-020933` idx3 at 630K). The sequencer's only context-hygiene
  exit is a verified rollover after a **fully-shipped** section (`overnight-sequencer/SKILL.md:372`), so a long section
  drags a 600K context through every one of its turns.
  **Fix:** add a size trigger to `run_phase_guard.py`: at a step boundary, if the running context estimate exceeds a
  threshold (start at 250K), the next legal step is a `rollover-wip` -- commit-and-push the WIP, rotate, resume the same
  section from the section pack. The section pack + checkpoint machinery (`section-pack.py`, `section-checkpoint.py`)
  already exists to make the resume lossless.
  **Small steps, ported 2026-07-27 from `todo/overnight-runner-improvements/overnight-runner-improvements-v01.md` when that item closed** (safest-first; the
  relaxed gate 1e/1f is the only high-risk piece -- ship it last and canary it). **1c is the gap the original item
  glossed over:** `session_brief_inject.py` recomputes `runner_status.full_brief` and never reads
  `section-checkpoint.json`, so 1b is dead weight until 1c lands.

  - [x] **(1a) EXISTS BUT IS DISABLED -- correction 2026-07-27.** `.claude/hooks/rotate_hint.py` implements the turn-count proxy (`ROTATE_HINT_TURNS = 140`), but `ROTATE_HINT_ENABLED = False`:
    the hint is RETIRED (B1, Canary #2 2026-07-14) and never fires. An earlier note here called it live; that was wrong.
  - [x] **(1b) ALREADY DONE** (P4.2 enrichment in `gather()`): captures `phase`, `open_findings` (loc + decision + title, newest 10), `findings_recorded`,
    `decisions_indexed`, and a DERIVED `next_action`. Additive and fail-open per field, as specified.
  - [x] **(1c) SHIPPED 2026-07-27** -- `session_brief_inject.checkpoint_block()` appends the checkpoint to the resume brief.
    The gap was real and total: the hook had ZERO references to `section-checkpoint.json`, so every field 1b captures was dead weight and each resumed session re-derived facts already on disk.
    **Bound to the current cursor** (same TODO file AND same `section_idx`) -- a checkpoint for other work is worse than none, since it hands the worker confident stale facts. Emits nothing on mismatch, missing file, malformed JSON, or an empty body. 3 tests.
  - [ ] **(1d) Attended-canary the enriched re-orient.** Prove a resumed session re-orients from the brief WITHOUT re-deriving the same file:line facts. Gates whether 1a-1c pay off before any gate work is built.
  - [ ] **(1e) Parallel `_rollover_failures_wip()` gate (HIGH-RISK).** Accepts a committed-unpushed-unstamped tree, KEEPS the review-received + no-background-jobs checks. Never weaken shipped `_rollover_failures()`; it governs ship rollover too.
    **[RETIRED-PRECONDITION -- see the T1-3 correction note.]**
  - [ ] **(1f) Safe-boundary firing (HIGH-RISK).** Rotate only at a WIP-clean tree, between Codex rounds, or after a green fix-loop round. Forbid it during a review wait, an uncommitted edit, or mid-fix-loop (fix-then-regress guard).
    **[RETIRED-PRECONDITION -- see the T1-3 correction note.]**
  - [ ] **(1g) Tests for the WIP gate + boundary guard.** Rejects open review, active background job, and dirty tree; accepts committed-clean-unpushed; boundary guard blocks a mid-fix-loop rotation.
    **[RETIRED-PRECONDITION -- see the T1-3 correction note.]**
  - [ ] **(1h) Attended canary of full mid-section rotation before ANY unattended arm** (control-plane-manifest change mandates the green canary).
    **[RETIRED-PRECONDITION -- see the T1-3 correction note.]**

  **CORRECTION 2026-07-27 -- the mid-section mechanism this item proposes was already TRIED AND RETIRED.** The
  sequencer skill records it: *"Mid-section context-cap rotation -- RETIRED (B1, Canary #2 2026-07-14). Do NOT attempt
  a mid-section rotation; the `rotate_hint` reminder is disabled so it will not fire."* The canary proved the
  PRECONDITION never occurs: commit and push are ATOMIC at ship, so there is never a committed-but-unpushed WIP window
  for `rollover-wip` to rotate at. Steps 1e-1h therefore rest on a state the runner does not produce, and building them
  would repeat a disproved experiment. What survives and is genuinely useful is 1b + 1c: a richer checkpoint that a
  resumed worker actually reads, which improves the EXISTING per-section ship rollover. 1d still gates that.
  **Revisit condition:** only if the ship flow changes so that a WIP window exists (commit without push), or a
  different rotation trigger is found that does not need one.

  **Gates another item:** `todo/overnight-runner-improvements/overnight-runner-improvements-v01.md` "a REFUSED rollover must BLOCK starting the next section"
  is BLOCKED-ON this item by decision 2026-07-27. That one hard-blocks the next section on a refused rollover, which can
  wedge an unattended run (its escape path, defer-with-state, does not exist). T1-3 attacks the same context balloon from
  the mid-section side and cannot wedge, because it fires only at safe boundaries. Land T1-3, re-measure boundary-side
  ballooning, and only then decide whether the hard block is still needed.

  **Sharpens:** `todo/overnight-runner-improvements/overnight-runner-improvements-v01.md` "Context-cap rollover ... (Path B)" -- that item is scoped as a flow
  fix; this adds the measured threshold and the arithmetic that justifies it.
  **Expected saving:** caps the worst-case multiplier. On the measured distribution, holding steady state at 250K instead
  of letting it run to 650K is **~15-20%**.
  **Acceptance:** no section resumes with a lost finding, lost review receipt, or lost stamp; `rollover-wip` never fires
  mid-review-round (only at a step boundary with a clean gate).

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
  [docs/infrastructure/skill-authoring.md "Progressive disclosure"](../../docs/infrastructure/skill-authoring.md).

  **Acceptance (met for the 3 split):** no skill's mandatory step list, gate, or hook trigger removed -- only
  relocated; catalog + hook audits pass; step sequences verified identical.

- [/] **T4-2. Regression gate on the context metrics.**
  > **Deferred:** 2026-07-28 | precondition failed -- T1 landed without reaching its targets, so there is no baseline worth gating on -> XREF: `token-saver/token-saver-v02.md` (item: "Context per turn RISES across rollover segments (248K -> 294K -> 347K)")
  **DEFERRED 2026-07-28 on the context-growth investigation in [`token-saver-v02.md`](token-saver-v02.md). Do not implement as written.** This item's precondition is "after T1 lands, record the achieved baseline", and T1 landed without reaching its targets. First post-T1 measurement (attended canary run, three segments): average context/turn **248K -> 294K -> 347K** against a 180K target, and re-read ratio **60% / 60% / 64%** against a 20% target -- a 1.9x and 3.2x miss. Recording that as "the achieved baseline" and warning at +25% would enshrine the miss as the standard and only complain once things got 25% worse than triple target.
  Two further reasons to hold. Sub-item **(5d)** already gates the budget backstop "behind the context-cap + review-spiral fixes landing", and the same measurement shows the context cap has not effectively landed -- so 5a-5e are blocked by their own stated condition. And the gate is warning-only by design ("it can only report"), so adding a report while the underlying metric is 2-3x off repeats the `interactive_offload_router` pattern (487 injections, zero follows) that T1-4 exists to correct.
  Unblock condition: the v02 investigation explains why per-turn context RISES across rollovers, and a baseline worth defending exists. Hook-fire budgeting did deliver (12-73 per segment against 1,609), so that third target is already met and can be gated independently if useful.
  **Fix:** after T1 lands, record the achieved baseline (target: average context/turn <= 180K, re-read ratio <= 20%,
  hook fires <= 400/run). A run that regresses more than 25% past baseline writes a WARN into
  `.claude/overnight/NEEDS-OPERATOR.md`. Warning only -- never block a run on a cost metric.
  **Acceptance:** the gate cannot stop shipping work; it can only report.

  **Budget backstop, ported 2026-07-27 from `todo/overnight-runner-improvements/overnight-runner-improvements-v01.md`.** Land AFTER T1/T2 bring the baseline
  down -- a runner that mostly sleeps ships nothing, so this must never become the primary strategy.

  - [ ] **(5a) Track cumulative 7-day token burn** in the arm/launch layer (rolling window sourced from run reports).
  - [ ] **(5b) Project 7-day burn** from the current rate; surface it in the status brief / monitor.
  - [ ] **(5c) Self-snooze until reset** via the existing snooze/backoff plumbing when projected burn would exceed 100% of the weekly budget.
  - [ ] **(5d) Add an activation floor** so the backstop can't dominate; gate it behind the context-cap + review-spiral fixes landing.
  - [ ] **(5e) Test the project -> snooze -> resume transition** at the ceiling.
