# AI Sync Checklist

## Common Triggers

- A Cursor rule or skill changes agent behavior.
- A Claude Code skill or CLAUDE.md rule changes agent behavior.
- MCP policy, hook policy, or `.githooks/` expectations change.
- The supported tooling contract changes in `TODO-02`.
- Product-direction guidance changes and affects AI decisions.

## Surfaces To Check

| System | Surface | What to verify |
|--------|---------|----------------|
| Claude Code | `CLAUDE.md` | Inline rules match project conventions |
| Claude Code | `.claude/skills/*/SKILL.md` | Self-contained, no Srclight references |
| Cursor | `.cursor/rules/*.mdc` | Rules match project conventions |
| Cursor | `.cursor/skills/*/SKILL.md` | Self-contained, Srclight references are Cursor-only |
| Shared | `.cursorignore` | `.claude/` is excluded from Cursor indexing |

## Preserve During Sync

- Production-grade posture and "Nothing is impossible" standard
- Repo-truth-over-local-state ownership model
- Independence of Cursor and Claude Code systems -- no cross-references
- Supported tooling awareness from `todo/00-infrastructure/TODO-02-developer-tooling-stack.md`
