# Overnight Runner Improvements v11 -- Findings (opened 2026-08-07)

Runner-behavior findings from the run armed after the 2026-08-07 close-out of [v10](overnight-runner-improvements-v10.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `overnight-runner-improvements-vNN.md` in this directory, which is this one until an operator opens v12.

**Scope:** flow, gates, wedges, and machinery correctness. Cost findings go to [`token-saver-v11.md`](../token-saver/token-saver-v11.md) -- but a MISFIRING GATE is both, and belongs here with its mechanism.

**Why findings land here instead of being fixed.** The run may not edit its own control plane (`.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json`) or the receipt surface. Record the finding in the same turn it is observed, then advance.

**What to write.** What was observed live (run id, segment, the exact refusal text or behavior), the mechanism confirmed at source with file:line, and what it cost or would cost. Lead <= 250 chars; sub-bullet bodies <= 1,000, one idea per sub-bullet.

## Carried from v10 -- open, and not to be re-derived

- **The 1,595-item `open-in-deferred` backlog.** Both drains (FILE_CLOSE and ADVANCE) are wired; what is UNVERIFIED is whether they actually reach the population end to end. Measure the remaining count against `scripts/lint.sh` Check 24's own machinery -- do not re-implement its rule, which produced a wrong figure (333 sections reported as 333 items) that stood for weeks.
- **60 stranded deferrals / the P6.4 fixpoint gate.** Unchanged.
- **The v06 #9 blob-SHA decision record.** Still unwritten.
- **A pure-block-move backstop for item LENGTH**, matching the one `todo-section-order.py` already has for ordering.
- **TODO-08 §32 (bashlex-backed argv parsing) stays OPERATOR-GATED.** It was rejected on measurement, not on taste: a dependency addition for a class the disqualifier approach now covers. Three v10 findings were in this class and all three were fixed without it. Do not re-propose it as new.

## Two lessons from v10 worth carrying as PRACTICE, not as items

- **A finding you cannot explain is still worth filing.** Two v10 items were reported BACKWARDS -- filed as false positives blocking legitimate work. The false positives did not reproduce; probing them found real BYPASSES in the opposite direction, one of which ran the full test suite in the main context with nothing reporting it.
- **Prose is not a gate.** The rule that the run may not edit its own control plane was stated in the skill, the doctrine file, and every capture-file header -- and no code enforced it. The run edited `scripts/overnight/decision-registry.py` for ~90 lines with rc 0 from every guard. When filing a finding of the form "the run did X, which is forbidden", check whether anything actually forbids it.

---

_No findings yet._
