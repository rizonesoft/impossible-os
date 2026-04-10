---
name: validate-todo-section
description: Validates whether a TODO section marked done or in progress matches actual code, build, and runtime evidence, then corrects the section state conservatively. Use when auditing completed TODO work, reconciling stale checklist state, or verifying a claimed implementation. In Cursor, read from .cursor/skills/validate-todo-section/SKILL.md and complete every workflow step; do not expand scope to full-file validation.
---

# Validate TODO Section (Cursor)

## Cursor: mandatory coverage

- **Load this skill** from `.cursor/skills/validate-todo-section/SKILL.md` when the task matches the description.
- **Copy the checklist below into the chat** at the start of a section validate run; in your final message, report **done/skipped + reason** per line.
- **Related rule:** `.cursor/rules/todo-validate-gap-workflows.mdc` when `todo/**/*.md` is in scope.
- **Full-file structure / parity:** `.cursor/skills/validate-todo-file/SKILL.md` -- not this skill.
- **Deep quality pipeline (Codex + tests + this):** `.claude/skills/verify-todo-section/SKILL.md` -- use when the user wants verification without re-implementing.
- **Implement pipeline Stage 7:** `.claude/skills/implement-todo-section/SKILL.md` invokes this style of check; follow this skill's workflow when validating after implement.

## Cursor reliability

- **Rules:** See `.cursor/rules/todo-workflows-always-pointer.mdc` (always apply) and `.cursor/rules/todo-validate-gap-workflows.mdc` (when `todo/**/*.md` is in scope). `@`-mention this skill if context is thin.
- **Hooks:** Reminders fire on reads of `todo/**/TODO-*.md` only. No History row is required for this skill; hooks that mention History refer to other workflows.
- **Full verify:** Codex-heavy verification stays in `.claude/skills/verify-todo-section/SKILL.md`.

## Progress checklist (copy for every run)

```text
validate-todo-section -- all steps (no skips):
- [ ] 1  Read section + notes + verification context
- [ ] 2  Evidence plan (search, build/test, runtime, cross-TODO)
- [ ] 3  Collect evidence (build.sh, tests, addr2line if crash)
- [ ] 4  Section plan quality (Commit line, Test checkpoint, concrete items, Inputs/symbols; no N.M sublabels)
- [ ] 5  Classify each checklist item (strict done gate, SSDT consistency if applicable)
- [ ] 6  Cross-TODO sync + ID/SATISFIES auto-close rules
- [ ] 7  Report: checked, passed, failed, state change rationale
```

## Use This Skill When

- A section was just implemented and you need to verify checklist items match actual code before committing.
- The implement-todo-section pipeline reaches Stage 7 (it invokes this kind of verification).
- Reconciling stale checklist state -- items marked `[x]` that may no longer be accurate after refactoring.
- The user asks "is §N correct?" or "validate this section."
- Do NOT use for full-file validation -- use `.cursor/skills/validate-todo-file/SKILL.md` instead.
- Do NOT use for deep quality audit only -- use `.claude/skills/verify-todo-section/SKILL.md` instead (which runs Codex + tests + section validation).

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
5. Classify each item conservatively.
   - Update `[x]`, `[/]`, `[ ]`, and mismatch wording based on actual evidence.
   - Fix stale names, paths, notes, or verification wording when the implementation differs from the old text.
   - Apply strict done proof gate: mark `[x]` only when implemented, wired, and functional on normal path.
   - Do not mark done for `*_stub` handlers or normal-path `STATUS_NOT_IMPLEMENTED` placeholders.
   - For SSDT claims, verify service number/index ↔ Nt function ↔ registration/dispatch entry ↔ SSDT table row consistency.
   - If blocked/incomplete work remains, keep `[ ]` or `[/]` and add missing unchecked prerequisite/ownership items in prerequisite-first order with owner scope and `→ XREF`.
6. Verify and apply cross-TODO synchronization when section dependencies are involved.
   - For directly referenced TODO sections affected by this status change, patch reciprocal dependency/status notes in the same run.
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
- **Never add or preserve N.M subnumbering** (`17.1`, `### 3.2`, `**4.1 Foo**`). One `## N.` -- one continuous `- [ ]` list. Full-file structure: `.cursor/skills/validate-todo-file/SKILL.md`.

## Additional Resources

- [status-evidence.md](status-evidence.md) (optional companion file; same name may exist under `.claude/skills/validate-todo-section/` when added)
