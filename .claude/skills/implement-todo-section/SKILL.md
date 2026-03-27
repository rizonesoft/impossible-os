---
name: implement-todo-section
description: Execute one bounded TODO section, resolve XREF dependencies, use the supported build and debug workflow, and update the section state when code or test evidence changes it. Use when implementing a specific TODO section or a clearly scoped subset of a TODO.
---

# Implement TODO Section

Read and follow the canonical workflow: `.impossible/workflows/implement-todo-section.md`

## Claude Code-Specific Notes

- Use Grep and Glob tools for codebase discovery — no Srclight MCP.
- Use `bash scripts/build.sh` for builds — check `tail -1 build/build.log` for `=== BUILD OK ===`.
- Use `bash scripts/build.sh run` for QEMU runtime verification.
- Use `llvm-addr2line-19 -e build/kernel.exe -f <RIP>` for crash debugging.
- Plan risky or ambiguous work before jumping in — use plan mode when available.
- Follow all project rules in `.impossible/rules/`.
