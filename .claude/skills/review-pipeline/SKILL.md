---
name: review-pipeline
description: Full verify + quality review pipeline for one TODO section. Invokes verify-todo-section then quality-review-section back-to-back. Both have MANDATORY Codex dispatches -- no skipping, no self-review shortcuts. Use when you want the complete audit in one pass.
---

# Review Pipeline

> **This is the "no shortcuts" skill.** It invokes `/verify-todo-section` then `/quality-review-section` back-to-back on the same section. Both skills have mandatory Codex adversarial dispatches that CANNOT be skipped.

## When to Use

- The user says "review this section", "full review", or "review pipeline"
- Batch-reviewing multiple sections in a TODO file
- Any time you want both compliance + quality in one pass

## Pipeline

### Phase 1: Verify

Invoke `/verify-todo-section` on the section. This skill:
- Explores codebase for evidence of each `[x]` item
- Walks domain-appropriate code quality gates
- Runs MANDATORY Codex adversarial review (step 8)
- Fixes all valid findings
- Adds Verified stamp or downgrades items

### Phase 2: Quality Review

Invoke `/quality-review-section` on the same section. This skill:
- Researches industry standards (UEFI/ACPI/SMBIOS/Win32/x86 specs)
- Analyzes Win11/Linux parity
- Runs MANDATORY Codex performance + consistency review (step 4)
- Fixes all valid findings
- Adds Quality reviewed stamp

### Phase 3: Commit

Single commit with both stamps + any fixes from both phases.
- If only stamps: `"review: <TODO> §N -- verified + quality reviewed clean"`
- If fixes applied: `"review: <TODO> §N -- <summary of fixes>"`
- Push to `origin/main` immediately.

## Rules

- Both skills are INVOKED, not summarized. The full skill workflow runs for each.
- Codex is dispatched in BOTH phases. No exceptions. No "already reviewed" or "no diff" exemptions.
- If Codex can't see the diff, provide the file content directly in the prompt.
- Phase 2 runs even if Phase 1 was clean. Clean compliance does not imply clean quality.
- All stamps use domain-qualified XREFs in Accepted fields.
