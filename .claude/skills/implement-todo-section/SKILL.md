---
name: implement-todo-section
description: Execute one bounded TODO section, resolve XREF dependencies, run Codex adversarial review, self-review for regressions, validate section, tie up loose ends, and commit. Use when implementing a specific TODO section or a clearly scoped subset of one.
---

# Implement TODO Section -- Pipeline

> 8 stages. Each stage completes before the next starts. Review stages invoke dedicated skills -- do NOT skip them or inline a lesser version.

## Use This Skill When

- The user asks to implement a specific TODO section (e.g., "implement §2 of TODO-16").
- The user says "implement", "build", or "code" followed by a section description pasted from a TODO.
- Working through a TODO file section-by-section after `/todo-pipeline` preparation.
- Do NOT use for verification-only -- use `/verify-todo-section` instead.
- Do NOT use for creating new TODOs -- use `/create-todo` instead.

## Pipeline

### 1. Understand

- Read the full section text, notes, test checkpoint, warning boxes.
- Follow every `-> XREF:` line. Stop and ask if a prerequisite is incomplete.
- Grep/Glob for symbols, call-graph, cross-file integration surface.
- If the section has > 10 checklist items, suggest splitting before implementing.

### 2. Design Review (high-risk sections)

**Skip for:** simple struct definitions, single-function additions, test-only work.
**Invoke for:** SMP-sensitive code, boot-path, page tables, interrupt handling, security-critical logic.

→ Invoke `/codex-design-review` with the section plan + integration surface + constraints.

Fix any blockers before proceeding.

### 3. Implement + Build

- `kernel-code-quality` skill auto-loads -- follow its gates.
- Freestanding kernel rules: `kernel/types.h`, `kmalloc`/`pmm_alloc_contiguous`, NASM x86-64.
- POST16 codes for boot-path/hardware code: check `boot_init.h` for conflicts first.
- Build: `bash scripts/build.sh` -- must show `=== BUILD OK ===`.

### 4. Test

- Read the TODO's `## Unit Tests` section for expected test file, registration function, category.
- Create or update test file with concrete assertions for the section's deliverables.
- Build to confirm tests compile.

→ Invoke `/codex-test-coverage` to find missing assertions. Add any gaps found.

### 5. Update TODO

- Mark checklist items `[x]` with evidence (implementation + wiring + functional path). Never `[x]` for stubs.
- Keep blocked items `[ ]` or `[/]` with explicit blocker notes.
- Update the **Implementation Order** table row: `[x]` or `[/]`.
- Update the **OS Comparison** table: replace `Planned` with concrete description.
- Resolve deferred items from earlier sections that pointed to this one.

### 6. Adversarial Review

→ Invoke `/codex-adversarial-review-section` scoped to this section's changed files.

This skill runs Codex adversarial review, fixes findings up to 3 rounds, and reports the final status. Do NOT proceed to stage 7 if unresolved Critical/High findings remain -- keep the section `[/]`.

### 7. Validate

→ Invoke `/validate-todo-section` on the completed section.

This catches stale checklist claims, missing cross-TODO sync, and loose ends in the current and XREF'd TODO files.

### 8. Ship

- `bash scripts/build.sh` -- final build confirmation.
- Commit using the section's `Commit:` line. Stage all source, headers, TODO, and test files.
- Push to `origin/main` immediately.
- Mark the `- [ ] Commit: "..."` item `[x]`.

## Guardrails

- **One section = one commit.** Never batch multiple sections.
- **Stages 6-7 are mandatory before stage 8.** The pattern of "build passes, skip review, commit" is explicitly prohibited. If you're about to `git add` without having run stages 6-7, stop.
- Do not mark items `[x]` from intent or stubs. Require implementation + wiring evidence.
- Do not create new TODO files. Do not widen scope beyond the section.
