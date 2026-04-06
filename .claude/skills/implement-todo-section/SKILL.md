---
name: implement-todo-section
description: Execute one bounded TODO section, resolve XREF dependencies, run Codex adversarial review, self-review for regressions, validate section, tie up loose ends, and commit. Use when implementing a specific TODO section or a clearly scoped subset of one.
---

# Implement TODO Section

## Execution Discipline

> The TODO section IS the plan. Apply `superpowers:executing-plans` principles:
> - **Follow the plan, don't improvise.** The checklist items define scope. Do not widen.
> - **Review checkpoints are mandatory.** Steps 13-18 are review checkpoints -- never skip them.
> - **If the plan is wrong, update the plan first.** If reality conflicts with the checklist, update the TODO section text before implementing a different approach.
> - **One section = one commit.** Each section is a milestone with a clean commit boundary.

## Workflow

1. **Read the section** -- full text, notes, test checkpoint, warning boxes.
2. **Resolve dependencies** -- follow every `-> XREF:` line. Stop and ask if a prerequisite is incomplete or scope conflicts with reality. If part is implementable and part is blocked, implement only the unblocked subset; keep blocked items `[ ]` or `[/]` with explicit blocker notes.
3. **Explore the codebase** -- Grep/Glob for symbols, call-graph tracing, cross-file discovery. Read relevant source files to understand the integration surface.
4. **Codex design review** (for complex/high-risk sections) -- if the section touches SMP-sensitive code, boot-path, page tables, interrupt handling, or security-critical logic, dispatch a design review via the Codex plugin BEFORE writing code. Follow the `codex-design-review` skill: send the plan + integration surface + constraints, evaluate for blockers/warnings. Skip for straightforward sections (simple struct definitions, single-function additions, test-only work).
    ```bash
    node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<design review prompt>"
    ```
5. **Run `kernel-code-quality` skill** -- walk the gates BEFORE writing code. If touching `src/kernel/`, `include/kernel/`, `src/boot/`, `src/desktop/`, or `src/shell/`: the skill applies. Do not skip. Past incidents: SMP race, memory leak, wrong test assertion -- all caused by writing code before checking quality gates.
6. **Implement** -- bounded scope only.
   - Freestanding kernel: `#include "kernel/types.h"` -- no `<stdint.h>`, `<string.h>`.
   - No `malloc()`/`printf()` -- use `kmalloc()` (<=4 KB), `pmm_alloc_contiguous()` (larger), `printk()`.
   - Assembly: NASM x86-64 only. UEFI-era, Long Mode, APIC -- no BIOS/VGA/PIC.
   - API surface: Win32 native. Windows-style canonical paths (`C:\Impossible\System32\`).
   - **POST16 codes** for boot-path/hardware code: `POST16(0xDDNN)` format, check `boot_init.h` and grep for conflicts before assigning.
7. **Build** -- `bash scripts/build.sh`, confirm `tail -1 build/build.log` shows `=== BUILD OK ===`.
8. **Wire unit tests** -- before writing any test code, read the TODO file's **Unit Tests** section (if one exists) to find:
   - The expected test file name (e.g. `test_exec.c`)
   - The expected registration function (e.g. `test_register_exec()`)
   - The expected test category (e.g. `TEST_CAT_EXEC`)
   - Which specific assertions are required for this section
   
   Create the test file / registration function if it doesn't exist yet. Do NOT piggy-back tests onto an unrelated test file just because it's convenient. Then add/update assertions and confirm build passes.
9. **Codex test coverage analysis** -- after wiring tests, dispatch a test coverage gap analysis to catch missing assertions before the adversarial review finds them. Follow the `codex-test-coverage` skill: list public functions, existing tests, and ask Codex to find untested error paths, boundaries, and negative cases. Add any missing tests found. Skip for trivial sections (< 3 test assertions).
    ```bash
    node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<test coverage prompt>"
    ```
10. **Update TODO section** -- mark items with evidence:
    - **Done proof gate:** `[x]` only when implemented + wired + functional on normal path. Never `[x]` for stubs or `STATUS_NOT_IMPLEMENTED` placeholders.
    - SSDT claims: verify service number <-> function <-> `ssdt_register()` <-> master table row consistency.
    - Blocked items: keep `[ ]` or `[/]`, add missing prerequisite/ownership items with owner scope and `-> XREF`.
    - **Deferred-item resolution:** scan earlier sections in the SAME TODO for items marked "deferred to section N" where N is this section. If the work was done, mark them `[x]`. Deferred items are promises.
    - **Cross-TODO sync:** when this section references or satisfies external TODO requirements, update those TODOs in the same run.
    - Preserve existing formatting (table headers, icons, column structure).
11. **Update Implementation Order table** -- `[x]` (fully done) or `[/]` (in progress).
12. **Update OS Comparison table** -- replace placeholders with concrete descriptions. `Planned` -> `Done` or `Partial`.
13. **Codex adversarial review** (MANDATORY) -- run via the Codex plugin, NOT self-review. Self-review has implementation bias; Codex reads code fresh and catches things you rationalized away. Proven: section 1 Codex found 2 Critical issues (SMP race, TOCTOU) that self-review missed. Follow the `codex-adversarial-review-section` skill workflow: scope to changed files, list adversarial angles, request severity-labeled findings. Command:
    ```bash
    node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<focus prompt>"
    ```
14. **Fix loop** (max 3 rounds) -- apply `superpowers:receiving-code-review` discipline: do NOT blindly implement every Codex finding. For each finding:
    - **Verify technically first.** Read the code Codex flagged. Is the finding correct? Codex can be wrong -- it doesn't have full runtime context.
    - **If the finding is valid:** fix the root cause, not the surface symptom. Rebuild.
    - **If the finding is wrong or misleading:** reject with a concrete technical reason (not "I disagree" -- explain WHY it's wrong with code evidence).
    - **If the finding is correct but out of scope:** accept with justification and note it as a follow-up item.
    - Fix all valid Critical and High. Fix valid Medium unless explicitly accepted. Re-review via Codex focusing on previous findings and changed files. If unresolved Critical/High remain after round 3: do not mark section complete, keep `[/]` or `[ ]`, add follow-up items.
15. **Final self-review** (MANDATORY) -- this catches what Codex misses at the integration level:
    - Regressions: did any existing functionality break?
    - Race conditions: any new shared mutable state without synchronization?
    - Bugs: edge cases, off-by-one, null pointer paths?
    - Performance: unnecessary allocations, O(n^2) where O(n) suffices?
    - Industry standards: no hacks, no patches, no workarounds, no TODO/FIXME in new code.
    - **Checklist item-by-item:** walk every `- [ ]` item in the section. For each one: is it implemented and wired (`[x]`), explicitly deferred with justification (`[/]`), or blocked with notes (`[ ]` + blocker)? If any item was silently skipped, go back and address it now.
    - Deferred items: are there items from earlier sections that were "deferred to this section"? If so, were they resolved?
16. **2nd final build** -- `bash scripts/build.sh`, confirm `=== BUILD OK ===`. This catches anything broken by the fix loop or self-review changes.
17. **Validate section** (MANDATORY) -- invoke `validate-todo-section` skill. Evidence-based checklist classification. Catches stale/optimistic status claims. Re-checks cross-TODO synchronization.
18. **Tie up loose ends** (MANDATORY) -- scan the ENTIRE TODO file and any XREF'd TODO files for:
    - Deferred items pointing to this section that weren't resolved in step 10.
    - Stale warning boxes that should be updated to NOTE (resolved).
    - Implementation Order rows that need status updates.
    - Cross-TODO dependency notes that are now satisfied.
    - Any checklist items in other sections affected by this implementation.
19. **Commit and push** -- only after steps 13-18 are ALL complete.
    - Use the section's `Commit:` line as the commit message.
    - Stage all changed source files, headers, the updated TODO file(s), and test changes together.
    - Push to `origin/main` immediately after successful commit.
    - Mark the section's `- [ ] Commit: "..."` item `[x]`.

## HARD GATE: Steps 13-18 are MANDATORY before step 19

> **You MUST NOT `git commit` or `git push` until steps 13 through 18 have all been completed.**
>
> This is not optional. This is not skippable for "simple" changes. Every section goes through this pipeline. No exceptions.
>
> If you find yourself about to run `git add` and you have not yet:
> - (13) Dispatched Codex adversarial review and received severity-labeled findings
> - (14) Fixed all Critical/High findings with build evidence
> - (15) Done final self-review for regressions, races, bugs, completeness
> - (16) Confirmed 2nd final build passes
> - (17) Invoked `validate-todo-section` skill and reconciled status
> - (18) Scanned for and tied up loose ends in current and XREF'd TODO files
>
> then **STOP and go back to step 13**.
>
> **Pattern to watch for:** "Build passes, looks straightforward, I'll just commit." That thought is the signal to STOP. The straightforward changes are exactly the ones where review catches the bug you didn't think about.

## Guardrails

- **Commit after each section.** Never batch multiple sections into one commit.
- **Steps 13-18 are blocking prerequisites for step 19.** This guardrail exists because it was violated multiple times -- the pattern of "build passes, skip review, commit" must be broken.
- Do not mark items `[x]` from intent, partial progress, or stubs. Require implementation + wiring evidence.
- For SSDT sections, verify service number <-> function <-> registration <-> master table consistency before editing status.
- If prerequisite work is missing, keep items open and add explicit prerequisite ownership items.
- Do not auto-close referenced TODO items without `ID`/`SATISFIES` mapping plus full acceptance proof.
- Do not create new TODO files here.
- Do not turn this into a broad file-wide cleanup pass.
- Do not silently widen scope when requirements conflict with reality.

## Additional Resources

- [build-evidence.md](build-evidence.md)
