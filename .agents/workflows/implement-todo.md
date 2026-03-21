---
description: How to implement a TODO section from start to commit
---

# Implement a TODO Section

// turbo-all

## Prerequisites
- Review AGENTS.md Architecture section if touching boot code
- Review AGENTS.md Development Roadmap section for TODO conventions

## Steps

1. **Read the TODO section prompt fully.** Understand the goal, constraints, and expected deliverables. Note the commit message at the bottom.

2. **Check `→ XREF:` lines.** Open each cross-referenced section and verify its items are `[x]` (complete). If a dependency is incomplete, stop and report to the user.

3. **Choose execution mode:**
   - **Plan Mode** (create `implementation_plan.md`, get user approval) for:
     - Kernel subsystems, drivers, memory management, boot sequence
     - New hardware support, API design, syscalls
   - **Fast Mode** (code directly) for:
     - UI/UX iteration, bug fixes, documentation, asset loading

4. **Implement the code** following the project rules (see `.agents/rules/coding.md`):
   - No angle-bracket headers — use `#include "kernel/types.h"`
   - Use `pmm_alloc_contiguous()` for any buffer > 4 KB (never `kmalloc` for large data)

5. **Build and test:**
   ```bash
   bash scripts/build.sh clean run
   ```
   Verify: `tail -1 build/build.log` shows `=== BUILD OK ===`

6. **Check serial output** in the QEMU window. Look for:
   - `[OK]` messages for the new feature
   - No `[!!]`, `[FAIL]`, `PANIC`, or `FAULT` messages
   - No regressions in existing boot log entries

7. **Mark all items `[x]`** in the TODO section.

8. **Rewrite the prompt** as a verification prompt. Change from "implement X" to "verify X is correctly implemented" with specific checks (file exists, function signature correct, build passes, boot log shows expected output).

9. **Commit** with the exact message specified in the TODO section's commit item:
   ```bash
   git add -A && git commit -m "scope: description from TODO"
   ```

## Troubleshooting

- **Build fails:** Check `build/build.log` for the first error. Fix and re-run step 5.
- **QEMU hangs:** The kernel likely panicked before serial init. Use `bash scripts/debug.sh` with GDB.
- **Dependency not met:** Report to the user which XREF is incomplete and ask how to proceed.
