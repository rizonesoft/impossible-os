# Overnight Runner v04 -- 3-Day Canary Findings (3-day canary, armed 2026-07-28)

Findings from the 3-day unattended canary armed 2026-07-28. This file is a CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run.

**Why findings land here instead of being fixed.** The run is explicitly NOT permitted to modify its own control plane while unattended -- `.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json`. Over three days with nobody watching, a bad edit to a gate or the sequencer skill is how a run becomes unrecoverable, and the runner suite gate runs at commit but cannot judge intent. Ordinary work is unaffected: kernel code, tests, docs and TODO files are fixed in place as normal, and the run SHOULD keep doing what it did on 2026-07-28 when it root-caused and fixed `test-smoke-matrix.sh` destroying its own leg logs.

**What to write.** One item per finding, in the house style: what was observed live (with timestamps and file:line), the mechanism confirmed at source, and what it would cost or risk. A projection is not a finding. If a fix is obvious, describe it -- do not apply it.

**Scope:** runner behaviour, gates, wedges, flow. Cost findings go to [`token-saver-v04.md`](../token-saver/token-saver-v04.md) instead.

**Predecessor:** [`overnight-runner-improvements-v03.md`](overnight-runner-improvements-v03.md) is the implementation list that preceded this canary; five of its six items shipped before the arm.

---

_No findings yet._
