# Token Saver v02 -- Cost Reduction Backlog (measured 2026-07-28 ->)

Successor to [`token-saver-v01.md`](token-saver-v01.md), whose H1 bounds it to measurements taken 2026-07-20 -> 2026-07-26. New cost findings belong here; a CORRECTION to an existing v01 item (a re-measurement, a revert, a closed premise) stays in v01 next to the item it corrects, so the history is not orphaned.

The v01 "Never cut" floor still applies unchanged: quality gates, adversarial review, bare-metal validation, and the trust contract are not cost levers.

---

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
