---
description: Reconcile a TODO section's status against code, build, and runtime evidence
---
> **Cursor skill:** [`.cursor/skills/verify-todo-section/SKILL.md`](../../.cursor/skills/verify-todo-section/SKILL.md) — `/verify-todo-section`
> **Antigravity workflow:** [`.agents/workflows/verify-todo-section.md`](../../.agents/workflows/verify-todo-section.md) — `/verify-todo-section`
> **Claude Code command:** `.claude/commands/verify-todo-section.md` — `/verify-todo-section`

See the Cursor skill for the full procedure (authoritative). Summary:

1. Read the TODO section and identify all `[x]` and `[/]` items
2. For each claimed-done item: find the code evidence (Srclight + file reads)
3. Build to confirm no regressions: `bash scripts/build.sh`
4. Test in QEMU if runtime evidence is needed
5. Correct checkbox state conservatively: only mark `[x]` when evidence is clear
6. Note any gaps or deferred limits explicitly in the TODO
