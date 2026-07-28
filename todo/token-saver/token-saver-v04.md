# Token Saver v04 -- 3-Day Canary Cost Findings (3-day canary, armed 2026-07-28)

Findings from the 3-day unattended canary armed 2026-07-28. This file is a CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run.

**Why findings land here instead of being fixed.** The run is explicitly NOT permitted to modify its own control plane while unattended -- `.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json`. Over three days with nobody watching, a bad edit to a gate or the sequencer skill is how a run becomes unrecoverable, and the runner suite gate runs at commit but cannot judge intent. Ordinary work is unaffected: kernel code, tests, docs and TODO files are fixed in place as normal, and the run SHOULD keep doing what it did on 2026-07-28 when it root-caused and fixed `test-smoke-matrix.sh` destroying its own leg logs.

**What to write.** One item per finding, in the house style: what was observed live (with timestamps and file:line), the mechanism confirmed at source, and what it would cost or risk. A projection is not a finding. If a fix is obvious, describe it -- do not apply it.

**Scope:** cost only. The >= 2% bar from v03 still applies -- anything measured below it is recorded as excluded, not filed as work. Runner/flow findings go to [`overnight-runner-improvements-v04.md`](../overnight-runner-improvements/overnight-runner-improvements-v04.md) instead.

**Baseline to compare against:** the 13:59 canary of 2026-07-28 -- 893 turns, $505.36, $80.64/hour, static base 58.3K, accumulation ~1,000-1,200 tok/turn. The split threshold was calibrated to `items >= 5` just before this arm, so a shift in section length distribution is expected and is the first thing to check.

---

_No findings yet._
