---
name: validate-todo-file
description: Validate a TODO file for structural completeness, Implementation Order accuracy, XREF continuity, cross-TODO scope overlap, Inputs path existence, and gap-free execution coverage without doing code-completion audits. Use when reviewing a TODO after creation or major edits, or before implementation begins.
---

# Validate TODO File

Read and follow the canonical workflow: `.impossible/workflows/validate-todo-file.md`

## Claude Code-Specific Notes

- Use Grep and Glob tools for scope-overlap detection — no Srclight MCP.
- Use `rg` patterns from the canonical workflow for line-break hygiene checks.
- For Inputs anchor checks, use Glob to confirm file existence on disk.
