---
name: sync-ai-system
description: Update AI-facing project guidance when shared conventions change, keeping all agent adapters aligned with repo truth. Use when AI workflow, tooling policy, or supported automation behavior changes.
---

# Sync AI System

Read and follow the canonical workflow: `.impossible/workflows/sync-ai-system.md`

## Claude Code-Specific Notes

- When checking Claude Code-specific surfaces, verify:
  - `CLAUDE.md` inline rules match `.impossible/rules/` content
  - `.claude/skills/*/SKILL.md` adapters reference the correct canonical workflow
  - No Srclight MCP references in Claude Code files (`git grep "Srclight" .claude/` should return 0)
