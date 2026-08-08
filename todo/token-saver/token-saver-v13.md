# Token Saver v13 -- Cost Findings (opened 2026-08-08)

Cost and token findings from the run armed after the 2026-08-08 close-out of [v12](token-saver-v12.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `token-saver-vNN.md` in this directory, which is this one until an operator opens v14.

**Why findings land here instead of being fixed.** Cost machinery is control plane (`.claude/hooks/**`, `scripts/overnight/**`) or receipt surface, both off-limits unattended. Record the finding in the same turn it is observed, then continue; a finding carried in-context to "report later" dies with the segment.

**What to write.** One item per finding: what was observed live (run id, segment, numbers), the mechanism confirmed at source, and the measured or bounded cost. **The >= 2% bar applies.** A projection is not a finding. Lead <= 250 chars; sub-bullet bodies <= 1,000.

---

## An empty file here is not evidence of silence

v11 and v12 both closed with zero items, and v12's close-out settled why: the run WAS producing cost numbers -- the ~6-minute tooling suite at pre-push, ~7 minutes of build and suite serialised by a blocked background call, a killed and re-run suite, recovery costs quoted in tool calls -- and every one of them arrived attached to a misfiring gate, so it filed next door with its mechanism, exactly as the scope rule instructs.

So the question for this cycle is narrower: **is there a cost finding with NO gate attached?** Pure waste -- re-reads, orientation churn, poll loops, redundant dispatches -- is what this file is for, and it is the class the run has not been reporting. If nothing lands here again, check the improvements file before concluding the run was quiet.

## Standing measurement obligations

- **The ~6-minute tooling suite at pre-push.** `.githooks/pre-push` runs `scripts/test-tooling.sh` (1293 tests) whenever a push touches `scripts/lint/`, `scripts/todo-graph/`, `scripts/test-tooling.sh` or `.claude/hooks/`. Measure: what fraction of ship pushes hit those prefixes, and the total wall-clock spent. If it is most of them, the conditional is not buying what it was designed to buy.
- **The new suite LOCK's wait cost.** As of 2026-08-08 the suite serialises per worktree instead of running concurrently. Measure: contention frequency, and seconds spent waiting -- a pre-push that now queues behind a manual run is correct but is new wall-clock, and it counts against the tool wall.
- **Backgrounded ship push.** The push is backgrounded and polled rather than run inline. Measure the poll overhead: iterations and tokens per ship, against the previous inline cost.
- **Agent-result cache hit rate.** Carried standing metric. Baseline needed: `cache-hit` entries versus total dispatches per segment.
- **Segment-start orientation cost.** Every relaunch re-reads state, runs the triage oracle and rebuilds the graph cache. Baseline needed: tokens from segment start to first section edit. `section-pack.py` now resolves symbols for non-kernel sections (0 of 24 -> 16 of 24 on the section that exposed it), which should REDUCE the exploration that follows orientation -- measure whether it does.
