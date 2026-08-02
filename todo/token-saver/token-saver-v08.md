# Token Saver v08 -- Cost Findings (opened 2026-08-02)

Cost and token findings from the run armed after the 2026-08-02 repair stop. CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `token-saver-vNN.md` in this directory, which is this one until an operator opens v09.

**Why findings land here instead of being fixed.** Cost machinery is control plane (`.claude/hooks/**`, `scripts/overnight/**`) or receipt surface, both off-limits unattended. Record the finding in the same turn it is observed, then continue; a finding carried in-context to "report later" dies with the segment.

**What to write.** One item per finding: what was observed live (run id, segment, numbers), the mechanism confirmed at source, and the measured or bounded cost. **The >= 2% bar applies.** A projection is not a finding. Lead <= 250 chars; sub-bullet bodies <= 1,000.

## THE standing question, unchanged and now three rounds old: does the mid-section rotation pay?

The 34.4h canary of 2026-08-01 settled everything EXCEPT whether it works:

- **It fires.** 65 records in `.claude/overnight/advisories.jsonl` across 12 segments -- the first canary in which this was observable at all.
- **It is delivered.** The systemMessage reaches the run's transcript.
- **It never acts.** **0** `rollover-wip` attempts in 12 segments.
- **The threshold is innocent.** Three rounds have adjusted or repaired this feature -- 200 (a mis-derived turn/event conversion), 90 (correct), then a transport defect (`attachment` events never reach the stream `stream-report` parses), then a message that still said "wait for a boundary" while the doctrine said "create one". Each time the NUMBER was blamed and each time it was not the cause.

**The surviving hypothesis is that the premise expired.** The feature was justified by segments of 334 / 420 / 768 tool-events. Measured since: **142, 148, 167, 169, 253, 259, 288, 291, 292, 335**. Roughly half now run under 260, so a hint at 90 leaves too little runway to be worth a relaunch -- and the run's own escape ("if the next action is the ship, skip") correctly declines. On 2026-08-01 the hint fired at 04:48 and the section staged its commit at 04:50:26.

**So the measurement this run owes, before ANY further tuning:**

- countable SECTIONS-phase events per segment, and what fraction exceed ~250 (where >= 100 events of post-hint runway exist);
- for any segment that DOES rotate: its end-of-segment context and cache-read share against a non-rotating segment of similar length;
- whether more than one rotation ever occurs in a single section (an abort condition, not a saving).

If long segments turn out to be a small minority, the honest conclusion is that the rotation is a TAIL-RISK GUARD rather than a cost lever, and its threshold should RISE so it stops firing where it cannot pay. **Do not change `ROTATE_HINT_TURNS` on reasoning alone -- it has been wrong three times.**

**Baselines (measured 2026-07-31 / 2026-08-01):** prior segments reached 492K / 542K / 798K end-of-segment context; one segment cost **$179.27** for a single section with cache-read at **85.3%** of spend.

---

_No findings yet._
