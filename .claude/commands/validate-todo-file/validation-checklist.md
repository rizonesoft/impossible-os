# Validation Checklist

## Line-Break Hygiene

- Run `rg '^ {2,}[a-zA-Z`'"]' <file>` first to get the complete continuation-line list; fix every match; re-run to confirm zero results before proceeding.
- No blank lines between consecutive list items in the same group (`- [ ]`, `- [x]`, `- bullet`); items must appear one directly after the other.
- Sub-bullets must immediately follow their parent item with no blank line between them.
- No mid-sentence or mid-paragraph hard line breaks in prose (Goal block, Prompt text, XREF notes, section intro paragraphs) — each prose paragraph is one unbroken line.
- No blank lines inside a callout block (`> [!NOTE]`, `> [!WARNING]`, `> [!IMPORTANT]`, `> [!TIP]`, `> [!CAUTION]`) — each callout is one contiguous `>` block.
- No blank lines between table rows; header row and separator row only, then data rows run uninterrupted.
- Blank lines ARE correct: between a section heading and its first content line, between visually distinct list groups covering different subjects, before and after `---` separators, and before/after code block fences (` ``` `).

## Required Structure Checks

- Clear `> **Goal:**` block.
- Inputs section with file paths for all declared source anchors.
- Explicit execution order via `Implementation Order`.
- Section-level checklist items and clear verification or exit criteria.
- Cross-reference or handoff notes wherever scope overlaps other TODOs.

## Inputs Anchor Checks

- Every file path in the Inputs section exists on disk.
- Paths that do not exist are flagged: either broken (file was moved/deleted) or planned (not yet created — note it as such).
- Do not read the files; existence check only.

## Cross-TODO Scope Overlap Checks

- No deliverable in this TODO is also claimed as a primary deliverable in another TODO in the same domain.
- Two TODOs depending on the same XREF section is acceptable; two TODOs implementing the same thing is a conflict that must be resolved.
- Scan domain `INDEX.md` summaries and the Implementation Order tables of adjacent TODOs.

## Execution-Coverage Checks

- Dependencies appear in the same order the work must actually happen.
- Every `→ XREF: TODO-XX §N` target: (1) the file exists, (2) the section number exists in that file, (3) the section still describes the dependency stated in the XREF.
- At each handoff boundary, the receiving TODO has a matching Inputs or XREF entry that covers what is being handed off.
- The file can be followed without inventing missing steps.

## Correction Rules

- Fix broken paths, stale wording, missing overlap notes, and outdated scope statements as you find them.
- Fix XREF section numbers that are off by one or that point at renamed sections.
- Keep wide tables compact enough to stay readable in wrapped editors; move verbose detail into bullets or notes.
- If the issue is really completion truth versus code, stop and use `verify-todo-section`.
