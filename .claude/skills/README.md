# Claude Code skills (source of truth)

Skills in this directory are for **Claude Code** (implementation, review, Codex dispatch, domain checklists, etc.).

## Cursor overlap

Cursor intentionally keeps **only** TODO roadmap skills under `.cursor/skills/` (`validate-todo-file`, `validate-todo-section`, `gap-analysis-todo`). See `.cursor/skills/README.md` and `.cursor/rules/todo-validate-gap-workflows.mdc`.

Do **not** run `rsync -a --delete .claude/skills/ .cursor/skills/` on this branch: it would wipe that minimal Cursor set. If you add a new skill that must also exist in Cursor, copy or author it explicitly under `.cursor/skills/` alongside the README policy.
