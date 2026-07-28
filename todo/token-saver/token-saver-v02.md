# Token Saver v02 -- Cost Reduction Backlog (measured 2026-07-28 ->)

Successor to [`token-saver-v01.md`](token-saver-v01.md), which is CLOSED and is a historical record. New cost findings belong here.

The v01 "Never cut" floor still applies unchanged: quality gates, adversarial review, bare-metal validation, and the trust contract are not cost levers.

**This file is deliberately short.** v01 grew to 16 items, five of whose estimates were materially wrong on re-check and one of which shipped negative and was reverted. The lesson recorded in the v01 POSTSCRIPT is that a long backlog of projected savings is a guess with a ~10x error bar. Everything below is measured on a real run, and anything not measured is not here.

---

## The one thing that matters

- [ ] **Context per turn RISES across rollover segments (248K -> 294K -> 347K). Rollover is not resetting context, and that is the bill.**
  Measured on the attended canary run of 2026-07-28 via `scripts/overnight/cost-summary.py`, three segments:

  | segment | turns | avg context/turn | peak | re-reads | hook fires | cost |
  |---|---|---|---|---|---|---|
  | run-020308 | 256 | 248,041 | 406,963 | 38/63 (60%) | 73 | $123.74 |
  | run-035000 | 316 | 293,886 | 466,548 | 15/25 (60%) | 12 | $164.86 |
  | run-064228 | 325 | 346,840 | 578,526 | 53/82 (64%) | 26 | $220.41 |

  **Why this is the whole game.** On the costliest segment the bill decomposes as: main cache-read $169.08 (**76.7%**), main output $18.91 (8.6%), sidechain cache-read $13.13 (6.0%), main cache-write $10.56 (4.8%), sidechain cache-write $8.65 (3.9%). Cache-read is `context_size x turns`, so per-turn context IS the cost. At 346,840 avg the content splits roughly: skill injection ~35K tokens (10%), CLAUDE.md ~13K (4%), and **~300K (86%) accumulated conversation and tool results**. Attack the 86%; the 10% is visible and cheap to chase and worth about a tenth as much.
  Rollover exists to reset the worker context, so the per-turn average should FALL after each one. It rises monotonically instead, ~40% across two rollovers, and the night cost $509 with ~82% of it cache-read.
  **The mechanism is UNKNOWN and must be found before any fix is designed.** Candidate explanations, none tested: the relaunched worker re-reads an orientation packet each segment (section-pack, brief, cursor state) that grows as the TODO accumulates stamps; the later segments worked a harder section with more files in play; or the rollover checkpoint carries forward more state than intended. Segment 3 is confounded by section difficulty, so a clean comparison needs two segments on comparable work.
  **Do NOT reach for a mid-section context-cap rotation as the fix.** That mechanism was already tried and retired -- `overnight-sequencer/SKILL.md:381` records "Mid-section context-cap rotation -- RETIRED (B1, Canary #2 2026-07-14)" and `rotate_hint.py` still carries `ROTATE_HINT_ENABLED = False`. The canary proved its precondition never occurs: commit and push are atomic at ship, so there is no committed-but-unpushed window to rotate at. An earlier draft of this item named that trigger as "the fix already written"; that was wrong and is corrected here.
  **Acceptance:** the mechanism is identified with evidence from a real run, and the fix is chosen on that evidence. A 347K -> 180K move is worth more than every other cost item combined; nothing else in this file comes close.

---

## Two shipped mechanisms that are not working

Both are small, concrete, and already built -- they simply do not do their job. Neither is a projection.

- [ ] **`agent_result_cache`: 0 hits against 5 stores on the canary run, the same zero-hit symptom v01 T2-2 was written to fix.**
  T2-2 shipped a scope-fingerprint fix on 2026-07-27 after measuring 155 stores and 0 hits all-time. The 2026-07-28 run recorded 5 stores and still 0 hits. Either the key is still too narrow to ever match, or dispatches genuinely never repeat within a run -- and those call for opposite responses, so determine WHICH before touching the code. If dispatches never repeat, the cache is dead weight and should be retired rather than repaired.

- [ ] **Review convergence suppressed 0 of 2 rounds -- the T3-3 gate did nothing.**
  The convergence gate exists to skip a re-dispatch when the review kind and inputs are unchanged. On the canary run it suppressed nothing across two re-dispatches. Same determination first: is the gate mis-keyed, or were those two rounds genuinely different inputs (in which case the gate is correct and the item is closed as a non-finding)?

---

## Housekeeping

- [ ] **Count the cost of operator/runner index contamination -- four cross-sweeps cost diagnosis turns on both sides.**
  Observed 2026-07-28 (four incidents in ~4 hours; correctness angle filed in `overnight-runner-improvements-v02.md`). The COST angle is unmeasured: each sweep cost the other party turns to notice HEAD had moved and reason about whether its work survived. Worth measuring before fixing, because the strong fix (a separate `git worktree` for the run) costs a second checkout and build directory. If it only bites when an operator works alongside a run, the cheap fixes may be the whole answer.

---

## Retired from v01 on 2026-07-28 -- do not resurrect without new evidence

These migrated across when v01 closed and are retired after review. Recorded so the reasoning is not lost and the same work is not re-proposed.

- **T1-3 (context-size rollover trigger) -- RETIRED, mechanism disproved.** Its own correction note already said so: the mid-section rotation was tried and retired by canary on 2026-07-14 because the committed-but-unpushed WIP window it needs never occurs. Sub-items 1e-1h were already marked `[RETIRED-PRECONDITION]`. What was genuinely useful (1b/1c, a richer checkpoint a resumed worker actually reads) SHIPPED in `bf3f8899`. Only 1d survived -- an attended canary of the enriched re-orient -- which is not worth a standing item; it will be observed on the next run for free. **Revisit condition unchanged: only if the ship flow changes so a WIP window exists.**
- **T1-2 (skill diet) -- RETIRED as low-value, not as wrong.** Partially shipped 2026-07-27 (the 4 highest-cost skills, ~327 KB saved). Its own correction revised the ceiling from 60% to ~15%, and the 2026-07-28 decomposition puts total skill injection at ~10% of per-turn context, worth ~$44/night against the 86% that is accumulated tool output. The 10 unsplit skills stay a "do it opportunistically when editing that skill anyway" note in [skill-authoring.md](../../docs/infrastructure/skill-authoring.md), not a tracked item.
- **T4-2 (regression gate on context metrics) -- RETIRED, precondition failed and the need is met for free.** Its premise was "after T1 lands, record the achieved baseline", and T1 landed at 347K/64% against 180K/20% targets. Gating at +25% of a 1.9x-3.2x miss would enshrine the miss. Its sub-item (5d) independently blocked the budget backstop behind a context-cap fix that has not landed. And the measurement it wanted is free: `cost-summary.py` already runs per segment, so a multi-day run re-baselines without a gate. Rebuild only if the numbers stop being read.
