# Overnight Runner Improvements v05 -- Month-Run Findings (armed 2026-07-30)

Findings from the long-horizon unattended run armed 2026-07-30 (month-scale target; the arm itself is an attended canary first). This file is a CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- when a new version supersedes it, the sequencer files to the newest `overnight-runner-improvements-vNN.md` in this directory.

**Why findings land here instead of being fixed.** The run is explicitly NOT permitted to modify its own control plane while unattended -- `.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json` -- nor the RECEIPT SURFACE (`Makefile*`, `scripts/build.sh`, the ABI generator), which `receipt_surface_guard.py` enforces: build/verification machinery whose content the run's own receipts are computed over must not drift while nobody is watching, because a subtly wrong `build.sh` keeps producing green receipts that no longer mean what they say. Ordinary work is unaffected: kernel code, tests, docs and TODO files are fixed in place as normal.

**Self-improvement is the point of this file, not a side effect.** When the run hits a runner defect -- a gate that misfires, a wedge, a flow inefficiency, an evasion it was tempted into -- the finding is RECORDED HERE in the same turn it is observed, then the run continues. Do not carry findings in-context to report later; a finding that lives only in the conversation dies with the segment.

**What to write.** One item per finding, house style: what was observed live (timestamps, file:line), the mechanism confirmed at source, and what it would cost or risk. A projection is not a finding. If a fix is obvious, describe it -- do not apply it. Lead line <= 250 chars; body as sub-bullets, none over 1,000 chars.

**Scope:** runner flow, gates, wedges, correctness of the run machinery. Cost/token findings go to the newest [`token-saver-vNN.md`](../token-saver/token-saver-v05.md) instead.

**Baselines this run should be compared against (2026-07-30):** mid-section rotation ACTIVE (`ROTATE_HINT_TURNS = 200`); prior segments ran 334/420/768 tool-events to 492K/542K/798K end-of-segment context; the rotation's 31-41% cache-read saving is MODELLED, not yet observed -- the first thing this run proves or disproves.

---

## 1. runner-doctor's expiry prune dirties a tracked file the rollover gate does not tolerate

- [ ] Classify the launcher's own live-gotchas expiry prune as tolerated dirt (or sweep it at launch), so a mid-run expiry cannot block a ship rollover
  - **Observed 2026-07-31 00:05, at arm time.** `.claude/state/live-gotchas.md` showed modified: exactly one deleted line, the 2026-07-28 entry carrying `(expires 2026-07-30)`. `runner-doctor.py` (invoked by `overnight-launch.sh:158` at every real launch) prunes expired entries and, correctly, never commits -- the launcher is not a committer -- so the deletion sits as working-tree dirt.
  - **The gate does not tolerate it.** `_ROLLOVER_AUTOGEN` (`run_phase_guard.py:426`) lists only coverage.json/coverage.md/COUNT.md/todo-graph.md, so the prune classifies `tracked-source` and a ship rollover refuses with "tree not clean". Repaired this time by an attended commit (`f0802dc1`) seconds after launch.
  - **This WILL recur mid-run**: two live entries carry `(expires 2026-08-15)`. When a watchdog relaunch on/after that date prunes them, the tree dirties with nobody attending, and the next ship rollover blocks until the run itself decides to own the file -- which no doctrine currently tells it to do.
  - **Proposed shape (control plane, file-not-fix while unattended):** either add `.claude/state/live-gotchas.md` to the tolerated set when the diff is deletion-only (a prune can only remove lines; an ADDITION is run/operator content and must stay blocking), or have the run own-and-commit doctor prunes as pipeline output the way it owns XREF line-number repairs. The deletion-only classifier is the narrower and safer of the two.
