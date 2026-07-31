# Overnight Runner Improvements v07 -- 24h Canary Findings (armed 2026-08-01)

Findings from the 24-hour canary armed after the 2026-07-31 repair stop. This file is a CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `overnight-runner-improvements-vNN.md` in this directory, which is this one until an operator opens v08.

**Why findings land here instead of being fixed.** The run is explicitly NOT permitted to modify its own control plane while unattended -- `.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json` -- nor the RECEIPT SURFACE (`Makefile*`, `scripts/build.sh`, the ABI generator), which `receipt_surface_guard.py` enforces: build/verification machinery whose content the run's own receipts are computed over must not drift while nobody is watching, because a subtly wrong `build.sh` keeps producing green receipts that no longer mean what they say. Ordinary work is unaffected: kernel code, tests, docs and TODO files are fixed in place as normal.

**Self-improvement is the point of this file, not a side effect.** When the run hits a runner defect -- a gate that misfires, a wedge, a flow inefficiency, an evasion it was tempted into -- the finding is RECORDED HERE in the same turn it is observed, then the run continues.

**What to write.** One item per finding, house style: what was observed live (timestamps, file:line), the mechanism confirmed at source, and what it would cost or risk. A projection is not a finding. If a fix is obvious, describe it -- do not apply it. Lead line <= 250 chars; body as sub-bullets, none over 1,000 chars.

**Scope:** runner flow, gates, wedges, correctness of the run machinery. Cost/token findings go to the newest [`token-saver-vNN.md`](../token-saver/token-saver-v07.md) instead.

**This run is a 24-HOUR CANARY with pre-registered acceptance criteria.** It stops itself cleanly at the deadline (the launcher refuses to spawn past it, so the last segment ends at its own rollover) and aborts early on: breaker trip, two consecutive segments with no section shipped, a rotation loop (>1 rotation per section), or an unexplained gate refusal. The criteria it is being judged against are recorded in [`docs/overnight/run-log.md`](../../docs/overnight/run-log.md) and must be judged on evidence, not impression.

**What shipped in the 2026-07-31 repair stop, and is therefore under test on this run:** filled in at arm time.

**Carried forward from v06 (still open):** filled in at arm time -- every v06 item not resolved during the repair stop, with a back-pointer to [`overnight-runner-improvements-v06.md`](overnight-runner-improvements-v06.md).

---

_No findings yet._
