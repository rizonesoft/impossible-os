---
description: Validate a TODO file for structural completeness, Implementation Order accuracy, XREF continuity, cross-TODO scope overlap, Inputs path existence, and gap-free execution coverage without doing code-completion audits.
---

# Validate TODO File

## Workflow

1. Read the full TODO file, not just a selected section.
2. Clean up unnecessary manual line breaks throughout the file.
   - **Before touching anything**, run this grep to get the complete list of all continuation lines in the file:
     ```
     rg '^ {2,}[a-zA-Z`'"'"'"]' <file>
     ```
     Fix **every** match before moving on. Re-run the same grep after fixes — proceed only when it returns zero matches.
   - **List items** (`- [ ]`, `- [x]`, `- bullet`): remove blank lines between consecutive items in the same group — items must appear one directly after the other with no intervening blank line. Sub-bullets must immediately follow their parent item with no blank line.
   - **Prose blocks** (Goal block, Prompt text, XREF notes, callout bodies, section intro paragraphs): join hard-wrapped mid-sentence line breaks into a single flowing line. A prose paragraph is one unbroken line unless it is genuinely a new paragraph.
   - **Callout blocks** (`> [!NOTE]`, `> [!IMPORTANT]`, `> [!WARNING]`, `> [!TIP]`, `> [!CAUTION]`): no blank lines inside a single callout — the whole callout is one contiguous `>` block.
   - **Tables**: no blank lines between rows; keep the header and separator rows but nothing else between data rows.
   - **Preserve** blank lines between: section headings and their first content line, visually distinct list groups covering different subjects, `---` horizontal separators, and code block fences (` ``` `).
   - Do NOT add new line breaks; only remove spurious ones.
3. Anchor-check the Inputs section.
   - For each file path listed in Inputs, confirm it exists on disk.
   - Flag any path that does not exist as a broken anchor; suggest the correct path or note it as planned.
   - Do not read the files — existence check only.
4. Read the domain `INDEX.md` and all other TODO files in the same domain folder.
   - Scan for scope overlap: flag any deliverable claimed by this TODO that is also claimed by another TODO in the domain.
   - Scan for duplicate XREFs pointing at the same target section — two TODOs both depending on the same section is fine; two TODOs both *implementing* it is a conflict.
5. Validate the current lean TODO structure.
   - Check required sections, numbering, checklist shape, references, and exit criteria.
   - Use `Implementation Order`, not legacy phase-table rules.
6. Validate execution coverage.
   - Check dependency order, `→ XREF:` lines, overlap notes, handoffs, and adjacent-file continuity.
   - For each `→ XREF: TODO-XX §N`, confirm the target TODO file exists **and** the referenced section number is present in that file. Flag section-level mismatches, not just missing files.
   - Check handoff boundaries: for each deliverable this TODO hands off to another, confirm the receiving TODO has a matching Inputs or XREF entry covering that handoff.
   - Fix stale planning text, broken links, and roadmap inconsistencies that the validation exposes.
7. If the problem is code-truth or completion-state accuracy, hand off to `verify-todo-section` instead of guessing here.

## Guardrails

- Do not implement code.
- Do not mark TODO work done based on code inspection alone.
- Do not turn this into a formatting-only cleanup pass; structural clarity is the goal.
- Inputs path checks are existence-only — do not read or analyse the referenced source files.

## Supporting References

- [validation-checklist.md](validation-checklist.md) — detailed validation checklist
