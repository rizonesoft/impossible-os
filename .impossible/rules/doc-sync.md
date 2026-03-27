# Documentation Sync Discipline

> **Applies to:** All files — when code changes, docs must change in the same task.
> **Canonical location:** `.impossible/rules/doc-sync.md`

## The Rule

When code, conventions, build flow, API names, paths, or tooling assumptions change, **update the affected tracked guidance in the same task**.

Do not leave a known contradiction between code and tracked guidance for "later".

## Primary Sync Targets

When making changes, check whether these need updating:

- `AGENTS.md` — human-facing project overview
- `CLAUDE.md` — Claude Code instructions
- `.github/copilot-instructions.md` — Copilot instructions
- `.impossible/rules/` — canonical rule content (this directory)
- `.cursor/rules/*.mdc` — Cursor rule adapters
- `.impossible/context.md` — current project state
- Affected `todo/**/*.md` files — checklist items, XREFs
- Affected `docs/**/*.md` files — architecture, specs, guides

## Common Triggers

These changes almost always require a doc sync:

- Allocator pattern changes (`kmalloc` vs `pmm_alloc_contiguous` thresholds)
- Boot sequence changes (new `boot_info` fields, new init order)
- Renamed APIs, functions, or structs
- Path or naming convention shifts
- Workflow or build flow changes
- MCP or tooling policy changes
- New rule or convention established during implementation

## Example

If a task changes the canonical build verification flow:
1. Update `.impossible/rules/build.md`
2. Update `AGENTS.md` build section
3. Update `.cursor/rules/safety-build.mdc` if it diverges
4. Update `CLAUDE.md` if it has inline build notes
5. Update any affected TODO or compatibility doc
