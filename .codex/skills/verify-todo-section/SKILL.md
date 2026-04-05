---
name: verify-todo-section
description: Verify whether one TODO section marked done or in progress matches actual code, build, runtime, and cross-TODO evidence, then correct section state conservatively.
---

# Verify TODO Section

## Workflow

1. Read only the target section, its notes, and any linked verification context.
2. Build an evidence plan before changing status.
   - Repo search and file inspection for claimed symbols/paths/wiring.
   - Build/test commands and runtime checks if required.
   - Cross-TODO evidence when the section references external requirements.
3. Collect evidence with supported tools.
   - Build: `bash scripts/build.sh`; confirm `tail -1 build/build.log` has `=== BUILD OK ===`.
   - Runtime when needed: `bash scripts/build.sh run` with serial evidence.
   - Crash debug when needed: `llvm-addr2line-19 -e build/kernel.exe -f <RIP>`, `llvm-objdump-19`.
4. Classify each checklist item conservatively.
   - Update `[x]`, `[/]`, `[ ]`, and stale wording based on evidence.
   - **Done proof gate:** mark `[x]` only when implemented, wired, and functional on normal path.
   - Never mark done for `*_stub`, placeholder flow, or normal-path `STATUS_NOT_IMPLEMENTED`.
   - For SSDT claims, verify service number/index ↔ function ↔ registration/dispatch ↔ SSDT table row consistency.
   - If blocked/incomplete work remains, keep `[ ]` or `[/]` and add missing unchecked prerequisite/ownership items in prerequisite-first order with owner scope and `→ XREF`.
5. Synchronize cross-TODO references when section dependencies are involved.
   - Patch directly referenced TODO sections in the same run when this section’s status change affects their dependency/status text.
   - Auto-close referenced TODO items only when both are true:
     1. explicit mapping metadata exists (`ID:` source and `SATISFIES:` target ID),
     2. evidence proves full target acceptance-criteria coverage.
   - If either condition fails, keep target items open/partial and add concrete missing work.
6. Record evidence summary.
   - What was checked, what passed, what failed, and exactly why status changed.

## Guardrails

- Keep this section-targeted; do not widen to full-file validation unless requested.
- Do not create new TODO files.
- Preserve document/table formatting (including OS header icons); edit only evidence-backed status/content.

## Additional Resources

- [status-evidence.md](status-evidence.md)
