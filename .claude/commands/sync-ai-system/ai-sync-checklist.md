# AI Sync Checklist

## Common Triggers

- A new command in `.claude/commands/` changes agent behavior.
- MCP policy, hook policy, or `.githooks/` expectations change.
- The supported tooling contract changes in `TODO-02`.
- Local, cloud, or team-managed AI features need clearer boundaries.
- Product-direction guidance changes and affects AI decisions.

## Surfaces To Check

- `AGENTS.md`
- `CLAUDE.md`
- `.github/copilot-instructions.md`
- `.impossible/rules/` (canonical source)
- `.impossible/context.md` (project state)
- `.agents/workflows/` (Antigravity redirects)
- `.claude/commands/` (canonical slash commands)
- `.cursor/rules/*.mdc` (Cursor rule adapters)
- User-facing docs such as `README.md` or `CONTRIBUTING.md` when contributor expectations changed

## Preserve During Sync

- Production-grade posture and "Nothing is impossible" standard
- Srclight-only supported MCP baseline unless explicitly changed
- Repo-truth-over-local-state ownership model
- Supported tooling awareness from `todo/00-infrastructure/TODO-02-developer-tooling-stack.md`
