# Token Saver v17 -- Cost Findings (opened 2026-08-24)

Cost and token findings from the run armed after the 2026-08-24 close-out of [v16](token-saver-v16.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `token-saver-vNN.md` in this directory, which is this one until an operator opens v18.

**Why findings land here instead of being fixed.** Cost machinery is control plane (`.claude/hooks/**`, `scripts/overnight/**`) or receipt surface, both off-limits unattended. Record the finding in the same turn it is observed, then continue; a finding carried in-context to "report later" dies with the segment.

**What to write.** One item per finding: what was observed live (run id, segment, numbers), the mechanism confirmed at source, and the measured or bounded cost. **The >= 2% bar applies.** A projection is not a finding. Lead <= 250 chars; sub-bullet bodies <= 1,000.

---

## The six-cycle open question is now an ASSIGNMENT, not a restatement

v11 through v16 all closed with no gate-free cost finding. v16 filed exactly one item and rejected it on this file's own >= 2% bar. Six restatements of "is there a cost finding with no gate attached?" have produced no answer, so it is not restated a seventh time.

The v16 diagnosis is accepted as correct: **a gate-triggered observer structurally cannot report ungated waste.** Every observation the run makes is prompted by a gate firing, so pure waste -- re-reads, orientation churn, poll loops, redundant dispatches, a wait longer than the work -- is invisible to the instrument, not absent from the system.

**The assignment, to be discharged ONCE and then reported either way:** point the `overnight-log-explorer` agent at a full run transcript under `.claude/overnight/reports/run-*.log` and ask it for tool-call accounting, wait/poll waste, re-read churn, and per-section wall-clock -- with no gate as the starting point. That agent exists for exactly this and has never been pointed at this question.

Two outcomes, both acceptable, neither of which is "carry it again":

- It finds ungated waste above the 2% bar -> file it here with numbers, and the class is real.
- It finds none -> record that result with the transcript id and the totals examined, and RETIRE this file's open question permanently.

## Found live this cycle

<!-- The run files here. Nothing yet: v17 opened at close-out, before the next arm. -->

## Standing measurement obligations

Carried from v16 with baselines. A measurement without one is an anecdote.

- **Did the v16 stamp-attribution fix remove real dispatch waste?** New this cycle and directly measurable. BASELINE: one v16 section paid ~20 minutes and 7 extra Codex dispatches re-running reviews that had already been performed correctly, because a bundled wave recorded only its leading kind. Measure: extra dispatches per section attributable to a stale-stamp refusal. Target zero.
- **The pre-push tooling suite receipt.** Measured 2026-08-09: a push went from a 10-minute wall kill to 44s on a valid receipt, 95-99s when stale. Measure: fraction of ship pushes hitting a valid receipt over a full run, and total wall-clock saved.
- **The suite LOCK's wait cost.** `test-tooling.sh` holds a per-worktree flock. Measure: contention frequency and seconds waiting.
- **J1 re-runs caused by attended commits.** Measured 2026-08-10: 3 operator commits forced `j1a -> j1b -> j1c`, ~17 minutes paid twice. The fix is behavioural (repair early in a section, not at the ship boundary) and only a number makes it stick.
- **Backgrounded ship push.** Measure poll iterations and tokens per ship against the previous inline cost. Note the v16 finding that a backgrounded push can die when a gate refuses the following poll -- a push that never landed also never shows up as a cost.
- **Agent-result cache hit rate.** Baseline needed: `cache-hit` entries versus total dispatches per segment.
- **Segment-start orientation cost.** Baseline needed: tokens from segment start to first section edit.
- **Cost of a re-verified finding versus a copied one.** The habit paid again at the v16 close-out: three items blaming a per-kind write race were re-checked at source and turned out to be one shared-path defect, so one fix closed all three and no per-kind instrumentation was built. Measure: how often a re-check changes a filed verdict, since that ratio is what justifies the re-check habit.
- **Unbounded per-event state walks.** From the one v16 item, rejected on cost but kept for the shape: `skill-progress.json` is append-only per event and now holds 383 keys, so any `for k,v in d.items()` probe over it grows without bound. The filter belongs in the probe, not in the reading. Measure: whether any single probe's output exceeds a few hundred lines.
