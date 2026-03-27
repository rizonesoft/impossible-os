---
name: create-todo
description: Create lean project TODO files under todo/, choose the correct domain and local number, build the canonical Implementation Order table, wire XREFs, and update indexes. Use when the user asks to create, author, scaffold, or derive a new TODO roadmap from a feature, spec, or subsystem.
---

# Create TODO

Read and follow the canonical workflow: [`.impossible/workflows/create-todo.md`](../../../.impossible/workflows/create-todo.md)

## Cursor-Specific Notes

- Use the `user-srclight` MCP for codebase discovery before scanning files manually.
  - `codebase_map()` — understand project structure, languages, and symbol counts.
  - `hybrid_search(query)` — find existing implementations, related symbols, and prior art.
  - `symbols_in_file(path, project)` — enumerate all symbols in relevant source files.
  - `get_callers`, `get_callees`, `get_dependents` — trace integration points and impact boundaries.
  - `whats_changed(project)` — discover recent work that may already cover planned scope.
  - Let Srclight discovery drive what goes into Inputs, XREFs, and dependency rows — only fall back to direct file reads when Srclight results are insufficient.
