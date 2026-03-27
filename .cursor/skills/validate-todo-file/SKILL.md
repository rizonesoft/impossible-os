---
name: validate-todo-file
description: Validate a TODO file for structural completeness, Implementation Order accuracy, XREF continuity, cross-TODO scope overlap, Inputs path existence, and gap-free execution coverage without doing code-completion audits. Use when reviewing a TODO after creation or major edits, or before implementation begins.
---

# Validate TODO File

Read and follow the canonical workflow: [`.impossible/workflows/validate-todo-file.md`](../../../.impossible/workflows/validate-todo-file.md)

## Cursor-Specific Notes

- Use the `user-srclight` MCP to accelerate scope-overlap detection before reading files manually.
  - `hybrid_search(query)` — find matching symbols, functions, or types already in the codebase or referenced in other TODOs.
  - `whats_changed(project)` and `recent_changes(project)` — surface work-in-progress that overlaps claimed scope.
  - `get_dependents(symbol, project)` — identify which existing components would be affected by this TODO's deliverables.
  - Only fall back to direct file reads when Srclight results are insufficient to resolve an overlap question.
