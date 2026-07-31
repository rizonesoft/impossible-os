# Overnight Runner Improvements v06 -- Month-Run Findings (armed 2026-07-31)

Findings from the long-horizon unattended run armed 2026-07-31 (month-scale target; the arm itself is an attended canary first). This file is a CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `overnight-runner-improvements-vNN.md` in this directory, which is this one until an operator opens v07.

**Why findings land here instead of being fixed.** The run is explicitly NOT permitted to modify its own control plane while unattended -- `.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json` -- nor the RECEIPT SURFACE (`Makefile*`, `scripts/build.sh`, the ABI generator), which `receipt_surface_guard.py` enforces: build/verification machinery whose content the run's own receipts are computed over must not drift while nobody is watching, because a subtly wrong `build.sh` keeps producing green receipts that no longer mean what they say. Ordinary work is unaffected: kernel code, tests, docs and TODO files are fixed in place as normal.

**Self-improvement is the point of this file, not a side effect.** When the run hits a runner defect -- a gate that misfires, a wedge, a flow inefficiency, an evasion it was tempted into -- the finding is RECORDED HERE in the same turn it is observed, then the run continues.

**What to write.** One item per finding, house style: what was observed live (timestamps, file:line), the mechanism confirmed at source, and what it would cost or risk. A projection is not a finding. If a fix is obvious, describe it -- do not apply it. Lead line <= 250 chars; body as sub-bullets, none over 1,000 chars.

**Scope:** runner flow, gates, wedges, correctness of the run machinery. Cost/token findings go to the newest [`token-saver-vNN.md`](../token-saver/token-saver-v06.md) instead.

**What shipped between v05 and this arm, and is therefore under test on this run:**

- `_is_gotcha_expiry_prune()` in `run_phase_guard.py` tolerates the launcher's deletion-only `.claude/state/live-gotchas.md` prune at a ship rollover (v05 item 1). Two entries expire 2026-08-15 and 2026-08-29 inside this run's horizon, so the tolerance will be exercised unattended -- a rollover that still refuses with "tree not clean" on that file is the finding.
- The `awaiting-operator` deferral token is live in six sections (v05 item 2); `--blockers` lists them and `--next` returns BLOCKED while they remain, so fixpoint cannot be declared over outstanding human work. A fixpoint claimed with `awaiting-operator` sections outstanding is the finding.
- `ROTATE_HINT_TURNS` dropped 200 -> 90; the rotation's behaviour is measured in [`token-saver-v06.md`](../token-saver/token-saver-v06.md), but a gate misfire it causes (a `rollover-wip` attempt refused at a boundary that should have been clean) belongs here.

---

_No findings yet._
