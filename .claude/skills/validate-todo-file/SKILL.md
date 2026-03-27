---
name: validate-todo-file
description: Validate a TODO file for structural completeness, Implementation Order accuracy, XREF continuity, cross-TODO scope overlap, Inputs path existence, and gap-free execution coverage without doing code-completion audits. Use when reviewing a TODO after creation or major edits, or before implementation begins.
---

# Validate TODO File

## Workflow

1. Read the full TODO file, not just a selected section.
2. Clean up unnecessary manual line breaks throughout the file.
   - **Before touching anything**, run Grep to find all continuation lines:
     ```
     rg '^ {2,}[a-zA-Z`'"'"'"]' <file>
     ```
     Fix **every** match before moving on. Re-run after fixes — proceed only when it returns zero matches.
   - **List items** (`- [ ]`, `- [x]`, `- bullet`): remove blank lines between consecutive items in the same group.
   - **Prose blocks** (Goal block, Prompt text, XREF notes, callout bodies): join hard-wrapped mid-sentence line breaks into a single flowing line.
   - **Callout blocks** (`> [!NOTE]`, `> [!IMPORTANT]`, etc.): no blank lines inside a single callout.
   - **Tables**: no blank lines between rows.
   - **Preserve** blank lines between: section headings and content, distinct list groups, `---` separators, and code block fences.
3. Anchor-check the Inputs section.
   - For each file path listed in Inputs, use Glob to confirm it exists on disk.
   - Flag any path that does not exist as a broken anchor; suggest the correct path or note it as planned.
   - Do not read the files — existence check only.
4. Read the domain `INDEX.md` and all other TODO files in the same domain folder.
   - Use Grep to search for deliverable names and feature keywords across domain TODOs to detect scope overlap.
   - Scan for duplicate XREFs pointing at the same target section — two TODOs depending on the same section is fine; two TODOs both *implementing* it is a conflict.
5. Validate the current lean TODO structure.
   - Check required sections, numbering, checklist shape, references, and exit criteria.
   - Use `Implementation Order`, not legacy phase-table rules.
6. Validate execution coverage.
   - Check dependency order, `→ XREF:` lines, overlap notes, handoffs, and adjacent-file continuity.
   - For each `→ XREF: TODO-XX §N`, confirm the target TODO file exists **and** the referenced section number is present in that file.
   - Check handoff boundaries: for each deliverable this TODO hands off to another, confirm the receiving TODO has a matching Inputs or XREF entry.
   - Fix stale planning text, broken links, and roadmap inconsistencies.
7. If the problem is code-truth or completion-state accuracy, hand off to `/verify-todo-section` instead.

## Guardrails

- Do not implement code.
- Do not mark TODO work done based on code inspection alone.
- Do not turn this into a formatting-only cleanup pass; structural clarity is the goal.
- Inputs path checks are existence-only — do not read or analyse the referenced source files.

## Additional Resources

- [validation-checklist.md](validation-checklist.md)
