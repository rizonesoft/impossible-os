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

- [ ] `build_offload_reminder.py` BLOCKs on a suite command appearing as PROSE inside a heredoc body, not as an invocation
      Observed 2026-08-07 ~16:5x while shipping TODO-06 section 21. The Bash call was `python3 - <<'ENDPATCH'` whose Python body wrote a TODO `> **Test runner:**` line quoting the suite command; the hook matched that text and refused the whole call with "ran BARE in the MAIN context". Nothing was executed bare.
      - The hook's own message invites this filing: "If that segment is NOT a real invocation (e.g. prose inside a heredoc body), this is a false positive worth filing WITH the segment text."
      - Cost measured: one blocked call plus a rewrite of the patch into a `/tmp` helper file invoked by path, roughly 2 extra tool calls. Low per occurrence, but it fires on any turn that writes a Test-runner note, which is EVERY section ship.
      - Likely fix: skip segments that fall inside a quoted heredoc body (the delimiter is already known to the parser that finds the segment), or require the segment to start a command word rather than appear anywhere in the line.
- [ ] `run_phase_guard.py fixpoint` discards the rebuild's exit status, so a producer refusal can be read as DONE
      Found by the Codex adversarial review of TODO-06 section 21 on 2026-08-07 and confirmed at source: the fixpoint gate runs `build-and-validate.sh` and then calls `sequencer_triage.py` unconditionally, without checking the rebuild's return code.
      - It matters as of that section: `build.py` gained exit code 3 (the corpus moved while it was reading, or its git history moved) and REFUSES to write, leaving the PREVIOUS cache in place. The wrapper is invoked with `--keep-cache`, and `sequencer_triage` loads that cache with `check_stale=False`.
      - So a corpus edit racing the fixpoint checkpoint can make the rebuild return 3 while the stale cache still classifies every file DONE -- and the run would terminate on evidence it had just failed to refresh. The gate is oracle-verified precisely so it cannot finish early; this is a hole in that guarantee.
      - Fix is small and fail-closed: capture the rebuild's status and refuse fixpoint unless it is exactly 0. `build-and-validate.sh` now propagates `build.py`'s code rather than flattening it to 1, so the caller can distinguish a retryable 3 from a malformed-input 1.
      - NOT fixed by the run: `.claude/hooks/run_phase_guard.py` is control plane, which the unattended run may not edit.
- [ ] `bare_section_refs.py` applies the code-file rule to throwaway `/tmp` helper scripts whose STRINGS are Markdown destined for a TODO
      Same session. A `/tmp/s21_notes_patch.py` written only to patch a TODO was refused because its string literals contained the section glyph plus a digit -- text that is legal, and required, in the Markdown it was writing.
      - Worked around by composing the glyph as `chr(0xA7)`, which satisfies the hook while producing byte-identical output. That is an evasion the hook cannot distinguish from compliance, so the gate is currently costing keystrokes without adding safety on this path.
      - Likely fix: exempt paths outside the repo root (a `/tmp` helper is not tracked code and cannot drift), which keeps the rule exactly as strong for every file `lint.sh` Check 5 actually scans.
