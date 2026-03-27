---
description: Validate a TODO file for gaps, inconsistencies, and missing features — then fix everything
---
> **Cursor skill:** [`.cursor/skills/validate-todo-file/SKILL.md`](../../.cursor/skills/validate-todo-file/SKILL.md) — `/validate-todo-file`
> **Antigravity workflow:** [`.agents/workflows/validate-todo-file.md`](../../.agents/workflows/validate-todo-file.md) — `/validate-todo-file`
> **Claude Code command:** `.claude/commands/validate-todo-file.md` — `/validate-todo-file`

See the Cursor skill for the full procedure (authoritative). Summary:

1. Read the TODO file and its domain INDEX.md
2. Check: Implementation Order table present and gap-free
3. Check: all XREFs valid, all sections numbered, no orphan items
4. Check: adjacent file continuity (no gaps between this TODO and the next)
5. Check: OS Comparison table present
6. Fix all issues found in the same task
7. Re-align all tables, verify no hard wraps
