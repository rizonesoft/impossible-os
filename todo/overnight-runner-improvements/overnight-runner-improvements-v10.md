# Overnight Runner Improvements v10 -- Findings (opened 2026-08-05)

Findings from the run armed after the 2026-08-05 attended stop. This file is a CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `overnight-runner-improvements-vNN.md` in this directory, which is this one until an operator opens v11.

**Why findings land here instead of being fixed.** The run is explicitly NOT permitted to modify its own control plane while unattended -- `.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json` -- nor the RECEIPT SURFACE (`Makefile*`, `scripts/build.sh`, the ABI generator). Ordinary work is unaffected: kernel code, tests, docs and TODO files are fixed in place as normal.

**Write the MECHANISM as a hypothesis unless you confirmed it at source.** Three of v09's findings named a plausible-but-wrong cause for a real symptom: a review-KIND mismatch that was an evidence TTL, a step-8 "broker not recognised" that was first-match truncation, and a flaky test blamed on state collision that reproduces under neither proposed cause. The SYMPTOM is what makes a finding valuable and is always worth recording; the mechanism is worth recording too, but marked for what it is. "Refused with X while Y was true" beats a confident wrong diagnosis.

**COMPLETION-FIRST IS NOT WEAKENED, AND SECTION COUNT IS NOT THE METRIC.** If a TODO genuinely needs 30 more sections, the run CREATES all 30 and FINISHES them; cross-domain work is filed in the owning TODO with its Implementation Order row and a reciprocal XREF, exactly as before. The only question is whether a section is COMPLETION (capability a real user hits next, a genuine coverage gap) or CLUTTER (recursive refinement of an already-adequate path, each section spawned by the previous one's review, worth measurably less each round). **When you cannot tell, FILE IT** -- dropped work is unrecoverable and invisible; clutter is merely expensive and stays visible.

**Scope:** runner flow, gates, wedges, correctness of the run machinery. Cost/token findings go to the newest [`token-saver-vNN.md`](../token-saver/token-saver-v10.md) instead.

## What shipped in the 2026-08-05 attended stop, and is therefore under test

**If any of these misfires it is a regression in brand-new code, not a legacy quirk: file it immediately.**

- **Spawn-chain sensor.** Sections record `> **Spawned-by:** §N (split|review)`. A `(split)` is decomposition at authoring time and contributes ZERO depth; consecutive `(review)` spawns accumulate. At depth >= 3 a further review-spawn needs a structured continuation waiver (`user_impact`, `not_parkable`, `severity_trend`, `surface`). Without one the finding is PARKED in the parent as `- [/]` -- filed either way. Additive: unmarked sections are roots, so the existing corpus reports clean.
- **`standing:` marker.** A recurring task (an annual re-review) stays `- [ ]` and carries `standing:`, which exempts it from `open-in-deferred`. Without it the doctrine and the completion gate contradicted each other and a CORRECTLY-shaped corpus could never reach fixpoint. Author-time, never inferred -- that is what distinguishes it from the `parked-ownerless` detector removed on 2026-08-02.
- **Check 7 counts what it cannot resolve**, with a coverage FLOOR in `scripts/lint/stub-lint-baseline.json` (55/189 today). A drop is an ERROR (rc 7) because an unresolved symbol yields no finding -- losing resolution looks exactly like passing. This gate exists because a resolver rewrite took coverage 55 -> 1 while the full suite stayed green.
- **`format-md-tables.py` measures DISPLAY width** (wide glyphs, VARIATION SELECTOR-16) and **re-escapes pipes on render**. It had been silently rewriting `\|` inside a cell as a column separator, making the table ragged so it would never be touched again. `--max-pad` exists but defaults OFF: capping breaks alignment for over-cap cells, which is worse than a wide table.
- **The deferred-item drain has two owners** -- FILE_CLOSE for its own file, ADVANCE for one already-DONE-or-BLOCKED file per advance. Every one of the 1,595 items now has an owner (806 / 789).
- **The run-log gate yields after 3 refusals** rather than wedging the rollover.

## Carried forward from v09 (still open)

- **CLOSE-OUT MUST VERIFY the `open-in-deferred` backlog is draining.** BASELINE at this arm: **1,595 items / 333 sections / 52 files**, split 806 NEEDS_WORK (FILE_CLOSE owns) + 702 DONE + 87 BLOCKED (ADVANCE owns both). Check the per-class split, not just the total -- the two owners fail independently. **Expect `stranded_deferrals.py` to RISE from 61**: it scans only `- [/]` items, so repairing shape FEEDS it. A rise is the mechanism working. The regression signal is the inverse -- Check 24 falling while stranded stays flat, meaning items were parked without naming an owner.
- **`section_commit_gate` sub-test "small-section commit with full evidence allows"** was filed intermittently red; NOT REPRODUCED in 40 consecutive runs, with BOTH proposed mechanisms ruled out at source (state paths are already fixture-scoped; `_repo_root()` resolves to the fixture under an inherited `GIT_DIR`). Fixture hardened preventively. Kept open: 40 runs bound a rate, they do not prove absence.
- **60 stranded deferrals that "will NOT flip naturally."** P6.4 bulk backfill remains "deliberately unbuilt"; 5 are classed safe auto-reopen.
- **v06 #9 -- the commit gate binds reviews to blob SHAs.** DECISION RECORD, deliberately unfixed: the binding is what makes a review verdict mean anything.
- **A pure block move trips the item-length backstop.** `todo-staged-check.py` judges ADDED lines, so a reordering re-adds every line it moves.
- **TODO-08 §32 (grammar-accurate shell segmentation) is operator-gated and bashlex is REJECTED.** Measured: bashlex fails quoted heredocs, `git commit -F -` heredocs, heredoc-into-pipe, and raises `NotImplementedError` on `case` -- the hand-rolled walk handles all four. Do not re-attempt bashlex; "or equivalent" now needs a parser that handles quoted heredocs and `case`, or the item becomes a deliberate hand-rolled tokenizer.

## Standing measurement obligations for this run

- **Does the spawn-chain sensor ever fire, and is it ever WRONG?** It is brand new and unexercised. A `(split)` scoring non-zero depth, or a legitimate deep chain blocked without a waiver path, is the failure to file.
- **Does Check 24's item count actually fall as files close?** If it does not, the drain instructions are not being followed and THAT is the finding, not the backlog.
- **Does the coverage floor ever fire falsely?** It errors on a drop below 55. A legitimate corpus change that lowers resolvable refs would trip it; that is a baseline update, not a bug, but it must be deliberate.
- **Do the argv-attribution fixes hold under a full run?** Controls that matter: a bare suite run in a loop still blocks, an unescaped backtick substitution still blocks, an executable heredoc still blocks, `L="$(rm ...)"` still blocks, a review-kind in commit PROSE is still not a dispatch.

---

_No findings yet._
