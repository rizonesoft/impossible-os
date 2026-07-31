# Token Saver v05 -- Month-Run Cost Findings (armed 2026-07-30)

Cost and token findings from the long-horizon unattended run armed 2026-07-30. CAPTURE surface, not a work queue: outside the sequencer's traversal, nothing here is implemented by the run. It is the CURRENT capture file -- when a new version supersedes it, the sequencer files to the newest `token-saver-vNN.md` in this directory.

**Why findings land here instead of being fixed.** Cost machinery is control plane (`.claude/hooks/**`, `scripts/overnight/**`) or receipt surface, both off-limits unattended. Record the finding in the same turn it is observed, then continue; a finding carried in-context dies with the segment.

**What to write.** One item per finding: what was observed live (run id, segment, numbers), the mechanism confirmed at source, and the measured or bounded cost. **The >= 2% bar applies** -- anything measured below 2% of the relevant bucket is recorded as excluded, not filed as work. A projection is not a finding. Lead <= 250 chars; sub-bullet bodies <= 1,000.

**Standing measurement obligations for this run (both are one `cost-summary.py` invocation at any segment end):**

- **Rotation efficacy (v04 item 1 follow-through).** The mid-section rotation shipped 2026-07-30 with a MODELLED 31-41% cache-read saving. Compare per-segment tool-events and `end-of-segment` context against the 334/420/768 and 492K/542K/798K baselines; record the measured saving (or its absence) here as a finding either way.
- **Re-read sizing (v04 item 2, still `[/]`).** `tool_history_writer.py` now records `{offset, limit, bytes}` per Read, so the first segment of THIS run is the first sizeable one. Run `cost-summary.py` on it and record the re-read share vs the >= 2% bar; that measurement closes or kills the v04 item.

---

## 1. The mid-section rotation did not fire in a 308-turn segment; the threshold counts a narrower event set than it appears to

- [ ] Re-derive `ROTATE_HINT_TURNS` against SECTIONS-phase countable events, not raw tool-events, or the trigger sits above every real segment
  - **Observed 2026-07-31 00:05-03:24, segment `run-20260731-000502`** -- the FIRST segment after the rotation was re-enabled, so this is the feature's first live test. It ran 308 turns / 364 tool-events and ended on a normal SHIP rollover. `grep -c "context-rotation hint"` over the segment log: **0**. `rollover-wip` attempts: **0**.
  - **The threshold should have been crossed on the face of it.** `ROTATE_HINT_TURNS = 200`, and the hook's matcher set (`Bash|Read|Grep|Glob|Agent|Task`) accounts for **253** of the segment's events (Bash 176, Read 41, Grep 32, Agent 2, Glob 2). Edit/Write (98) are deliberately not counted.
  - **Most likely mechanism, NOT yet confirmed.** `_in_sections()` counts only while `phase == SECTIONS`, and this segment spent two full file-pipelines' worth of turns in TRIAGE/VALIDATE/GAP_AUDIT (it opened on TODO-01, deferred §13, advanced to TODO-04). So the EFFECTIVE threshold in wall terms is well above 200 raw events. A second, unexcluded possibility: the hint fired and `stream-report` simply does not log advisory hook `systemMessage`s (it does log hook ERRORS, e.g. the 20:16:47 PreToolUse block on 2026-07-30) -- in which case the mechanism is fine and only the observability is missing.
  - **Cheap discriminator, no instrumentation needed.** `.claude/state/rotate-hint.json` now persists between rollovers and is readable live: watch `count` climb during the next segment. If it tracks the countable-event rate, the threshold is merely too high; if it stalls well below while SECTIONS work continues, the phase gate is eating the count. Do this before changing any number.
  - **Cost of the ambiguity.** The 31-41% cache-read saving this feature was re-enabled for is still unrealised: this segment paid full quadratic cost (308 turns) with the rotation armed and silent. Until the discriminator runs, treat the re-enable as UNPROVEN rather than shipped -- v04 item 1 recorded the saving as modelled, and this is the first evidence bearing on it.

_No further findings yet._
