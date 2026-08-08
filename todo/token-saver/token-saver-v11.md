# Token Saver v11 -- Cost Findings (opened 2026-08-07)

**CLOSED 2026-08-08.** Successor: [`token-saver-v12.md`](token-saver-v12.md). Every item above carries a verdict (RESOLVED / NOT REPRODUCED / CARRIED); unresolved items are carried forward by name in the successor.


Cost and token findings from the run armed after the 2026-08-07 close-out of [v10](token-saver-v10.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `token-saver-vNN.md` in this directory, which is this one until an operator opens v12.

**Why findings land here instead of being fixed.** Cost machinery is control plane (`.claude/hooks/**`, `scripts/overnight/**`) or receipt surface, both off-limits unattended. Record the finding in the same turn it is observed, then continue; a finding carried in-context to "report later" dies with the segment.

**What to write.** One item per finding: what was observed live (run id, segment, numbers), the mechanism confirmed at source, and the measured or bounded cost. **The >= 2% bar applies.** A projection is not a finding. Lead <= 250 chars; sub-bullet bodies <= 1,000.

**A misfiring gate is a COST defect -- file it in [`overnight-runner-improvements-v11.md`](../overnight-runner-improvements/overnight-runner-improvements-v11.md) anyway.** THREE consecutive cycles have now collected ZERO findings here while their siblings collected many whose cost was measured purely in wasted tool calls -- v10 alone had a gate refusing a `git commit` over its own commit MESSAGE, a stale CI verdict re-charging a fix demand at every boundary for two days, and a subagent Read that blocked the main session from the verification its doctrine requires. **Do not read an empty token-saver file as evidence that nothing was wasted.** It more likely means the waste was routed to its mechanism, which is correct.

## The rotation hint: DECIDED, not carried again

Four cycles asked whether the mid-section rotation pays. It was measured (146 hints, 0 actions, runway available and unused) and the answer did not change. **Do not re-open it as a measurement, and do not tune `ROTATE_HINT_TURNS` a fifth time.** If it is still wired when this cycle closes, the standing recommendation is unchanged: retire it and reclaim the hook.

## What is worth measuring THIS cycle instead

The rotation question consumed four cycles because it was the only cost question with a number attached. These are the open ones with a plausible >= 2% answer:

- **Review-round cost per section.** The last canary ran sections to 7 and 14 review rounds. Rounds are producing real defects (the operator's standing instruction is **do not touch the review rounds**), so this is NOT a proposal to cap them -- it is a request for the per-round marginal cost, so that any future round-shaping argument starts from a measurement rather than an impression.
- **Re-read churn within a segment.** How many Read calls in a segment return content the same segment already read. The agent-result cache covers dispatches; nothing covers plain re-reads.
- **Cost of a refused gate.** Every refusal costs a diagnosis + a retry. v10 fixed several; the residual rate is unmeasured.

---

_No findings yet._
