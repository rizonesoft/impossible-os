# Token Saver v15 -- Cost Findings (opened 2026-08-16)

Cost and token findings from the run armed after the 2026-08-16 close-out of [v14](token-saver-v14.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `token-saver-vNN.md` in this directory, which is this one until an operator opens v16.

**Why findings land here instead of being fixed.** Cost machinery is control plane (`.claude/hooks/**`, `scripts/overnight/**`) or receipt surface, both off-limits unattended. Record the finding in the same turn it is observed, then continue; a finding carried in-context to "report later" dies with the segment.

**What to write.** One item per finding: what was observed live (run id, segment, numbers), the mechanism confirmed at source, and the measured or bounded cost. **The >= 2% bar applies.** A projection is not a finding. Lead <= 250 chars; sub-bullet bodies <= 1,000.

---

## The open question, four cycles running

v11-v14 all closed with no gate-free cost finding: every cost number arrived attached to a misfiring gate and filed next door in `overnight-runner-improvements` with its mechanism, which is what the scope rule instructs. So an empty file here is NOT evidence of a quiet run. The question still unanswered: **is there a cost finding with NO gate attached?** Pure waste -- re-reads, orientation churn, poll loops, redundant dispatches, a wait longer than the work -- is the class no run has yet reported. If nothing lands here again, check the improvements file before concluding anything.

## Standing measurement obligations

Carried from v14 with baselines. A measurement without one is an anecdote.

- **The pre-push tooling suite receipt.** Measured 2026-08-09: a push went from a 10-minute wall kill to 44s on a valid receipt, 95-99s when stale. Measure: fraction of ship pushes hitting a valid receipt over a full run, and total wall-clock saved.
- **The suite LOCK's wait cost.** `test-tooling.sh` holds a per-worktree flock. Measure: contention frequency and seconds waiting.
- **J1 re-runs caused by attended commits.** Measured 2026-08-10: 3 operator commits forced `j1a -> j1b -> j1c`, ~17 minutes paid twice. Measure per attended session -- the fix is behavioural (repair early in a section, not at the ship boundary) and only a number makes it stick.
- **Backgrounded ship push.** Measure poll iterations and tokens per ship against the previous inline cost.
- **Agent-result cache hit rate.** Baseline needed: `cache-hit` entries versus total dispatches per segment.
- **Segment-start orientation cost.** Baseline needed: tokens from segment start to first section edit. `section-pack.py` symbol resolution + `spawn_chain` provenance should reduce the following exploration -- measure whether they do.
