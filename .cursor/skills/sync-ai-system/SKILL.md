---
name: sync-ai-system
description: Update AI-facing project guidance when shared conventions change, keeping Cursor and Claude Code systems aligned with repo truth. Use when AI workflow, tooling policy, or supported automation behavior changes.
---

# Sync AI System

## Architecture

Cursor and Claude Code are **independent systems** with no shared layer:

- **Cursor** owns `.cursor/rules/` and `.cursor/skills/` -- self-contained, uses Srclight MCP.
- **Claude Code** owns `CLAUDE.md` and `.claude/skills/` -- self-contained, uses Grep/Glob.
- **`.cursorignore`** prevents Cursor from indexing `.claude/`.

Both systems enforce the same project conventions but through their own tool-native mechanisms.

## Workflow

1. Identify the changed convention or policy.
2. Read the affected repo-owned AI surfaces.
   - **Cursor:** relevant `.cursor/rules/`, relevant `.cursor/skills/`
   - **Claude Code:** `CLAUDE.md`, relevant `.claude/skills/`
   - `todo/00-infrastructure/TODO-02-developer-tooling-stack.md` when tooling-contract assumptions changed
3. Update the affected guidance in the same task so tracked docs stop contradicting repo reality.
4. Preserve the top-level project posture.
   - Impossible OS is production-grade.
   - Difficulty is never a reason to lower standards or normalize hacks.
5. Keep repo-owned guidance canonical.
   - Local MCP state, dashboard settings, cloud features, and machine-local config stay downstream of tracked repo truth.
   - When equivalent skill content exists in multiple assistant folders, `.claude/skills/` is the source of truth and downstream copies should be resynced from it unless the repo explicitly documents a deliberate divergence.
6. Report what changed, what remains deferred, and any surfaces that still need follow-up.

## Guardrails

- Do not treat untracked local state as canonical project behavior.
- Do not perform TODO roadmap sync or checkbox-vs-code audits here.
- Do not weaken safety, build, or tooling expectations for convenience.
- Do not add cross-references between `.cursor/` and `.claude/` -- they are independent.

## Additional Resources

- [ai-sync-checklist.md](ai-sync-checklist.md)
