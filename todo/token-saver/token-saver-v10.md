# Token Saver v10 -- Cost Findings (opened 2026-08-05)

> **CLOSED 2026-08-07.** Successor: [`token-saver-v11.md`](token-saver-v11.md). **Zero cost findings filed -- the third consecutive cycle at zero**, and the >= 2% bar held throughout.
>
> **The rotation decision was TAKEN, not deferred again.** v10 opened saying the question was closed as a measurement and only an operator decision remained. It remains open as a decision and is carried once more to v11 with the same recommendation (retire it) -- but note the standing instruction: **do not tune `ROTATE_HINT_TURNS` a fifth time**, and do not re-open it as something to measure. 146 hints, 0 actions, with runway available and unused.
>
> **Where this cycle's cost actually went, recorded so an empty file is not misread as an idle one:** twelve gate defects fixed in `overnight-runner-improvements-v10.md`, several of which were pure waste -- a gate blocking a `git commit` whose MESSAGE mentioned a script, a stale `ours_red` re-charging "fix CI before the next section" at every boundary for two days, and a subagent's Read blocking the main session from the verification doctrine requires.


Cost and token findings from the run armed after the 2026-08-05 attended stop. CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `token-saver-vNN.md` in this directory, which is this one until an operator opens v11.

**Why findings land here instead of being fixed.** Cost machinery is control plane (`.claude/hooks/**`, `scripts/overnight/**`) or receipt surface, both off-limits unattended. Record the finding in the same turn it is observed, then continue; a finding carried in-context to "report later" dies with the segment.

**What to write.** One item per finding: what was observed live (run id, segment, numbers), the mechanism confirmed at source, and the measured or bounded cost. **The >= 2% bar applies.** A projection is not a finding. Lead <= 250 chars; sub-bullet bodies <= 1,000.

**A misfiring gate is a COST defect -- file it in [`overnight-runner-improvements-v10.md`](../overnight-runner-improvements/overnight-runner-improvements-v10.md) anyway.** Two consecutive cycles collected ZERO findings here while their siblings collected several whose cost was measured purely in wasted tool calls. Do not conclude from an empty token-saver file that nothing was wasted.

## THE rotation question is CLOSED as a measurement. What remains is an operator decision.

Do NOT re-open this as something to measure. It has been measured, across four cycles:

- **146 hints fired on record. 0 `rollover-wip` actions. Ever.**
- The last canary fired 20 (threshold 90; event counts at hint time 90 -> 240, median 150).
- **The "premise expired" hypothesis is disproven too.** It held that segments now run too short for a hint at 90 to leave useful runway. But **5 of those 20 hints fired at >= 190 events** -- 100+ events of runway past the threshold -- and the run still did not rotate. Runway was available and unused.
- That is FOUR rounds in which the threshold was blamed and exonerated (200 mis-derived, 90 correct, a transport defect, a contradicting message).

**The decision, for an operator, not the run:** either make the hint an ACTION rather than an advisory, or retire it and reclaim the hook. Recommendation: **retire it.** A feature that fires 146 times, is delivered every time, has runway, and never acts is not a cost lever -- it is a line in a log. **Do not tune `ROTATE_HINT_TURNS` a fifth time.**

Baselines, if the decision is ever revisited: prior segments reached 492K / 542K / 798K end-of-segment context; one segment cost **$179.27** for a single section with cache-read at **85.3%** of spend.

## Standing measurement obligations for this run

- **Does the Check 7 coverage floor cost anything?** `stub-lint-baseline.json` adds a resolve pass over every stamped symbol ref to each lint run. Measured at 0.172s for the whole check today; watch it as the corpus grows and file if it clears the 2% bar.
- **Do the two backlog drains (FILE_CLOSE + ADVANCE) slow a file close measurably?** They add per-item judgment to work that used to be pure bookkeeping. Record the wall-clock delta on a file with a large parked set.

---

_No findings yet._
