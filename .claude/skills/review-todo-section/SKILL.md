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
6. **Codex adversarial review (CONDITIONAL)** -- check if the section already has a `> **Verified:**` stamp from a prior run OR was just committed by `/implement-todo-section` in this session:
   - **If NO prior stamp AND NOT called from implement step 20:** this is a standalone review of old code. Run MANDATORY Codex adversarial review with all prescribed angles (integer overflow, buffer overread, NULL deref, SMP races, resource leaks, ABI mismatch, bounds). Apply `superpowers:receiving-code-review` to every finding.
   - **If called from implement step 20 OR section already has a Verified stamp:** skip adversarial Codex (implementation already ran one). The quality-phase Codex in Phase 2 asks DIFFERENT questions.
7. **Reconcile tables** -- checklist items, IO row, OS Comparison row. Add Verified stamp if all items survived.

### Phase 2: Quality Review (MANDATORY Codex)

8. **Industry standards research** -- spec compliance for the section's domain.
9. **Win11/Linux parity** -- concrete function/file references.
10. **Codex comprehensive review** (MANDATORY -- NO EXCEPTIONS) -- single dispatch covering performance, consistency, AND dead code. If Codex says "no diff", provide file content. Apply `superpowers:receiving-code-review` to EVERY finding.
   ```bash
   node "/home/derickpayne/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<comprehensive prompt>"
   ```
   **CRITICAL -- Performance (mandatory):** allocations in hot paths, O(n^2), lock hold times, byte-at-a-time ops.
   **CRITICAL -- Consistency (mandatory):** struct layout matches, constants in one place, API contract violations, error code mapping.
   **CRITICAL -- Dead code (mandatory):** unreachable functions, unused defines, orphaned types, stale declarations.
11. **Feature completeness audit** -- grep for `STATUS_NOT_IMPLEMENTED` stubs, partial implementations, dead API promises. Standalone stubs: implement. Infrastructure stubs: Accept with XREF.
12. **Self-review** -- regressions, races, edge cases, resource leaks. Walk every fix before applying.
13. **Fix loop** -- fix all valid findings. Build after each batch.
14. **Stamps** -- Verified stamp (from Phase 1) + Quality reviewed stamp (from Phase 2). No blank line between.

### Phase 3: Commit

15. **Commit and push** -- single commit with both stamps + any fixes.
    - If only stamps: `"review: <TODO> §N -- verified + quality reviewed clean"`
    - If fixes: `"review: <TODO> §N -- <summary>"`

## Adversarial Codex Decision Logic (step 6)

| Context | Has Verified stamp? | Adversarial Codex? |
|---------|--------------------|--------------------|
| Called from implement step 20 | No (just implemented) | **SKIP** -- implement step 13 already ran it |
| Called standalone, first review | No | **RUN** -- no prior adversarial review exists |
| Called standalone, re-review | Yes | **SKIP** -- prior review already covered adversarial angles |

## Rules

- Quality-phase Codex (step 9) is MANDATORY. No exceptions.
- `superpowers:receiving-code-review` on every Codex finding.
- All stamps use domain-qualified XREFs.
- No blank line between stamps.
