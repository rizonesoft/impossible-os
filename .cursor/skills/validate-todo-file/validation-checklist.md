# Validation Checklist

## Required Structure

- Clear `> **Goal:**` block.
- Inputs, references, or equivalent source anchors for the declared scope.
- Explicit execution order via `Implementation Order`.
- Section-level checklist items and clear verification or exit criteria.
- Cross-reference or handoff notes wherever scope overlaps other TODOs.

## Execution-Coverage Checks

- Dependencies appear in the same order the work must actually happen.
- `→ XREF:` targets exist and still describe the right dependency.
- Adjacent TODOs do not leave unowned gaps between handoff boundaries.
- The file can be followed without inventing missing steps.

## Correction Rules

- Fix broken paths, stale wording, missing overlap notes, and outdated scope statements as you find them.
- Keep wide tables compact enough to stay readable in wrapped editors; move verbose detail into bullets or notes.
- If the issue is really completion truth versus code, stop and use `verify-todo-section`.
