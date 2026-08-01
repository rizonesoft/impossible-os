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

## 1. The 3a observability fix was verified against the wrong source and does not work on the live path

- [x] Write the advisory record from the hook itself instead of depending on what the stream carries
  - **Observed 2026-08-01 03:12 on the canary's FIRST segment** (`run-20260801-000513`, 292 countable events, threshold 90). The hint fired: the run's session transcript carries **8** `context-rotation hint` occurrences. The run LOG carries **0**. So the fix shipped hours earlier at `646cb2a3` did not take effect on the path that matters.
  - **Mechanism, and it is a testing error rather than a coding one.** `stream-report.py` was taught to decode `attachment` events, and that was verified by REPLAYING transcript lines through it -- which proves it handles those events IF they arrive, not that they arrive. `attachment` events are written to the session TRANSCRIPT; they are not emitted on `claude -p --output-format stream-json` stdout, which is the only thing `stream-report` reads. The replay was evidence about the parser, presented as evidence about the pipeline.
  - **Fixed by removing the dependency**: `rotate_hint.py` now appends its own line to `.claude/overnight/advisories.jsonl` when the hint fires. That survives the rollover which unlinks the counter, so "did the rotation fire, and at what count" is a grep rather than a forensic reconstruction of state that no longer exists. The `stream-report` decoder is retained -- it is correct, and costs nothing if the stream ever does carry these events.
  - **The 3b half also missed, for a separate and embarrassing reason.** The doctrine in `SKILL.md` was changed to CREATE the WIP boundary, but the message the run actually reads at the moment of firing still said "At the NEXT WIP-clean boundary" -- the old wait-for-one wording. The run received a create instruction in doctrine and a wait instruction in the notification, four times, and did the latter. `rollover-wip` attempts this segment: **0**. The message now carries the create instruction and the measurement that justifies it.
  - **Lesson worth keeping**: two rounds in a row, this feature was declared fixed on evidence that did not cover the live path (v05 patched the PreToolUse shape and missed PostToolUse; this round patched the parser and missed the transport). The acceptance criterion for the NEXT round is not "the code looks right" but a non-zero `advisories.jsonl` and a verified `rollover-wip` in a real segment.
