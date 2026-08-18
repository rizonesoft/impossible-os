# Token Saver v16 -- Cost Findings (opened 2026-08-17)

Cost and token findings from the run armed after the 2026-08-17 close-out of [v15](token-saver-v15.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `token-saver-vNN.md` in this directory, which is this one until an operator opens v17.

**Why findings land here instead of being fixed.** Cost machinery is control plane (`.claude/hooks/**`, `scripts/overnight/**`) or receipt surface, both off-limits unattended. Record the finding in the same turn it is observed, then continue; a finding carried in-context to "report later" dies with the segment.

**What to write.** One item per finding: what was observed live (run id, segment, numbers), the mechanism confirmed at source, and the measured or bounded cost. **The >= 2% bar applies.** A projection is not a finding. Lead <= 250 chars; sub-bullet bodies <= 1,000.

---

## The open question, FIVE cycles running

v11 through v15 all closed with no gate-free cost finding. Every cost number arrived attached to a misfiring gate and was filed next door in `overnight-runner-improvements` with its mechanism, which is what the scope rule instructs. v15 is the sharpest instance: 11 items filed next door over ~17 hours and 6 segments, 0 here. An empty file here is therefore NOT evidence of a quiet run, and it never has been.

The question still unanswered: **is there a cost finding with NO gate attached?** Pure waste -- re-reads, orientation churn, poll loops, redundant dispatches, a wait longer than the work -- is the class no run has yet reported.

At five cycles this deserves a decision rather than a sixth restatement. Either the class genuinely does not exist in this control plane (in which case say so and retire the file), or it exists and the run cannot SEE it because every observation the run makes is triggered by a gate firing. The second is the more likely reading and suggests the instrument is wrong: a gate-triggered observer structurally cannot report ungated waste. Settled by ONE deliberate measurement pass that looks for waste without waiting for a gate to point at it -- the `overnight-log-explorer` agent over a full run transcript is exactly that instrument and has never been pointed at this question.

## Found live this cycle

<!-- The run files here. Nothing yet: v16 opened at close-out, before the next arm. -->

- [ ] I dumped an unfiltered `skill-progress.json` walk and paid ~5K tokens to read one line (below the 2% bar; recorded as EXCLUDED, kept for the reasoning lesson)
      - OBSERVED 2026-08-18 diagnosing the section-commit-gate misfire on TODO-13 section 22. I needed ONE fact: whether a non-orphaned `review-todo-section` entry existed. The probe I wrote printed every matching key, which is ~130 rows of orphaned history, because the file accumulates an entry per review ever run.
      - The filter I actually wanted was one clause I already knew: `if not v.get('compaction_orphaned')`. I had even printed that field in the loop, so the information needed to bound the output was in the probe I wrote.
      - Measured cost is roughly 5K tokens against a session far above 250K, so this sits UNDER the >= 2% filing bar and is not proposed as work. It is recorded because the capture file asks for approaches committed to too early, and because the shape generalises: when a state file is append-only per event, any `for k,v in d.items()` walk is unbounded by construction and the filter belongs in the probe rather than in the reading.

## Standing measurement obligations

Carried from v15 with baselines. A measurement without one is an anecdote.

- **The pre-push tooling suite receipt.** Measured 2026-08-09: a push went from a 10-minute wall kill to 44s on a valid receipt, 95-99s when stale. Measure: fraction of ship pushes hitting a valid receipt over a full run, and total wall-clock saved.
- **The suite LOCK's wait cost.** `test-tooling.sh` holds a per-worktree flock. Measure: contention frequency and seconds waiting.
- **J1 re-runs caused by attended commits.** Measured 2026-08-10: 3 operator commits forced `j1a -> j1b -> j1c`, ~17 minutes paid twice. Measure per attended session -- the fix is behavioural (repair early in a section, not at the ship boundary) and only a number makes it stick.
- **Backgrounded ship push.** Measure poll iterations and tokens per ship against the previous inline cost.
- **Agent-result cache hit rate.** Baseline needed: `cache-hit` entries versus total dispatches per segment.
- **Segment-start orientation cost.** Baseline needed: tokens from segment start to first section edit. `section-pack.py` symbol resolution + `spawn_chain` provenance should reduce the following exploration -- measure whether they do.
- **Cost of a re-verified finding versus a copied one.** New this cycle. The v15 close-out overturned one filed finding by re-checking its consequence at source (an arm blamed for lowering a threshold that arithmetic showed it could never reach). Re-verification cost a handful of reads; acting on the finding as filed would have bought a wrong fix to a flow-critical predictor. Measure: how often a re-check changes a filed verdict, since that ratio is what justifies the re-check habit.
