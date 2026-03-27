---
name: sync-ai-system
description: Update AI-facing project guidance when shared conventions change, keeping all agent adapters aligned with repo truth. Use when AI workflow, tooling policy, or supported automation behavior changes.
---

# Sync AI System

## Workflow

1. Identify the changed convention or policy.
2. Update `.impossible/rules/<name>.md` — the canonical source first.
3. Check all thin adapters for stale inline summaries:
   - `.cursor/rules/*.mdc`
   - `AGENTS.md`
   - `CLAUDE.md`
   - `.github/copilot-instructions.md`
   - `.impossible/context.md` if project state changed
4. Check `.agents/workflows/` for stale redirect descriptions.
5. Update `.impossible/index.md` if a rule/workflow was added or removed.
6. Update affected `todo/**/*.md` XREFs or guidance.
7. Preserve the top-level project posture.
   - Impossible OS is production-grade.
   - Difficulty is never a reason to lower standards or normalize hacks.
8. Keep repo-owned guidance canonical.
   - Local MCP state, dashboard settings, cloud features, and machine-local config stay downstream of tracked repo truth.
9. Report what changed, what remains deferred, and any surfaces that still need follow-up.

## Guardrails

- Do not treat untracked local state as canonical project behavior.
- Do not perform TODO roadmap sync or checkbox-vs-code audits here.
- Do not weaken safety, build, or tooling expectations for convenience.

## Additional Resources

- [ai-sync-checklist.md](sync-ai-system/ai-sync-checklist.md)
