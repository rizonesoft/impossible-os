# Overnight Runner Improvements v05 -- Month-Run Findings (armed 2026-07-30)

Findings from the long-horizon unattended run armed 2026-07-30 (month-scale target; the arm itself is an attended canary first). This file is a CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- when a new version supersedes it, the sequencer files to the newest `overnight-runner-improvements-vNN.md` in this directory.

**Why findings land here instead of being fixed.** The run is explicitly NOT permitted to modify its own control plane while unattended -- `.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json` -- nor the RECEIPT SURFACE (`Makefile*`, `scripts/build.sh`, the ABI generator), which `receipt_surface_guard.py` enforces: build/verification machinery whose content the run's own receipts are computed over must not drift while nobody is watching, because a subtly wrong `build.sh` keeps producing green receipts that no longer mean what they say. Ordinary work is unaffected: kernel code, tests, docs and TODO files are fixed in place as normal.

**Self-improvement is the point of this file, not a side effect.** When the run hits a runner defect -- a gate that misfires, a wedge, a flow inefficiency, an evasion it was tempted into -- the finding is RECORDED HERE in the same turn it is observed, then the run continues. Do not carry findings in-context to report later; a finding that lives only in the conversation dies with the segment.

**What to write.** One item per finding, house style: what was observed live (timestamps, file:line), the mechanism confirmed at source, and what it would cost or risk. A projection is not a finding. If a fix is obvious, describe it -- do not apply it. Lead line <= 250 chars; body as sub-bullets, none over 1,000 chars.

**Scope:** runner flow, gates, wedges, correctness of the run machinery. Cost/token findings go to the newest [`token-saver-vNN.md`](../token-saver/token-saver-v05.md) instead.

**Baselines this run should be compared against (2026-07-30):** mid-section rotation ACTIVE (`ROTATE_HINT_TURNS = 200`); prior segments ran 334/420/768 tool-events to 492K/542K/798K end-of-segment context; the rotation's 31-41% cache-read saving is MODELLED, not yet observed -- the first thing this run proves or disproves.

---

_No findings yet._
