---
name: implement-todo-section
description: Execute one bounded TODO section, resolve XREF dependencies, use the supported build and debug workflow, and update the section state when code or test evidence changes it. Use when implementing a specific TODO section or a clearly scoped subset of a TODO.
---

# Implement TODO Section

Read and follow the canonical workflow: [`.impossible/workflows/implement-todo-section.md`](../../../.impossible/workflows/implement-todo-section.md)

## Cursor-Specific Notes

- Use the `user-srclight` MCP for fast symbol search, call-graph navigation, and cross-file dependency analysis — prefer it over grep when navigating unfamiliar code.
  - `hybrid_search(query)` — best general search; combines keyword + semantic via RRF fusion
  - `get_symbol(name)` — full source of a function or type by name
  - `get_callers(symbol, project)` / `get_callees(symbol, project)` — impact analysis
  - `get_dependents(symbol, project)` — what breaks if this symbol changes
  - `codebase_map()` — project orientation (run once per session)
  - Pass `project="impossible-os"` when prompted.
- grep is fine for: checking if a literal string exists in a known file, listing `#include` lines, exact token pattern checks.
- Treat `.srclight/` as disposable — repo truth wins if results conflict.
- Use Plan mode for risky, ambiguous, architectural, or multi-file trade-off work.
- Use Debug mode when runtime evidence is the real bottleneck.
- **Build timeouts:** run incremental builds with `block_until_ms: 60000` and clean builds with `block_until_ms: 120000`.
