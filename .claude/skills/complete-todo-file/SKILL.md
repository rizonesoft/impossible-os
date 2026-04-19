---
name: complete-todo-file
description: Finalize a TODO file whose `## N.` implementation sections are all shipped: dispatches `/implement-unit-tests` on the TODO-level `## Unit Tests` section, executes every automatable command in the `## Verification` section (build, test runner, smoke test, grep checks), marks verified items `[x]` with captured evidence, flags manual-only items (bare-metal platforms, visual checks) as follow-ups, and commits the closure. Use when the last numbered section of a TODO is `[x]` but the Unit Tests + Verification caps still have unchecked items.
---

# Complete TODO File

> **External-Reviewer Contract:** This skill dispatches Codex indirectly via `/implement-unit-tests` (Codex test-coverage step) and through any automated test runs that invoke the Codex-review pipeline. Every finding from those dispatches goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex-copilot](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex-copilot).

## Execution Discipline

> A closed TODO is a claim that what it promised is done. This skill owns the final two blocks of that claim -- **never fake a PASS you didn't run**.
> - **No PASS without execution.** Every `[x]` this skill writes into `## Verification` must come from a command that actually ran and returned the expected result. Paper completion is the exact failure mode this skill guards against.
> - **Bare-metal items stay `[ ]`.** Lines that say "Verify on bare metal" / "Verify on VirtualBox" are NOT automatable in this session. Mark them with a follow-up note, do not check them off.
> - **One TODO = one close-out commit.** Stage test-file wiring + Verification updates + any stamp edits together.
> - **If the TODO's `## N.` sections are NOT all `[x]`, STOP and report.** This skill finalizes; it does not implement sections.

## Use This Skill When

- A TODO's last numbered `## N.` section just shipped and the `## Unit Tests` + `## Verification` blocks still have unchecked items.
- The user says "close TODO-XX" / "finalize TODO-XX" / "run the verification on TODO-XX".
- Before graduating a TODO file from "active" to "doc-only reference" (after §5 work on long-lived TODOs).
- Do NOT use to finalize a single `## N.` section -- that is `/implement-todo-section` step 8 + step 17. This skill operates at the whole-TODO level.
- Do NOT use if any `## N.` section is still `[ ]` or `[/]` -- finish those first with `/implement-todo-section`.

## Workflow

1. **Read the TODO file.** Locate:
   - Implementation Order table status (every row must be `[x]`; flag any `[ ]` / `[/]` and stop).
   - `## Unit Tests` section (usually at end, before `## Verification`).
   - `## Verification` section.
   - Any forward-referenced `§N regression pack` items that live in later sections (skip; those ship under their own section).
2. **Precondition check.** If Implementation Order has any non-`[x]` row, report it and STOP. The user runs `/implement-todo-section` on the remaining sections first.
3. **Dispatch `/implement-unit-tests`** on the TODO's `## Unit Tests` section via the Skill tool, passing the TODO path as the argument. Let it run to completion: it creates the test file, registers it in `test_runner.c`, adds a `make test-<cat>` target if needed, runs the suite, and marks the Unit Tests items `[x]` with a short note (suite count + pass count). If the Unit Tests section says `> **Note:** No kernel test surface -- ...`, skip the dispatch and note the reason in the close-out report.
4. **Parse `## Verification`** line-by-line. Classify each item:
   - **Automatable:** `bash scripts/build.sh[ clean]` / `make test-<cat>` / `bash scripts/test.sh QUIET=1` / `bash scripts/test-smoke.sh` / `grep -rn 'pattern' todo/` / `python3 ...` / `tail -1 build/build.log` / specific serial-log greps against existing capture files.
   - **Semi-automatable:** "POST16 codes 0xDDNN appear in correct order" -- run via `bash scripts/test-smoke.sh` + grep the stripped log at `build/smoke-test.stripped.log`.
   - **Manual only:** "Verify on QEMU WHPX (2 CPUs)" / "Verify on VirtualBox" / "Verify on bare metal" / "visual frame inspection" / any item requiring native Windows.
5. **Execute the automatable items in order.** For each:
   - Capture the command + exit code + relevant stdout/stderr snippet (last 20 lines of relevant output is usually enough).
   - On PASS, mark the item `[x]` in the TODO file.
   - On FAIL, keep it `[ ]`, record the failure reason inline as a NOTE, and do NOT proceed to stamp the TODO as closed. The user resolves the failure before re-running this skill.
6. **For semi-automatable smoke-test items:** run `bash scripts/test-smoke.sh`, grep `build/smoke-test.stripped.log` for each expected POST16 code / klog marker, and mark individually.
7. **For manual-only items:** leave `[ ]`, append a `(manual -- run on <platform>)` tag so the user sees the remaining surface. Do NOT mark manual items `[x]` under any circumstance.
8. **Generate a close-out report** (printed to the chat, not written into the TODO):
   - Implementation Order rows: N/N `[x]`.
   - Unit Tests: `implement-unit-tests` result (suite count + pass count) OR "N/A (docs-only)" OR "skipped -- user gap-noted".
   - Verification items: M automated PASS, K automated FAIL (listed with reason), L manual-only (listed with platform).
   - Close-out state: **CLOSED** if M=full + 0 FAIL, **PARTIAL** if any FAIL exists, **MANUAL-PENDING** if only manual items remain.
9. **Commit and push** (only if state is CLOSED or MANUAL-PENDING):
   - Commit message: `"close: <TODO file> -- unit tests wired; verification N/M automated PASS; L manual items pending"`.
   - If state is PARTIAL, do NOT commit; report the failure and exit.
10. **Mark any `Commit:` checklist item inside Unit Tests / Verification as `[x]`** if the commit succeeded.

## Hook + Policy Integration

- **PreToolUse on `Skill` matcher includes this skill** in the completion-first reminder list (hook 8 in the Hook Routing Matrix). The reminder fires before the skill starts and pins the "no PASS without execution" rule.
- **Cross-TODO scope-gap awareness:** if the `## Verification` section references external TODO sections (e.g. "verify TODO-05 §14 stamps resolve"), this skill runs the lookup but does NOT author fixes in other TODO files. Out-of-scope findings land in the close-out report as `Accepted:` with an XREF pointer (the Accepted-XREF concreteness check from `review-todo-section` step 15 applies).
- **Inbound stamp sweep:** if this is the last section of a TODO marked `[x]` and other TODOs held `Accepted:` / `Deferred:` XREFs pointing at items this close-out just confirmed, grep `todo/` for those XREFs and report them as sweep candidates. Do NOT auto-rewrite inbound stamps; user confirms.

## Guardrails

- **No PASS without execution.** Every `[x]` this skill writes must come from a command that actually ran. Replay in chat the exact command + a pass-proof snippet.
- **No `[x]` on manual items.** Bare-metal / VirtualBox / native-Windows items stay `[ ]` with an explicit `(manual -- ...)` tag. The user ticks them off after running on the real platform.
- **No section-level work.** If a `## N.` section has unshipped items, STOP -- user runs `/implement-todo-section` on that section first.
- **No skipping `/implement-unit-tests`.** Even if the Unit Tests section looks partially wired, dispatch the skill; let it do the completeness check. Exception: the section explicitly documents no-kernel-test-surface in a `**Note:**` block, in which case capture that note in the report.
- **Bat file / test category drift.** If the TODO's `## Unit Tests` names a `scripts/debug/kernel/run-<suite>-tests.bat` that doesn't exist, flag it -- `/implement-unit-tests` owns creating it per its workflow but this skill surfaces the gap.
- **Commit discipline.** The close-out commit goes through the usual `Bash(git commit:*)` permission flow; the pre-commit hook 6 (section-commit GATE) does NOT fire here because this commit does not mix src + TODO edits -- it is a TODO-level close-out, not a section-implementation commit. The pre-commit lint hook still runs; TODO markdown passes as long as linters are clean.
- **Never turn this into a broad file-wide cleanup.** Only the `## Unit Tests` and `## Verification` blocks get modified. Implementation Order, OS Comparison, Notes, and stamps on earlier sections are read-only here.

## Relationship to other skills

- [`/implement-todo-section`](../implement-todo-section/SKILL.md) -- operates on a single `## N.` section with its own Unit Tests wiring (step 8) and Verification (step 17). This skill is the TODO-level counterpart that runs after all `## N.` sections ship.
- [`/implement-unit-tests`](../implement-unit-tests/SKILL.md) -- this skill dispatches it in step 3. Delegates the test-file creation + runner registration + bat-file wiring entirely.
- [`/review-todo-section`](../review-todo-section/SKILL.md) / [`/verify-todo-section`](../verify-todo-section/SKILL.md) -- section-level quality review and audit. This skill does NOT re-run them; sections that closed through review already carry their stamps.
- [`/validate-todo-file`](../validate-todo-file/SKILL.md) -- runs on TODO structure (numbering, XREFs, OS Comparison, Notes). This skill does not re-validate structure; if you want both, run `/validate-todo-file` before `/complete-todo-file`.
