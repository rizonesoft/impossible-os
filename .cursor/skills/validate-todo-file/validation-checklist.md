# Validation Checklist

## Line-Break Hygiene

- Run `rg '^ {2,}[a-zA-Z`'"'"'"]' <file>` first to get the complete continuation-line list; fix every match; re-run to confirm zero results before proceeding.
- No blank lines between consecutive list items in the same group (`- [ ]`, `- [x]`, `- bullet`); items must appear one directly after the other.
- Sub-bullets must immediately follow their parent item with no blank line between them.
- No mid-sentence or mid-paragraph hard line breaks in prose (Goal block, Prompt text, XREF notes, section intro paragraphs) -- each prose paragraph is one unbroken line.
- No blank lines inside a callout block (`> [!NOTE]`, `> [!WARNING]`, `> [!IMPORTANT]`, `> [!TIP]`, `> [!CAUTION]`) -- each callout is one contiguous `>` block.
- No blank lines between table rows; header row and separator row only, then data rows run uninterrupted.
- Blank lines ARE correct: between a section heading and its first content line, between visually distinct list groups covering different subjects, before and after `---` separators, and before/after code block fences (` ``` `).

## Core Structure

- Clear `> **Goal:**` block.
- Inputs section with file paths for all declared source anchors.
- Explicit execution order via `Implementation Order`.
- Section-level checklist items and clear verification or exit criteria.
- Cross-reference or handoff notes wherever scope overlaps other TODOs.
- Preserve table/icon/header formatting; update only cells/content justified by evidence.

## Inputs Anchor Checks

- Every file path in the Inputs section exists on disk.
- Paths that do not exist are flagged: either broken (file was moved/deleted) or planned (not yet created -- note it as such).
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

## Cross-File Sync Checks

- For each dependency finding tied to an external `→ XREF` target, patch the referenced TODO section in the same run (not report-only).
- Ensure reciprocal back-references exist in both source and target TODO sections.
- Add mirrored unchecked prerequisite/ownership items in the target section when required work is missing.
- If referenced section/order status is marked complete but required work remains unresolved, downgrade it consistently.
- Never mark `[x]` in referenced TODO files without strict implementation + wiring + behavior proof.
- For propagated completion, require explicit source `ID:` and target `SATISFIES:` mapping.
- Auto-close target `[ ] -> [x]` only when evidence fully covers target acceptance criteria.
- If only partial coverage exists, use `[/]` (or `[ ]`) and add concrete missing sub-items.

## Completion-Truth Checks

- Audit every checklist item (`[x]` and `[ ]`) against live codebase evidence.
- Mark done only with strict proof: implemented + wired + functional normal path (not stub, not `STATUS_NOT_IMPLEMENTED` placeholder flow).
- Never keep `[x]` when implementation is `_stub`, explicitly placeholder, or normal-path `STATUS_NOT_IMPLEMENTED`.
- When SSDT claims are present, verify service number ↔ function ↔ registration/dispatch table ↔ TODO row consistency before changing status.
- If a section marked complete still has gaps, add missing unchecked prerequisite items in logical order.
- Every new missing item must include ownership and references (`src/...` and/or `→ XREF: TODO-XX §N`).
- If required unchecked items remain, section status and `Implementation Order` must not stay complete.

## Findings Format

- Report findings in severity order: `Critical`, `High`, `Medium`, `Low`.
- Each finding includes evidence (`path:line` + symbol), confidence (`confirmed` or `inferred`), and disposition (`auto-fixed` or `follow-up`).
- Include a `Cross-file sync` block listing referenced TODO files edited, touched sections, and exact item/status changes.
- Include an `Auto-closure decisions` block (source ID, target ID, closure result, proof or missing criteria).
- If no findings exist, state that explicitly and summarize what was validated.

## Correction Rules

- Fix broken paths, stale wording, missing overlap notes, and outdated scope statements as you find them.
- Fix XREF section numbers that are off by one or that point at renamed sections.
- Keep wide tables compact enough to stay readable in wrapped editors; move verbose detail into bullets or notes.
- Use `verify-todo-section` only for deep-dive escalation; baseline completion-truth checks are required here.
- Keep cross-file edits limited to directly referenced TODO sections tied to the finding.
- Keep changes idempotent; a second validation pass on clean content should produce no additional edits.
