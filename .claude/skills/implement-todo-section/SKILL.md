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
3. Explore the codebase before editing.
   - Use Grep/Glob tools for symbol search, call-graph tracing, and cross-file discovery.
   - Read relevant source files to understand the integration surface.
4. Implement only the bounded section scope.
   - Freestanding kernel: no `<stdint.h>` / `<string.h>` -- use `#include "kernel/types.h"`.
   - No `malloc()` / `printf()` -- use `kmalloc()` (≤ 4 KB), `pmm_alloc_contiguous()` (larger), `printk()`.
   - Assembly: NASM x86-64 only. UEFI-era, Long Mode, APIC -- no BIOS/VGA/PIC.
   - API surface: Win32 native. Windows-style canonical paths (`C:\Impossible\System32\`).
   - **Diagnostic POST codes:** When modifying boot-path or hardware code, add `POST16()` calls around the change using the `0xD000–0xDFFF` debug range. Format: `POST16(0xDDNN)` where `DD` = section number, `NN` = step (00=entry, 01=exit, 02+=sub-steps). Before assigning codes, grep `include/kernel/boot_init.h` for all `POST16_` defines and the codebase for `POST16(0xD` to avoid conflicts. On bare metal crash, the last POST code on VPD/serial pinpoints the failure in minutes. Remove debug POST codes after verification on all platforms.
5. Verify with supported evidence.
   - Build: `bash scripts/build.sh` (incremental) or `bash scripts/build.sh clean` (full).
   - Check: `tail -1 build/build.log` -- must show `=== BUILD OK ===`.
   - Crash debug: `llvm-addr2line-19 -e build/kernel.exe -f <RIP>`.
   - Runtime: `bash scripts/build.sh run` for headless QEMU + serial output.
6. Update the TODO section before finishing.
   - Correct stale checklist state, notes, and verification wording when evidence contradicts the current text.
   - Keep TODO edits scoped to the section you actually executed.
7. Update the Implementation Order table.
   - Find the row(s) whose `Deliverable` maps to the section you just implemented.
   - Change the `Status` cell to `[x]` (fully done) or `[/]` (in progress) to match the evidence.
   - Do not change `Order`, `Deliverable`, or `Depends On` cells unless the implementation revealed they were wrong.
8. Update the OS Comparison table.
   - Find the row(s) whose `Feature` maps to what the section delivers.
   - Replace placeholder text in the `🚀 Impossible OS` cell with a concrete description and section reference.
   - If a row was `⬜ Planned` and is now fully working, change `⬜` to `✅`; if partial, use `🔄`.
9. Wire up unit tests for the section's deliverables.
   - If the TODO has a `## Unit Tests` section, check if the new code is testable.
   - Add or update test assertions in the relevant `test_*.c` file for the functionality just implemented.
   - If tests already exist but skip (e.g., "not yet allocated"), update them to verify the new state.
   - Run `bash scripts/build.sh` to confirm tests compile.
10. **Commit and push IMMEDIATELY after the section is complete and the build passes.**
   - **CRITICAL: Never batch multiple sections or features into one commit.** Each completed implementation gets its own commit+push before starting the next task. This keeps COUNT.md current, git history granular, and rollback possible.
   - Use the section's `Commit:` line as the commit message.
   - Stage all changed source files, headers, the updated TODO file, and any test changes together.
   - Always push to `origin/main` immediately after a successful commit.
   - After a successful push, mark the section's `- [ ] Commit: "..."` checklist item `[x]`.

## Guardrails

- **Commit after each implementation.** Do not accumulate multiple implementations before committing. The post-commit hook updates COUNT.md and the user expects incremental progress.
- Do not create new TODO files here.
- Do not turn this into a broad file-wide roadmap cleanup pass.
- Do not silently widen scope when requirements, repo state, or verification evidence conflict.

## Additional Resources

- [build-evidence.md](build-evidence.md)
