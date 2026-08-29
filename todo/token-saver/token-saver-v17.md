# Token Saver v17 -- Cost Findings (opened 2026-08-24)

> **CLOSED 2026-08-29.** Superseded by [v18](token-saver-v18.md). The run filed nothing here (0 items). The six-cycle open question was DISCHARGED as assigned, once, by pointing `overnight-log-explorer` at `run-20260826-160133.log`: ungated waste measured at **12-14 of 602 tool calls (2.0-2.3%)**, so the class is real but sits AT the bar rather than above it, and two of its three contributors were fixed by this close-out. Result recorded under the assignment below; the question is RETIRED as a restatement and replaced by a standing close-out measurement in v18.

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

## Assignment result (2026-08-29 close-out)

- [x] DISCHARGED on transcript `run-20260826-160133` (2026-08-26 16:01 to 2026-08-27 08:02, one section: TODO-06 section 55); every total below re-verified by the main session's own grep.
  - Totals examined: 602 tool calls (499 Bash, 65 Write, 38 Skill, 0 Read/Grep/Edit/Agent), 23 tool errors, 41 `wait-for-codex-verdict.sh` calls over 29 broker dispatches, 36 full `test_build.sh` runs.
  - UNGATED WASTE: 12-14 calls, 2.0-2.3%, in three incidents. (1) `( cmd ) & echo started` returns at fork, so an empty log was polled three times, the same shape was repeated once more and polled twice again before the run named it "the documented hazard" (lines 602-611, 6 calls). (2) A review log path built from the `jobId` instead of the broker's returned `logFile` was polled to a false hung verdict twice; round 8 paid a needless re-dispatch (~18 min), round 9 self-caught in ~9 min. (3) One `[SEQ-WORKTREE]` false positive on heredoc data (line 92, 1 call).
  - NOT waste: 15 of the 23 tool errors were gates working as designed (receiving-review, bare-flag, post-ship block, build-offload reminder), one retry each. No file was re-read at an identical offset; the 131 mentions of `identity-gate.sh` are ~35 distinct slices of a 4,500-line script across 29 review rounds. Zero agent dispatches, so nothing to duplicate.
  - CONSEQUENCE for the class: a gate-triggered observer cannot report this, but a close-out transcript digest can, cheaply (one Sonnet dispatch, ~138K tokens, 5.6 min). Incidents (2) and (3) are closed by the v17 close-out (`MISSING` waiter exit, command-position worktree guard); incident (1) is a re-learned idiom the sequencer skill already documents, now carried as a v18 measurement (does the subshell false-start recur once the skill is re-read at each relaunch).
  - The largest cost in the transcript is not waste: 16 hours for one section, 29 Codex rounds and 36 suite reruns, is the mandated convergence loop. Whether that loop is right-sized is a doctrine question, filed nowhere by this digest because the digest was told not to judge it.

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
