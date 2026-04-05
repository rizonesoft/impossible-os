---
name: verify-todo-section
description: Verify whether a TODO section marked done or in progress matches actual code, build, and runtime evidence, then correct the section state conservatively. Use when auditing completed TODO work, reconciling stale checklist state, or verifying a claimed implementation.
---

# Verify TODO Section

## Workflow

1. Read the exact section, its notes, and any linked verification context.
2. Build an evidence plan before changing status.
   - Repo search and file inspection (use Grep/Glob)
   - Build and test commands
   - Runtime evidence if the section depends on execution
   - Cross-TODO evidence if the section references external TODO requirements
3. Collect evidence with supported tools.
   - Build: `bash scripts/build.sh` -- check `tail -1 build/build.log` for `=== BUILD OK ===`.
   - Runtime: `bash scripts/build.sh run` for headless QEMU + serial output.
   - Crash debug: `llvm-addr2line-19 -e build/kernel.exe -f <RIP>`.
4. Classify each item conservatively.
   - Update `[x]`, `[/]`, `[ ]`, and mismatch wording based on actual evidence.
   - Fix stale names, paths, notes, or verification wording when the implementation differs from the old text.
   - Apply strict done proof gate: mark `[x]` only when implemented, wired, and functional on normal path.
   - Do not mark done for `*_stub` handlers or normal-path `STATUS_NOT_IMPLEMENTED` placeholders.
   - For SSDT claims, verify service number/index ↔ Nt function ↔ registration/dispatch entry ↔ SSDT table row consistency.
   - If blocked/incomplete work remains, keep `[ ]` or `[/]` and add missing unchecked prerequisite/ownership items in prerequisite-first order with owner scope and `→ XREF`.
5. Verify and apply cross-TODO synchronization when section dependencies are involved.
   - For directly referenced TODO sections affected by this status change, patch reciprocal dependency/status notes in the same run.
   - Auto-close referenced TODO checklist items only when both conditions are true:
     1. explicit mapping exists (`ID:` source and `SATISFIES:` target ID), and
     2. evidence proves full target acceptance-criteria coverage.
   - If either condition fails, keep target items open/partial and add concrete missing work.
6. Record what was checked, what passed, what failed, and why the section state changed.

## Guardrails

- Do not turn this into broad file-wide TODO validation.
- Do not create new TODO files here.
- Do not assume a master-and-child TODO cascade unless the current TODO actually uses one.
- Preserve table/header formatting (including OS header icons) while editing status/content.
- Keep scope section-targeted unless direct cross-TODO sync is required.

## Additional Resources

- [status-evidence.md](status-evidence.md)
