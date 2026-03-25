---
name: sync-ai-system
description: Update AI-facing project guidance when shared conventions change, keeping AGENTS.md, Cursor rules, Cursor skills, and compatibility docs aligned with repo truth. Use when AI workflow, tooling policy, or supported automation behavior changes.
---

# Sync AI System

## Workflow

1. Identify the changed convention or policy.
2. Read the affected repo-owned AI surfaces.
   - `AGENTS.md`
   - Relevant `.cursor/rules/`
   - Relevant `.cursor/skills/`
   - Compatibility docs or notes
   - `todo/00-infrastructure/TODO-02-developer-tooling-stack.md` when tooling-contract assumptions changed
3. Update the affected guidance in the same task so tracked docs stop contradicting repo reality.
4. Preserve the top-level project posture.
   - Impossible OS is production-grade.
   - Difficulty is never a reason to lower standards or normalize hacks.
5. Keep repo-owned guidance canonical.
   - Local MCP state, dashboard settings, cloud features, and machine-local config stay downstream of tracked repo truth.
6. Report what changed, what remains deferred, and any surfaces that still need follow-up.

## Guardrails

- Do not treat untracked local state as canonical project behavior.
- Do not perform TODO roadmap sync or checkbox-vs-code audits here.
- Do not weaken safety, build, or tooling expectations for convenience.

## Additional Resources

- [ai-sync-checklist.md](ai-sync-checklist.md)
