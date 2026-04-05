---
name: implement-todo-section
description: Execute one bounded TODO section, resolve XREF dependencies, run Codex adversarial review, self-review for regressions, validate section, tie up loose ends, and commit. Use when implementing a specific TODO section or a clearly scoped subset of one.
---

# Implement TODO Section

## Workflow

1. **Read the section** -- full text, notes, test checkpoint, warning boxes.
2. **Resolve dependencies** -- follow every `-> XREF:` line. Stop and ask if a prerequisite is incomplete or scope conflicts with reality. If part is implementable and part is blocked, implement only the unblocked subset; keep blocked items `[ ]` or `[/]` with explicit blocker notes.
3. **Explore the codebase** -- Grep/Glob for symbols, call-graph tracing, cross-file discovery. Read relevant source files to understand the integration surface.
4. **Run `kernel-code-quality` skill** -- walk the gates BEFORE writing code. If touching `src/kernel/`, `include/kernel/`, `src/boot/`, `src/desktop/`, or `src/shell/`: the skill applies. Do not skip. Past incidents: SMP race, memory leak, wrong test assertion -- all caused by writing code before checking quality gates.
5. **Implement** -- bounded scope only.
   - Freestanding kernel: `#include "kernel/types.h"` -- no `<stdint.h>`, `<string.h>`.
   - No `malloc()`/`printf()` -- use `kmalloc()` (<=4 KB), `pmm_alloc_contiguous()` (larger), `printk()`.
   - Assembly: NASM x86-64 only. UEFI-era, Long Mode, APIC -- no BIOS/VGA/PIC.
   - API surface: Win32 native. Windows-style canonical paths (`C:\Impossible\System32\`).
   - **POST16 codes** for boot-path/hardware code: `POST16(0xDDNN)` format, check `boot_init.h` and grep for conflicts before assigning.
6. **Build** -- `bash scripts/build.sh`, confirm `tail -1 build/build.log` shows `=== BUILD OK ===`.
7. **Update TODO section** -- mark items with evidence:
   - **Done proof gate:** `[x]` only when implemented + wired + functional on normal path. Never `[x]` for stubs or `STATUS_NOT_IMPLEMENTED` placeholders.
   - SSDT claims: verify service number <-> function <-> `ssdt_register()` <-> master table row consistency.
   - Blocked items: keep `[ ]` or `[/]`, add missing prerequisite/ownership items with owner scope and `-> XREF`.
   - **Deferred-item resolution:** scan earlier sections in the SAME TODO for items marked "deferred to section N" where N is this section. If the work was done, mark them `[x]`. Deferred items are promises.
   - **Cross-TODO sync:** when this section references or satisfies external TODO requirements, update those TODOs in the same run.
   - Preserve existing formatting (table headers, icons, column structure).
8. **Update Implementation Order table** -- `[x]` (fully done) or `[/]` (in progress).
9. **Update OS Comparison table** -- replace placeholders with concrete descriptions. `Planned` -> `Done` or `Partial`.
10. **Wire unit tests** -- add/update assertions in relevant `test_*.c`, confirm build passes.
11. **Codex adversarial review** (MANDATORY) -- dispatch to the Codex rescue subagent (`Agent` tool with `subagent_type: "codex:codex-rescue"`). NOT self-review. Self-review has implementation bias; the Codex agent reads code fresh and catches things you rationalized away. Proven: section 1 Codex found 2 Critical issues (SMP race, TOCTOU) that self-review missed. Follow the `codex-adversarial-review-section` skill workflow: scope to changed files, list adversarial angles, request severity-labeled findings.
12. **Fix loop** (max 3 rounds) -- fix all Critical and High findings. Fix Medium unless explicitly accepted with a concrete technical reason. Rebuild after each fix round. Re-review via Codex focusing on previous findings and changed files. If unresolved Critical/High remain after round 3: do not mark section complete, keep `[/]` or `[ ]`, add follow-up items.
13. **Final self-review** -- this catches what Codex misses at the integration level:
    - Regressions: did any existing functionality break?
    - Race conditions: any new shared mutable state without synchronization?
    - Bugs: edge cases, off-by-one, null pointer paths?
    - Performance: unnecessary allocations, O(n^2) where O(n) suffices?
    - Industry standards: no hacks, no patches, no workarounds, no TODO/FIXME in new code.
    - Completeness: is everything implemented that CAN be implemented? No deferred items that should have been done now?
14. **2nd final build** -- `bash scripts/build.sh`, confirm `=== BUILD OK ===`. This catches anything broken by the fix loop or self-review changes.
15. **Validate section** (MANDATORY) -- invoke `validate-todo-section` skill. Evidence-based checklist classification. Catches stale/optimistic status claims. Re-checks cross-TODO synchronization.
16. **Tie up loose ends** -- scan the ENTIRE TODO file and any XREF'd TODO files for:
    - Deferred items pointing to this section that weren't resolved in step 7.
    - Stale warning boxes that should be updated to NOTE (resolved).
    - Implementation Order rows that need status updates.
    - Cross-TODO dependency notes that are now satisfied.
    - Any checklist items in other sections affected by this implementation.
17. **Commit and push** -- only after steps 11-16 are ALL complete.
    - Use the section's `Commit:` line as the commit message.
    - Stage all changed source files, headers, the updated TODO file(s), and test changes together.
    - Push to `origin/main` immediately after successful commit.
    - Mark the section's `- [ ] Commit: "..."` item `[x]`.

## HARD GATE: Steps 11-16 are MANDATORY before step 17

> **You MUST NOT `git commit` or `git push` until steps 11 through 16 have all been completed.**
>
> This is not optional. This is not skippable for "simple" changes. Every section goes through this pipeline. No exceptions.
>
> If you find yourself about to run `git add` and you have not yet:
> - (11) Dispatched Codex adversarial review and received severity-labeled findings
> - (12) Fixed all Critical/High findings with build evidence
> - (13) Done final self-review for regressions, races, bugs, completeness
> - (14) Confirmed 2nd final build passes
> - (15) Invoked `validate-todo-section` skill and reconciled status
> - (16) Scanned for and tied up loose ends in current and XREF'd TODO files
>
> then **STOP and go back to step 11**.
>
> **Pattern to watch for:** "Build passes, looks straightforward, I'll just commit." That thought is the signal to STOP. The straightforward changes are exactly the ones where review catches the bug you didn't think about.

## Guardrails

- **Commit after each section.** Never batch multiple sections into one commit.
- **Steps 11-16 are blocking prerequisites for step 17.** This guardrail exists because it was violated multiple times -- the pattern of "build passes, skip review, commit" must be broken.
- Do not mark items `[x]` from intent, partial progress, or stubs. Require implementation + wiring evidence.
- For SSDT sections, verify service number <-> function <-> registration <-> master table consistency before editing status.
- If prerequisite work is missing, keep items open and add explicit prerequisite ownership items.
- Do not auto-close referenced TODO items without `ID`/`SATISFIES` mapping plus full acceptance proof.
- Do not create new TODO files here.
- Do not turn this into a broad file-wide cleanup pass.
- Do not silently widen scope when requirements conflict with reality.

## Additional Resources

- [build-evidence.md](build-evidence.md)
