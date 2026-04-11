---
name: review-todo-section
description: Post-implementation review pipeline. Runs verify (evidence mapping + test checkpoint, skips adversarial Codex since implementation already ran it) then quality review (MANDATORY Codex perf/consistency/dead-code). Use after implement-todo-section commits, or standalone for already-implemented sections.
---

# Review TODO Section

> Post-implementation review. The implementation skill already ran a Codex adversarial review (step 13). This skill focuses on what implementation missed: evidence mapping, test checkpoint verification, and a MANDATORY quality-phase Codex covering perf/consistency/dead-code.

## Pipeline

### Phase 1: Verify (evidence mapping -- no adversarial Codex)

1. **Evidence map** -- for each `[x]` item, prove via Grep/Read: "claim -> file:line -> snippet". Downgrade to `[/]` or `[ ]` on regression.
2. **Domain quality gates** -- spot-check boot-code-quality / kernel-code-quality / etc. against the implementation.
3. **Scope-gap audit** -- grep for `TODO`, `FIXME`, `HACK`, `STATUS_NOT_IMPLEMENTED`. Cross-check against TODO text.
4. **Test checkpoint verification** -- read the section's Test checkpoint paragraph. For each expected serial/klog line, grep the source to confirm the message exists and the code path is reachable. Flag any checkpoint claims that don't match the code.
5. **Build** -- `bash scripts/build.sh`, confirm `=== BUILD OK ===`.
6. **Reconcile tables** -- checklist items, IO row, OS Comparison row. Add Verified stamp if all items survived.

> **Why no adversarial Codex here:** The implementation skill's step 13 already dispatched one. Re-running the same adversarial angles on the same code 2 minutes later produces "no findings" (proven today on §11). The quality-phase Codex in Phase 2 asks DIFFERENT questions (perf/consistency/dead-code) and catches different bugs.

### Phase 2: Quality Review (MANDATORY Codex)

7. **Industry standards research** -- spec compliance for the section's domain.
8. **Win11/Linux parity** -- concrete function/file references.
9. **Codex comprehensive review** (MANDATORY -- NO EXCEPTIONS) -- single dispatch covering performance, consistency, AND dead code. If Codex says "no diff", provide file content. Apply `superpowers:receiving-code-review` to EVERY finding.
   ```bash
   node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<comprehensive prompt>"
   ```
   **CRITICAL -- Performance (mandatory):** allocations in hot paths, O(n^2), lock hold times, byte-at-a-time ops.
   **CRITICAL -- Consistency (mandatory):** struct layout matches, constants in one place, API contract violations, error code mapping.
   **CRITICAL -- Dead code (mandatory):** unreachable functions, unused defines, orphaned types, stale declarations.
10. **Feature completeness audit** -- grep for `STATUS_NOT_IMPLEMENTED` stubs, partial implementations, dead API promises. Standalone stubs: implement. Infrastructure stubs: Accept with XREF.
11. **Self-review** -- regressions, races, edge cases, resource leaks. Walk every fix before applying.
12. **Fix loop** -- fix all valid findings. Build after each batch.
13. **Stamps** -- Verified stamp (from Phase 1) + Quality reviewed stamp (from Phase 2). No blank line between.

### Phase 3: Commit

14. **Commit and push** -- single commit with both stamps + any fixes.
    - If only stamps: `"review: <TODO> §N -- verified + quality reviewed clean"`
    - If fixes: `"review: <TODO> §N -- <summary>"`

## When Called from implement-todo-section (Phase 2, step 20)

The implementation already ran:
- Codex adversarial review (step 13)
- Self-review (step 14)
- Fix loop (step 15)
- Build (step 16)

So this skill skips re-running adversarial Codex and focuses on evidence mapping + quality Codex (which asks different questions).

## When Called Standalone

If invoked directly (not from implement), the section was implemented in a prior session. In this case, the adversarial Codex skip is still valid IF the section has never been reviewed. If you want the full adversarial review, use `/verify-todo-section` + `/quality-review-section` separately.

## Rules

- Quality-phase Codex (step 9) is MANDATORY. No exceptions.
- `superpowers:receiving-code-review` on every Codex finding.
- All stamps use domain-qualified XREFs.
- No blank line between stamps.
