---
name: todo-pipeline
description: Run the 3-stage TODO preparation pipeline -- validate structure, run gap analysis with Win11/Linux research, then validate again as sanity check. Use before implementing a TODO file to ensure it is structurally sound, competitively complete, and ready for section-by-section implementation.
---

# TODO Pipeline

## Use This Skill When

- The user says "prepare TODO-XX for implementation" or "pipeline TODO-XX".
- A TODO file needs to go from draft/stale to implementation-ready.
- Before starting a batch of `/implement-todo-section` calls on a TODO.

## Pipeline Stages

```
Stage 1: /validate-todo-file     -- structural cleanup
Stage 2: /gap-audit-todo         -- Win11/Linux research, fill gaps (mandatory Codex red-team via /codex-gap-audit)
Stage 3: /validate-todo-file     -- sanity check after edits
```

Implementation of individual sections is manual (`/implement-todo-section §N`) -- the pipeline only prepares the TODO file.

## Workflow

### Stage 1 -- Structural Validation

1. Announce: `"Stage 1/3: Validating TODO structure..."`
2. Invoke the `validate-todo-file` skill on the target file.
3. Let it run to completion -- fix numbering, XREFs, flat sections, `§` notation, OS Comparison format, Unit Tests section, test checkpoints, etc.
4. After completion, summarize what was fixed.
5. **Gate check:** If Critical findings remain unresolved, STOP and report. Do not proceed to Stage 2 with a structurally broken file.

### Stage 2 -- Gap Analysis

6. Announce: `"Stage 2/3: Running gap analysis (Win11/Linux research)..."`
7. Invoke the `gap-audit-todo` skill on the target file. Its Phase 3.5 dispatches `codex-gap-audit` as a mandatory secondary red-team pass before turning the inventory into TODO edits; the wrapping pipeline does not need to invoke that skill directly.
8. Let it run to completion -- web research, feature inventory, missing section creation, cross-TODO overlap scan, code-truth audit.
9. After completion, summarize:
   - How many new sections were added
   - How many parity gaps (💎) vs competitive edges (⭐) were found
   - Any cross-TODO patches applied
   - Any status downgrades from code-truth audit
10. **User checkpoint:** Present the summary and ask: `"Gap analysis added N sections. Review the changes before Stage 3? (continue/stop)"`
    - If the user says stop, end the pipeline. They can review and re-run later.
    - If the user says continue (or doesn't object), proceed.

### Stage 3 -- Sanity Check

11. Announce: `"Stage 3/3: Final validation (sanity check)..."`
12. Invoke the `validate-todo-file` skill again on the same file.
13. This catches anything the gap analysis introduced that doesn't meet structural standards -- broken `§` references from renumbering, missing test checkpoints on new sections, Implementation Order rows without body sections, etc.
14. After completion, summarize final state:
    - Total sections
    - Sections ready for implementation (`[ ]`)
    - Sections already done (`[x]`)
    - Any remaining issues

### Pipeline Complete

15. Announce: `"Pipeline complete. TODO-XX is ready for implementation."`
16. Show the Implementation Order table so the user can see which sections to implement next.
17. Suggest: `"To implement, run: /implement-todo-section §N (starting from the first [ ] section)"`

## Guardrails

- Do NOT implement any code. This pipeline prepares the TODO, not the codebase.
- Do NOT skip Stage 1. A structurally broken TODO will produce bad gap analysis results.
- Do NOT skip the user checkpoint after Stage 2. Gap analysis can add many sections and the user should review before the sanity check locks them in.
- If any stage fails (skill error, critical unresolved findings), stop and report rather than continuing with bad state.
- Each stage invokes the full skill -- do not abbreviate or skip steps within a stage.
