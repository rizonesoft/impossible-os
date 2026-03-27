---
name: sync-ai-system
description: Update AI-facing project guidance when shared conventions change, keeping Cursor and Claude Code systems aligned with repo truth. Use when AI workflow, tooling policy, or supported automation behavior changes.
---

# Sync AI System

## Architecture

Cursor and Claude Code are **independent systems** with no shared layer:

- **Cursor** owns `.cursor/rules/` and `.cursor/skills/` — self-contained, uses Srclight MCP.
- **Claude Code** owns `CLAUDE.md` and `.claude/skills/` — self-contained, uses Grep/Glob.
- **`.cursorignore`** prevents Cursor from indexing `.claude/`.

Both systems enforce the same project conventions but through their own tool-native mechanisms.

## Workflow

1. Identify the changed convention or policy.
2. Update the affected surfaces in **both** systems:
   - **Claude Code:** `CLAUDE.md`, relevant `.claude/skills/*/SKILL.md`
   - **Cursor:** relevant `.cursor/rules/*.mdc`, relevant `.cursor/skills/*/SKILL.md`
3. Verify consistency — both systems should enforce the same rules, just with different tooling references.
4. Preserve the top-level project posture.
   - Impossible OS is production-grade.
   - Difficulty is never a reason to lower standards or normalize hacks.
5. Keep repo-owned guidance canonical.
   - Local MCP state, dashboard settings, cloud features, and machine-local config stay downstream of tracked repo truth.
6. Report what changed, what remains deferred, and any surfaces that still need follow-up.

## Surfaces To Check

| System | Files |
|--------|-------|
| Claude Code | `CLAUDE.md`, `.claude/skills/*/SKILL.md` |
| Cursor | `.cursor/rules/*.mdc`, `.cursor/skills/*/SKILL.md` |
| Shared | `todo/00-infrastructure/TODO-02-developer-tooling-stack.md` (tooling contract) |

## Guardrails

- Do not treat untracked local state as canonical project behavior.
- Do not perform TODO roadmap sync or checkbox-vs-code audits here.
- Do not weaken safety, build, or tooling expectations for convenience.
- Do not add cross-references between `.cursor/` and `.claude/` — they are independent.
