---
description: Create a new TODO file following the project's established format and structure
---
> **Cursor skill:** [`.cursor/skills/create-todo/SKILL.md`](../../.cursor/skills/create-todo/SKILL.md) — `/create-todo`
> **Antigravity workflow:** [`.agents/workflows/create-todo.md`](../../.agents/workflows/create-todo.md) — `/create-todo`
> **Claude Code command:** `.claude/commands/create-todo.md` — `/create-todo`

See the Cursor skill for the full procedure (authoritative). Summary:

1. Read `todo/TODO-00-INDEX.md` and the target domain `INDEX.md`
2. Gather inputs (spec doc, existing code, related TODOs)
3. Pick the next local filename: `todo/<domain>/TODO-NN-short-name.md`
4. Draft in lean format: goal, Implementation Order table, `💎`/`⭐` markers, XREFs
5. Assign `[Sonnet]`/`[Opus]` tag to each section heading
6. Add OS Comparison table
7. Update domain `INDEX.md` and root `TODO-00-INDEX.md`
