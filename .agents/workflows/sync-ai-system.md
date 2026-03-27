---
description: Update AI-facing project guidance when shared conventions change, keeping all agent adapters aligned with repo truth.
---

> **Cursor skill (authoritative):** [`.cursor/skills/sync-ai-system/SKILL.md`](../../.cursor/skills/sync-ai-system/SKILL.md)
> **Canonical workflow:** [`.impossible/workflows/sync-ai-system.md`](../../.impossible/workflows/sync-ai-system.md)

## Workflow

1. Identify the changed convention or policy
2. Update `.impossible/rules/<name>.md` — the canonical source first
3. Check all thin adapters for stale inline summaries:
   - `.cursor/rules/*.mdc`
   - `AGENTS.md`
   - `CLAUDE.md`
   - `.github/copilot-instructions.md`
   - `.impossible/context.md` if project state changed
4. Update `.impossible/index.md` if a rule/workflow was added or removed
5. Update affected `todo/**/*.md` XREFs or guidance
6. Report what changed, what remains deferred, and any surfaces that still need follow-up

## Guardrails

- Do not treat untracked local state as canonical project behavior
- Do not perform TODO roadmap sync or checkbox-vs-code audits here
- Do not weaken safety, build, or tooling expectations for convenience

## Additional Resources

- See [ai-sync-checklist.md](../../.cursor/skills/sync-ai-system/ai-sync-checklist.md)
