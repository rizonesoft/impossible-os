---
description: Keep repo-owned AI guidance aligned when shared conventions change
---
> **Cursor skill:** [`.cursor/skills/sync-ai-system/SKILL.md`](../../.cursor/skills/sync-ai-system/SKILL.md) — `/sync-ai-system`
> **Antigravity workflow:** [`.agents/workflows/sync-ai-system.md`](../../.agents/workflows/sync-ai-system.md) — `/sync-ai-system`
> **Claude Code command:** `.claude/commands/sync-ai-system.md` — `/sync-ai-system`

See the Cursor skill for the full procedure (authoritative). Summary:

1. Identify what changed (rule, convention, build flow, tooling)
2. Update `.impossible/rules/` — the canonical rule content
3. Update `.cursor/rules/*.mdc` adapters to reflect the change
4. Update `AGENTS.md`, `CLAUDE.md`, `.github/copilot-instructions.md`
5. Update `.impossible/context.md` if the current project state changed
6. Update affected `todo/**/*.md` XREFs or guidance
