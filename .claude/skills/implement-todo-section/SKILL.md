---
name: implement-todo-section
description: Execute one bounded TODO section, resolve XREF dependencies, use the supported build and debug workflow, and update the section state when code or test evidence changes it. Use when implementing a specific TODO section or a clearly scoped subset of a TODO.
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
11. Run section-scoped adversarial review and fix loop.
   - Use `$codex-adversarial-review-section` for the implemented section.
   - Fix findings, then re-review until no unresolved High/Critical findings remain (max 3 rounds).
12. Run targeted section verification.
   - Use `$verify-todo-section` on the implemented section after fixes.
   - Reconcile any status drift from fixes before commit.
13. **Commit and push IMMEDIATELY after the section is complete and the build passes.**
   - **CRITICAL: Never batch multiple sections or features into one commit.** Each completed implementation gets its own commit+push before starting the next task. This keeps COUNT.md current, git history granular, and rollback possible.
   - Use the section's `Commit:` line as the commit message.
   - Stage all changed source files, headers, the updated TODO file, and any test changes together.
   - Always push to `origin/main` immediately after a successful commit.
   - After a successful push, mark the section's `- [ ] Commit: "..."` checklist item `[x]`.

## Guardrails

- **Commit after each implementation.** Do not accumulate multiple implementations before committing. The post-commit hook updates COUNT.md and the user expects incremental progress.
- Do not mark checklist items `[x]` from intent, partial progress, or stubs; require implementation + wiring evidence first.
- For SSDT sections, do not edit checklist/table status until service number ↔ function ↔ registration ↔ master-table consistency is verified.
- If prerequisite work is missing, keep items open/partial and add explicit prerequisite ownership items before dependents.
- Do not auto-close referenced TODO items from plain `→ XREF` text alone; require `ID`/`SATISFIES` mapping plus full acceptance-criteria proof.
- Do not skip section adversarial review and targeted section verification before commit.
- Do not create new TODO files here.
- Do not turn this into a broad file-wide roadmap cleanup pass.
- Do not silently widen scope when requirements, repo state, or verification evidence conflict.

## Additional Resources

- [build-evidence.md](build-evidence.md)
