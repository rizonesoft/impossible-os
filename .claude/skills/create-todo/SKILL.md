---
name: create-todo
description: Create lean project TODO files under todo/, choose the correct domain and local number, build the canonical Implementation Order table, wire XREFs, and update indexes. Use when the user asks to create, author, scaffold, or derive a new TODO roadmap from a feature, spec, or subsystem.
---

# Create TODO

Read and follow the canonical workflow: `.impossible/workflows/create-todo.md`

## Claude Code-Specific Notes

- Use Grep and Glob tools for codebase discovery — no Srclight MCP.
- Search for existing implementations and related symbols before drafting the TODO.
- Use `git log` to discover recent work that may already cover planned scope.
