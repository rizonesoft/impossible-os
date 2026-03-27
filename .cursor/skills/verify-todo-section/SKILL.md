---
name: verify-todo-section
description: Verify whether a TODO section marked done or in progress matches actual code, build, and runtime evidence, then correct the section state conservatively. Use when auditing completed TODO work, reconciling stale checklist state, or verifying a claimed implementation.
---

# Verify TODO Section

## Workflow

1. Read the exact section, its notes, and any linked verification context.
2. Build an evidence plan before changing status.
   - Repo search and file inspection
   - Build and test commands
   - Runtime evidence if the section depends on execution
3. Collect evidence with supported tools.
   - Use the build log sentinel and the approved local tooling contract.
   - Accept headless QEMU plus serial output, including captured serial logs surfaced as usable evidence.
   - For crash-related fixes, use `llvm-addr2line-19` and `llvm-objdump-19` before speculating.
4. Classify each item conservatively.
   - Update `[x]`, `[/]`, `[ ]`, and mismatch wording based on actual evidence.
   - Fix stale names, paths, notes, or verification wording when the implementation differs from the old text.
5. Record what was checked, what passed, what failed, and why the section state changed.

## Guardrails

- Do not turn this into broad file-wide TODO validation.
- Do not create new TODO files here.
- Do not assume a master-and-child TODO cascade unless the current TODO actually uses one.

## Additional Resources

- [status-evidence.md](status-evidence.md)
