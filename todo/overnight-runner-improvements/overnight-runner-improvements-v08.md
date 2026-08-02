# Overnight Runner Improvements v08 -- Findings (opened 2026-08-02)

Findings from the run armed after the 2026-08-02 repair stop. This file is a CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `overnight-runner-improvements-vNN.md` in this directory, which is this one until an operator opens v09.

**Why findings land here instead of being fixed.** The run is explicitly NOT permitted to modify its own control plane while unattended -- `.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json` -- nor the RECEIPT SURFACE (`Makefile*`, `scripts/build.sh`, the ABI generator), which `receipt_surface_guard.py` enforces: build/verification machinery whose content the run's own receipts are computed over must not drift while nobody is watching. Ordinary work is unaffected: kernel code, tests, docs and TODO files are fixed in place as normal.

**Self-improvement is the point of this file, not a side effect.** When the run hits a runner defect -- a gate that misfires, a wedge, a flow inefficiency, an evasion it was tempted into -- the finding is RECORDED HERE in the same turn it is observed, then the run continues.

**What to write.** One item per finding, house style: what was observed live (timestamps, file:line), the mechanism confirmed at source, and what it would cost or risk. A projection is not a finding. If a fix is obvious, describe it -- do not apply it. Lead line <= 250 chars; body as sub-bullets, none over 1,000 chars.

**Scope:** runner flow, gates, wedges, correctness of the run machinery. Cost/token findings go to the newest [`token-saver-vNN.md`](../token-saver/token-saver-v08.md) instead.

## What shipped in the 2026-08-02 repair stop, and is therefore under test

- **The post-commit amend no longer widens commits.** `.githooks/post-commit` amended with a bare `git commit --amend`, which commits the WHOLE INDEX -- so every path-limited `git commit -- <paths>` in the repo was silently converted to a full-index commit after the fact. Three index sweeps over two days were each blamed on something else. Now `--amend --only -- <paths>`, mutation-tested. **This is the single most load-bearing fix of the stop**: it is what makes attended repair alongside a live run actually safe rather than nominally safe.
- **Section-count cap, 40 soft / 60 hard**, enforced on GROWTH not existence, so an oversized file can still finish. At the cap a discovered gap is filed in the DOMAIN-CORRECT TODO with its Implementation Order row and a reciprocal XREF -- never dropped, and never into a `-part-2` bucket.
- **Reachability gate** (`scripts/todo-reachability.py` + lint Check 24): ERROR on work in DONE sections that nothing will revisit (currently 0), WARN on the 333-item deferred-shape backlog.
- **Section placement** is now blocking (Check 22b), and three files were repaired as verified pure block moves.
- **Rotation observability**: the hint writes `.claude/overnight/advisories.jsonl` itself rather than depending on the stream, which does not carry `attachment` events.

## Carried forward from v07 (still open)

- **333 `open-in-deferred` items need per-item repair to `- [/]` naming the blocker.** NOT a bulk rewrite: each needs its owner identified. Lint Check 24 warns; the count is the progress metric.
- **60 stranded deferrals that "will NOT flip naturally"** (`stranded_deferrals.py`'s own words) because they sit in DONE-parked sections fixpoint never revisits. P6.1 (owner-side sweep) and P6.3 (fixpoint gate) exist but fire FORWARD only. The rescue path is **P6.4 bulk backfill**, recorded in that script's header as "deliberately unbuilt". 2 candidates are classified `flip` (safe auto-reopen) and are the cheapest starting point.
- **v06 #5 -- read-only gates block info-gathering Bash by SHAPE rather than effect.** Hit repeatedly during both stops: a pure `grep`/`find` command refused for beginning with a variable assignment. Costs retries, never data.
- **v06 #9 -- the commit gate binds reviews to blob SHAs**, so fixing a finding invalidates the leg that raised it. DECISION RECORD, deliberately unfixed: the binding is what makes a review verdict mean anything. Needs evidence that the existing convergence path fails in practice, not a weakening on suspicion.
- **A pure block move trips the item-length backstop.** `todo-staged-check.py` judges ADDED lines, and a reordering re-adds every line it moves, so legacy over-cap items read as new. Repair tooling and guard disagree about what "added" means.

## Standing measurement obligations for this run

- **Does the rotation ACT?** 34.4h and 65 hint records produced **0** `rollover-wip`. The threshold is innocent (three rounds proved it); the live hypothesis is that segments now run 142-335 countable events where the feature was justified by 334-768, so the post-hint runway is too short to be worth a relaunch. Record countable events per segment and whether any segment both exceeds ~250 events AND rotates. **Do not change `ROTATE_HINT_TURNS` without a measured saving.**
- **Does the clean stop work?** The 24h deadline machinery has NEVER fired -- the canary was stopped early by the operator. If this run is armed with `--hours`, its stop is the first real test.

---

_No findings yet._
