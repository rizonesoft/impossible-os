# Token Saver v12 -- Cost Findings (opened 2026-08-08)

**CLOSED 2026-08-08.** Successor: [`token-saver-v13.md`](token-saver-v13.md). Zero items were filed here, for the second cycle running -- see the verdict on that question below; the standing measurement obligations carry forward.

Cost and token findings from the run armed after the 2026-08-08 close-out of [v11](token-saver-v11.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `token-saver-vNN.md` in this directory, which is this one until an operator opens v13.

**Why findings land here instead of being fixed.** Cost machinery is control plane (`.claude/hooks/**`, `scripts/overnight/**`) or receipt surface, both off-limits unattended. Record the finding in the same turn it is observed, then continue; a finding carried in-context to "report later" dies with the segment.

**The >= 2% bar still applies.** A cost finding needs a measured saving of at least 2% of segment tokens, or a measured wall-clock cost with the number attached. "This felt wasteful" is not a finding.

---

## v11 filed NOTHING here, and that is itself the first thing to check

v11 closed with **zero** cost findings across a full day of running -- while the runner-improvements surface took eight. Two readings, and this run should settle which:

- the cost side genuinely had nothing above the 2% bar (plausible: the deterministic-wrapper routing has been in place for several cycles and the expensive mistakes were fixed in v08-v10); or
- cost observations were being made and not written down, because a cost finding needs a measurement and the run had no cheap way to take one mid-section.

If it is the second, that is a reporting-surface defect and belongs in [`overnight-runner-improvements-v12.md`](../overnight-runner-improvements/overnight-runner-improvements-v12.md), not here.

**VERDICT (close-out 2026-08-08): neither reading, and the question was mis-posed.** v12 also filed zero here -- but the same run filed cost numbers freely NEXT DOOR, inside gate findings: the ~6-minute tooling suite at pre-push, ~7 minutes of build+suite serialised by a blocked background call, one killed and re-run suite (~6 min), and recovery costs quoted in tool calls. So cost observations were made and written down; they routed to whichever file owned the MECHANISM, which is exactly what the "a misfiring gate is both" rule instructs. A cost finding with no gate attached is the rare case, not the common one. Carry the obligations below; do not treat an empty file as silence again without checking the other surface first.

## Standing measurement obligations

- **The ~6-minute tooling suite at pre-push.** `.githooks/pre-push` runs `scripts/test-tooling.sh` (1293 tests, ~6 min by its own comment) whenever a push touches `scripts/lint/`, `scripts/todo-graph/`, `scripts/test-tooling.sh` or `.claude/hooks/`. The run spent a long stretch of 2026-08-07 working inside `scripts/todo-graph/`, so effectively every ship push paid it. Measure: what fraction of ship pushes hit those prefixes, and the total wall-clock spent. If it is most of them, the conditional is not buying what it was designed to buy.
- **Backgrounded ship push.** The push is now backgrounded and polled rather than run inline. Measure the poll overhead: number of poll iterations and tokens per ship, against the previous inline cost.
- **Agent-result cache hit rate.** Carried standing metric. Baseline needed: `cache-hit` entries versus total dispatches per segment.
- **Segment-start orientation cost.** Every relaunch re-reads state, runs the triage oracle and rebuilds the graph cache. Baseline needed: tokens from segment start to first section edit.
