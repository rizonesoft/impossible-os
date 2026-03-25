# AI Sync Checklist

## Common Triggers

- A new Cursor rule or skill changes agent behavior.
- MCP policy, hook policy, or `.githooks/` expectations change.
- The supported tooling contract changes in `TODO-02`.
- Local, cloud, or team-managed AI features need clearer boundaries.
- Product-direction guidance changes and affects AI decisions.

## Surfaces To Check

- `AGENTS.md`
- Relevant `.cursor/rules/`
- Relevant `.cursor/skills/`
- Compatibility notes for Antigravity or other shared AI tooling
- User-facing docs such as `README.md` or `CONTRIBUTING.md` when contributor expectations changed

## Preserve During Sync

- Production-grade posture and "Nothing is impossible" standard
- Srclight-only supported MCP baseline unless explicitly changed
- Repo-truth-over-local-state ownership model
- Supported tooling awareness from `todo/00-infrastructure/TODO-02-developer-tooling-stack.md`
