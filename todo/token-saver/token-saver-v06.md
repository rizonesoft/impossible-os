# Token Saver v06 -- Month-Run Cost Findings (armed 2026-07-31)

Cost and token findings from the long-horizon unattended run armed 2026-07-31. CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `token-saver-vNN.md` in this directory, which is this one until an operator opens v07.

**Why findings land here instead of being fixed.** Cost machinery is control plane (`.claude/hooks/**`, `scripts/overnight/**`) or receipt surface, both off-limits unattended. Record the finding in the same turn it is observed, then continue; a finding carried in-context to "report later" dies with the segment.

**What to write.** One item per finding: what was observed live (run id, segment, numbers), the mechanism confirmed at source, and the measured or bounded cost. **The >= 2% bar applies** -- anything measured below 2% of the relevant bucket is recorded as excluded, not filed as work. A projection is not a finding. Lead <= 250 chars; sub-bullet bodies <= 1,000.

**Standing measurement obligations for this run (all three are one `cost-summary.py` + `section-cost-report.py` invocation at any segment end):**

- **Rotation efficacy, the real test (v05 item 1 follow-through).** `ROTATE_HINT_TURNS` was re-derived from 200 to **90** countable SECTIONS-phase events on 2026-07-31 after the v05 forensics showed the hint firing at event 201 of 201 -- correct and useless, with no runway left. At 90 it should fire roughly 45% into a segment of the observed shape. Record for the first segments of this run: did `hook: [rotate-hint]` appear in the run log, at which event, and did any `rollover-wip` attempt follow? The 31-41% cache-read saving stays MODELLED until a segment actually rotates mid-section; a segment that fires the hint and still runs to a natural SHIP rollover is a finding too.
- **Advisory-hook observability (v05 item 1, second half).** `stream-report.py` now logs bracketed-tag advisory `systemMessage`s as `hook: [tag] ...` and `_bash` clips at 500 rather than 200 chars. Confirm on this run's first segment that both hold live -- a log-based diagnostic that is quietly unreliable costs a forensic pass every time it is trusted.
- **Re-read sizing (carried from v04 item 2, still `[/]`).** `tool_history_writer.py` records `{offset, limit, bytes}` per Read. Run `cost-summary.py` on the first sizeable segment and record the re-read share against the >= 2% bar; that measurement closes or kills the v04 item.

**Baselines to compare against (2026-07-30/31):** prior segments ran 334 / 420 / 768 tool-events to 492K / 542K / 798K end-of-segment context; segment `run-20260731-000502` ran 308 turns / 364 tool-events (253 countable, 201 of them in SECTIONS) to 520,582 end-of-segment context at 1,311 tok/turn.

---

_No findings yet._
