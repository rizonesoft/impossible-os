---
name: validate-todo-section
description: Validate whether a TODO section marked done or in progress matches actual code, build, and runtime evidence, then correct the section state conservatively. Use when auditing completed TODO work, reconciling stale checklist state, or verifying a claimed implementation.
---

# Validate TODO Section

## Use This Skill When

- A section was just implemented and you need to verify checklist items match actual code before committing.
- The `/implement-todo-section` pipeline reaches Stage 7 (it invokes this skill).
- Reconciling stale checklist state -- items marked `[x]` that may no longer be accurate after refactoring.
- The user asks "is §N correct?" or "validate this section."
- Do NOT use for full-file validation -- use `/validate-todo-file` instead.
- Do NOT use for deep quality audit -- use `/verify-todo-section` instead (which runs Codex + tests + this).

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
4. **Section plan quality check** (from `superpowers:writing-plans`):
   - Does the section have a `- [ ] Commit:` line? Flag if missing.
   - Does the section have a `**Test checkpoint:**` block with concrete pass/fail criteria? Flag if missing or vague.
   - Do checklist items reference specific functions/types/files, or are they vague ("implement X")? Flag vague items.
   - Are symbols referenced in the checklist defined in Inputs or created by a prior section? Flag orphaned references.
   - If the section would leave obvious adjacent work ownerless when marked done, flag the plan as incomplete. "Ship now, track later" is not acceptable without a concrete owner item.
5. Classify each item conservatively.
   - Update `[x]`, `[/]`, `[ ]`, and mismatch wording based on actual evidence.
   - Fix stale names, paths, notes, or verification wording when the implementation differs from the old text.
   - Apply strict done proof gate: mark `[x]` only when implemented, wired, and functional on normal path.
   - Apply **false-completeness gate:** do not leave `[x]` on a section that technically landed but still lacks obvious adjacent wiring, parity-critical behavior, or a concrete owner TODO item for the remaining gap.
   - Do not mark done for `*_stub` handlers or normal-path `STATUS_NOT_IMPLEMENTED` placeholders.
   - For SSDT claims, verify service number/index ↔ Nt function ↔ registration/dispatch entry ↔ SSDT table row consistency.
   - If blocked/incomplete work remains, keep `[ ]` or `[/]` and add missing unchecked prerequisite/ownership items in prerequisite-first order with owner scope and `→ XREF`.
6. Verify and apply cross-TODO synchronization when section dependencies are involved.
   - For directly referenced TODO sections affected by this status change, patch reciprocal dependency/status notes in the same run.
   - If validation discovers ownerless adjacent work, file the concrete owner item in the same run when the owning section is obvious; otherwise leave the section partial and record the missing ownership clearly.
   - Auto-close referenced TODO checklist items only when both conditions are true:
     1. explicit mapping exists (`ID:` source and `SATISFIES:` target ID), and
     2. evidence proves full target acceptance-criteria coverage.
   - If either condition fails, keep target items open/partial and add concrete missing work.
7. Record what was checked, what passed, what failed, and why the section state changed.

## Guardrails

- Do not turn this into broad file-wide TODO validation.
- Do not create new TODO files here.
- Do not assume a master-and-child TODO cascade unless the current TODO actually uses one.
- Preserve table/header formatting (including OS header icons) while editing status/content.
- Keep scope section-targeted unless direct cross-TODO sync is required.
- **Never add or preserve N.M subnumbering** (`17.1`, `### 3.2`, `**4.1 Foo**`). One `## N.` -- one continuous `- [ ]` list. For full-file structure passes, use `validate-todo-file`.

## Additional Resources

- [status-evidence.md](status-evidence.md)
