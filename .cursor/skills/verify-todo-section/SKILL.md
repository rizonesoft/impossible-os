---
name: verify-todo-section
description: Verify whether a TODO section marked done or in progress matches actual code, build, and runtime evidence, then correct the section state conservatively. Use when auditing completed TODO work, reconciling stale checklist state, or verifying a claimed implementation.
---

# Verify TODO Section

Read and follow the canonical workflow: [`.impossible/workflows/verify-todo-section.md`](../../../.impossible/workflows/verify-todo-section.md)

## Cursor-Specific Notes

- Use the `user-srclight` MCP for finding code evidence before grep.
  - `hybrid_search(query)` — find implementations matching TODO claims.
  - `get_symbol(name)` — inspect specific functions or types.
  - `get_dependents(symbol, project)` — check integration points.
- Treat `.srclight/` as disposable — repo truth wins if results conflict.
