# Token Saver v14 -- Cost Findings (opened 2026-08-10)

Cost and token findings from the run armed after the 2026-08-10 close-out of [v13](token-saver-v13.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `token-saver-vNN.md` in this directory, which is this one until an operator opens v15.

**Why findings land here instead of being fixed.** Cost machinery is control plane (`.claude/hooks/**`, `scripts/overnight/**`) or receipt surface, both off-limits unattended. Record the finding in the same turn it is observed, then continue; a finding carried in-context to "report later" dies with the segment.

**What to write.** One item per finding: what was observed live (run id, segment, numbers), the mechanism confirmed at source, and the measured or bounded cost. **The >= 2% bar applies.** A projection is not a finding. Lead <= 250 chars; sub-bullet bodies <= 1,000.

---

## Three cycles with nothing filed here -- the question is now narrower

v11, v12 and v13 all closed empty, and the reason is on record rather than open: the runs WERE producing cost numbers, and every one arrived attached to a misfiring gate, so it filed next door with its mechanism. That is exactly what the scope rule instructs, and it means an empty file here is not evidence of a quiet run.

So the question for this cycle is the one that keeps going unanswered: **is there a cost finding with NO gate attached?** Pure waste -- re-reads, orientation churn, poll loops, redundant dispatches, a wait that was longer than the work -- is what this file is for, and it is the class no run has yet reported. If nothing lands here again, check the improvements file before concluding anything.

## Standing measurement obligations

- **The pre-push tooling suite, now with a receipt.** `scripts/tooling-receipt.py` lets pre-push skip the ~6-minute suite when a green run already covered the exact bytes. Measured 2026-08-09: a push went from a 10-minute wall kill to **44s** on a valid receipt, and 95-99s when the receipt was stale. Measure: what fraction of ship pushes hit a valid receipt over a full run, and the total wall-clock saved.
- **The suite LOCK's wait cost.** `test-tooling.sh` holds a per-worktree flock. Observed arbitrating between the run's J1 chain and an attended push on 2026-08-09. Measure: contention frequency and seconds spent waiting -- correct behaviour, but new wall-clock nobody has costed.
- **J1 re-runs caused by attended commits.** Measured 2026-08-10, and it is the sharpest cost number of the cycle: section 36 shipped at 12:06 and did not roll over until 12:23 because three operator commits moved the tree under its smoke receipt, forcing `j1a -> j1b -> j1c`. Roughly 17 minutes of a build+test+smoke chain, paid twice. Measure it per attended session, because the fix is behavioural (repair early in a section, not at the ship boundary) and only a number will make it stick.
- **Backgrounded ship push.** The push is backgrounded and polled rather than inline. Measure poll iterations and tokens per ship against the previous inline cost.
- **Agent-result cache hit rate.** Carried standing metric. Baseline needed: `cache-hit` entries versus total dispatches per segment.
- **Segment-start orientation cost.** Every relaunch re-reads state, runs the triage oracle and rebuilds the graph cache. Baseline needed: tokens from segment start to first section edit. `section-pack.py` now resolves symbols for non-kernel sections and carries `spawn_chain` provenance, both of which should REDUCE the exploration that follows orientation -- measure whether they do.

## Filed by the 2026-08-13 run, TODO-10 §16 ship -- EXCLUDED, below the bar

- [ ] EXCLUDED (below the >= 2% bar, recorded so a later pass does not re-derive it): the ~22 full gate runs on this section cost ~90 minutes of wall-clock but almost no TOKENS.
  - Every one went through `run-artifact.sh`, so a green run returns a bounded JSON envelope and spends zero model tokens on the output. The in-context cost per gate is one Bash call plus a ~6-line tail -- far under 2% of the segment even multiplied by 22.
  - Recorded here anyway because the wall-clock number is large enough to LOOK like a token problem at a glance. The correct home is the FLOW finding in `overnight-runner-improvements-v14.md` (gate cadence inside a fix loop), not this file. The artifact wrapper is doing exactly what it was built to do, and this is evidence it works.
