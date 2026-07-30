# Token Saver v05 -- Month-Run Cost Findings (armed 2026-07-30)

Cost and token findings from the long-horizon unattended run armed 2026-07-30. CAPTURE surface, not a work queue: outside the sequencer's traversal, nothing here is implemented by the run. It is the CURRENT capture file -- when a new version supersedes it, the sequencer files to the newest `token-saver-vNN.md` in this directory.

**Why findings land here instead of being fixed.** Cost machinery is control plane (`.claude/hooks/**`, `scripts/overnight/**`) or receipt surface, both off-limits unattended. Record the finding in the same turn it is observed, then continue; a finding carried in-context dies with the segment.

**What to write.** One item per finding: what was observed live (run id, segment, numbers), the mechanism confirmed at source, and the measured or bounded cost. **The >= 2% bar applies** -- anything measured below 2% of the relevant bucket is recorded as excluded, not filed as work. A projection is not a finding. Lead <= 250 chars; sub-bullet bodies <= 1,000.

**Standing measurement obligations for this run (both are one `cost-summary.py` invocation at any segment end):**

- **Rotation efficacy (v04 item 1 follow-through).** The mid-section rotation shipped 2026-07-30 with a MODELLED 31-41% cache-read saving. Compare per-segment tool-events and `end-of-segment` context against the 334/420/768 and 492K/542K/798K baselines; record the measured saving (or its absence) here as a finding either way.
- **Re-read sizing (v04 item 2, still `[/]`).** `tool_history_writer.py` now records `{offset, limit, bytes}` per Read, so the first segment of THIS run is the first sizeable one. Run `cost-summary.py` on it and record the re-read share vs the >= 2% bar; that measurement closes or kills the v04 item.

---

_No findings yet._
