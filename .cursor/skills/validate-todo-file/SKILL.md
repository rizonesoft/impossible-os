---
name: validate-todo-file
description: Validate a TODO file for structural completeness, Implementation Order accuracy, XREF continuity, and gap-free execution coverage without doing code-completion audits. Use when reviewing a TODO after creation or major edits, or before implementation begins.
---

# Validate TODO File

## Workflow

1. Read the full TODO file, not just a selected section.
2. Read nearby related TODOs, domain indexes, and any linked files needed to validate continuity.
3. Validate the current lean TODO structure.
   - Check required sections, numbering, checklist shape, references, and exit criteria.
   - Use `Implementation Order`, not legacy phase-table rules.
4. Validate execution coverage.
   - Check dependency order, `→ XREF:` lines, overlap notes, handoffs, and adjacent-file continuity.
   - Fix stale planning text, broken links, and roadmap inconsistencies that the validation exposes.
5. If the problem is code-truth or completion-state accuracy, hand off to `verify-todo-section` instead of guessing here.

## Guardrails

- Do not implement code.
- Do not mark TODO work done based on code inspection alone.
- Do not turn this into a formatting-only cleanup pass; structural clarity is the goal.

## Additional Resources

- [validation-checklist.md](validation-checklist.md)
