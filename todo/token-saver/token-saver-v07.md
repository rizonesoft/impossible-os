# Token Saver v07 -- 24h Canary Cost Findings (armed 2026-08-01)

Cost and token findings from the 24-hour canary armed after the 2026-07-31 repair stop. CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `token-saver-vNN.md` in this directory, which is this one until an operator opens v08.

**Why findings land here instead of being fixed.** Cost machinery is control plane (`.claude/hooks/**`, `scripts/overnight/**`) or receipt surface, both off-limits unattended. Record the finding in the same turn it is observed, then continue; a finding carried in-context to "report later" dies with the segment.

**What to write.** One item per finding: what was observed live (run id, segment, numbers), the mechanism confirmed at source, and the measured or bounded cost. **The >= 2% bar applies** -- anything measured below 2% of the relevant bucket is recorded as excluded, not filed as work. A projection is not a finding. Lead <= 250 chars; sub-bullet bodies <= 1,000.

**THE standing measurement for this run: does the mid-section rotation finally fire, and what does it save?** v06 established by direct measurement that the threshold is correct (fired at exactly 90), the systemMessage IS delivered (4 occurrences in the run's own transcript), and the feature is nonetheless inert because `rollover-wip` needs committed-but-unpushed work that the runner's workflow never produces -- 0 attempts across every segment of 2026-07-31. The repair stop addressed both halves. So for THIS run, record:

- **Did it fire, visibly?** `grep -c "context-rotation hint"` over each segment log should now be non-zero. A zero here means the observability half regressed and every later cost number in this file is suspect.
- **Did it act?** Count `rollover-wip` attempts and successes per section. Zero successes with non-zero hints means the precondition is still unsatisfiable and the doctrine fix did not take.
- **What did it save?** Compare per-segment end-of-segment context and cache-read share against the 2026-07-31 baselines below. The 31-41% saving has been MODELLED since 2026-07-30 and has never once been observed; this run either measures it or kills it.
- **Did it overfire?** More than one rotation per section is an abort condition, not a saving.

**Baselines to compare against (measured 2026-07-31):** segments ran **142, 169, 253, 259, 288, 291, 335** countable tool-events; prior segments reached 492K / 542K / 798K end-of-segment context; one measured segment cost $179.27 for a single section with cache-read at **85.3%** of spend.

---

_No findings yet._
