---
name: sync-ai-system
description: Update AI-facing project guidance when shared conventions change, keeping AGENTS.md, Cursor rules, Cursor skills, and compatibility docs aligned with repo truth. Use when AI workflow, tooling policy, or supported automation behavior changes.
---

# Sync AI System

Read and follow the canonical workflow: [`.impossible/workflows/sync-ai-system.md`](../../../.impossible/workflows/sync-ai-system.md)

## Cursor-Specific Notes

- When checking Cursor-specific surfaces, also verify:
  - `.cursor/rules/*.mdc` adapters match `.impossible/rules/` content
  - `.cursor/skills/*/SKILL.md` adapters reference the correct canonical workflow
  - Srclight MCP usage guidance is consistent across skill files
