---
name: implement-todo-section
description: Execute one bounded TODO section, resolve XREF dependencies, run embedded adversarial review/fix/re-review, run embedded section verification, and update the section state when code or test evidence changes it.
---

# Implement TODO Section

## Workflow

1. Read the exact section and its local notes end to end.
2. Resolve dependencies first.
   - Follow every `→ XREF:` line that affects the scoped section.
   - Stop and ask if a prerequisite is incomplete or the scope conflicts with current reality.
   - If part of the section is implementable and part is blocked, implement only the currently unblocked subset and keep blocked checklist items open (`[ ]` or `[/]`) with explicit blocker notes.
3. Explore the codebase before editing.
   - Use Grep/Glob tools for symbol search, call-graph tracing, and cross-file discovery.
   - Read relevant source files to understand the integration surface.
4. **Run the applicable code quality skill BEFORE writing code.**
   - If touching files under `src/kernel/`, `include/kernel/`, `src/boot/`, `src/desktop/`, or `src/shell/`: the `kernel-code-quality` skill applies. Walk through its gates before writing.
   - For non-kernel code (future user-mode libraries, tools, scripts): follow whatever quality skill applies to that domain.
   - **Do not skip this step.** Past incidents: SMP race, memory leak, wrong test assertion -- all caused by writing code before checking the quality gates.
5. Implement only the bounded section scope.
   - Freestanding kernel: no `<stdint.h>` / `<string.h>` -- use `#include "kernel/types.h"`.
   - No `malloc()` / `printf()` -- use `kmalloc()` (≤ 4 KB), `pmm_alloc_contiguous()` (larger), `printk()`.
   - Assembly: NASM x86-64 only. UEFI-era, Long Mode, APIC -- no BIOS/VGA/PIC.
   - API surface: Win32 native. Windows-style canonical paths (`C:\Impossible\System32\`).
   - **Diagnostic POST codes:** When modifying boot-path or hardware code, add `POST16()` calls around the change using the `0xD000–0xDFFF` debug range. Format: `POST16(0xDDNN)` where `DD` = section number, `NN` = step (00=entry, 01=exit, 02+=sub-steps). Before assigning codes, grep `include/kernel/boot_init.h` for all `POST16_` defines and the codebase for `POST16(0xD` to avoid conflicts. On bare metal crash, the last POST code on VPD/serial pinpoints the failure in minutes. Remove debug POST codes after verification on all platforms.
6. Verify with supported evidence.
   - Build: `bash scripts/build.sh` (incremental) or `bash scripts/build.sh clean` (full).
   - Check: `tail -1 build/build.log` -- must show `=== BUILD OK ===`.
   - Crash debug: `llvm-addr2line-19 -e build/kernel.exe -f <RIP>`.
   - Runtime: `bash scripts/build.sh run` for headless QEMU + serial output.
7. Update the TODO section before finishing.
   - Correct stale checklist state, notes, and verification wording when evidence contradicts the current text.
   - Keep TODO edits scoped to the section you actually executed.
   - **Done proof gate for checklist updates:** only change `[ ]` -> `[x]` when the item is both functionally implemented and wired in the real call path. Do not mark done for `*_stub` handlers or normal-path `STATUS_NOT_IMPLEMENTED` placeholders.
   - For SSDT-related claims, verify **service number/index ↔ Nt function ↔ `ssdt_register()` entry ↔ SSDT Master Table row** consistency before changing TODO text.
   - If an item cannot be completed yet due to missing prerequisites, do not force completion; leave it open and add missing unchecked prerequisite/ownership items in logical order, with concrete owner scope (`src/...`/symbol) and `→ XREF` where external.
   - **Deferred-item resolution rule:** before finishing this section, scan earlier sections in the SAME TODO file for items marked "deferred to §N" where N is this section. If such items exist and the prerequisite work was done, go back and mark them `[x]` with evidence. Deferred items are promises -- when the target section runs, the promise must be kept or explicitly re-deferred with a new concrete blocker.
   - **Cross-TODO sync rule:** when this section references or satisfies external TODO requirements, update directly referenced TODO section(s) in the same run so dependency/status text remains consistent on both sides.
   - **Cross-TODO auto-closure rule:** propagate `[ ]` -> `[x]` in referenced TODOs only when both are true:
     1. explicit mapping metadata exists (`ID:` on source item and `SATISFIES:` target item ID), and
     2. evidence proves full target acceptance-criteria coverage.
     If either condition fails, keep target items open/partial and add concrete missing work.
   - Preserve existing document formatting while editing: keep table headers, icons, section headers, and column structure intact; only update evidence-backed status/content cells.
8. Update the Implementation Order table.
   - Find the row(s) whose `Deliverable` maps to the section you just implemented.
   - Change the `Status` cell to `[x]` (fully done) or `[/]` (in progress) to match the evidence.
   - Do not change `Order`, `Deliverable`, or `Depends On` cells unless the implementation revealed they were wrong.
9. Update the OS Comparison table.
   - Find the row(s) whose `Feature` maps to what the section delivers.
   - Replace placeholder text in the `🚀 Impossible OS` cell with a concrete description and section reference.
   - If a row was `⬜ Planned` and is now fully working, change `⬜` to `✅`; if partial, use `🔄`.
   - Preserve the OS Comparison table format exactly (including header icons and column order); edit only the relevant status and content cells.
10. Wire up unit tests for the section's deliverables.
   - If the TODO has a `## Unit Tests` section, check if the new code is testable.
   - Add or update test assertions in the relevant `test_*.c` file for the functionality just implemented.
   - If tests already exist but skip (e.g., "not yet allocated"), update them to verify the new state.
   - Run `bash scripts/build.sh` to confirm tests compile.
11. Run adversarial review via Codex (required).
   - **Dispatch to the Codex rescue subagent** (`Agent` tool with `subagent_type: "codex:codex-rescue"`), NOT self-review. Self-review has implementation bias; the Codex agent reads the code fresh and catches things you rationalized away. Proven: §1 Codex found 2 Critical issues (SMP race, TOCTOU) that self-review missed.
   - Follow the `codex-adversarial-review-section` skill workflow: scope to this section's changed files, list adversarial angles, request severity-labeled findings.
   - The review challenges: concurrency/SMP safety, error-path handling, wiring correctness, boundary/ABI assumptions.
   - Produces findings by severity: `Critical`, `High`, `Medium`, `Low`.
12. Run fix -> build/test -> re-review loop (max 3 rounds).
   - Fix all `Critical` and `High` findings.
   - Fix `Medium` unless explicitly accepted with a concrete technical reason.
   - Rebuild after each fix round:
     - `bash scripts/build.sh`
     - confirm `tail -1 build/build.log` is `=== BUILD OK ===`
   - Re-review the same section focusing on previous findings and changed files.
   - If unresolved `Critical`/`High` remain after round 3:
     - do not mark section complete,
     - keep `[/]` or `[ ]`,
     - add explicit follow-up checklist items with ownership and `→ XREF` where needed.
13. Run section verification via `validate-todo-section` skill (required).
   - **Use the `validate-todo-section` skill, not manual checklist review.** The skill enforces evidence collection methodology and catches stale/optimistic status claims.
   - Verify the section end-to-end against code/build/runtime evidence before final TODO status updates.
   - Classify each checklist item conservatively:
     - `[x]` only when implemented + wired + functional normal path,
     - never `[x]` for stubs/placeholders/normal-path `STATUS_NOT_IMPLEMENTED`.
   - For SSDT claims, verify service number/index ↔ function ↔ registration/dispatch ↔ table row consistency.
   - If blocked/incomplete work remains, keep `[ ]` or `[/]` and add missing prerequisite/ownership items in prerequisite-first order with owner scope and `→ XREF`.
   - Re-check cross-TODO synchronization and auto-closure gates (`ID`/`SATISFIES` + full acceptance coverage).
14. **Commit and push IMMEDIATELY after the section is complete and the build passes.**
   - **CRITICAL: Never batch multiple sections or features into one commit.** Each completed implementation gets its own commit+push before starting the next task. This keeps COUNT.md current, git history granular, and rollback possible.
   - Use the section's `Commit:` line as the commit message.
   - Stage all changed source files, headers, the updated TODO file, and any test changes together.
   - Always push to `origin/main` immediately after a successful commit.
   - After a successful push, mark the section's `- [ ] Commit: "..."` checklist item `[x]`.

## HARD GATE: Steps 11-13 are MANDATORY before step 14

> **You MUST NOT `git commit` or `git push` until steps 11, 12, and 13 have all been completed for this section.**
>
> This is not optional. This is not skippable for "simple" changes. This is not deferrable to "after the commit". Every section implementation goes through adversarial review + fix loop + verification BEFORE the commit. No exceptions.
>
> If you find yourself about to run `git add` and you have not yet:
> - (11) Invoked the `codex-adversarial-review-section` skill (NOT self-review) and received findings with severity labels
> - (12) Fixed all Critical/High findings with build evidence
> - (13) Invoked the `validate-todo-section` skill (NOT manual checklist scan) and reconciled status
>
> then **STOP and go back to step 11**. The commit can wait 5 minutes. Shipping unreviewed code cannot be undone.
>
> **Pattern to watch for:** You finish coding, the build passes, and you think "this is straightforward, I'll just commit." That thought is the signal to stop and run steps 11-13. The straightforward changes are exactly the ones where review catches the bug you didn't think about.

## Guardrails

- **Commit after each implementation.** Do not accumulate multiple implementations before committing. The post-commit hook updates COUNT.md and the user expects incremental progress.
- **Steps 11-13 are blocking prerequisites for step 14.** Do not commit without adversarial review + fix loop + verification. This guardrail exists because it was violated -- the pattern of "build passes, skip review, commit" must be broken.
- Do not mark checklist items `[x]` from intent, partial progress, or stubs; require implementation + wiring evidence first.
- For SSDT sections, do not edit checklist/table status until service number ↔ function ↔ registration ↔ master-table consistency is verified.
- If prerequisite work is missing, keep items open/partial and add explicit prerequisite ownership items before dependents.
- Do not auto-close referenced TODO items from plain `→ XREF` text alone; require `ID`/`SATISFIES` mapping plus full acceptance-criteria proof.
- Do not create new TODO files here.
- Do not turn this into a broad file-wide roadmap cleanup pass.
- Do not silently widen scope when requirements, repo state, or verification evidence conflict.

## Additional Resources

- [build-evidence.md](build-evidence.md)
