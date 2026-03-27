---
name: improve-implementation-order
description: Audit and rewrite the Implementation Order table in a TODO file — verify execution order is correct and gap-free, compress verbose cross-domain references into compact notation (D=domain, T=TODO, §=section), and ensure every body section maps to a table row. Use when asked to improve, fix, audit, or review an Implementation Order table in a TODO file.
---

# Improve Implementation Order Table

Read and follow the canonical workflow: `.impossible/workflows/improve-implementation-order.md`

## Claude Code-Specific Notes

- Use Grep to search across TODO files when verifying cross-domain references.
- Use Glob to find TODO files by domain pattern (e.g., `todo/02-kernel-core/TODO-*.md`).
