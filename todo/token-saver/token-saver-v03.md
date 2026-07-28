# Token Saver v03 -- Actionable Cost Work (measured 2026-07-28, ordered for implementation)

Successor to [`token-saver-v02.md`](token-saver-v02.md), which is bounded to the 02:03 attended canary of 2026-07-28 and is now a historical record. This file is the IMPLEMENTATION LIST for the cost work to land before the 3-day canary.

**Inclusion rule, applied strictly:** an item earns a slot only if it is MEASURED on a real run and worth **>= 2%** of run spend. Everything measured below that, or measured at zero, appears once under "Excluded" so it is not re-proposed. Two items survive. That is not a shortage of ideas -- it is what measuring produced: the cost is concentrated, and one item holds almost all of it.

**Evidence base -- the 13:59 attended canary of 2026-07-28**, three segments, sections §20/§21/§22 of `todo/00-infrastructure/TODO-04-usermode-test-framework.md`:

| segment | turns | avg context/turn | end-of-segment | accum/turn | cost |
|---|---|---|---|---|---|
| 13:59 | 289 | 247,853 | 393,341 | 1,006 | $127.46 |
| 16:10 | 223 | 248,243 | 380,962 | 1,190 | $121.61 |
| 17:36 | 381 | 346,536 | 532,376 | 1,069 | $256.29 |
| **total** | **893** | | | | **$505.36** |

Cache-read is 68-84% of every segment. The cost model was validated to 0.4% against the morning run: `cache-read = base x T + sum(delta_i x (T - i))` -- quadratic in segment length, because a token added at turn `i` is re-read on each remaining turn.

---

## 1. Bound segment length -- CALIBRATED AND SHIPPED. Ceiling is 11.7%, not the 27-41% first claimed.

- [x] **SHIPPED: split threshold calibrated on 15 shipped sections. Captures +7.0% of the 11.7% available.**
  **THE HEADLINE NUMBER WAS WRONG AND IS CORRECTED HERE.** Earlier revisions of this item claimed 27-41%, taken from `cost-summary.py`'s split estimate. That model halves a segment's turns and assumes NO per-section overhead. Real sections re-pay the review pipeline every time -- ~60 turns, which is exactly what the zero-item sections cost (58 and 85 turns observed). Modelling the 15-section set with that overhead included, a PERFECT oracle that split every section over 250 turns and nothing else saves **11.7%**. That is the ceiling for this lever. Anything above it was an artifact of ignoring the cost of the split itself.
  **Splitting is not free and below T ~= 220 it LOSES.** Modelled: T=120 costs 33% more split, T=160 15% more, T=200 4% more, T=250 saves 6%, T=350 saves 17%, T=400 saves 21%.
  **The dataset, and how to rebuild it.** 15 sections, reconstructed by pairing each metrics row's `start_sha..end_sha` range with the section whose `**Verified:**` stamp landed in it, then running `section-manifest.py` on the TODO as it stood at `start_sha`. No new instrumentation was needed; the method is repeatable as more nights land.
  **Only `open_items` predicts anything.** Correlation against actual turns: items **+0.50**, files +0.16, subsystems +0.05, abi_impact **-0.22**. The old composite verdict scored **+0.20 -- near-random**: it flagged 2 of the 8 sections that ran past 250 turns (25% recall) while firing on one that took 248.
  **Threshold chosen to minimise TOTAL cost, not recall.** Over the 15-section set: old `> 12` captured +2.5%, `>= 6` +6.5%, **`>= 5` +7.0% (shipped)**, `>= 4` +9.5%, `>= 3` +11.4%, perfect oracle +11.7%. `>= 5` splits 9 of 15 rather than 11 or 12; the marginal gain from 5 to 3 is 4.4 points for three more splits, each of which is a real TODO restructure and a full extra review cycle. Pinned by `test_calibrated_item_threshold_boundary`.
  **Residual, and it is NOT a threshold problem.** Two of the longest sections (352 and 379 turns) carry only 3-4 items and are invisible to any item-count rule. Catching them needs a different mechanism -- most plausibly the run splitting a section when it NOTICES it is running long, which is a sequencer change and must land attended. Worth at most the remaining ~4.7 points, so it is not urgent.
  **Prerequisite (b) shipped separately:** `section_source` (`explicit` / `derived` / `stale` / `unset` / `env`) is recorded beside `section` in every metrics row, so future calibration can exclude rows whose section was carried over rather than measured.

---

## 2. Trim the static base -- 3-4%, one-off, independent of item 1

- [ ] **Cut the always-present listings from the 58.3K static base. Measured at 19-24% of each segment's cache-read; the removable slice is ~3-4% of a night.**
  **The measurement.** Turn-1 context across all three segments: **58,289 / 58,280 / 58,282 tokens** -- constant to within 9 tokens, which is what makes it a fixed cost rather than a symptom. Re-read on every turn it accounts for 15.0M / 10.8M / 14.5M token-reads, i.e. **$22.56 / $16.26 / $21.77 per segment (19-24% of cache-read), about $60 across the canary**. It is the one cost a LONGER section does not dilute, so it is entirely independent of item 1 and the two can land in either order.
  **What is removable, and what is not.** Two components are oversized for an unattended kernel run: the SKILL listing (21,358 chars) and the AGENT listing (17,710 chars) -- roughly 10K tokens describing ~40 skills and 21 agents, where a sequencer segment invokes 5-15 skills and 0-4 agents. **`CLAUDE.md` is a further ~13K and is doctrine, not padding: do not cut it to save tokens.** The launcher already trims TOOL schemas via `--disallowedTools`, so both the precedent and the mechanism exist; the listings are simply the untrimmed part.
  **Measure the removable fraction before cutting.** v01's recorded lesson is that estimates here carry ~10x error bars and one shipped negative. If ~10K of the 58.3K goes, that is ~17% of the base and **3-4% of a night** -- real, one-off, worth doing, and an order of magnitude below item 1. Do not let it be sold as more, and do not let it displace item 1 in the ordering.
  **Acceptance:** turn-1 context measurably below ~50K on the next run, with the sequencer still able to invoke every skill and agent it actually uses -- verify against the canary's own invocation list, not against a guess.

---

## Excluded -- measured and below the bar, or measured at zero. Recorded so they are not re-proposed.

- **Accumulation-rate spread (1,309 -> 1,780 tok/turn) -- NOT waste, do not chase.** The 36% spread is work VOLUME: the dear segment ran 142 Edits against 88, while per-call averages barely moved (Edit 923 vs 999, Bash 1,046 vs 1,132). The dearer segment was doing more work per turn, which is the outcome the runner exists to produce.
- **Per-call tool cost -- already at the floor.** Bash ~1.0-1.1K, Edit ~0.9-1.0K, Read 1.4-2.5K tokens per call. Nothing to trim.
- **Read re-reads (41% -> 68% -> 73%) -- overstated by the metric, and already handled.** The figure counts re-reads by PATH; on a segment running 142 Edits, re-reading an edited file is legitimate. The genuinely redundant case (same byte range, UNCHANGED content hash) is already blocked by `read_cache_block.py`, which is wired and never needed to fire during the canary. No lever.
- **Skill-BODY injection (54.7K/segment, 12-16% of accumulation) -- already retired as T1-2** in v02 after its ceiling was revised down. The always-present LISTINGS are a different surface and are covered by item 2.
- **Convergence-metric denominator -- a reporting fix worth 0%.** `review convergence: N/M rounds suppressed` counts FIRST dispatches, where suppression is impossible by definition, so a correctly-working gate always reports 0/M on a fresh section (0/4, 0/5, 0/1 across the canary; every record reads "no prior verdict ... first dispatch"). It should report `suppressed / eligible` with first dispatches separate. Excluded under the 2% rule -- but it is what caused v02's "the T3-3 gate did nothing" to be filed against a gate that was right every time, so fix it opportunistically when `cost-summary.py` is next open.
