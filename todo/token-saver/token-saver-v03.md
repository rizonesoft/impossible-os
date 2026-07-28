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

## 1. Bound segment length -- 27-41% of run spend. Everything else here is a rounding error beside it.

- [ ] **Split oversized sections so no segment runs past ~250 turns. Worth 27-41%; nothing else measured all day comes close.**
  **The measurement.** Cost is QUADRATIC in segment length. The conservative linear model in `cost-summary.py` puts the same work split across 2 sections at **71-73%** of actual cache-read, consistently across all three segments; the exact counterfactual replayed from the morning trace put it at **59%**. So the honest range is a **27-41% saving**, and it is the only measured item above 4%. On the 17:36 segment alone that is $50-100.
  **The mechanism is the section SPLIT the doctrine already prescribes** (`overnight-sequencer/SKILL.md:388`, "a section too big for one context should be SPLIT... not rotated mid-way"). It is NOT the mid-section context-cap rotation retired on 2026-07-14, which needed a committed-but-unpushed window that never occurs. Do not re-open that.
  **PREREQUISITE (a) -- fix the predictor's measured false negative.** `section-manifest.py` passed TODO-04 §20 as `fits-one-context` at 5 files / 10 open items / 2 subsystems / non-ABI, and that section then consumed TWO full segments (525 and 653 assistant messages). Ten items sits under the `> 12` gate, and being non-ABI it never reached the `>= 6` ABI-weighted gate. Any recalibration that only tightens the ABI path still passes §20; the non-ABI item threshold is the one to lower.
  **PREREQUISITE (b) -- keep the section dataset trustworthy for the 3-day run.** `run_phase_guard._derive_section_idx()` shells out to `sequencer_triage.py --classify`, which hard-fails when `build/todo-cache.json` is absent; the derivation then fail-opens to `None` and the cursor keeps its PREVIOUS value, so a stale section number is indistinguishable from a fresh one. The cache is genuinely transient -- `build-and-validate.sh` deletes it unless `--keep-cache`, and it was observed absent at 16:47 and rebuilt at 16:48. Record `section_source` (`derived` / `stale`) beside `section` so the calibration can EXCLUDE unreliable rows. Do NOT let the derivation block a cursor move: a metrics field must never be able to wedge a run. Worth 0% by itself, and listed here only because item 1 is worthless without it.
  **Order of work:** (b) first -- it costs almost nothing, and every night of the 3-day run without it produces data that cannot be trusted; then (a); then set the threshold from the 3-day data rather than from three segments.
  **Acceptance:** a section that would run past ~250 turns is flagged before implementation and split, and a segment's `end-of-segment` context stays under ~250K. The calibration itself is owned by `token-saver-v02.md` (item: "Calibrate the `section-manifest.py` split predictor against the measured cost curve"), which carries the S18/S19/S20 evidence -- do the work there, and close this item when sections are actually being split.

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
