# Token Saver v03 -- Cost Findings (measured 2026-07-28 14:00 ->)

Successor to [`token-saver-v02.md`](token-saver-v02.md), which is bounded to the 02:03 attended canary of 2026-07-28. This file collects cost findings from the SECOND attended canary of that day (armed 13:59, proving `4466c81f..01c0d634`) onward. A CORRECTION to a v02 item stays in v02 next to the item it corrects.

The v01 "Never cut" floor still applies unchanged: quality gates, adversarial review, bare-metal validation, and the trust contract are not cost levers. Everything below is measured on a real run; a projection is not a finding.

---

## Metrics that make a working system look broken

- [ ] **`review convergence: N/M rounds suppressed` counts FIRST dispatches in its denominator, so a correctly-working gate always reports 0/M on a fresh section.**
  Measured on the 13:59 canary segment: the cost summary printed `review convergence: 0/4 rounds suppressed`, which reads as a dead gate. All four `redispatch` records in `.claude/state/offload-events.jsonl` say the same thing -- **"no prior <kind> verdict for todo/00-infrastructure/TODO-04-usermode-test-framework.md#20 -- first dispatch"** -- for `design`, `adversarial`, `perf` and `re-adversarial`. Every one was the first time that kind had run against that section, so there was no recorded verdict to compare against and suppression was impossible BY DEFINITION.
  **The gate was right four times out of four and the metric reported it as 0% effective.** A convergence gate can only ever suppress a RE-dispatch of a kind that already has a recorded verdict; counting first dispatches as unsuppressed rounds guarantees a bad-looking number on every section that runs each kind once, which is the normal case.
  **This is the same defect class as the `avg context/turn` length confound fixed this morning**, and it has the same consequence: it caused a backlog item. `token-saver-v02.md` filed "Review convergence suppressed 0 of 2 rounds -- the T3-3 gate did nothing" on exactly this reading, and the determination that closed it had to go read the raw redispatch records to discover three of the four rounds were legitimately un-suppressible. Nobody should have to do that twice.
  **Fix:** exclude first-dispatch rounds from the denominator and report them separately -- e.g. `review convergence: 0 suppressed / 0 eligible (4 first dispatches)`. When `eligible` is 0 the gate had no opportunity and the line should say so rather than implying failure. `cost-summary.py` reads `converged` / `redispatch` kinds from `offload-events.jsonl`; the `detail` string already distinguishes "first dispatch" from "inputs changed", so the data needed is present and no new instrumentation is required.
  **NOT fixed during this canary:** `scripts/overnight/cost-summary.py` is FLOW-CRITICAL control plane and is one of the paths this canary is proving; editing it mid-run would make the stamp cover a reporter the run never used. Land after the stamp.

---

## Measurements from the 13:59 segment (recorded, no action implied)

- **The new length-aware cost reporting shipped this morning is live and reads correctly.** The segment printed `context/turn avg 247,853 over 289 turns end-of-segment 393,341`, plus the accumulation rate and split estimate. The turn count now sits beside the average, which is what makes the number interpretable rather than alarming.
- **Accumulation was ~1,006 tok/turn against 1,241 / 1,302 / 1,607 for the three morning segments.** Lower, but this is ONE segment on different work with 0 agent dispatches, 3 hook fires and 41% re-reads (against 1/0/4, 73/12/26 and 60-64% respectively), so the difference is not attributable to any single change and is NOT claimed as an improvement caused by today's commits. Recorded as a data point for the split-predictor calibration, which now has real section numbers to key on.
- **Segment cost $127.46 at 289 turns**, against the morning's $123.74 at 256 turns for the comparable first segment. Consistent with the length-confound model (cost tracks segment length, not rollover health) and offered as further evidence for it, not as a saving.
