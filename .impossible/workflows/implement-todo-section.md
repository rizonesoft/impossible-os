---
description: Implement one scoped TODO section from start to commit
---
> **Cursor skill:** [`.cursor/skills/implement-todo-section/SKILL.md`](../../.cursor/skills/implement-todo-section/SKILL.md) — `/implement-todo-section`
> **Antigravity workflow:** [`.agents/workflows/implement-todo-section.md`](../../.agents/workflows/implement-todo-section.md) — `/implement-todo-section`
> **Claude Code command:** `.claude/commands/implement-todo-section.md` — `/implement-todo-section`

See the Cursor skill for the full procedure (authoritative). Summary:

1. Read the target TODO section and its XREFs
2. Orient with Srclight: `codebase_map()`, `hybrid_search()`, affected symbols
3. Implement the changes (code + headers)
4. Build: `bash scripts/build.sh` → verify `BUILD OK`
5. Run in QEMU: `bash scripts/build.sh run` → verify serial output
6. Mark section items `[x]`, update checklist state
7. Commit: `git add -A && git commit -m "scope: message"`
