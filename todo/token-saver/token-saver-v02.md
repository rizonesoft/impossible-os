# Token Saver v02 -- Cost Reduction Backlog (measured 2026-07-28 ->)

Successor to [`token-saver-v01.md`](token-saver-v01.md), which is CLOSED and is a historical record. New cost findings belong here.

The v01 "Never cut" floor still applies unchanged: quality gates, adversarial review, bare-metal validation, and the trust contract are not cost levers.

**This file is deliberately short.** v01 grew to 16 items, five of whose estimates were materially wrong on re-check and one of which shipped negative and was reverted. The lesson recorded in the v01 POSTSCRIPT is that a long backlog of projected savings is a guess with a ~10x error bar. Everything below is measured on a real run, and anything not measured is not here.

---

## The one thing that matters

- [x] **DIAGNOSED 2026-07-28: rollover works. The rising `avg context/turn` was LENGTH CONFOUND, and the real finding is that cache-read cost is QUADRATIC in segment length.**
  The original item claimed "rollover is not resetting context". That premise is disproved. Diagnosis ran against the three canary session transcripts themselves (`~/.claude/projects/-home-derickpayne-projects-impossible-os/`: `d865d802`, `3a6a2283`, `e711ac7b`), reading per-message `input + cache_read + cache_creation` rather than the per-segment aggregate the metric reports.

  **Evidence 1: every segment starts at the same context.** Opening context was 59,681 / 57,539 / 57,498 tokens. Rollover resets the worker completely, every time, and the spread across the night is 3.8%. This kills the leading hypothesis outright: the orientation packet (section-pack, brief, cursor state) is NOT growing as the TODO accumulates stamps.
  **Evidence 2: context never falls inside a segment.** Zero drops greater than 50K in any of the three transcripts; growth is strictly monotone for 2.5 hours. Nothing trims, and auto-compaction never fires (peak 580K against a 1M window).
  **Evidence 3: length-matched, the segments are identical.** Truncated to a common 423 assistant messages, mean context was 244,332 / 243,832 / 254,769 -- a 4.5% spread, against the 40% spread the raw averages showed. Over that same window the accumulation slope was 832 / 808 / 792 tokens per message: monotonically DECREASING, so the later segments were marginally more efficient per turn, not less. The three segments ran 423 / 525 / 653 assistant messages (256 / 316 / 325 metrics-turns), and that difference is the entire effect.

  **The mechanism, stated exactly.** Context grows monotonically from a ~57.5K base at a near-constant rate and is never trimmed, so `avg context/turn ~= (start + end) / 2` is a LINEAR FUNCTION OF SEGMENT LENGTH. Total cache-read is `base x T + sum(delta_i x (T - i))`, which is QUADRATIC in T because every token added at turn `i` is re-read on each of the remaining `T - i` turns. Modelled against the costliest segment that gives 18,629,352 + 94,600,413 = 113,229,765 token-reads versus 112,723,268 measured (0.4% error), so the model is validated: 84% of the bill is re-reading accumulated content and 16% is the static base. One segment equals one section (`overnight-sequencer/SKILL.md:359`, rollover after every fully-shipped section), so `avg context/turn` was measuring how big the section was, never whether rollover worked.

  **What accumulates, by the tool whose result landed, on the costliest segment (522,274 tokens added, weighted by the turns that must re-read it).** Bash 290,714 raw tokens / 45.7% of re-read cost / $64.82 over 207 calls; Read 62,142 / 17.3% / $24.55 over 30 calls; Edit 83,552 / 16.6% / $23.53 over 71 calls; Skill 70,162 / 16.0% / $22.75 over 12 calls; Agent 15,091 / 4.2% / $5.96 over 3 calls. Note that 108 of the 207 Bash calls were `bash_search`, which is Bash-as-grep and already forbidden by the Grep-tool-over-Bash-grep floor in CLAUDE.md.

  **The fix chosen on this evidence, in value order.** (1) BOUND SEGMENT LENGTH. Because cost is quadratic in T, the exact counterfactual replayed from the real trace is that the same work split across 2 sections costs 59% of the actual cache-read and across 3 sections costs 44%. Nothing else on this page is worth as much. This is NOT the retired mid-section rotation and needs no committed-but-unpushed window: it is the section SPLIT that `overnight-sequencer/SKILL.md:388` already prescribes ("a section too big for one context should be SPLIT... not rotated mid-way"), which until now had no cost calibration to size its thresholds against. (2) ATTACK THE BASH SLOPE, worth 46% of the re-read bill, where over half the calls are searches doctrine already routes elsewhere.
  **Reporting fix SHIPPED with this diagnosis.** `cost-summary.py` printed `avg context/turn` with no turn count beside it, which is what produced the wrong call; it now prints the turn count, the end-of-segment context, the accumulation rate, an explicit statement that the average scales with segment length and is not a rollover-health metric, and a per-run estimate of what splitting the section would have saved. Pinned by two new cases in `scripts/overnight/tests/test_cost_summary.py` (the caveat is present; the split estimate is omitted when there is no slope to fit it from).

- [ ] **Calibrate the `section-manifest.py` split predictor against the measured cost curve, so oversized sections are split before they are implemented.**
  The predictor's `SPLIT-RECOMMENDED` thresholds (files, `open_items > 12`, subsystem count, ABI-weighted `>= 6`) are authoring-time heuristics that were never checked against what a long section actually costs. The 2026-07-28 diagnosis supplies the missing curve: cost is quadratic in segment length, and the three canary sections ran 256 / 316 / 325 turns and cost $123.74 / $164.86 / $220.41. Splitting the costliest in two would have cost 59% of its cache-read. Work the calibration from real section outcomes, not from a fresh guess: correlate recorded per-section turn counts against the predictor's verdict on those same sections, and set the threshold where the quadratic actually starts to bite. A section that ships in ~250 turns is fine; one heading past ~400 is paying the T-squared penalty and should have been two sections.

- [ ] **Cut Bash accumulation: 46% of the costliest segment's re-read bill, and 108 of 207 calls were `bash_search`.**
  Measured 2026-07-28 (position-weighted, costliest canary segment): Bash contributed 290,714 raw tokens and $64.82 of cache-read across 207 calls, more than Read, Edit and Skill combined. The `bash_search` half is the tractable part because it is already a doctrine violation, not a new rule: CLAUDE.md's Grep-tool-over-Bash-grep floor is MCP-independent and always applies, yet 108 searches went through Bash on one section. Determine first whether these are searches that should have been Grep-tool calls (in which case enforce, and `cd_prefix_reminder.py` is the model for a warn-only hook) or genuinely non-search Bash misattributed by the `bash_search` classifier, because those call for opposite responses. The remaining 99 non-search Bash calls averaged ~1,400 tokens each and are mostly build/test output, which `run-artifact.sh` already exists to bound.

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
